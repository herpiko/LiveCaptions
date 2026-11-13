/* asrproc.c
 * This file implements asr_thread which takes in audio, passes it to a speech
 * recognition backend (aprilasr, or sherpa-onnx when built with it), and
 * passes the output to line_generator to update the GtkLabel.
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

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <glib.h>
#include <time.h>

#include <stdbool.h>
#include <april_api.h>

#ifdef LIVE_CAPTIONS_SHERPA
#include <sherpa-onnx/c-api/c-api.h>
#endif

#include "asrproc.h"
#include "line-gen.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "history.h"
#include "common.h"

#define SHERPA_SAMPLE_RATE 16000

#ifdef LIVE_CAPTIONS_SHERPA
// Audio handed from the audio capture thread to the sherpa decoding thread
struct sherpa_chunk {
    bool flush;
    int32_t count;
    float samples[];
};
#endif

struct asr_thread_i {
    volatile size_t sound_counter;
    size_t silence_counter;

    GThread * thread_id;

    struct line_generator line;

    GMutex text_mutex;
    char text_buffer[32768];

    AprilASRModel model;
    AprilASRSession session;

#ifdef LIVE_CAPTIONS_SHERPA
    const SherpaOnnxOnlineRecognizer *sherpa_recognizer;
    const SherpaOnnxOnlineStream *sherpa_stream;
    GThread *sherpa_thread;
    GAsyncQueue *sherpa_queue;
    volatile bool sherpa_stop;
    volatile bool sherpa_flushed;
    volatile gint sherpa_queued_samples;
#endif

    LiveCaptionsWindow *window;

    size_t layout_counter;

    volatile bool text_stream_active;
    volatile bool pause;

    volatile time_t last_silence_time;

    volatile bool ending;

    bool errored;
};


static gboolean main_thread_update_label(void *userdata);

static void *run_asr_thread(void *userdata){
    asr_thread data = (asr_thread)userdata;

    while(!data->ending){
        sleep(1);

        if(data->last_silence_time == 0) continue;

        time_t current_time = time(NULL);

        if(difftime(current_time, data->last_silence_time) >= 6.0) {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = 0;
            for(int i=1; i<AC_LINE_COUNT; i++) line_generator_break(&data->line);
            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
        }
    }

    //sleep(40);

    //if(data->sound_counter < 512) {
    //    gtk_label_set_text(data->label, "[No audio is being received. If you are playing audio,\nmost likely the audio recording isn't working]");
    //}

    return NULL;
}

static gboolean main_thread_update_label(void *userdata){
    asr_thread data = userdata;

    if((data->window == NULL) || (data->pause)) return G_SOURCE_REMOVE;

    g_mutex_lock(&data->text_mutex);
    line_generator_set_text(&data->line, data->window->label);

    if(data->text_stream_active) {
        LiveCaptionsApplication *application = LIVECAPTIONS_APPLICATION(gtk_window_get_application(GTK_WINDOW(data->window)));
        livecaptions_application_stream_text(application, line_generator_get_plaintext(&data->line));
    }

    g_mutex_unlock(&data->text_mutex);

    return G_SOURCE_REMOVE;
}

// Shared by both backends: April calls it directly, the sherpa backend
// converts its results to AprilTokens and calls it from its decoding thread.
static void april_result_handler(void* userdata, AprilResultType result, size_t count, const AprilToken* tokens) {
    asr_thread data = userdata;
    if((data->window == NULL) || (data->pause)) return;

    switch(result) {
        case APRIL_RESULT_RECOGNITION_PARTIAL:
        case APRIL_RESULT_RECOGNITION_FINAL:
        {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = 0;

            if((data->layout_counter != data->window->font_layout_counter) || (data->line.layout == NULL)) {
                if(data->line.layout != NULL) g_object_unref(data->line.layout);

                data->line.layout = pango_layout_copy(data->window->font_layout);
                data->line.max_text_width = data->window->max_text_width;

                data->layout_counter = data->window->font_layout_counter;
            }

            line_generator_update(&data->line, count, tokens);
            if(result == APRIL_RESULT_RECOGNITION_FINAL) {
                line_generator_finalize(&data->line);
                commit_tokens_to_current_history(tokens, count);
            }

            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
            break;
        }

        case APRIL_RESULT_ERROR_CANT_KEEP_UP: {
            livecaptions_window_warn_slow(data->window);
            break;
        }

        case APRIL_RESULT_SILENCE: {
            g_mutex_lock(&data->text_mutex);
            data->last_silence_time = time(NULL);

            line_generator_break(&data->line);
            save_silence_to_history();

            g_mutex_unlock(&data->text_mutex);
            g_idle_add(main_thread_update_label, data);
            break;
        }
    }
}


/* ---------------------------------------------------------------------------
 * sherpa-onnx backend
 *
 * A model is a sherpa-onnx streaming transducer directory containing
 * tokens.txt and encoder*.onnx / decoder*.onnx / joiner*.onnx. The model path
 * may be the directory itself or its tokens.txt file.
 * ------------------------------------------------------------------------- */

