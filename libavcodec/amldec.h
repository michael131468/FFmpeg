#ifndef _AMLDEC_H_
#define _AMLDEC_H_

#include "amlqueue.h"
#include "amlion.h"
#include <amcodec/codec.h>
#include <time.h>

// AMCodec defines
#define TRICKMODE_NONE  0x00
#define TRICKMODE_I     0x01
#define TRICKMODE_FFFB  0x02

#define EXTERNAL_PTS    1
#define SYNC_OUTSIDE    2

#define PTS_FREQ       90000
#define AV_SYNC_THRESH PTS_FREQ * 1


#define MAX_HEADER_SIZE         4096
#define MIN_DECODER_PACKETS     16
#define MAX_DEQUEUE_TIMEOUT_MS  100

typedef struct {
  char data[MAX_HEADER_SIZE];
  int size;
} AMLHeader;

typedef struct
{
  double pts;
} AMLFramePrivate;

typedef struct {
  AVClass *av_class;
  codec_para_t codec;
  int first_packet;
//  double last_checkin_pts;
  AVBSFContext *bsf;
//  PacketQueue writequeue;
  struct buf_status buffer_status;
  struct vdec_status decoder_status;
//  AMLHeader prefeed_header;
  AMLIonContext ion_context;
  int packets_written;
} AMLDecodeContext;

// Functions prototypes
int   ffmal_init_bitstream(AVCodecContext *avctx);
int   ffaml_write_codec_data(AVCodecContext *avctx, char *data, int size);
//int   ffaml_write_pkt_data(AVCodecContext *avctx, AVPacket *avpkt, AMLHeader *header);
void  ffaml_checkin_packet_pts(AVCodecContext *avctx, AVPacket *avpkt);
void  ffaml_create_prefeed_header(AVCodecContext *avctx, AVPacket* pkt, AMLHeader *header, char *extradata, int extradatasize);
void  ffaml_get_packet_header(AVCodecContext *avctx, AMLHeader *header, AVPacket *pkt);
void  ffaml_log_decoder_info(AVCodecContext *avctx);

#endif /* _AMLDEC_H_ */
