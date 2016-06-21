#ifndef LAVC_AML_H
#define LAVC_AML_H

#include "libavutil/common.h"

#define AML_BUFFER_COUNT        (4)

typedef struct {
  int width;
  int height;
  int bpp;
  int stride;
  int size;

  int64_t pts;

  int handle;    // handle to the allocated buffer in ion memory
  int fd_handle; // file descriptor to  buffer for dmabuf

  int queued;
  int index;
} AMLBuffer;

#endif // LAVC_AML_H