static char *sherpa_model_dir(const char *model_path) {
    if(model_path == NULL || model_path[0] == '\0') return NULL;

    char *dir;
    if(g_file_test(model_path, G_FILE_TEST_IS_DIR)) {
        dir = g_strdup(model_path);
    } else {
        g_autofree char *base = g_path_get_basename(model_path);
        if(!g_str_equal(base, "tokens.txt")) return NULL;
        dir = g_path_get_dirname(model_path);
    }

    g_autofree char *tokens = g_build_filename(dir, "tokens.txt", NULL);
    if(!g_file_test(tokens, G_FILE_TEST_IS_REGULAR)) {
        g_free(dir);
        return NULL;
    }

    return dir;
}

bool asr_is_sherpa_model(const char *model_path) {
    g_autofree char *dir = sherpa_model_dir(model_path);
    return dir != NULL;
}

bool asr_model_supported(const char *model_path) {
#ifdef LIVE_CAPTIONS_SHERPA
    (void)model_path;
    return true;
#else
    return !asr_is_sherpa_model(model_path);
#endif
}

char *asr_model_display_name(const char *model_path) {
    g_autofree char *dir = sherpa_model_dir(model_path);
    if(dir != NULL) return g_path_get_basename(dir);
    return g_path_get_basename(model_path);
}

#ifdef LIVE_CAPTIONS_SHERPA

// Finds "<prefix>*.onnx" in dir, preferring an int8-quantized file
static char *sherpa_find_onnx(const char *dir, const char *prefix) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if(d == NULL) return NULL;

    char *found = NULL;
    const char *name;
    while((name = g_dir_read_name(d)) != NULL) {
        if(!g_str_has_prefix(name, prefix) || !g_str_has_suffix(name, ".onnx")) continue;

        bool is_int8 = strstr(name, "int8") != NULL;
        if(found == NULL || is_int8) {
            g_free(found);
            found = g_build_filename(dir, name, NULL);
            if(is_int8) break;
        }
    }

    g_dir_close(d);
    return found;
}

// True if the vocabulary has both upper and lower case letters, meaning the
// model produces properly cased text that shouldn't be lowercased.
static bool sherpa_tokens_are_cased(const char *tokens_path) {
    g_autofree char *contents = NULL;
    if(!g_file_get_contents(tokens_path, &contents, NULL, NULL)) return false;

    bool upper = false, lower = false;
    for(const char *p = contents; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if(g_unichar_isupper(c)) upper = true;
        if(g_unichar_islower(c)) lower = true;
        if(upper && lower) return true;
    }
    return false;
}

