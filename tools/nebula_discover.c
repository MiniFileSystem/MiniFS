/*
 * nebula_discover.c - Scan paths / patterns and report Nebula devices.
 *
 * Usage:
 *   nebula_discover [--domain <failure-domain>] [--format json|text] <path>...
 *
 * For each positional path:
 *   - If directory, walk it (depth-1) and try each regular file.
 *   - Otherwise treat as a file path.
 * Reads the MBR; if it's a Nebula device, prints uuid, version, capacity.
 *
 * Output format:
 *   text  (default) - one device per line
 *   json            - JSON array, ready to pipe into etcdctl or jq
 *
 * Failure domain is taken from:
 *   1. --domain flag
 *   2. $NEBULA_FAILURE_DOMAIN environment variable
 *   3. hostname (fallback)
 */
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"

#include "../src/util/uuid.h"
#include "../src/util/log.h"
#include "../src/nebula/nebula_mbr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_DEVICES 256

struct discovered {
    char      path[512];
    bool      is_nebula;
    uint8_t   uuid[16];
    uint16_t  vmajor, vminor;
    uint64_t  capacity_blocks;
};

static const char *g_domain    = NULL;
static const char *g_hostname  = "unknown";
static bool        g_json      = false;

static void try_device(const char *path, struct discovered *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);

    struct nebula_io *io = NULL;
    int rc = nebula_io_open(path, false, 0, &io);
    if (rc != NEBULA_OK) {
        out->is_nebula = false;
        return;
    }
    struct nebula_mbr mbr;
    rc = nebula_mbr_read(io, &mbr);
    nebula_io_close(io);
    if (rc != NEBULA_OK) {
        out->is_nebula = false;
        return;
    }
    out->is_nebula       = true;
    memcpy(out->uuid, mbr.device_uuid, 16);
    out->vmajor          = mbr.version_major;
    out->vminor          = mbr.version_minor;
    out->capacity_blocks = mbr.device_capacity_blocks;
}

static void scan_one(const char *p, struct discovered *arr, int *n)
{
    struct stat st;
    if (stat(p, &st) < 0) {
        fprintf(stderr, "stat %s: %s\n", p, strerror(errno));
        return;
    }

    if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        if (*n >= MAX_DEVICES) return;
        try_device(p, &arr[(*n)++]);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) { fprintf(stderr, "opendir %s: %s\n", p, strerror(errno)); return; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", p, de->d_name);
            struct stat cs;
            if (stat(full, &cs) < 0) continue;
            if (!S_ISREG(cs.st_mode) && !S_ISBLK(cs.st_mode) && !S_ISCHR(cs.st_mode))
                continue;
            if (*n >= MAX_DEVICES) break;
            try_device(full, &arr[(*n)++]);
        }
        closedir(d);
    }
}

static void print_text(const struct discovered *arr, int n)
{
    int nebula_count = 0;
    for (int i = 0; i < n; i++) {
        if (!arr[i].is_nebula) continue;
        nebula_count++;
        char u[NEBULA_UUID_STR_LEN + 1];
        nebula_uuid_format(arr[i].uuid, u);
        printf("%s  uuid=%s v%u.%u cap=%.2fMiB domain=%s host=%s\n",
               arr[i].path, u, arr[i].vmajor, arr[i].vminor,
               (double)arr[i].capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0*1024.0),
               g_domain, g_hostname);
    }
    fprintf(stderr, "Scanned %d path(s); %d Nebula device(s) found.\n", n, nebula_count);
}

static void print_json(const struct discovered *arr, int n)
{
    printf("[");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!arr[i].is_nebula) continue;
        char u[NEBULA_UUID_STR_LEN + 1];
        nebula_uuid_format(arr[i].uuid, u);
        printf("%s\n  {\n"
               "    \"path\": \"%s\",\n"
               "    \"uuid\": \"%s\",\n"
               "    \"version\": \"%u.%u\",\n"
               "    \"capacity_blocks\": %lu,\n"
               "    \"capacity_bytes\":  %lu,\n"
               "    \"failure_domain\": \"%s\",\n"
               "    \"hostname\":       \"%s\",\n"
               "    \"etcd_key\": \"/nebula/devices/%s/%s\"\n"
               "  }",
               first ? "" : ",",
               arr[i].path, u, arr[i].vmajor, arr[i].vminor,
               (unsigned long)arr[i].capacity_blocks,
               (unsigned long)arr[i].capacity_blocks * NEBULA_BLOCK_SIZE,
               g_domain, g_hostname, g_domain, u);
        first = false;
    }
    printf("\n]\n");
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [--domain D] [--format text|json] <path>...\n", p);
}

int main(int argc, char **argv)
{
    static char hostname_buf[128];
    if (gethostname(hostname_buf, sizeof(hostname_buf)) == 0) {
        g_hostname = hostname_buf;
    }
    g_domain = getenv("NEBULA_FAILURE_DOMAIN");
    if (!g_domain) g_domain = g_hostname;

    int first_path = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--domain") && i + 1 < argc) {
            g_domain = argv[++i];
        } else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            const char *f = argv[++i];
            if (!strcmp(f, "json")) g_json = true;
            else if (!strcmp(f, "text")) g_json = false;
            else { usage(argv[0]); return 2; }
        } else if (argv[i][0] == '-') {
            usage(argv[0]); return 2;
        } else {
            first_path = i;
            break;
        }
    }
    if (first_path >= argc) { usage(argv[0]); return 2; }

    struct discovered *arr = calloc(MAX_DEVICES, sizeof(*arr));
    if (!arr) { perror("calloc"); return 1; }
    int n = 0;
    for (int i = first_path; i < argc; i++) {
        scan_one(argv[i], arr, &n);
    }

    if (g_json) print_json(arr, n);
    else        print_text(arr, n);

    free(arr);
    return 0;
}
