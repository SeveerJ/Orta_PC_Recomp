/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Initialize with 2 (stereo) or 6 (5.1) channels of input. Returns the
 * device's own channel count on success, 0 on failure. */
int xa2_init_channels(int channels);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

#endif /* APU_XAUDIO2_H */
