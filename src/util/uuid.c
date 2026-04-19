/*
 * uuid.c - Random UUID generation from /dev/urandom.
 */
#include "uuid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int nebula_uuid_generate(uint8_t uuid[NEBULA_UUID_SIZE])
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    {
        size_t got = 0;
        while (got < NEBULA_UUID_SIZE) {
            ssize_t n = read(fd, uuid + got, NEBULA_UUID_SIZE - got);
            if (n < 0) {
                if (errno == EINTR) continue;
                int e = errno;
                close(fd);
                return -e;
            }
            if (n == 0) {
                close(fd);
                return -EIO;
            }
            got += (size_t)n;
        }
    }
    close(fd);

    /* Set RFC-4122 v4 bits: version=4, variant=10xx. */
    uuid[6] = (uuid[6] & 0x0Fu) | 0x40u;
    uuid[8] = (uuid[8] & 0x3Fu) | 0x80u;
    return 0;
}

void nebula_uuid_format(const uint8_t uuid[NEBULA_UUID_SIZE],
                        char out[NEBULA_UUID_STR_LEN + 1])
{
    snprintf(out, NEBULA_UUID_STR_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}
