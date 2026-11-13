/* audiocap.h
 * This file contains declarations for audio_thread, which reads system audio
 * and passes them to the provided asr_thread.
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

#include <glib.h>
#include "asrproc.h"

struct audio_thread_i;
typedef struct audio_thread_i * audio_thread;

// Desktop audio is always captured; with microphone=true the microphone is
// mixed in as well. desktop_source / microphone_source are PulseAudio source
// names; NULL or "" uses the default (the default output's monitor for
// desktop audio, the default input for the microphone).
audio_thread create_audio_thread(bool microphone, const char *desktop_source,
                                 const char *microphone_source, asr_thread asr);
void free_audio_thread(audio_thread thread);

typedef struct {
    char *name;
    char *description;
} audio_source_info;

void audio_source_info_free(gpointer info);

// Lists PulseAudio sources. With monitors=true returns only sink monitors
// (desktop audio), otherwise only real inputs (microphones).
// Returns a GPtrArray of audio_source_info*, which frees its elements.
GPtrArray *audiocap_list_sources(bool monitors);
