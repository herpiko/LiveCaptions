/* cloudasr.c
 * Streaming transcription through OpenAI's Realtime API.
 *
 * Protocol (transcription session, see OpenAI's "Realtime transcription"
 * guide): connect to wss://api.openai.com/v1/realtime?intent=transcription,
 * send session.update with type "transcription", 24 kHz PCM input and no
 * server turn detection, then stream input_audio_buffer.append events. The
 * server streams ...input_audio_transcription.delta events per item and a
 * ...completed event once the item is committed; we commit when the speaker
 * pauses (client-side VAD, done by the caller).
 *
 * All networking runs on a private thread with its own GMainContext.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "cloudasr.h"

#define DEFAULT_URL "wss://api.openai.com/v1/realtime?intent=transcription"
#define OUT_RATE 24000
// Audio kept while (re)connecting; older audio is dropped beyond this
#define MAX_PENDING_BYTES (OUT_RATE * 2 * 10)
#define RETRY_SECONDS 5

struct turn {
    char *item_id;
    GString *text;
    bool done;
    char *final_text;
};

struct cloud_asr {
    GMainContext *ctx;
    GMainLoop *loop;
    GThread *thread;

    // Owned by the cloud thread
    SoupSession *session;
    SoupWebsocketConnection *ws;
    bool connecting;
    guint connected_gen;
    gint64 retry_after;
    GQueue turns;

    cloud_result_cb cb;
    void *userdata;

    // Shared, protected by lock
    GMutex lock;
    char *api_key;
    char *model;
    char *delay;
    guint config_gen;
    GByteArray *pending;      // resampled PCM16 waiting to be sent
    bool commit_requested;
    bool sent_since_commit;
    bool flush_scheduled;
    char *status;

    // Resampler state (feeding thread only)
    double resample_t;
    short resample_last;
    bool resample_primed;
};

static void set_status(cloud_asr *c, const char *status) {
    g_mutex_lock(&c->lock);
    g_free(c->status);
    c->status = g_strdup(status);
    g_mutex_unlock(&c->lock);
}

/* ---------------------------------------------------------------- turns */

static void turn_free(struct turn *t) {
    g_free(t->item_id);
    g_string_free(t->text, TRUE);
    g_free(t->final_text);
    g_free(t);
}

static struct turn *find_turn(cloud_asr *c, const char *item_id) {
    for(GList *l = c->turns.head; l != NULL; l = l->next) {
        struct turn *t = l->data;
        if(g_strcmp0(t->item_id, item_id) == 0) return t;
    }

    struct turn *t = g_new0(struct turn, 1);
    t->item_id = g_strdup(item_id);
    t->text = g_string_new(NULL);
    g_queue_push_tail(&c->turns, t);
    return t;
}

// Reports finished turns in order, then the text still in progress
static void publish(cloud_asr *c) {
    while(!g_queue_is_empty(&c->turns)) {
        struct turn *t = g_queue_peek_head(&c->turns);
        if(!t->done) break;
        g_queue_pop_head(&c->turns);

        const char *text = t->final_text != NULL ? t->final_text : t->text->str;
        if(text[0] != '\0') c->cb(c->userdata, CLOUD_RESULT_FINAL, text);
        turn_free(t);
    }

    if(g_queue_is_empty(&c->turns)) return;

    GString *partial = g_string_new(NULL);
    for(GList *l = c->turns.head; l != NULL; l = l->next) {
        struct turn *t = l->data;
        const char *text = t->final_text != NULL ? t->final_text : t->text->str;
        if(text[0] == '\0') continue;
        if(partial->len > 0) g_string_append_c(partial, ' ');
        g_string_append(partial, text);
    }
    if(partial->len > 0) c->cb(c->userdata, CLOUD_RESULT_PARTIAL, partial->str);
    g_string_free(partial, TRUE);
}

// Connection lost: keep whatever was transcribed so far
static void finish_all_turns(cloud_asr *c) {
    for(GList *l = c->turns.head; l != NULL; l = l->next) ((struct turn *)l->data)->done = true;
    publish(c);
}

/* -------------------------------------------------------------- receive */

