#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/mman.h>

#include "avcodec.h"
#include "amldec.h"
#include "amltools.h"
#include "amlion.h"


#define BUFFER_PIXEL_FORMAT V4L2_PIX_FMT_RGB32
//#define BUFFER_PIXEL_FORMAT V4L2_PIX_FMT_NV12
//#define BUFFER_PIXEL_FORMAT V4L2_PIX_FMT_YUV420


enum ion_heap_type
{
  ION_HEAP_TYPE_SYSTEM,
  ION_HEAP_TYPE_SYSTEM_CONTIG,
  ION_HEAP_TYPE_CARVEOUT,
  ION_HEAP_TYPE_CHUNK,
  ION_HEAP_TYPE_CUSTOM,
  ION_NUM_HEAPS = 16
};


#define ION_HEAP_SYSTEM_MASK        (1 << ION_HEAP_TYPE_SYSTEM)
#define ION_HEAP_SYSTEM_CONTIG_MASK (1 << ION_HEAP_TYPE_SYSTEM_CONTIG)
#define ION_HEAP_CARVEOUT_MASK      (1 << ION_HEAP_TYPE_CARVEOUT)

typedef int ion_handle;

struct ion_allocation_data
{
  size_t len;
  size_t align;
  unsigned int heap_id_mask;
  unsigned int flags;
  ion_handle handle;
};

struct ion_fd_data
{
  ion_handle handle;
  int fd;
};

struct ion_handle_data
{
  ion_handle handle;
};

#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE  _IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_SHARE _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)

#define ALIGN(value, alignment) (((value)+(alignment-1))&~(alignment-1))

int vtop(int vaddr);

int aml_ion_open(AVCodecContext *avctx, AMLIonContext *ionctx)
{
  struct v4l2_format fmt = { 0 };
  struct v4l2_requestbuffers req = { 0 };
  int type;
  int ret;

  memset(ionctx, 0, sizeof(*ionctx));
  ionctx->pixel_format = BUFFER_PIXEL_FORMAT;
  
  // open the ion device
  if ((ionctx->ion_fd = open(ION_DEVICE_NAME, O_RDWR)) < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open %s\n", ION_DEVICE_NAME);
    return -1;
  }

  av_log(avctx, AV_LOG_DEBUG, "openned %s with fd=%d\n", ION_DEVICE_NAME, ionctx->ion_fd);

  // create the video Buffers
  for (int i=0; i < AML_BUFFER_COUNT; i++)
  {
    memset(&ionctx->buffers[i], 0, sizeof(ionctx->buffers[i]));
    ionctx->buffers[i].index = i;
    ret = aml_ion_create_buffer(avctx, ionctx, &ionctx->buffers[i]);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to create ion buffer %d\n", i);
      return -1;
    }
  }

  // open the ion video device
  if ((ionctx->video_fd = open(ION_VIDEO_DEVICE_NAME, O_RDWR | O_NONBLOCK)) < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open %s\n", ION_VIDEO_DEVICE_NAME);
    goto err;
  }

  av_log(avctx, AV_LOG_DEBUG, "openned %s with fd=%d\n", ION_VIDEO_DEVICE_NAME, ionctx->video_fd);

  // Now setup the format
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix_mp.width = avctx->width;
  fmt.fmt.pix_mp.height = avctx->height;
  fmt.fmt.pix_mp.pixelformat = ionctx->pixel_format;

  if (ioctl(ionctx->video_fd, VIDIOC_S_FMT, &fmt))
  {
    av_log(avctx, AV_LOG_ERROR, "ioctl for VIDIOC_S_FMT failed\n");
    goto err;
  }

  // setup the V4l buffers
  req.count = AML_BUFFER_COUNT;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_DMABUF;

  if (ioctl(ionctx->video_fd, VIDIOC_REQBUFS, &req))
  {
    av_log(avctx, AV_LOG_ERROR, "ioctl for VIDIOC_REQBUFS failed\n");
    goto err;
  }

  // setup streaming
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (ioctl(ionctx->video_fd, VIDIOC_STREAMON, &type))
  {
    av_log(avctx, AV_LOG_ERROR, "ioctl for VIDIOC_STREAMON failed\n");
    goto err;
  }

  // queue the buffers
  for (int i=0; i < AML_BUFFER_COUNT; i++)
  {
    ret = aml_ion_queue_buffer(avctx, ionctx, &ionctx->buffers[i]);
    if (ret < 0)
    {
      goto err;
    }
  }

  // setup vfm : we remove default frame handler and add ion handler
  amlsysfs_write_string(avctx, "/sys/class/vfm/map", "rm default");
  amlsysfs_write_string(avctx, "/sys/class/vfm/map", "add default decoder ionvideo");
  amlsysfs_write_int(avctx, "/sys/class/ionvideo/scaling_rate", 100);

  return 0;

