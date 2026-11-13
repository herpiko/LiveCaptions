/* secret-store.h
 * Keeps the OpenAI API key in the desktop keyring (libsecret).
 */

#pragma once

#include <stdbool.h>

// $OPENAI_API_KEY if set, else the key saved in the keyring (caller frees); NULL if none
char *secret_store_get_openai_key(void);

// Stores the key; NULL or empty removes it. Returns false on failure.
bool secret_store_set_openai_key(const char *key);

// True if a key is saved in the keyring (not counting the environment)
bool secret_store_has_openai_key(void);
