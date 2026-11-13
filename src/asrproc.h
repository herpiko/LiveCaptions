/* asrproc.h
 * This file contains declarations for asr_thread which performs the ASR
 * processing.
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

#pragma once

#include <adwaita.h>

struct _LiveCaptionsWindow;

struct asr_thread_i;
typedef struct asr_thread_i * asr_thread;


asr_thread create_asr_thread(const char *model_path);
bool asr_thread_update_model(asr_thread thread, const char *model_path);
bool asr_thread_is_errored(asr_thread thread);
void asr_thread_set_main_window(asr_thread thread, struct _LiveCaptionsWindow *window);
void asr_thread_enqueue_audio(asr_thread thread, short *data, size_t num_shorts);
gpointer asr_thread_get_model(asr_thread thread);
gpointer asr_thread_get_session(asr_thread thread);
void asr_thread_pause(asr_thread thread, bool pause);
void asr_thread_set_text_stream_active(asr_thread thread, bool active);
int asr_thread_samplerate(asr_thread thread);
float asr_thread_get_speedup(asr_thread thread);
bool asr_thread_is_loaded(asr_thread thread);
// Runs ~30s of noise through the loaded model; returns realtime speed factor
double asr_thread_benchmark(asr_thread thread, volatile double *progress);

// True if model_path is a sherpa-onnx model (its directory or tokens.txt)
bool asr_is_sherpa_model(const char *model_path);
// False for sherpa-onnx models when this build doesn't include sherpa-onnx
bool asr_model_supported(const char *model_path);
// True for offline models (Parakeet) that can only be used to refine finished lines
bool asr_is_refine_model(const char *model_path);
// Finds an installed refine model in the system models directory (caller frees), or NULL
char *asr_find_refine_model(void);
// Re-transcribe each finished utterance with an offline model; NULL turns it off.
// Loads in the background. Only applies when the live model is a sherpa-onnx one.
void asr_thread_set_refine_model(asr_thread thread, const char *model_path);
// Use OpenAI's Realtime API instead of the local model; a NULL/empty key goes
// back to the local model. Does nothing when built without cloud support.
void asr_thread_set_cloud(asr_thread thread, const char *api_key, const char *model, const char *delay, const char *languages);
// A message to show instead of captions (e.g. a cloud error), or NULL. Caller frees.
char *asr_thread_get_status(asr_thread thread);
// True if this build includes OpenAI cloud transcription
bool asr_cloud_supported(void);
// Short name to show for a model path (caller frees)
char *asr_model_display_name(const char *model_path);
void asr_thread_flush(asr_thread thread);
void free_asr_thread(asr_thread thread);