static const SherpaOnnxOnlineStream *sherpa_new_stream(const SherpaOnnxOnlineRecognizer *recognizer) {
    const SherpaOnnxOnlineStream *stream = SherpaOnnxCreateOnlineStream(recognizer);
    // Multilingual models (e.g. Nemotron) take a per-stream language; others ignore it
    SherpaOnnxOnlineStreamSetOption(stream, "language", "en");
    return stream;
}

#define SHERPA_MAX_TOKENS 1024
#define SHERPA_TOKEN_CHARS 32

struct sherpa_tokens {
    size_t count;
    AprilToken tokens[SHERPA_MAX_TOKENS];
    char text[SHERPA_MAX_TOKENS][SHERPA_TOKEN_CHARS];
};

static bool is_sentence_end(const char *s) {
    return (s[0] == '.' || s[0] == '!' || s[0] == '?') && s[1] == '\0';
}

// Converts a sherpa result into AprilTokens. Word starts are marked with a
// leading space, as April does; sherpa's BPE marker U+2581 becomes a space.
static void sherpa_convert_result(const SherpaOnnxOnlineRecognizerResult *r, struct sherpa_tokens *out) {
    out->count = 0;
    bool pending_boundary = false;

    for(int32_t i = 0; i < r->count && out->count < SHERPA_MAX_TOKENS; i++) {
        const char *tok = r->tokens_arr[i];
        if(tok == NULL || tok[0] == '\0') continue;

        // Skip special tokens such as <unk>, <blk>, <en-US>
        size_t len = strlen(tok);
        if(tok[0] == '<' && tok[len - 1] == '>') continue;

        char *dst = out->text[out->count];
        size_t pos = 0;
        bool boundary = false;

        if(g_str_has_prefix(tok, "\xe2\x96\x81")) {
            tok += 3;
            boundary = true;
        } else if(tok[0] == ' ') {
            tok += 1;
            boundary = true;
        }

        // A bare word-boundary token: the next token starts the word
        if(boundary && tok[0] == '\0') {
            pending_boundary = true;
            continue;
        }
        if(pending_boundary) {
            boundary = true;
            pending_boundary = false;
        }

        // Punctuation never starts a word
        if(boundary && (is_sentence_end(tok) || ((tok[0] == ',' || tok[0] == ';' || tok[0] == ':') && tok[1] == '\0')))
            boundary = false;

        if(boundary) dst[pos++] = ' ';
        // Copy without splitting a UTF-8 character
        while(*tok && pos < SHERPA_TOKEN_CHARS - 1) {
            const char *next = g_utf8_next_char(tok);
            size_t clen = next - tok;
            if(pos + clen >= SHERPA_TOKEN_CHARS - 1) break;
            memcpy(&dst[pos], tok, clen);
            pos += clen;
            tok = next;
        }
        dst[pos] = '\0';

        if(pos == 0) continue;

        AprilToken *t = &out->tokens[out->count];
        t->token = dst;
        // sherpa doesn't give per-token confidence; report full confidence
        t->logprob = 6.0f;
        t->flags = 0;
        if(boundary || out->count == 0) t->flags |= APRIL_TOKEN_FLAG_WORD_BOUNDARY_BIT;
        if(is_sentence_end(boundary ? dst + 1 : dst)) t->flags |= APRIL_TOKEN_FLAG_SENTENCE_END_BIT;
        t->time_ms = (r->timestamps != NULL) ? (size_t)(r->timestamps[i] * 1000.0f) : 0;
        t->reserved = NULL;

        out->count++;
    }
}

