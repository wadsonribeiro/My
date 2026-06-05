#include "device_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
# include <windows.h>
# include <shlwapi.h>
#else
# include <libgen.h>
# include <limits.h>
# include <unistd.h>
#endif

#include "../util/log.h"

/* ---------- helpers ---------------------------------------------------- */

static void
get_config_path(char *out, size_t out_len) {
#ifdef _WIN32
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    PathRemoveFileSpecA(exe);
    snprintf(out, out_len, "%s\\devices_data.json", exe);
#else
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) { strncpy(exe, ".", sizeof(exe)); }
    else exe[n] = '\0';
    char *dir = dirname(exe);
    snprintf(out, out_len, "%s/devices_data.json", dir);
#endif
}

static void
get_prints_dir(char *out, size_t out_len) {
#ifdef _WIN32
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    PathRemoveFileSpecA(exe);
    snprintf(out, out_len, "%s\\Prints", exe);
    CreateDirectoryA(out, NULL);
#else
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) { strncpy(exe, ".", sizeof(exe)); }
    else exe[n] = '\0';
    char *dir = dirname(exe);
    snprintf(out, out_len, "%s/Prints", dir);
    mkdir(out, 0755);
#endif
}

/* ---------- tiny JSON helpers (no external deps) ----------------------- */

/* Return pointer to the first occurrence of key in the JSON blob.
 * Works for simple flat objects only — good enough here. */
static const char *
json_find_key(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(json, needle);
}

/* Extract a string value for "key":"<value>" → copies into buf */
static bool
json_get_string(const char *json, const char *key, char *buf, size_t buf_len) {
    const char *p = json_find_key(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < buf_len - 1) buf[i++] = *p++;
        buf[i] = '\0';
        return true;
    }
    return false;
}

/* Extract an integer value for "key": <int> */
static bool
json_get_int(const char *json, const char *key, int *out) {
    const char *p = json_find_key(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        /* "120" form */
        p++;
        *out = atoi(p);
        return true;
    }
    *out = atoi(p);
    return true;
}

/* Extract a bool value: true / false */
static bool
json_get_bool(const char *json, const char *key, bool *out) {
    const char *p = json_find_key(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    *out = (strncmp(p, "true", 4) == 0);
    return true;
}

/* Find the JSON object block for device_id */
static const char *
json_find_device(const char *json, const char *device_id, size_t *block_len) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", device_id);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    /* skip past key to opening '{' */
    p = strchr(p, '{');
    if (!p) return NULL;
    /* find matching '}' */
    int depth = 0;
    const char *start = p;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { *block_len = (size_t)(p - start + 1); return start; } }
        p++;
    }
    return NULL;
}

/* ---------- public API ------------------------------------------------- */

void
sc_device_config_init(sc_device_config *cfg, const char *device_id) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->device_id, device_id, sizeof(cfg->device_id) - 1);
    cfg->fps        = DC_DEFAULT_FPS;
    cfg->resolution = DC_DEFAULT_RES;
    strncpy(cfg->bitrate, DC_DEFAULT_BITRATE, sizeof(cfg->bitrate) - 1);
    cfg->audio      = true;
}

bool
sc_device_config_load(sc_device_config *cfg, const char *device_id) {
    sc_device_config_init(cfg, device_id);

    char path[512];
    get_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        LOGI("devices_data.json not found — using defaults");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    size_t block_len = 0;
    const char *block = json_find_device(buf, device_id, &block_len);
    if (!block) {
        LOGI("Device %s not found in devices_data.json — using defaults", device_id);
        free(buf);
        return false;
    }

    /* Work on a null-terminated copy of the block */
    char *blk = malloc(block_len + 1);
    if (!blk) { free(buf); return false; }
    memcpy(blk, block, block_len);
    blk[block_len] = '\0';

    char tmp[64];
    if (json_get_string(blk, "name", tmp, sizeof(tmp)))
        strncpy(cfg->device_name, tmp, sizeof(cfg->device_name) - 1);

    int ival;
    if (json_get_int(blk, "fps", &ival))     cfg->fps        = ival;
    if (json_get_int(blk, "res", &ival))     cfg->resolution = ival;
    if (json_get_string(blk, "bitrate", tmp, sizeof(tmp)))
        strncpy(cfg->bitrate, tmp, sizeof(cfg->bitrate) - 1);

    bool bval;
    if (json_get_bool(blk, "audio", &bval)) cfg->audio = bval;

    free(blk);
    free(buf);
    return true;
}

