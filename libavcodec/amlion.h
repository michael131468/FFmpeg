#ifndef  _AMLION_H_
#define _AMLION_H_

#include <libavcodec/aml.h>

#define ION_DEVICE_NAME         "/dev/ion"
#define ION_VIDEO_DEVICE_NAME   "/dev/video13"

typedef struct {
  int ion_fd;
  int video_fd;
  int pixel_format;
  AMLBuffer buffers[AML_BUFFER_COUNT];

} AMLIonContext;



int aml_ion_open(AVCodecContext *avctx, AMLIonContext *ionctx);
int aml_ion_close(AVCodecContext *avctx, AMLIonContext *ionctx);
int aml_ion_create_buffer(AVCodecContext *avctx, AMLIonContext *ionctx, AMLBuffer *buffer);
int aml_ion_free_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, AMLBuffer *buffer);
int aml_ion_queue_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, AMLBuffer *buffer);
int aml_ion_dequeue_buffer(AVCodecContext *avctx,AMLIonContext *ionctx, int *got_buffer);

#endif /* _AMLION_H_ */
