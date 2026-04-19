/*
 * uuid.h - 128-bit UUID generation and formatting.
 */
#ifndef NEBULA_UUID_H
#define NEBULA_UUID_H

#include <stdint.h>

#define NEBULA_UUID_SIZE    16
#define NEBULA_UUID_STR_LEN 36  /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */

/* Generate a random v4-style UUID from /dev/urandom.
 * Returns 0 on success, -errno on failure.
 */
int nebula_uuid_generate(uint8_t uuid[NEBULA_UUID_SIZE]);

/* Format as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" + NUL (37 chars). */
void nebula_uuid_format(const uint8_t uuid[NEBULA_UUID_SIZE],
                        char out[NEBULA_UUID_STR_LEN + 1]);

#endif