static void on_ws_message(G_GNUC_UNUSED SoupWebsocketConnection *ws, gint type,
                          GBytes *message, gpointer userdata) {
    cloud_asr *c = userdata;
    if(type != SOUP_WEBSOCKET_DATA_TEXT) return;

    gsize len;
    const char *data = g_bytes_get_data(message, &len);

    g_autoptr(JsonParser) parser = json_parser_new();
    if(!json_parser_load_from_data(parser, data, (gssize)len, NULL)) return;

    JsonNode *root = json_parser_get_root(parser);
    if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) return;
    JsonObject *obj = json_node_get_object(root);

    const char *ev = json_object_get_string_member_with_default(obj, "type", "");
    const char *item_id = json_object_get_string_member_with_default(obj, "item_id", "");

    if(g_str_equal(ev, "conversation.item.input_audio_transcription.delta")) {
        struct turn *t = find_turn(c, item_id);
        g_string_append(t->text, json_object_get_string_member_with_default(obj, "delta", ""));
        publish(c);
    } else if(g_str_equal(ev, "conversation.item.input_audio_transcription.completed")) {
        struct turn *t = find_turn(c, item_id);
        g_free(t->final_text);
        t->final_text = g_strdup(json_object_get_string_member_with_default(obj, "transcript", ""));
        g_strstrip(t->final_text);
        t->done = true;
        publish(c);
    } else if(g_str_equal(ev, "conversation.item.input_audio_transcription.failed")) {
        struct turn *t = find_turn(c, item_id);
        t->done = true;
        publish(c);
    } else if(g_str_equal(ev, "input_audio_buffer.committed")) {
        // Fixes the turn's place in the order even before any text arrives
        find_turn(c, item_id);
    } else if(g_str_equal(ev, "session.created") || g_str_equal(ev, "session.updated")) {
        set_status(c, NULL);
    } else if(g_str_equal(ev, "error")) {
        const char *msg = "unknown error";
        JsonObject *err = json_object_get_object_member(obj, "error");
        if(err != NULL) msg = json_object_get_string_member_with_default(err, "message", msg);

        // Committing with nothing new since the last commit isn't a real problem
        const char *code = err != NULL ? json_object_get_string_member_with_default(err, "code", "") : "";
        if(g_str_equal(code, "input_audio_buffer_commit_empty")) return;

        printf("OpenAI error: %s\n", msg);
        g_autofree char *status = g_strdup_printf("[OpenAI error: %s]", msg);
        set_status(c, status);
    }
}

static void on_ws_closed(SoupWebsocketConnection *ws, gpointer userdata) {
    cloud_asr *c = userdata;
    if(c->ws != ws) return;

    gushort code = soup_websocket_connection_get_close_code(ws);
    const char *reason = soup_websocket_connection_get_close_data(ws);
    printf("OpenAI connection closed (%u%s%s)\n", code, reason ? ": " : "", reason ? reason : "");

    finish_all_turns(c);
    g_clear_object(&c->ws);

    g_mutex_lock(&c->lock);
    c->sent_since_commit = false;
    g_mutex_unlock(&c->lock);
}

/* ----------------------------------------------------------------- send */

static void send_text(cloud_asr *c, const char *text) {
    if(c->ws == NULL || soup_websocket_connection_get_state(c->ws) != SOUP_WEBSOCKET_STATE_OPEN) return;
    soup_websocket_connection_send_text(c->ws, text);
}

static void send_session_update(cloud_asr *c, const char *model, const char *delay) {
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "session.update");
    json_builder_set_member_name(b, "session");
    json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "transcription");
        json_builder_set_member_name(b, "audio");
        json_builder_begin_object(b);
            json_builder_set_member_name(b, "input");
            json_builder_begin_object(b);
                json_builder_set_member_name(b, "format");
                json_builder_begin_object(b);
                    json_builder_set_member_name(b, "type");
                    json_builder_add_string_value(b, "audio/pcm");
                    json_builder_set_member_name(b, "rate");
                    json_builder_add_int_value(b, OUT_RATE);
                json_builder_end_object(b);

                json_builder_set_member_name(b, "transcription");
                json_builder_begin_object(b);
                    json_builder_set_member_name(b, "model");
                    json_builder_add_string_value(b, model);

                    // gpt-live-transcribe takes a list of languages, the others a single one
                    if(g_str_equal(model, "gpt-live-transcribe")) {
                        json_builder_set_member_name(b, "languages");
                        json_builder_begin_array(b);
                        json_builder_add_string_value(b, "en");
                        json_builder_end_array(b);
                    } else {
                        json_builder_set_member_name(b, "language");
                        json_builder_add_string_value(b, "en");
                    }

                    // Latency/accuracy trade-off of the streaming models
                    if(delay != NULL && delay[0] != '\0' &&
                       (g_str_equal(model, "gpt-realtime-whisper") || g_str_equal(model, "gpt-live-transcribe"))) {
                        json_builder_set_member_name(b, "delay");
                        json_builder_add_string_value(b, delay);
                    }
                json_builder_end_object(b);

                // We commit turns ourselves when the speaker pauses
                json_builder_set_member_name(b, "turn_detection");
                json_builder_add_null_value(b);
            json_builder_end_object(b);
        json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    g_autofree char *text = json_generator_to_data(gen, NULL);
    send_text(c, text);
}

static void connect_ws(cloud_asr *c);