// Emits the current result. Returns the number of tokens emitted.
static size_t sherpa_emit(asr_thread data, struct sherpa_tokens *tok, AprilResultType type) {
    const SherpaOnnxOnlineRecognizerResult *r =
        SherpaOnnxGetOnlineStreamResult(data->sherpa_recognizer, data->sherpa_stream);
    sherpa_convert_result(r, tok);

    // LIVECAPTIONS_SHERPA_DEBUG=1 prints each finalized utterance as decoded
    if(type == APRIL_RESULT_RECOGNITION_FINAL && getenv("LIVECAPTIONS_SHERPA_DEBUG") != NULL)
        fprintf(stderr, "sherpa final: %s\n", r->text);

    SherpaOnnxDestroyOnlineRecognizerResult(r);

    // An empty final would be saved to history as a silence marker; skip it
    if(tok->count > 0)
        april_result_handler(data, type, tok->count, tok->tokens);

    return tok->count;
}

static void *run_sherpa_thread(void *userdata) {
    asr_thread data = userdata;
    struct sherpa_tokens *tok = g_new0(struct sherpa_tokens, 1);
    g_autofree char *last_text = NULL;
    bool warned_slow = false;

    while(!data->sherpa_stop) {
        struct sherpa_chunk *chunk = g_async_queue_timeout_pop(data->sherpa_queue, 200000);
        if(chunk == NULL) continue;

        if(chunk->flush) {
            g_free(chunk);

            // Let the model see the end of the utterance so the last words are
            // decoded, then start a fresh stream for the next utterance.
            // Streaming models only emit a chunk's words once they've seen
            // enough audio after it, so pad with a second of silence first.
            static const float tail_padding[SHERPA_SAMPLE_RATE] = { 0 };
            SherpaOnnxOnlineStreamAcceptWaveform(data->sherpa_stream, SHERPA_SAMPLE_RATE, tail_padding, SHERPA_SAMPLE_RATE);
            SherpaOnnxOnlineStreamInputFinished(data->sherpa_stream);
            while(SherpaOnnxIsOnlineStreamReady(data->sherpa_recognizer, data->sherpa_stream))
                SherpaOnnxDecodeOnlineStream(data->sherpa_recognizer, data->sherpa_stream);

            sherpa_emit(data, tok, APRIL_RESULT_RECOGNITION_FINAL);
            april_result_handler(data, APRIL_RESULT_SILENCE, 0, NULL);

            SherpaOnnxDestroyOnlineStream(data->sherpa_stream);
            data->sherpa_stream = sherpa_new_stream(data->sherpa_recognizer);
            g_clear_pointer(&last_text, g_free);
            continue;
        }

        g_atomic_int_add(&data->sherpa_queued_samples, -chunk->count);
        SherpaOnnxOnlineStreamAcceptWaveform(data->sherpa_stream, SHERPA_SAMPLE_RATE, chunk->samples, chunk->count);
        g_free(chunk);

        while(SherpaOnnxIsOnlineStreamReady(data->sherpa_recognizer, data->sherpa_stream))
            SherpaOnnxDecodeOnlineStream(data->sherpa_recognizer, data->sherpa_stream);

        // More than 3 seconds of audio waiting means decoding can't keep up
        int queued = g_atomic_int_get(&data->sherpa_queued_samples);
        if(queued > 3 * SHERPA_SAMPLE_RATE) {
            if(!warned_slow) april_result_handler(data, APRIL_RESULT_ERROR_CANT_KEEP_UP, 0, NULL);
            warned_slow = true;
        } else if(queued < SHERPA_SAMPLE_RATE / 2) {
            warned_slow = false;
        }

        bool endpoint = SherpaOnnxOnlineStreamIsEndpoint(data->sherpa_recognizer, data->sherpa_stream);

        const SherpaOnnxOnlineRecognizerResult *r =
            SherpaOnnxGetOnlineStreamResult(data->sherpa_recognizer, data->sherpa_stream);
        bool changed = (last_text == NULL) || !g_str_equal(last_text, r->text);
        if(changed) {
            g_free(last_text);
            last_text = g_strdup(r->text);
        }
        SherpaOnnxDestroyOnlineRecognizerResult(r);

        if(endpoint) {
            sherpa_emit(data, tok, APRIL_RESULT_RECOGNITION_FINAL);
            SherpaOnnxOnlineStreamReset(data->sherpa_recognizer, data->sherpa_stream);
            g_clear_pointer(&last_text, g_free);
        } else if(changed) {
            sherpa_emit(data, tok, APRIL_RESULT_RECOGNITION_PARTIAL);
        }
    }

    g_free(tok);
    return NULL;
}

