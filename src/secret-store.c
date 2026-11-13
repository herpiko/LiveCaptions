/* secret-store.c
 * Keeps the OpenAI API key in the desktop keyring (libsecret).
 */

#include <string.h>
#include <stdio.h>
#include <libsecret/secret.h>

#include "secret-store.h"

static const SecretSchema *key_schema(void) {
    static const SecretSchema schema = {
        "net.sapples.LiveCaptions.ApiKey", SECRET_SCHEMA_NONE,
        {
            { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { NULL, 0 },
        }
    };
    return &schema;
}

static char *lookup(void) {
    GError *error = NULL;
    char *key = secret_password_lookup_sync(key_schema(), NULL, &error, "service", "openai", NULL);
    if(error != NULL) {
        printf("Couldn't read the OpenAI key from the keyring: %s\n", error->message);
        g_error_free(error);
    }
    return key;
}

char *secret_store_get_openai_key(void) {
    // An explicit $OPENAI_API_KEY wins, and avoids touching the keyring
    const char *env = g_getenv("OPENAI_API_KEY");
    if(env != NULL && env[0] != '\0') return g_strdup(env);

    char *key = lookup();
    if(key != NULL && key[0] != '\0') {
        char *copy = g_strdup(key);
        secret_password_free(key);
        return copy;
    }
    if(key != NULL) secret_password_free(key);
    return NULL;
}

bool secret_store_has_openai_key(void) {
    char *key = lookup();
    bool has = key != NULL && key[0] != '\0';
    if(key != NULL) secret_password_free(key);
    return has;
}

bool secret_store_set_openai_key(const char *key) {
    GError *error = NULL;
    bool ok;

    if(key == NULL || key[0] == '\0') {
        secret_password_clear_sync(key_schema(), NULL, &error, "service", "openai", NULL);
        ok = error == NULL;
    } else {
        ok = secret_password_store_sync(key_schema(), SECRET_COLLECTION_DEFAULT, "Live Captions: OpenAI API key",
                                        key, NULL, &error, "service", "openai", NULL);
    }

    if(error != NULL) {
        printf("Couldn't update the OpenAI key in the keyring: %s\n", error->message);
        g_error_free(error);
    }
    return ok;
}