err:
  aml_ion_close(avctx, ionctx);
  return -1;
}

int aml_ion_close(AVCodecContext *avctx, AMLIonContext *ionctx)
{
  int type;
  int ret;
  struct v4l2_requestbuffers req = { 0 };

  av_log(avctx, AV_LOG_DEBUG, "Closing ion driver\n");

  if (ionctx->video_fd)
  {
    // Stop streaming
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    av_log(avctx, AV_LOG_DEBUG, "Streaming Off video capture\n");
    if (ioctl(ionctx->video_fd, VIDIOC_STREAMOFF, &type))
    {
      av_log(avctx, AV_LOG_ERROR, "ioctl for VIDIOC_STREAMOFF failed\n");
      return -1;
    }

    // free the buffers
    av_log(avctx, AV_LOG_DEBUG, "Releasing V4L Buffers\n");
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;

    if (ioctl(ionctx->video_fd, VIDIOC_REQBUFS, &req))
    {
      av_log(avctx, AV_LOG_ERROR, "ioctl for VIDIOC_REQBUFS failed\n");
      return -1;
    }

    // close video device
    av_log(avctx, AV_LOG_DEBUG, "Closing ion video %s with fd=%d\n", ION_VIDEO_DEVICE_NAME, ionctx->video_fd);
    ret = close(ionctx->video_fd);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to close %s with fd=%d (code=%d)\n",
             ION_VIDEO_DEVICE_NAME, ionctx->video_fd, ret);
      return -1;
    }

    ionctx->video_fd = 0;
   }

  // close ion device
  if (ionctx->ion_fd)
  {
    // free the buffers
    for (int i=0; i < AML_BUFFER_COUNT; i++)
    {
      aml_ion_free_buffer(avctx, ionctx, &ionctx->buffers[i]);
    }

    av_log(avctx, AV_LOG_DEBUG, "Closing ion device %s with fd=%d\n", ION_DEVICE_NAME, ionctx->ion_fd);
    ret = close(ionctx->ion_fd);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to close %s with fd=%d (code=%d)\n",
             ION_DEVICE_NAME, ionctx->ion_fd, ret);
      return -1;
    }

    ionctx->ion_fd = 0;
  }

  return 0;
}

