/* spectrum.c
 * Small FFT-based band analyser for the captions window visualizer.
 */

#include <math.h>
#include <string.h>
#include <glib.h>
#include "spectrum.h"

#define FFT_SIZE 512
#define HOP_SIZE 256
#define SAMPLE_RATE 16000.0f
#define MIN_FREQ 70.0f
#define MAX_FREQ 7800.0f
#define DB_FLOOR -75.0f
#define DB_CEIL  -15.0f

static float ring[FFT_SIZE];
static size_t ring_pos = 0;
static size_t since_last = 0;

static float window_fn[FFT_SIZE];
static float window_sum = 0.0f;
static int band_lo[SPECTRUM_BANDS], band_hi[SPECTRUM_BANDS];
static gsize initialized = 0;

static GMutex levels_mutex;
static float levels[SPECTRUM_BANDS];

static void init_tables(void) {
    window_sum = 0.0f;
    for(int i = 0; i < FFT_SIZE; i++) {
        window_fn[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1));
        window_sum += window_fn[i];
    }

    // Log-spaced bands; every band covers at least one FFT bin
    float bin_hz = SAMPLE_RATE / FFT_SIZE;
    int prev_hi = 0;
    for(int b = 0; b < SPECTRUM_BANDS; b++) {
        float f0 = MIN_FREQ * powf(MAX_FREQ / MIN_FREQ, (float)b / SPECTRUM_BANDS);
        float f1 = MIN_FREQ * powf(MAX_FREQ / MIN_FREQ, (float)(b + 1) / SPECTRUM_BANDS);
        int lo = (int)floorf(f0 / bin_hz);
        int hi = (int)ceilf(f1 / bin_hz);
        if(lo < prev_hi) lo = prev_hi;
        if(lo < 1) lo = 1;
        if(hi <= lo) hi = lo + 1;
        if(hi > FFT_SIZE / 2) hi = FFT_SIZE / 2;
        if(lo >= hi) lo = hi - 1;
        band_lo[b] = lo;
        band_hi[b] = hi;
        prev_hi = hi;
    }
}

// In-place iterative radix-2 FFT
static void fft(float *re, float *im, int n) {
    for(int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for(int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for(int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for(int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static void analyse(void) {
    float re[FFT_SIZE], im[FFT_SIZE];
    for(int i = 0; i < FFT_SIZE; i++) {
        re[i] = ring[(ring_pos + i) % FFT_SIZE] * window_fn[i];
        im[i] = 0.0f;
    }

    fft(re, im, FFT_SIZE);

    float out[SPECTRUM_BANDS];
    for(int b = 0; b < SPECTRUM_BANDS; b++) {
        float peak = 0.0f;
        for(int k = band_lo[b]; k < band_hi[b]; k++) {
            float p = re[k] * re[k] + im[k] * im[k];
            if(p > peak) peak = p;
        }
        float amp = 2.0f * sqrtf(peak) / window_sum;
        float db = 20.0f * log10f(amp + 1e-9f);
        float v = (db - DB_FLOOR) / (DB_CEIL - DB_FLOOR);
        out[b] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    g_mutex_lock(&levels_mutex);
    memcpy(levels, out, sizeof(levels));
    g_mutex_unlock(&levels_mutex);
}

void spectrum_feed(const short *data, size_t num_shorts) {
    if(g_once_init_enter(&initialized)) {
        init_tables();
        g_once_init_leave(&initialized, 1);
    }

    for(size_t i = 0; i < num_shorts; i++) {
        ring[ring_pos] = data[i] / 32768.0f;
        ring_pos = (ring_pos + 1) % FFT_SIZE;
        if(++since_last >= HOP_SIZE) {
            since_last = 0;
            analyse();
        }
    }
}

void spectrum_get(float *out) {
    g_mutex_lock(&levels_mutex);
    memcpy(out, levels, sizeof(levels));
    g_mutex_unlock(&levels_mutex);
}

void spectrum_reset(void) {
    g_mutex_lock(&levels_mutex);
    memset(levels, 0, sizeof(levels));
    g_mutex_unlock(&levels_mutex);
}