static void sherpa_stop_worker(asr_thread data) {
    if(data->sherpa_thread == NULL) return;

    data->sherpa_stop = true;
    g_thread_join(data->sherpa_thread);
    data->sherpa_thread = NULL;

    struct sherpa_chunk *chunk;
    while((chunk = g_async_queue_try_pop(data->sherpa_queue)) != NULL) g_free(chunk);
    g_atomic_int_set(&data->sherpa_queued_samples, 0);
}

static void sherpa_free_model(asr_thread data) {
    if(data->sherpa_stream != NULL) SherpaOnnxDestroyOnlineStream(data->sherpa_stream);
    if(data->sherpa_recognizer != NULL) SherpaOnnxDestroyOnlineRecognizer(data->sherpa_recognizer);
    data->sherpa_stream = NULL;
    data->sherpa_recognizer = NULL;
}

static const SherpaOnnxOnlineRecognizer *sherpa_create_recognizer(const char *dir) {
    g_autofree char *tokens  = g_build_filename(dir, "tokens.txt", NULL);
    g_autofree char *encoder = sherpa_find_onnx(dir, "encoder");
    g_autofree char *decoder = sherpa_find_onnx(dir, "decoder");
    g_autofree char *joiner  = sherpa_find_onnx(dir, "joiner");

    if(encoder == NULL || decoder == NULL || joiner == NULL) {
        printf("sherpa-onnx: %s is missing encoder/decoder/joiner .onnx files\n", dir);
        return NULL;
    }

    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));

    config.feat_config.sample_rate = SHERPA_SAMPLE_RATE;
    config.feat_config.feature_dim = 80;

    config.model_config.transducer.encoder = encoder;
    config.model_config.transducer.decoder = decoder;
    config.model_config.transducer.joiner = joiner;
    config.model_config.tokens = tokens;
    config.model_config.provider = "cpu";
    config.model_config.debug = 0;

    // LIVECAPTIONS_SHERPA_THREADS overrides the thread count (for benchmarking)
    const char *threads = getenv("LIVECAPTIONS_SHERPA_THREADS");
    config.model_config.num_threads = (threads != NULL) ? atoi(threads) : 2;
    if(config.model_config.num_threads < 1) config.model_config.num_threads = 1;

    config.decoding_method = "greedy_search";

    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 2.4f;
    config.rule2_min_trailing_silence = 1.2f;
    config.rule3_min_utterance_length = 20.0f;

    return SherpaOnnxCreateOnlineRecognizer(&config);
}

static bool sherpa_load_model(asr_thread data, const char *dir) {
    const SherpaOnnxOnlineRecognizer *recognizer = sherpa_create_recognizer(dir);
    if(recognizer == NULL) return false;

    data->sherpa_recognizer = recognizer;
    data->sherpa_stream = sherpa_new_stream(recognizer);

    g_autofree char *tokens = g_build_filename(dir, "tokens.txt", NULL);
    data->line.preserve_case = sherpa_tokens_are_cased(tokens);

    printf("\n-- Model metadata --\n");
    printf("Engine: sherpa-onnx\n");
    printf("Path: %s\n", dir);
    printf("Cased output: %s\n", data->line.preserve_case ? "yes" : "no");
    printf("-- --\n\n");

    return true;
}

static void sherpa_start_worker(asr_thread data) {
    data->sherpa_stop = false;
    data->sherpa_flushed = false;
    data->sherpa_thread = g_thread_new("lcap-sherpa", run_sherpa_thread, data);
}

