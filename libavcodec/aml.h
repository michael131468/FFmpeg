#ifndef LAVC_AML_H
#define LAVC_AML_H

#include "libavutil/common.h"

#define AML_BUFFER_COUNT        (2)

typedef struct {
  int width;
  int height;
  int bpp;
  int stride;
  int size;

  int64_t pts;
  double fpts;

  int handle;    // handle to the allocated buffer in ion memory
  int fd_handle; // file descriptor to  buffer for dmabuf

  int queued;
  int requeue;
  int index;
} AMLBuffer;

#endif // LAVC_AML_H
