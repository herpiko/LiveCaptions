/* cloudasr.h
 * Streaming transcription through OpenAI's Realtime API (transcription
 * sessions over a WebSocket). Audio goes in as 16 kHz PCM16 from any thread;
 * transcripts come back through a callback on the client's own thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct cloud_asr cloud_asr;

typedef enum {
    CLOUD_RESULT_PARTIAL, // text of the turn in progress, so far
    CLOUD_RESULT_FINAL,   // final text of a finished turn
} cloud_result_type;

// Called on the cloud client's thread
typedef void (*cloud_result_cb)(void *userdata, cloud_result_type type, const char *text);

cloud_asr *cloud_asr_new(cloud_result_cb cb, void *userdata);

// Sets the API key, model, latency ("minimal".."xhigh") and expected languages
// (comma-separated ISO-639-1 codes such as "en,id"; empty to detect any).
// Reconnects on the next audio if anything changed. A NULL or empty key
// disables the client.
void cloud_asr_configure(cloud_asr *c, const char *api_key, const char *model, const char *delay, const char *languages);

// Queue audio (16 kHz mono PCM16) for sending; connects if needed
void cloud_asr_feed(cloud_asr *c, const short *pcm, size_t count);

// The speaker paused: ask for the final transcript of what was sent so far
void cloud_asr_commit(cloud_asr *c);

// A short status message to show instead of captions (caller frees), or NULL
// when everything is fine
char *cloud_asr_get_status(cloud_asr *c);

void cloud_asr_free(cloud_asr *c);