#endif // LIVE_CAPTIONS_SHERPA

static bool using_sherpa(asr_thread thread) {
#ifdef LIVE_CAPTIONS_SHERPA
    return thread->sherpa_recognizer != NULL;
#else
    (void)thread;
    return false;
#endif
}


void asr_thread_enqueue_audio(asr_thread thread, short *data, size_t num_shorts) {
    if((thread->window == NULL) || thread->pause) return;

    bool sherpa = using_sherpa(thread);
    if(!sherpa && ((thread->session == NULL) || (thread->model == NULL))) return;


    bool found_nonzero = false;
    for(size_t i=0; i<num_shorts; i++){
        if((data[i] > 16) || (data[i] < -16)){
            found_nonzero = true;
            break;
        }
    }

    thread->silence_counter = found_nonzero ? 0 : (thread->silence_counter + num_shorts);

#ifdef LIVE_CAPTIONS_SHERPA
    if(sherpa) {
        if(thread->silence_counter >= 24000){
            thread->silence_counter = 24000;
            // Flush once per stretch of silence
            if(!thread->sherpa_flushed) {
                thread->sherpa_flushed = true;
                struct sherpa_chunk *chunk = g_malloc0(sizeof(struct sherpa_chunk));
                chunk->flush = true;
                g_async_queue_push(thread->sherpa_queue, chunk);
            }
            return;
        }
        thread->sherpa_flushed = false;

        struct sherpa_chunk *chunk = g_malloc(sizeof(struct sherpa_chunk) + num_shorts * sizeof(float));
        chunk->flush = false;
        chunk->count = (int32_t)num_shorts;
        for(size_t i = 0; i < num_shorts; i++) chunk->samples[i] = data[i] / 32768.0f;

        thread->sound_counter += num_shorts;
        g_atomic_int_add(&thread->sherpa_queued_samples, (gint)num_shorts);
        g_async_queue_push(thread->sherpa_queue, chunk);
        return;
    }
#endif

    if(thread->silence_counter >= 24000){
        thread->silence_counter = 24000;
        return aas_flush(thread->session);
    }

    thread->sound_counter += num_shorts;
    aas_feed_pcm16(thread->session, data, num_shorts); // TODO?
}

gpointer asr_thread_get_model(asr_thread thread) {
    return thread->model;
}

gpointer asr_thread_get_session(asr_thread thread) {
    return thread->session;
}

float asr_thread_get_speedup(asr_thread thread) {
#ifdef LIVE_CAPTIONS_SHERPA
    if(using_sherpa(thread)) {
        // Report backlog the same way April reports how much it must speed up
        float queued = g_atomic_int_get(&thread->sherpa_queued_samples) / (float)SHERPA_SAMPLE_RATE;
        return 1.0f + queued / 2.0f;
    }
#endif
    if(thread->session == NULL) return 1.0f;
    return aas_realtime_get_speedup(thread->session);
}

void asr_thread_pause(asr_thread thread, bool pause) {
    thread->pause = pause;
}

void asr_thread_set_text_stream_active(asr_thread thread, bool active) {
    thread->text_stream_active = active;
}

int asr_thread_samplerate(asr_thread thread) {
    if(using_sherpa(thread)) return SHERPA_SAMPLE_RATE;
    return aam_get_sample_rate(thread->model);
}

static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

static void benchmark_dummy_handler(G_GNUC_UNUSED void* _userdata,
                                    G_GNUC_UNUSED AprilResultType _result,
                                    G_GNUC_UNUSED size_t _count,
                                    G_GNUC_UNUSED const AprilToken* _tokens)
{
    // do nothing with result, we don't need it
}

