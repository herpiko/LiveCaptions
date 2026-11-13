/* audiocap-pa.c
 * This file contains the pulse implementation of audio_thread
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pulse/introspect.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <pulse/pulseaudio.h>
#include <glib.h>

#include <april_api.h>

#include "audiocap-internal.h"
#include "audiocap.h"

#define MAX_INPUTS 2
// Each input buffers at most 2 seconds while waiting to be mixed
#define MIX_MAX_SAMPLES (2 * 16000)
// If one input has 200 ms waiting and the other has nothing (e.g. a suspended
// output's monitor), pass it through alone instead of waiting forever
#define MIX_SOLO_SAMPLES (16000 / 5)

struct pa_input {
    audio_thread_pa parent;
    const char *label;
    char *requested;      // user-selected source, or NULL for the default
    pa_stream *stream;
    short buf[MIX_MAX_SAMPLES];
    size_t len;
};

struct audio_thread_pa_i {
    asr_thread asr;
    bool microphone;      // also capture the microphone, mixed with desktop audio
    size_t sample_rate;

    char *sink_name;
    char *source_name;

    int requested_lookup; // 0 = pending, 1 = found, -1 = not found

    pa_threaded_mainloop *mainloop;
    pa_mainloop_api *mainloop_api;
    pa_context *context;

    // inputs[0] is desktop audio; inputs[1] is the microphone when enabled
    int n_inputs;
    struct pa_input inputs[MAX_INPUTS];
    short mix_out[MIX_MAX_SAMPLES];
};

static void context_state_cb(pa_context* context, void* userdata);
static void stream_state_cb(pa_stream *s, void *userdata);
static void stream_success_cb(pa_stream *stream, int success, void *userdata);
static void stream_read_cb(pa_stream *stream, size_t nbytes, void *userdata);

static void requested_source_info_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    audio_thread_pa data = (audio_thread_pa)userdata;

    if(eol == 0 && i != NULL) {
        data->requested_lookup = 1;
        return;
    }

    if(data->requested_lookup == 0) data->requested_lookup = -1;
    pa_threaded_mainloop_signal(data->mainloop, 0);
}

static void server_info_callback(pa_context *c, const pa_server_info *i, void *userdata){
    audio_thread_pa data = (audio_thread_pa)userdata;

    data->source_name = (char *)calloc(1, strlen(i->default_source_name) + 1);
    strcpy(data->source_name, i->default_source_name);

    data->sink_name = (char *)calloc(1, strlen(i->default_sink_name) + 9);
    strcpy(data->sink_name, i->default_sink_name);
    strcat(data->sink_name, ".monitor");

    pa_threaded_mainloop_signal(data->mainloop, 0);
}

// Returns the user-selected source if it currently exists, otherwise the
// fallback, so an unplugged device doesn't break captioning.
// Must be called with the mainloop locked.
static const char *resolve_source(audio_thread_pa data, const char *requested, const char *fallback) {
    if(requested == NULL) return fallback;

    data->requested_lookup = 0;
    pa_operation *op = pa_context_get_source_info_by_name(data->context,
        requested, requested_source_info_cb, data);
    if(op != NULL) {
        while(pa_operation_get_state(op) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(data->mainloop);
        pa_operation_unref(op);
    }

    if(data->requested_lookup == 1) return requested;

    printf("Audio source %s not found, using default\n", requested);
    return fallback;
}

// Creates, connects and starts a recording stream for one input.
// dev_name NULL means the default input device.
// Must be called with the mainloop locked.
static void open_input(audio_thread_pa data, struct pa_input *input, const char *dev_name) {
    pa_sample_spec sample_specifications;
    sample_specifications.format = PA_SAMPLE_S16LE;
    sample_specifications.rate = data->sample_rate;
    sample_specifications.channels = 1;

    pa_channel_map map;
    pa_channel_map_init_mono(&map);

    input->stream = pa_stream_new(data->context, input->label, &sample_specifications, &map);
    g_assert(input->stream);

    pa_stream_set_state_callback(input->stream, stream_state_cb, data);
    pa_stream_set_read_callback(input->stream, stream_read_cb, input);

    // recommended settings, i.e. server uses sensible values
    pa_buffer_attr buffer_attr;
    buffer_attr.maxlength = (uint32_t) -1;
    buffer_attr.tlength = (uint32_t) -1;
    buffer_attr.prebuf = (uint32_t) -1;
    buffer_attr.minreq = (uint32_t) -1;
    buffer_attr.fragsize = pa_usec_to_bytes(50000, &sample_specifications);

    // Settings copied as per the chromium browser source
    pa_stream_flags_t stream_flags;
    stream_flags = PA_STREAM_START_CORKED | PA_STREAM_INTERPOLATE_TIMING |
        PA_STREAM_NOT_MONOTONIC | PA_STREAM_AUTO_TIMING_UPDATE |
        PA_STREAM_ADJUST_LATENCY;

    assert(pa_stream_connect_record(input->stream, dev_name, &buffer_attr, stream_flags) == 0);

    // Wait for the stream to be ready
    for(;;) {
        pa_stream_state_t stream_state = pa_stream_get_state(input->stream);
        assert(PA_STREAM_IS_GOOD(stream_state));
        if (stream_state == PA_STREAM_READY) break;
        pa_threaded_mainloop_wait(data->mainloop);
    }

    // Uncork the stream so it will start recording
    pa_stream_cork(input->stream, 0, stream_success_cb, data);
    for(;;) {
        if (pa_stream_is_corked(input->stream) == 0) break;
        pa_threaded_mainloop_wait(data->mainloop);
    }
}

void *run_audio_thread_pa(void *userdata) {
    audio_thread_pa data = (audio_thread_pa)userdata;

    // Get a mainloop and its context
    data->mainloop = pa_threaded_mainloop_new();
    g_assert(data->mainloop);
    data->mainloop_api = pa_threaded_mainloop_get_api(data->mainloop);
    data->context = pa_context_new(data->mainloop_api, "lcap-acap");
    g_assert(data->context);

    // Set a callback so we can wait for the context to be ready
    pa_context_set_state_callback(data->context, &context_state_cb, data);

    // Lock the mainloop so that it does not run and crash before the context is ready
    pa_threaded_mainloop_lock(data->mainloop);

    // Start the mainloop
    g_assert(pa_threaded_mainloop_start(data->mainloop) == 0);
    g_assert(pa_context_connect(data->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) == 0);

    // Wait for the context to be ready
    for(;;) {
        pa_context_state_t context_state = pa_context_get_state(data->context);
        assert(PA_CONTEXT_IS_GOOD(context_state));
        if (context_state == PA_CONTEXT_READY) break;
        pa_threaded_mainloop_wait(data->mainloop);
    }

    pa_context_get_server_info(data->context, server_info_callback, data);
    for(;;) {
        if(data->source_name != NULL) break;
        pa_threaded_mainloop_wait(data->mainloop);
    }

    // Desktop audio is always captured; the microphone is added to it
    open_input(data, &data->inputs[0],
        resolve_source(data, data->inputs[0].requested, data->sink_name));

    if(data->microphone)
        open_input(data, &data->inputs[1],
            resolve_source(data, data->inputs[1].requested, NULL));

    pa_threaded_mainloop_unlock(data->mainloop);

    return NULL;
}

static void context_state_cb(pa_context* context, void* userdata) {
    audio_thread_pa data = userdata;
    pa_threaded_mainloop_signal(data->mainloop, 0);
}

static void stream_state_cb(pa_stream *s, void *userdata) {
    audio_thread_pa data = userdata;
    pa_threaded_mainloop_signal(data->mainloop, 0);
}

static void input_consume(struct pa_input *input, size_t n) {
    memmove(input->buf, input->buf + n, (input->len - n) * sizeof(short));
    input->len -= n;
}

// Mixes whatever both inputs have available and sends it to the ASR thread.
// All read callbacks run on the PulseAudio mainloop thread, so no locking.
static void mix_and_send(audio_thread_pa data) {
    struct pa_input *a = &data->inputs[0];
    struct pa_input *b = &data->inputs[1];

    size_t n = MIN(a->len, b->len);
    if(n > 0) {
        for(size_t i = 0; i < n; i++) {
            int s = (int)a->buf[i] + (int)b->buf[i];
            data->mix_out[i] = (short)CLAMP(s, -32768, 32767);
        }
        if(data->asr != NULL) asr_thread_enqueue_audio(data->asr, data->mix_out, n);
        input_consume(a, n);
        input_consume(b, n);
    }

    // One input stalled (nothing playing, suspended device): don't hold up the other
    struct pa_input *pairs[2][2] = { { a, b }, { b, a } };
    for(int k = 0; k < 2; k++) {
        struct pa_input *x = pairs[k][0], *y = pairs[k][1];
        if(y->len == 0 && x->len >= MIX_SOLO_SAMPLES) {
            if(data->asr != NULL) asr_thread_enqueue_audio(data->asr, x->buf, x->len);
            x->len = 0;
        }
    }
}

static void stream_read_cb(pa_stream *stream, size_t nbytes, void *userdata) {
    struct pa_input *input = userdata;
    audio_thread_pa data = input->parent;

    ssize_t nbytes1 = (ssize_t)nbytes;
    while(nbytes1 > 0) {
        size_t count = nbytes1;
        const void *audio_data;
        int result = pa_stream_peek(stream, &audio_data, &count);

        if(count == 0) return;

        if(audio_data == NULL) {
            // hole
            pa_stream_drop(stream);
            return;
        }

        if(result != 0) {
            printf("pa_stream_peek error %d\n", result);
            return;
        }

        size_t samples = count / 2;
        if(data->n_inputs == 1) {
            if(data->asr != NULL)
                asr_thread_enqueue_audio(data->asr, (short *)audio_data, samples);
        } else {
            // Keep the newest audio if the buffer is somehow full
            if(samples > MIX_MAX_SAMPLES) samples = MIX_MAX_SAMPLES;
            if(input->len + samples > MIX_MAX_SAMPLES)
                input_consume(input, input->len + samples - MIX_MAX_SAMPLES);
            memcpy(input->buf + input->len, audio_data, samples * sizeof(short));
            input->len += samples;

            mix_and_send(data);
        }

        pa_stream_drop(stream);

        nbytes1 -= count;
    }
}

static void stream_success_cb(pa_stream *stream, int success, void *userdata) {
    audio_thread_pa data = userdata;
    pa_threaded_mainloop_signal(data->mainloop, 0);
}


audio_thread_pa create_audio_thread_pa(bool microphone, const char *desktop_source,
                                       const char *microphone_source, asr_thread asr) {
    audio_thread_pa data = calloc(1, sizeof(struct audio_thread_pa_i));

    data->microphone = microphone;
    data->n_inputs = microphone ? 2 : 1;
    data->asr = asr;
    data->sample_rate = asr_thread_samplerate(asr);

    const char *requested[MAX_INPUTS] = { desktop_source, microphone_source };
    const char *labels[MAX_INPUTS] = { "Desktop audio", "Microphone" };
    for(int i = 0; i < MAX_INPUTS; i++) {
        data->inputs[i].parent = data;
        data->inputs[i].label = labels[i];
        if(requested[i] != NULL && requested[i][0] != '\0')
            data->inputs[i].requested = g_strdup(requested[i]);
    }

    return data;
}


void free_audio_thread_pa(audio_thread_pa thread){
    // Cork the streams
    pa_threaded_mainloop_lock(thread->mainloop);
    for(int i = 0; i < thread->n_inputs; i++) {
        pa_stream *stream = thread->inputs[i].stream;
        if(stream == NULL) continue;
        pa_stream_cork(stream, 1, stream_success_cb, thread);
        for(;;) {
            if (pa_stream_is_corked(stream) == 1) break;
            pa_threaded_mainloop_wait(thread->mainloop);
        }
    }
    pa_threaded_mainloop_unlock(thread->mainloop);

    // Stop the main loop
    pa_threaded_mainloop_stop(thread->mainloop);

    // Disconnect and free everything
    for(int i = 0; i < thread->n_inputs; i++) {
        if(thread->inputs[i].stream == NULL) continue;
        pa_stream_disconnect(thread->inputs[i].stream);
        pa_stream_unref(thread->inputs[i].stream);
    }

    pa_context_disconnect(thread->context);
    pa_context_unref(thread->context);

    pa_threaded_mainloop_free(thread->mainloop);

    free(thread->sink_name);
    free(thread->source_name);
    for(int i = 0; i < MAX_INPUTS; i++) g_free(thread->inputs[i].requested);
}


void audio_source_info_free(gpointer info) {
    audio_source_info *s = info;
    g_free(s->name);
    g_free(s->description);
    g_free(s);
}

struct source_list_ctx {
    GPtrArray *sources;
    bool monitors;
    bool done;
};

static void source_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    struct source_list_ctx *ctx = userdata;

    if(eol != 0 || i == NULL) {
        ctx->done = true;
        return;
    }

    bool is_monitor = i->monitor_of_sink != PA_INVALID_INDEX;
    if(is_monitor != ctx->monitors) return;

    audio_source_info *s = g_new0(audio_source_info, 1);
    s->name = g_strdup(i->name);
    s->description = g_strdup((i->description != NULL && i->description[0] != '\0') ? i->description : i->name);
    g_ptr_array_add(ctx->sources, s);
}

GPtrArray *audiocap_list_sources(bool monitors) {
    GPtrArray *sources = g_ptr_array_new_with_free_func(audio_source_info_free);

    pa_mainloop *mainloop = pa_mainloop_new();
    if(mainloop == NULL) return sources;

    pa_context *context = pa_context_new(pa_mainloop_get_api(mainloop), "lcap-list-sources");
    if(context == NULL) goto free_mainloop;

    if(pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) goto free_context;

    for(;;) {
        pa_context_state_t state = pa_context_get_state(context);
        if(state == PA_CONTEXT_READY) break;
        if(!PA_CONTEXT_IS_GOOD(state)) goto disconnect;
        if(pa_mainloop_iterate(mainloop, 1, NULL) < 0) goto disconnect;
    }

    struct source_list_ctx ctx = { sources, monitors, false };
    pa_operation *op = pa_context_get_source_info_list(context, source_list_cb, &ctx);
    if(op != NULL) {
        while(!ctx.done && pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            if(pa_mainloop_iterate(mainloop, 1, NULL) < 0) break;
        }
        pa_operation_unref(op);
    }

disconnect:
    pa_context_disconnect(context);
free_context:
    pa_context_unref(context);
free_mainloop:
    pa_mainloop_free(mainloop);

    return sources;
}
