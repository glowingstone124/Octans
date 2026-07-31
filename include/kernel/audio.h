#ifndef LAMP_KERNEL_AUDIO_H
#define LAMP_KERNEL_AUDIO_H

#include "types.h"

void audio_init(void);
void audio_irq_handler(void);
uint32_t audio_active(void);

/* The PCM buffer must remain valid until its completion interrupt arrives. */
uint32_t audio_submit_pcm(const int16_t *samples, uint32_t frames,
                          uint32_t cookie);
uint32_t audio_completed_count(void);
uint32_t audio_error_count(void);

#endif