double asr_thread_benchmark(asr_thread thread, volatile double *progress) {
    const int seconds = 30;

    short noise_data[48000];
    for(int i=0; i<48000; i++){
        noise_data[i] = rand();
    }

    int sr = asr_thread_samplerate(thread);
    g_assert(sr < 48000);

#ifdef LIVE_CAPTIONS_SHERPA
    const SherpaOnnxOnlineStream *sherpa_stream = NULL;
    float *float_data = NULL;
#endif
    AprilASRSession session = NULL;

    if(using_sherpa(thread)) {
#ifdef LIVE_CAPTIONS_SHERPA
        sherpa_stream = SherpaOnnxCreateOnlineStream(thread->sherpa_recognizer);
        float_data = g_new(float, 48000);
        for(int i=0; i<48000; i++) float_data[i] = noise_data[i] / 32768.0f;
#endif
    } else {
        g_assert(thread->model != NULL);

        AprilConfig config = {
            .handler = benchmark_dummy_handler,
            .userdata = NULL,
            .flags = APRIL_CONFIG_FLAG_ZERO_BIT
        };

        session = aas_create_session(thread->model, config);
        g_assert(session != NULL);
    }

    uint64_t begin = get_time_us();
    double result = -1.0;

    int idx = 0;
    for(int sec=0; sec<seconds; sec++){
#ifdef LIVE_CAPTIONS_SHERPA
        if(sherpa_stream != NULL) {
            SherpaOnnxOnlineStreamAcceptWaveform(sherpa_stream, sr, &float_data[idx], sr);
            while(SherpaOnnxIsOnlineStreamReady(thread->sherpa_recognizer, sherpa_stream))
                SherpaOnnxDecodeOnlineStream(thread->sherpa_recognizer, sherpa_stream);
        } else
#endif
        aas_feed_pcm16(session, &noise_data[idx], sr);

        idx = (idx + sr) % 48000;
        if(progress != NULL) *progress = ((double)sec) / (double)seconds;

        double time_diff = (get_time_us() - begin) / 1000.0 / 1000.0;
        if(time_diff > 55.0) {
            result = ((double)sec) / time_diff;
            break;
        }
    }

    if(result < 0) {
        double time_diff = (get_time_us() - begin) / 1000.0 / 1000.0;
        result = seconds / time_diff;
    }

#ifdef LIVE_CAPTIONS_SHERPA
    if(sherpa_stream != NULL) SherpaOnnxDestroyOnlineStream(sherpa_stream);
    g_free(float_data);
#endif
    if(session != NULL) aas_free(session);

    return result;
}

asr_thread create_asr_thread(const char *model_path){
    asr_thread data = calloc(1, sizeof(struct asr_thread_i));

    line_generator_init(&data->line);
    g_mutex_init(&data->text_mutex);

#ifdef LIVE_CAPTIONS_SHERPA
    data->sherpa_queue = g_async_queue_new();
#endif

    if(!asr_thread_update_model(data, model_path)){
        char *model_default = GET_MODEL_PATH();
        if(!asr_thread_update_model(data, model_default)) {
            return NULL;
        }

        GSettings *settings = g_settings_new("net.sapples.LiveCaptions");
        g_settings_set_string(settings, "active-model", model_default);
        g_object_unref(G_OBJECT(settings));
    }

    data->thread_id = g_thread_new("lcap-audiothread", run_asr_thread, data);

    data->text_stream_active = false;

    return data;
}

