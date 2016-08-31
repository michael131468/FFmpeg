/*
 * MMAL Video Decoder
 * Copyright (c) 2016 Lionel Chazallon
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * AMLogic Video Decoder
 */

#include "avcodec.h"
#include "aml.h"
#include "internal.h"
#include "amltools.h"

#include "libavutil/atomic.h"
#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"
#include <unistd.h>
#include "amldec.h"
#include "time.h"
#include <poll.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/videodev2.h>

#undef DEBUG
#define DEBUG (1)

// amvideo freerun_frame mode device & ioctl
#define AMV_DEVICE_NAME   "/dev/video10"

#define _A_M  'S'
#define AMSTREAM_IOC_GET_FREERUN_FRAME _IOR((_A_M), 0xb1, unsigned long)
#define AMSTREAM_IOC_PUT_FREERUN_FRAME _IOW((_A_M), 0xb2, unsigned long)
#define AMSTREAM_IOC_FREE_FREERUN_FRAME _IOW((_A_M), 0xb3, unsigned long)


#define MIN_DECODER_PACKET 16  // minimum packet amount in decoder before returning frames

void ffaml_log_decoder_info(AVCodecContext *avctx)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;

  av_log(avctx, AV_LOG_DEBUG, "Decoder buffer : filled %d bytes (%f%%), read=%d, write=%d, pkts written=%d\n",
        aml_context->buffer_status.data_len, 
        (double)(aml_context->buffer_status.data_len * 100) / (double)(aml_context->buffer_status.data_len + aml_context->buffer_status.free_len),
        aml_context->buffer_status.read_pointer, aml_context->buffer_status.write_pointer, aml_context->packets_written);
}

int ffmal_init_bitstream(AVCodecContext *avctx)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;
  int ret = 0;

  if (!aml_context->bsf)
  {
    const AVBitStreamFilter *bsf;

    // check if we need a bitstream filter
    switch(avctx->codec_id)
    {
      case AV_CODEC_ID_H264:
        bsf = av_bsf_get_by_name("h264_mp4toannexb");
      break;

      case AV_CODEC_ID_HEVC:
        bsf = av_bsf_get_by_name("hevc_mp4toannexb");
      break;

    default:
      av_log(avctx, AV_LOG_DEBUG, "Not using any bitstream filter\n");
      return 0;
    }

    if(!bsf)
        return AVERROR_BSF_NOT_FOUND;

    av_log(avctx, AV_LOG_DEBUG, "using bitstream filter %s\n", bsf->name);

    if ((ret = av_bsf_alloc(bsf, &aml_context->bsf)))
        return ret;

    if (((ret = avcodec_parameters_from_context(aml_context->bsf->par_in, avctx)) < 0) ||
        ((ret = av_bsf_init(aml_context->bsf)) < 0))
    {
        av_bsf_free(&aml_context->bsf);
        return ret;
    }
  }

  return 0;
}

int ffaml_write_codec_data(AVCodecContext *avctx, char *data, int size)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;
  codec_para_t *pcodec  = &aml_context->codec;
  int bytesleft = size;
  int written = 0;
  static int totalbytes = 0;

#if DEBUG
  av_log(avctx, AV_LOG_DEBUG, "codec -> (%d bytes) : %x %x %x %x\n", size, data[0], data[1], data[2], data[3]);
#endif

  while (bytesleft)
  {
    written = codec_write(pcodec, data, bytesleft);
    if (written < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to write data to codec (code = %d)\n", written);
      usleep(10);
    }
    else
    {
      data += written;
      bytesleft -= written;
    }
  }

  totalbytes += size;
  av_log(avctx, AV_LOG_DEBUG, "codec total bytes = %d\n", totalbytes);
  return 0;
}

void ffaml_create_prefeed_header(AVCodecContext *avctx, AVPacket* pkt, AMLHeader *header, char *extradata, int extradatasize)
{
  switch(aml_get_vformat(avctx))
  {
    case VFORMAT_VC1:
      memcpy(header->data, extradata+1, extradatasize-1);
      header->size = extradatasize-1;
    break;

    default:
      // just copy over the extradata for those
      memcpy(header->data, extradata, extradatasize);
      header->size = extradatasize;
    break;
  }
}

void ffaml_checkin_packet_pts(AVCodecContext *avctx, AVPacket *avpkt)
{
  int ret;
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;

  double pts = ((double)avpkt->pts * (double)PTS_FREQ) * av_q2d(avctx->time_base);
  av_log(avctx, AV_LOG_DEBUG, "checking in  pts =%f\n", pts);
  if ((ret = codec_checkin_pts(&aml_context->codec, (unsigned long)pts)) < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to checkin the pts (code = %d)\n", ret);
  }
}