bool
sc_device_config_save(const sc_device_config *cfg) {
    char path[512];
    get_config_path(path, sizeof(path));

    /* Load existing JSON so we can update only this device's entry */
    FILE *f = fopen(path, "r");
    char *existing = NULL;
    long  existing_sz = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        existing_sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        existing = malloc(existing_sz + 1);
        if (existing) { fread(existing, 1, existing_sz, f); existing[existing_sz] = '\0'; }
        fclose(f);
    }

    /* Build new entry */
    char new_entry[512];
    snprintf(new_entry, sizeof(new_entry),
        "  \"%s\": {\n"
        "    \"name\": \"%s\",\n"
        "    \"device_id\": \"%s\",\n"
        "    \"fps\": \"%d\",\n"
        "    \"res\": \"%d\",\n"
        "    \"bitrate\": \"%s\",\n"
        "    \"audio\": %s\n"
        "  }",
        cfg->device_id,
        cfg->device_name[0] ? cfg->device_name : cfg->device_id,
        cfg->device_id,
        cfg->fps,
        cfg->resolution,
        cfg->bitrate,
        cfg->audio ? "true" : "false");

    /* Compose final JSON */
    char *out = NULL;
    size_t out_len = 0;

    if (existing && existing_sz > 4) {
        size_t block_len = 0;
        const char *block = json_find_device(existing, cfg->device_id, &block_len);
        if (block) {
            /* Replace existing block */
            size_t prefix = (size_t)(block - existing);
            size_t suffix_start = prefix + block_len;
            size_t new_len = prefix + strlen(new_entry) + (existing_sz - suffix_start) + 1;
            out = malloc(new_len + 1);
            if (!out) { free(existing); return false; }
            memcpy(out, existing, prefix);
            memcpy(out + prefix, new_entry, strlen(new_entry));
            memcpy(out + prefix + strlen(new_entry), existing + suffix_start,
                   existing_sz - suffix_start);
            out[new_len - 1] = '\0';
            out_len = new_len - 1;
        } else {
            /* Append new device before final '}' */
            char *last_brace = strrchr(existing, '}');
            if (last_brace) {
                size_t prefix = (size_t)(last_brace - existing);
                const char *comma = ",\n";
                out_len = prefix + strlen(comma) + strlen(new_entry) + 3;
                out = malloc(out_len + 1);
                if (!out) { free(existing); return false; }
                memcpy(out, existing, prefix);
                memcpy(out + prefix, comma, strlen(comma));
                memcpy(out + prefix + strlen(comma), new_entry, strlen(new_entry));
                strcpy(out + prefix + strlen(comma) + strlen(new_entry), "\n}");
            }
        }
    }

    if (!out) {
        /* Fresh file */
        out_len = strlen(new_entry) + 6;
        out = malloc(out_len + 1);
        if (!out) { free(existing); return false; }
        snprintf(out, out_len + 1, "{\n%s\n}\n", new_entry);
    }

    f = fopen(path, "w");
    if (!f) { free(out); free(existing); return false; }
    fwrite(out, 1, strlen(out), f);
    fclose(f);
    free(out);
    free(existing);
    LOGI("Config saved for device %s", cfg->device_id);
    return true;
}

void
sc_device_config_build_cmd(const sc_device_config *cfg,
                           char *out, size_t out_len) {
    /* Always-on flags: --stay-awake --gamepad=uhid --print-fps */
    snprintf(out, out_len,
        "scrcpy -s %s --gamepad=uhid --print-fps --stay-awake "
        "--max-fps=%d -m %d -b %s%s",
        cfg->device_id,
        cfg->fps,
        cfg->resolution,
        cfg->bitrate,
        cfg->audio ? "" : " --no-audio");
}