bool asr_thread_update_model(asr_thread data, const char *model_path) {
#ifdef LIVE_CAPTIONS_SHERPA
    // The decoding thread takes text_mutex, so stop it before taking it here
    data->pause = true;
    sherpa_stop_worker(data);
#endif

    // Freeing model frees token list, which may be being accessed during
    // line generation
    g_mutex_lock(&data->text_mutex);

    data->pause = true;

    AprilASRModel old_model = data->model;
    AprilASRSession old_session = data->session;

    data->model = NULL;
    data->session = NULL;

    if(old_session != NULL)
        aas_free(old_session);

    if(old_model != NULL)
        aam_free(old_model);

#ifdef LIVE_CAPTIONS_SHERPA
    sherpa_free_model(data);
#endif
    data->line.preserve_case = false;

    g_autofree char *sherpa_dir = sherpa_model_dir(model_path);
    if(sherpa_dir != NULL) {
#ifdef LIVE_CAPTIONS_SHERPA
        if(!sherpa_load_model(data, sherpa_dir)) {
            printf("Loading model %s failed!\n", model_path);
            data->errored = true;
            g_mutex_unlock(&data->text_mutex);
            return false;
        }

        line_generator_set_language(&data->line, "en");

        data->errored = false;
        data->ending = false;
        data->pause = false;

        line_generator_finalize(&data->line);
        g_mutex_unlock(&data->text_mutex);

        sherpa_start_worker(data);
        return true;
#else
        printf("Model %s needs sherpa-onnx, but this build doesn't include it\n", model_path);
        data->errored = true;
        g_mutex_unlock(&data->text_mutex);
        return false;
#endif
    }

    AprilConfig config = {
        .handler = april_result_handler,
        .flags = APRIL_CONFIG_FLAG_ASYNC_RT_BIT,
        .userdata = data
    };

    AprilASRModel new_model = aam_create_model(model_path);
    if(new_model == NULL) {
        printf("Loading model %s failed!\n", model_path);
        data->errored = true;
        g_mutex_unlock(&data->text_mutex);
        return false;
    }

    {
        printf("\n-- Model metadata --\n");
        printf("Name: %s\n", aam_get_name(new_model));
        char *description = (char*)aam_get_description(new_model);
        for(int i=0; description[i]; i++){
            if((description[i] == ' ') && (description[i+1] == 'D') && (description[i+2] == 'i')){
                description[i] = 0;
                break;
            }
        }
        printf("Description: %s\n", description);
        printf("Language: %s\n", aam_get_language(new_model));
        printf("-- --\n\n");
    }

    line_generator_set_language(&data->line, aam_get_language(new_model));

    AprilASRSession new_session = aas_create_session(new_model, config);
    if(new_session == NULL) {
        printf("Creating session %s failed!\n", model_path);
        data->errored = true;
        g_mutex_unlock(&data->text_mutex);
        return false;
    }

    data->model = new_model;
    data->session = new_session;

    data->errored = false;
    data->ending = false;
    data->pause = false;

    line_generator_finalize(&data->line);

    g_mutex_unlock(&data->text_mutex);

    return true;
}

bool asr_thread_is_errored(asr_thread thread) {
    return thread->errored;
}

bool asr_thread_is_loaded(asr_thread thread) {
    return (thread->session != NULL) || using_sherpa(thread);
}

void asr_thread_set_main_window(asr_thread thread, LiveCaptionsWindow *window) {
    thread->window = window;
}

void asr_thread_flush(asr_thread thread) {
#ifdef LIVE_CAPTIONS_SHERPA
    if(using_sherpa(thread)) {
        struct sherpa_chunk *chunk = g_malloc0(sizeof(struct sherpa_chunk));
        chunk->flush = true;
        g_async_queue_push(thread->sherpa_queue, chunk);
        return;
    }
#endif
    if(thread->session == NULL) return;
    aas_flush(thread->session);
}

void free_asr_thread(asr_thread thread) {
    thread->ending = true;

#ifdef LIVE_CAPTIONS_SHERPA
    sherpa_stop_worker(thread);
#endif

    g_mutex_lock(&thread->text_mutex);

    g_thread_join(thread->thread_id);

    if(thread->session != NULL)
        aas_free(thread->session);

    if(thread->model != NULL)
        aam_free(thread->model);

#ifdef LIVE_CAPTIONS_SHERPA
    sherpa_free_model(thread);
    g_async_queue_unref(thread->sherpa_queue);
#endif

    g_thread_unref(thread->thread_id); // ?

    free(thread);
}