static av_cold int ffaml_init_decoder(AVCodecContext *avctx)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;
  codec_para_t *pcodec  = &aml_context->codec;
  int ret = 0;

  // reset the first packet attribute
  aml_context->first_packet = 1;
  aml_context->bsf = NULL;
  aml_context->packets_written = 0;
  aml_context->frame_count = 0;

  ffaml_init_queue(&aml_context->framequeue);

  // setup the aml decoding pipeline
  amlsysfs_write_string(avctx, "/sys/class/vfm/map", "rm default");
  amlsysfs_write_string(avctx, "/sys/class/vfm/map", "add default decoder ppmgr deinterlace amlvideo amvideo");

  amlsysfs_write_int(avctx, "/sys/class/video/blackout_policy", 0);                   // disable blackout policy
  amlsysfs_write_int(avctx, "/sys/module/amlvideodri/parameters/freerun_mode", 1);    // enable freerun
  amlsysfs_write_int(avctx, "/sys/class/tsync/enable", 0);                            // disable tsync, we are playing video disconnected from audio.

  // setup the codec structure for amcodec
  memset(pcodec, 0, sizeof(codec_para_t));
  memset(&aml_context->buffer_status, 0, sizeof(aml_context->buffer_status));

  pcodec->stream_type = STREAM_TYPE_ES_VIDEO;
  pcodec->has_video = 1;
  pcodec->video_type = aml_get_vformat(avctx);
  pcodec->am_sysinfo.format = aml_get_vdec_type(avctx);
  pcodec->am_sysinfo.param = (void*)(EXTERNAL_PTS | SYNC_OUTSIDE);
  pcodec->am_sysinfo.width = avctx->width;
  pcodec->am_sysinfo.height = avctx->height;
  pcodec->noblock = 0;

  // checks if codec formats and decoder have been properly setup
  if (pcodec->video_type == -1)
  {
    av_log(avctx, AV_LOG_ERROR, "Cannot determine proper video type : Codec ID=%d\n", avctx->codec_id);
    return -1;
  }

  if (pcodec->am_sysinfo.format == -1)
  {
    av_log(avctx, AV_LOG_ERROR, "Cannot determine proper video decder : Codec TAG=0x%x\n", avctx->codec_tag);
    return -1;
  }

  // open the aml video device
  if ((aml_context->amv_fd = open(AMV_DEVICE_NAME, O_RDWR)) <= 0)
  {
   av_log(avctx, AV_LOG_ERROR, "Failed to open %s (code : %d)\n", AMV_DEVICE_NAME, aml_context->amv_fd);
   return -1;
  }

   av_log(avctx, AV_LOG_DEBUG, "openned %s with fd=%d\n", AMV_DEVICE_NAME, aml_context->amv_fd);

  ret = codec_init(pcodec);
  if (ret != CODEC_ERROR_NONE)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to init amcodec decoder\n");
    return -1;
  }

  codec_resume(pcodec);

  // eventually create a bistream filter for formats tha require it
  ret = ffmal_init_bitstream(avctx);
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to init AML bitstream\n");
    return -1;
  }

  av_log(avctx, AV_LOG_DEBUG, "amcodec intialized successfully (%s / %s)\n",
         aml_get_vformat_name(pcodec->video_type),
         aml_get_vdec_name(pcodec->am_sysinfo.format));

  return 0;
}

static av_cold int ffaml_close_decoder(AVCodecContext *avctx)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;
  codec_para_t *pcodec  = &aml_context->codec;


  // we need to blackout here otherwise codec buffers won't
  // be realeased properly as some buffr are uses by display.
  amlsysfs_write_int(avctx, "/sys/class/video/blackout_policy", 1);

  if (pcodec)
  {
    codec_close(pcodec);
  }

  // free bitstream
  if (aml_context->bsf)
    av_bsf_free(&aml_context->bsf);

  // close amvideo driver
  if (aml_context->amv_fd)
  {
    av_log(avctx, AV_LOG_DEBUG, "closed device %s with fd=%d\n", AMV_DEVICE_NAME, aml_context->amv_fd);
    close(aml_context->amv_fd);
  }

  av_log(avctx, AV_LOG_DEBUG, "amcodec closed successfully\n");
  return 0;
}