// Sends queued audio and commits; runs on the cloud thread
static gboolean flush_cb(gpointer userdata) {
    cloud_asr *c = userdata;

    g_mutex_lock(&c->lock);
    c->flush_scheduled = false;
    bool configured = c->api_key != NULL;
    bool stale = c->ws != NULL && c->connected_gen != c->config_gen;
    g_mutex_unlock(&c->lock);

    // Settings changed: start a new session with them
    if(stale) {
        soup_websocket_connection_close(c->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        finish_all_turns(c);
        g_clear_object(&c->ws);
    }

    if(!configured) return G_SOURCE_REMOVE;

    if(c->ws == NULL) {
        connect_ws(c);
        return G_SOURCE_REMOVE;
    }
    if(soup_websocket_connection_get_state(c->ws) != SOUP_WEBSOCKET_STATE_OPEN) return G_SOURCE_REMOVE;

    g_mutex_lock(&c->lock);
    GByteArray *audio = c->pending;
    c->pending = g_byte_array_new();
    bool commit = c->commit_requested;
    c->commit_requested = false;
    if(audio->len > 0) c->sent_since_commit = true;
    if(commit && !c->sent_since_commit) commit = false;
    if(commit) c->sent_since_commit = false;
    g_mutex_unlock(&c->lock);

    if(audio->len > 0) {
        g_autofree char *b64 = g_base64_encode(audio->data, audio->len);
        g_autofree char *msg = g_strdup_printf("{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
        send_text(c, msg);
    }
    g_byte_array_unref(audio);

    if(commit) send_text(c, "{\"type\":\"input_audio_buffer.commit\"}");

    return G_SOURCE_REMOVE;
}

static void schedule_flush(cloud_asr *c) {
    // Caller holds lock
    if(c->flush_scheduled) return;
    c->flush_scheduled = true;
    // Always deferred (never run inline, even on the cloud thread, where the
    // caller may hold lock)
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, flush_cb, c, NULL);
    g_source_attach(source, c->ctx);
    g_source_unref(source);
}

static void on_connected(GObject *source, GAsyncResult *res, gpointer userdata) {
    cloud_asr *c = userdata;
    SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
    g_autoptr(GError) error = NULL;
    SoupWebsocketConnection *ws = soup_session_websocket_connect_finish(SOUP_SESSION(source), res, &error);
    c->connecting = false;

    if(ws == NULL) {
        guint status = msg != NULL ? soup_message_get_status(msg) : 0;
        const char *text;
        if(status == SOUP_STATUS_UNAUTHORIZED) text = "[OpenAI: the API key was rejected]";
        else if(status == SOUP_STATUS_FORBIDDEN) text = "[OpenAI: this API key can't use realtime transcription]";
        else if(status == 429) text = "[OpenAI: rate limited or out of credit]";
        else text = NULL;

        printf("OpenAI connection failed (HTTP %u): %s\n", status, error ? error->message : "");
        if(text != NULL) {
            set_status(c, text);
        } else {
            g_autofree char *s = g_strdup_printf("[OpenAI: can't connect: %s]", error ? error->message : "unknown error");
            set_status(c, s);
        }

        c->retry_after = g_get_monotonic_time() + RETRY_SECONDS * G_USEC_PER_SEC;
        return;
    }

    c->ws = ws;
    soup_websocket_connection_set_keepalive_interval(ws, 20);
    // Transcripts are small, but leave room for long turns
    soup_websocket_connection_set_max_incoming_payload_size(ws, 4 * 1024 * 1024);
    g_signal_connect(ws, "message", G_CALLBACK(on_ws_message), c);
    g_signal_connect(ws, "closed", G_CALLBACK(on_ws_closed), c);

    g_mutex_lock(&c->lock);
    g_autofree char *model = g_strdup(c->model);
    g_autofree char *delay = g_strdup(c->delay);
    g_mutex_unlock(&c->lock);

    send_session_update(c, model, delay);
    printf("OpenAI realtime transcription connected (%s, delay %s)\n", model, delay ? delay : "default");

    // Send what was captured while connecting
    g_mutex_lock(&c->lock);
    schedule_flush(c);
    g_mutex_unlock(&c->lock);
}

static void connect_ws(cloud_asr *c) {
    if(c->connecting) return;
    if(g_get_monotonic_time() < c->retry_after) return;

    g_mutex_lock(&c->lock);
    g_autofree char *key = g_strdup(c->api_key);
    c->connected_gen = c->config_gen;
    g_mutex_unlock(&c->lock);

    // LIVECAPTIONS_OPENAI_URL points at another endpoint (e.g. a test server)
    const char *url = g_getenv("LIVECAPTIONS_OPENAI_URL");
    if(url == NULL) url = DEFAULT_URL;

    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
    if(msg == NULL) {
        set_status(c, "[OpenAI: invalid server URL]");
        return;
    }
    g_autofree char *auth = g_strdup_printf("Bearer %s", key);
    soup_message_headers_replace(soup_message_get_request_headers(msg), "Authorization", auth);

    c->connecting = true;
    soup_session_websocket_connect_async(c->session, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL, on_connected, c);
    g_object_unref(msg);
}

/* --------------------------------------------------------------- thread */

static gpointer cloud_thread(gpointer userdata) {
    cloud_asr *c = userdata;
    g_main_context_push_thread_default(c->ctx);

    c->session = soup_session_new_with_options("user-agent", "LiveCaptions", NULL);
    g_main_loop_run(c->loop);

    if(c->ws != NULL) {
        soup_websocket_connection_close(c->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        g_clear_object(&c->ws);
    }
    g_clear_object(&c->session);
    g_queue_clear_full(&c->turns, (GDestroyNotify)turn_free);

    g_main_context_pop_thread_default(c->ctx);
    return NULL;
}

cloud_asr *cloud_asr_new(cloud_result_cb cb, void *userdata) {
    cloud_asr *c = g_new0(cloud_asr, 1);
    c->cb = cb;
    c->userdata = userdata;
    g_mutex_init(&c->lock);
    g_queue_init(&c->turns);
    c->pending = g_byte_array_new();
    c->ctx = g_main_context_new();
    c->loop = g_main_loop_new(c->ctx, FALSE);
    c->thread = g_thread_new("lcap-cloud", cloud_thread, c);
    return c;
}

void cloud_asr_configure(cloud_asr *c, const char *api_key, const char *model, const char *delay) {
    g_mutex_lock(&c->lock);
    g_clear_pointer(&c->api_key, g_free);
    if(api_key != NULL && api_key[0] != '\0') c->api_key = g_strdup(api_key);
    g_free(c->model);
    c->model = g_strdup(model != NULL && model[0] != '\0' ? model : "gpt-realtime-whisper");
    g_free(c->delay);
    c->delay = g_strdup(delay);
    c->config_gen++;
    g_byte_array_set_size(c->pending, 0);
    g_clear_pointer(&c->status, g_free);
    schedule_flush(c);
    g_mutex_unlock(&c->lock);

    // Retry right away with the new settings
    c->retry_after = 0;
}

// Linear-interpolation resampling from 16 kHz to 24 kHz
static void resample_append(cloud_asr *c, const short *in, size_t n) {
    if(n == 0) return;
    if(!c->resample_primed) {
        c->resample_last = in[0];
        c->resample_t = 0.0;
        c->resample_primed = true;
    }

    const double step = 16000.0 / OUT_RATE;
    double t = c->resample_t;
    short out[4096];
    size_t m = 0;

    while(t <= (double)(n - 1)) {
        long i = (long)(t < 0.0 ? -1 : t);
        double frac = t - (double)i;
        short a = (i < 0) ? c->resample_last : in[i];
        short b = in[i + 1 < (long)n ? i + 1 : (long)n - 1];
        out[m++] = (short)(a + (b - a) * frac);
        t += step;

        if(m == G_N_ELEMENTS(out)) {
            g_byte_array_append(c->pending, (const guint8 *)out, m * sizeof(short));
            m = 0;
        }
    }
    if(m > 0) g_byte_array_append(c->pending, (const guint8 *)out, m * sizeof(short));

    c->resample_t = t - (double)n;
    c->resample_last = in[n - 1];
}

void cloud_asr_feed(cloud_asr *c, const short *pcm, size_t count) {
    g_mutex_lock(&c->lock);
    if(c->api_key != NULL) {
        resample_append(c, pcm, count);
        if(c->pending->len > MAX_PENDING_BYTES)
            g_byte_array_remove_range(c->pending, 0, c->pending->len - MAX_PENDING_BYTES);
        schedule_flush(c);
    }
    g_mutex_unlock(&c->lock);
}

void cloud_asr_commit(cloud_asr *c) {
    g_mutex_lock(&c->lock);
    if(c->api_key != NULL) {
        c->commit_requested = true;
        schedule_flush(c);
    }
    g_mutex_unlock(&c->lock);
}

char *cloud_asr_get_status(cloud_asr *c) {
    g_mutex_lock(&c->lock);
    char *status = (c->api_key != NULL) ? g_strdup(c->status) : NULL;
    g_mutex_unlock(&c->lock);
    return status;
}

void cloud_asr_free(cloud_asr *c) {
    g_main_loop_quit(c->loop);
    g_main_context_wakeup(c->ctx);
    g_thread_join(c->thread);

    g_main_loop_unref(c->loop);
    g_main_context_unref(c->ctx);
    g_byte_array_unref(c->pending);
    g_free(c->api_key);
    g_free(c->model);
    g_free(c->delay);
    g_free(c->status);
    g_mutex_clear(&c->lock);
    g_free(c);
}