int aml_ion_create_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, AMLBuffer *buffer)
{
  struct ion_allocation_data ion_alloc;
  struct ion_fd_data fd_data;
  int ret;

  memset(&ion_alloc, 0, sizeof(ion_alloc));
  memset(&fd_data, 0 , sizeof(fd_data));

  buffer->width = avctx->width;
  buffer->height = avctx->height;

  switch(ionctx->pixel_format)
  {
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_YUV420:
      buffer->size = ALIGN(buffer->width, 16) * (ALIGN(buffer->height, 32) + ALIGN(buffer->height, 32) / 2);
      buffer->stride = ALIGN(buffer->width, 16);
      buffer->bpp = 1;
    break;

    case V4L2_PIX_FMT_RGB32:
      buffer->size = ALIGN(buffer->width, 16) * (ALIGN(buffer->height, 32)) * 4;
      buffer->stride = ALIGN(buffer->width, 16) * 4;
      buffer->bpp = 4;
    break;

  default:
    av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format\n");
    return -1;
    break;
  }

  // allocate the buffer
  ion_alloc.len = buffer->size;
  ion_alloc.heap_id_mask = ION_HEAP_CARVEOUT_MASK;

  int maxretry = 3;

  while (maxretry)
  {
    ret = ioctl(ionctx->ion_fd, ION_IOC_ALLOC, &ion_alloc);
    if (ret < 0)
    {
      if (maxretry == 0)
      {
        av_log(avctx, AV_LOG_ERROR, "failed to allocate ion buffer %d (code=%d)\n", buffer->index, ret);
        return -1;
      }
      else
      {
        // we failed, retry
        maxretry--;
        usleep(10000);
      }
    }
    else
      break;
  }
  buffer->handle = ion_alloc.handle;

  // share the buffer
  fd_data.handle = buffer->handle;
  ret = ioctl(ionctx->ion_fd, ION_IOC_SHARE, &fd_data);
  if (ret < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to retrieve ion buffer handle \n");
    return -1;
  }

  buffer->fd_handle = fd_data.fd;

  av_log(avctx, AV_LOG_DEBUG, "Alloced dmabuf bufferd #%d (h=%d, fd=%d, %d x %d)\n", buffer->index, buffer->handle, buffer->fd_handle, buffer->stride, buffer->height);

  return 0;
}

int aml_ion_free_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, AMLBuffer *buffer)
{
  int ret;
  struct ion_handle_data handle_data;

  if (buffer->handle)
  {
    memset(&handle_data, 0 , sizeof(handle_data));
    handle_data.handle = buffer->handle;

    av_log(avctx, AV_LOG_DEBUG, "Freeing buffer #%d\n", buffer->handle);

    ret = ioctl(ionctx->ion_fd, ION_IOC_FREE, &handle_data);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "failed to free ion buffer handle\n");
      return -1;
    }

    buffer->handle = 0;
  }

  if (buffer->fd_handle)
  {
    close(buffer->fd_handle);
    buffer->fd_handle = 0;
  }


  return 0;
}

int aml_ion_queue_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, AMLBuffer *buffer)
{
  int ret;
  struct v4l2_buffer vbuf;

  memset(&vbuf, 0, sizeof(vbuf));
  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  vbuf.memory = V4L2_MEMORY_DMABUF;
  vbuf.index = buffer->index;
  vbuf.m.fd = buffer->fd_handle;
  vbuf.length = buffer->size;

  ret = ioctl(ionctx->video_fd, VIDIOC_QBUF, &vbuf);
  if (ret < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "failed to queue ion buffer #%d (size = %d), code=%d\n", buffer->index, buffer->size, ret);
    return -1;
  }

  av_log(avctx, AV_LOG_DEBUG, "Queued ion buffer #%d (size = %d)\n", buffer->index, buffer->size);
  buffer->queued = 1;
  return buffer->index;
}


int aml_ion_dequeue_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, int *got_buffer)
{
  int ret;
  struct v4l2_buffer vbuf = { 0 };

  *got_buffer = 0;

  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  vbuf.memory = V4L2_MEMORY_DMABUF;

  ret = ioctl(ionctx->video_fd, VIDIOC_DQBUF, &vbuf);
  if (ret < 0)
  {
    if (errno == EAGAIN)
    {
       av_log(avctx, AV_LOG_DEBUG, "LongChair :dequeuing EAGAIN #%d, pts=%ld\n", vbuf.index, vbuf.timestamp.tv_usec);
       usleep(10000);
      return 0;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "failed to dequeue ion (code %d)\n", ret);
      return -1;
    }
  }

  ionctx->buffers[vbuf.index].queued = 0;
  ionctx->buffers[vbuf.index].pts = ((double)vbuf.timestamp.tv_usec / 1000000.0) / av_q2d(avctx->time_base);
  *got_buffer = 1;

  return vbuf.index;
}