void ffaml_get_packet_header(AVCodecContext *avctx, AMLHeader *header, AVPacket *pkt)
{

  header->size = 0;

  switch(aml_get_vformat(avctx))
  {
    case VFORMAT_VC1:
      if ((pkt->data[0]==0x0) && (pkt->data[1]==0x0) && (pkt->data[2]==0x1) && \
          ((pkt->data[3]==0xD) || (pkt->data[3]==0xF)))
      {
        // then header is already there, we don't need it
      }
      else
      {
        // otherwise, add the header
        header->data[0] = 0x0;
        header->data[1] = 0x0;
        header->data[2] = 0x1;
        header->data[3] = 0xd;
        header->size = 4;
      }
    break;
  }
}

static int ffaml_decode(AVCodecContext *avctx, void *data, int *got_frame,
                         AVPacket *avpkt)
{
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;
  AMLHeader header = { 0 } ;
  codec_para_t *pcodec  = &aml_context->codec;
  int ret = 0;
  AVPacket filter_pkt = {0};
  AVPacket filtered_packet = {0};
  AVFrame *frame = data;
  uint8_t *extradata;
  int extradata_size;
  int enough_space = 0;
  unsigned long frame_handle;
  AMLBuffer *buffer;

#if DEBUG
  ffaml_log_decoder_info(avctx);
#endif

  if ((!avpkt) && (!avpkt->data) || (avpkt->size == 0))
    return 0;

  // grab the video decoder buffer status
  do
  {
    ret = codec_get_vbuf_state(pcodec, &aml_context->buffer_status);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to retrieve video buffer status (code=%d)\n", ret);
      return -1;
    }

    // compute buffer fullpercentage
    aml_context->decoder_buffer_pc =  (double)(aml_context->buffer_status.data_len * 100)
        / (double)(aml_context->buffer_status.data_len + aml_context->buffer_status.free_len);


    if (aml_context->buffer_status.free_len > (avpkt->size + MAX_HEADER_SIZE))
      enough_space = 1;
    else
    {
      av_log(avctx, AV_LOG_DEBUG, "decoder input buffer stuffed, waiting 10ms\n");
      usleep(10000);
    }

  } while (!enough_space);

  // here we should have a valid packet and enough space in the decoder
  // so let's start to write

  // first we bitstream the packet if it's required
  // this seems to be requried for H264 / HEVC / H265
  if (aml_context->bsf)
  {
    // if we use a bitstream filter, then use it
    if ((ret = av_packet_ref(&filter_pkt, avpkt)) < 0)
        return ret;

    if ((ret = av_bsf_send_packet(aml_context->bsf, &filter_pkt)) < 0) {
        av_packet_unref(&filter_pkt);
        return ret;
    }

    if ((ret = av_bsf_receive_packet(aml_context->bsf, &filtered_packet)) < 0)
        return ret;

    //av_packet_unref(avpkt);
    avpkt = &filtered_packet;
    extradata = aml_context->bsf->par_out->extradata;
    extradata_size = aml_context->bsf->par_out->extradata_size;
  }
  else
  {
    // otherwise, we shouldn't need it, just use plain extradata
    extradata = avctx->extradata;
    extradata_size = avctx->extradata_size;
  }

  // now we need to write packet to decoder which requires
  // - a prefeed header priori to any packet
  // - an optionnal packet header
  // - the packet data itself

  // we need to write the prefeed header on first packet
  if (aml_context->first_packet)
  {
    // we need make a header from extradata to prefeed the decoder
    ffaml_create_prefeed_header(avctx, avpkt, &header, extradata, extradata_size);

    if (header.size > 0)
    {
      ret = ffaml_write_codec_data(avctx, header.data, header.size);
      if (ret < 0)
      {
        av_log(avctx, AV_LOG_ERROR, "Failed to write prefeed header\n");
        return -1;
      }
    }

    aml_context->first_packet = 0;
  }

  // checkin the codec pts
  ffaml_checkin_packet_pts(avctx, avpkt);

  // now write the packet header if any
  ffaml_get_packet_header(avctx, &header, avpkt);
  if (header.size > 0)
  {
    ret = ffaml_write_codec_data(avctx, header.data, header.size);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Failed to write packet header\n");
      return -1;
    }
  }

  // now write packet data
#if DEBUG
  av_log(avctx, AV_LOG_DEBUG, "Writing frame with pts=%f, wpkt=%d, size=%d header=%d\n",
         avpkt->pts * av_q2d(avctx->time_base),
         aml_context->packets_written,
         avpkt->size,
         header.size);
