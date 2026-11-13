/* spectrum.h
 * Small FFT-based band analyser for the captions window visualizer.
 * Fed from the audio thread, read from the GTK main thread.
 */

#pragma once

#include <stddef.h>

#define SPECTRUM_BANDS 32

// Feed 16 kHz mono PCM16 samples (any thread)
void spectrum_feed(const short *data, size_t num_shorts);

// Copy the latest band levels (0..1) into out[SPECTRUM_BANDS]
void spectrum_get(float *out);

// Zero all levels (e.g. when listening stops)
void spectrum_reset(void);