#endif

  ret = ffaml_write_codec_data(avctx, avpkt->data, avpkt->size);
  if (ret < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to write packet data\n");
    return -1;
  }

  aml_context->packets_written++;

  // realease the ref created by filtered packet
  if (aml_context->bsf)
    av_packet_unref(avpkt);

  // make sure we have enough packets in decoder
  // some streams like h264, have packets in the wrong pts order
  // so we need to have enough to be able to reorder them outselves
//  if (aml_context->packets_written < MIN_DECODER_PACKET)
//    return 0;

  struct v4l2_buffer vbuf = { 0 };
  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int64_t aml_pts;

  for (int i=0; i < 10; i++)
  {
    ret = ioctl(aml_context->amv_fd, VIDIOC_DQBUF, &vbuf);

    if (ret==0)
    {
      *got_frame = 1;
      aml_pts = vbuf.timestamp.tv_sec * PTS_FREQ + vbuf.timestamp.tv_usec * PTS_FREQ / 1000000;
      break;
    }
    else
    {
      usleep(5000);
    }
  }

  if (*got_frame)
  {
    aml_context->frame_count++;

    frame->format = AV_PIX_FMT_AML;
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->linesize[0] = avctx->width;

    frame->buf[0] = av_buffer_create(NULL, 0, NULL, NULL, AV_BUFFER_FLAG_READONLY);
    frame->data[0] = (uint8_t*)pcodec;
    frame->pkt_pts = (int64_t)((double)aml_pts / (double)PTS_FREQ / av_q2d(avctx->time_base));

#if DEBUG
    av_log(avctx, AV_LOG_DEBUG, "Sending frame %d with pts =%f, handle=%ld\n",
           aml_context->frame_count,
           (double)frame->pkt_pts * av_q2d(avctx->time_base),
           frame_handle);
#endif
  }

  return 0;
}

static void ffaml_flush(AVCodecContext *avctx)
{
  int count;
  AMLDecodeContext *aml_context = (AMLDecodeContext*)avctx->priv_data;


  av_log(avctx, AV_LOG_DEBUG, "Flushing ...\n");

  ffaml_close_decoder(avctx);
  ffaml_init_decoder(avctx);
  amlsysfs_write_int(avctx, "/sys/class/video/disable_video", 1);   // disable video output
  amlsysfs_write_int(avctx, "/sys/class/video/blackout_policy", 0); // disable blackout again

  av_log(avctx, AV_LOG_DEBUG, "Flushing done.\n");

}


#define FFAML_DEC_HWACCEL(NAME, ID) \
  AVHWAccel ff_##NAME##_aml_hwaccel = { \
      .name       = #NAME "_aml", \
      .type       = AVMEDIA_TYPE_VIDEO,\
      .id         = ID, \
      .pix_fmt    = AV_PIX_FMT_AML,\
  };

#define FFAML_DEC_CLASS(NAME) \
    static const AVClass ffaml_##NAME##_dec_class = { \
        .class_name = "aml_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFAML_DEC(NAME, ID) \
    FFAML_DEC_CLASS(NAME) \
    FFAML_DEC_HWACCEL(NAME, ID) \
    AVCodec ff_##NAME##_aml_decoder = { \
        .name           = #NAME "_aml", \
        .long_name      = NULL_IF_CONFIG_SMALL(#NAME " (aml)"), \
        .type           = AVMEDIA_TYPE_VIDEO, \
        .id             = ID, \
        .priv_data_size = sizeof(AMLDecodeContext), \
        .init           = ffaml_init_decoder, \
        .close          = ffaml_close_decoder, \
        .decode         = ffaml_decode, \
        .flush          = ffaml_flush, \
        .priv_class     = &ffaml_##NAME##_dec_class, \
        .capabilities   = AV_CODEC_CAP_DELAY, \
        .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS, \
        .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P /*AV_PIX_FMT_AML*/, \
                                                         AV_PIX_FMT_NONE}, \
    };

FFAML_DEC(h264, AV_CODEC_ID_H264)

FFAML_DEC(hevc, AV_CODEC_ID_HEVC)

FFAML_DEC(mpeg4, AV_CODEC_ID_MPEG4)
FFAML_DEC(msmpeg4v1, AV_CODEC_ID_MSMPEG4V1)
FFAML_DEC(msmpeg4v2, AV_CODEC_ID_MSMPEG4V2)
FFAML_DEC(msmpeg4v3, AV_CODEC_ID_MSMPEG4V3)

FFAML_DEC(vc1, AV_CODEC_ID_VC1)

FFAML_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO)
