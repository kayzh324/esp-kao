/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "claw_tier.h"
#include "claw_utils_string.h"
#include "esp_log.h"

static const char *TAG = "cap_files";

/* Per-tier (claw_tier.h): the file-size limits are sized so a whole source file
 * fits one read/edit on modules with the RAM for it. */
#define CAP_FILES_MAX_FILE_SIZE CLAW_TIER_CAP_FILES_MAX_FILE_SIZE

/* read_file paging (added): line-numbered output with offset/limit so the model
 * can page through large files instead of re-reading whole files every time.
 * A single physical line longer than the per-line cap is truncated (UTF-8 safe)
 * and marked, so one minified/base64 line cannot consume the whole output. */
#define CAP_FILES_READ_LINE_MAX_BYTES 2048
#define CAP_FILES_READ_HINT_RESERVE 256
#define CAP_FILES_READ_COUNT_SCAN_MAX_LINES 200000

/* edit_file (added): anchored old_text -> new_text replacement. Bounds: whole
 * file must fit the edit buffer; replacements must be unique per old_text. */
#define CAP_FILES_EDIT_MAX_FILE_SIZE CLAW_TIER_CAP_FILES_EDIT_MAX_FILE_SIZE
#define CAP_FILES_EDIT_TMP_SUFFIX ".tmp"

/* grep_files (added): content search. Without it the model had to read whole
 * files (hitting output truncation) or write throwaway Lua probes just to locate
 * code - the main reason modifying an existing program cost more than writing a
 * new one. Plain-text matching: the platform has no POSIX regex. */
#define CAP_FILES_GREP_MAX_FILE_SIZE CLAW_TIER_CAP_FILES_GREP_MAX_FILE_SIZE
#define CAP_FILES_GREP_LINE_MAX_BYTES 400
#define CAP_FILES_GREP_DEFAULT_MAX_RESULTS 100
#define CAP_FILES_GREP_MAX_RESULTS_LIMIT 200

/* versioned writes (added): before a write/edit replaces a file, the previous
 * content is snapshotted to a deterministic path and reported in the tool result,
 * so "is there a backup and where" is TOLD to the model instead of probed. */
#define CAP_FILES_VERSIONS_DIRNAME ".versions"
#define CAP_FILES_VERSIONS_KEEP 5
#define CAP_FILES_VERSION_PATH_MAX 320

/* Size guidance (added): a file that eats a large share of the per-round context
 * is expensive on EVERY read, and re-reads dominate long tasks - measured on one
 * real task, a 12 KB main.lua was re-read 23 times and a 9 KB spec 13 times, with
 * the task's inputs very nearly filling the window on their own. So the write tools
 * report the cost in the unit that actually matters (fraction of one round) at the
 * moment the file is created - which is when splitting is still cheap.
 *
 * A hint, never a refusal: whether to split is an engineering judgement, but it
 * should be made with the number in hand. The thresholds are fractions of the
 * window, so on a bigger module the same file stops being flagged. */
static bool cap_files_is_source_path(const char *path)
{
    static const char *const exts[] = {
        ".lua", ".c", ".h", ".cpp", ".hpp", ".cc", ".py", ".js", ".ts", ".sh",
    };
    const char *dot = path ? strrchr(path, '.') : NULL;

    if (!dot) {
        return false;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void cap_files_append_size_hint(char *output, size_t output_size,
                                       const char *path, size_t size)
{
    size_t guide = cap_files_is_source_path(path) ? CLAW_TIER_FILE_GUIDE_CODE_BYTES
                                                  : CLAW_TIER_FILE_GUIDE_TEXT_BYTES;
    size_t used;
    unsigned pct;

    if (!output || !path || size <= guide) {
        return;
    }
    used = strlen(output);
    if (used + 240 >= output_size) {
        return;
    }
    pct = (unsigned)((size * 100) / CLAW_TIER_RUNTIME_BUDGET_BYTES);
    snprintf(output + used, output_size - used,
             "\nhint: %s is %u KB = ~%u%% of ONE round's context on this board (%u KB). "
             "Reading it costs that much of your working set every time. If it has more than one "
             "responsibility, split it by responsibility (aim <= %u KB per file).",
             path, (unsigned)((size + 1023) / 1024), pct,
             (unsigned)(CLAW_TIER_RUNTIME_BUDGET_BYTES / 1024),
             (unsigned)(guide / 1024));
}

/* Sandbox roots are resolved from claw_paths on demand so cap_files always
 * follows the live mount points (flash vs SD for DATA). Writability is fixed
 * by the semantic role of each logical root: DATA is writable, SYSTEM is the
 * read-only firmware partition. All tool paths must be absolute and fall under
 * one of these roots. */
static const struct {
    claw_path_root_t key;
    bool             writable;
} s_root_specs[] = {
    { CLAW_PATH_DATA,   true  },
    { CLAW_PATH_SYSTEM, false },
};

#define CAP_FILES_ROOT_SPEC_COUNT (sizeof(s_root_specs) / sizeof(s_root_specs[0]))

typedef struct {
    const char *dir;       /* NULL means path matched no configured root */
    bool        writable;
} cap_files_resolved_root_t;

/* Resolve which configured root contains path, returning its live mount path
 * and writability. The dir field is NULL when path escapes every root. */
static cap_files_resolved_root_t cap_files_match_root(const char *path)
{
    cap_files_resolved_root_t result = { NULL, false };

    for (size_t i = 0; i < CAP_FILES_ROOT_SPEC_COUNT; i++) {
        const char *base = claw_paths_get(s_root_specs[i].key);
        size_t      base_len;

        if (!base) {
            continue;
        }
        base_len = strlen(base);
        if (strncmp(path, base, base_len) == 0
            && (path[base_len] == '\0' || path[base_len] == '/')) {
            result.dir = base;
            result.writable = s_root_specs[i].writable;
            return result;
        }
    }
    return result;
}

static bool cap_files_any_root_configured(void)
{
    for (size_t i = 0; i < CAP_FILES_ROOT_SPEC_COUNT; i++) {
        if (claw_paths_get(s_root_specs[i].key)) {
            return true;
        }
    }
    return false;
}

static bool cap_files_text_contains_ci(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (!needle || !needle[0]) {
        return true;
    }
    if (!haystack) {
        return false;
    }

    needle_len = strlen(needle);
    for (const char *cursor = haystack; *cursor; cursor++) {
        size_t i;

        for (i = 0; i < needle_len; i++) {
            unsigned char hc = (unsigned char)cursor[i];
            unsigned char nc = (unsigned char)needle[i];

            if (hc == '\0') {
                return false;
            }
            if (hc >= 'A' && hc <= 'Z') {
                hc = (unsigned char)(hc - 'A' + 'a');
            }
            if (nc >= 'A' && nc <= 'Z') {
                nc = (unsigned char)(nc - 'A' + 'a');
            }
            if (hc != nc) {
                break;
            }
        }
        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

/* Names beginning with '.' are treated as hidden (Unix convention). Seed trees
 * such as /system/.recovery use this prefix to stay out of list_dir output so
 * the LLM does not see or recurse into them. Access by explicit path still works. */
static bool cap_files_is_hidden_name(const char *name)
{
    return name && name[0] == '.';
}

static bool cap_files_path_is_valid(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    return cap_files_match_root(path).dir != NULL;
}

/* A path is writable only when it falls under a root whose role is writable. */
static bool cap_files_path_is_writable(const char *path)
{
    return cap_files_match_root(path).writable;
}

/* Tool paths must be absolute. Validate the path lies under a sandbox root and
 * copy it out verbatim; no relative-to-workspace resolution is performed. */
static esp_err_t cap_files_resolve_path(const char *path, char *resolved, size_t resolved_size)
{
    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_files_any_root_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path[0] != '/') {
        ESP_LOGE(TAG, "path must be absolute: %s", path);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_files_path_is_valid(path)) {
        ESP_LOGE(TAG, "path escapes roots: %s", path);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(resolved, path, resolved_size) >= resolved_size) {
        ESP_LOGE(TAG, "path too long: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_files_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_files_ensure_parent_dirs(const char *path)
{
    char dir[256];
    char *slash = NULL;
    char *cursor = NULL;
    cap_files_resolved_root_t root;
    size_t base_len;

    root = cap_files_match_root(path);
    if (!root.dir) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(dir, path, sizeof(dir));
    slash = strrchr(dir, '/');
    if (!slash) {
        return ESP_OK;
    }

    base_len = strlen(root.dir);
    if ((size_t)(slash - dir) <= base_len) {
        return cap_files_ensure_dir(root.dir);
    }
    *slash = '\0';

    if (cap_files_ensure_dir(root.dir) != ESP_OK) {
        return ESP_FAIL;
    }

    cursor = dir + base_len + 1;
    while (*cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (cap_files_ensure_dir(dir) != ESP_OK) {
                *cursor = '/';
                return ESP_FAIL;
            }
            *cursor = '/';
        }
        cursor++;
    }

    return cap_files_ensure_dir(dir);
}

static esp_err_t cap_files_list_recursive(const char *dir_path,
                                          const char *keyword,
                                          char *output,
                                          size_t output_size,
                                          size_t *offset,
                                          int *count)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "opendir %s: errno=%d", dir_path, errno);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[256];
        struct stat st = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Hide dot-prefixed entries (files and dirs); also stops recursion into them. */
        if (cap_files_is_hidden_name(entry->d_name)) {
            continue;
        }

        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(full_path)) {
            ESP_LOGE(TAG, "path too long: %s/%s", dir_path, entry->d_name);
            closedir(dir);
            return ESP_ERR_INVALID_SIZE;
        }

        if (!cap_files_path_is_valid(full_path)) {
            continue;
        }

        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = cap_files_list_recursive(full_path,
                                                     keyword,
                                                     output,
                                                     output_size,
                                                     offset,
                                                     count);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
            continue;
        }

        if (keyword && !cap_files_text_contains_ci(full_path, keyword)) {
            continue;
        }

        if (*offset < output_size - 1) {
            int written = snprintf(output + *offset, output_size - *offset, "%s\n", full_path);
            if (written < 0) {
                closedir(dir);
                return ESP_FAIL;
            }
            if ((size_t)written >= output_size - *offset) {
                *offset = output_size - 1;
            } else {
                *offset += (size_t)written;
            }
        }
        (*count)++;
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t cap_files_copy_file_internal(const char *src_path, const char *dst_path)
{
    FILE *src = NULL;
    FILE *dst = NULL;
    uint8_t buffer[1024];
    struct stat st = {0};
    esp_err_t err = ESP_OK;

    if (!cap_files_path_is_valid(src_path) || !cap_files_path_is_valid(dst_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(src_path, dst_path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (cap_files_ensure_parent_dirs(dst_path) != ESP_OK) {
        return ESP_FAIL;
    }

    src = fopen(src_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "fopen src %s: errno=%d", src_path, errno);
        return ESP_FAIL;
    }

    dst = fopen(dst_path, "wb");
    if (!dst) {
        ESP_LOGE(TAG, "fopen dst %s: errno=%d", dst_path, errno);
        fclose(src);
        return ESP_FAIL;
    }

    while (!feof(src)) {
        size_t read_size = fread(buffer, 1, sizeof(buffer), src);

        if (read_size > 0 && fwrite(buffer, 1, read_size, dst) != read_size) {
            ESP_LOGE(TAG, "fwrite %s: errno=%d", dst_path, errno);
            err = ESP_FAIL;
            break;
        }
        if (ferror(src)) {
            ESP_LOGE(TAG, "fread %s: errno=%d", src_path, errno);
            err = ESP_FAIL;
            break;
        }
    }

    fclose(dst);
    fclose(src);

    if (err != ESP_OK) {
        unlink(dst_path);
    }

    return err;
}

/* Consume the remainder of an over-long physical line so the next fgets starts
 * at a real line boundary. */
static void cap_files_drain_line(FILE *file)
{
    int c;

    while ((c = fgetc(file)) != EOF && c != '\n') {
        /* discard */
    }
}

/* Append one emitted source line to the output buffer: a "N: " prefix plus the
 * line body, truncating an over-long body at a UTF-8 boundary. */
static void cap_files_emit_line(char *output,
                                size_t output_size,
                                size_t *used,
                                uint32_t display_no,
                                const char *body,
                                size_t body_len,
                                bool truncated)
{
    int written = snprintf(output + *used, output_size - *used, "%u: ", (unsigned)display_no);

    if (written < 0 || (size_t)written >= output_size - *used) {
        return;
    }
    *used += (size_t)written;

    if (truncated) {
        size_t avail = (output_size - *used > 16) ? (output_size - *used - 16) : 0;
        size_t keep = claw_utils_utf8_prefix_len(body, avail);
        memcpy(output + *used, body, keep);
        *used += keep;
        *used += snprintf(output + *used, output_size - *used, " [truncated]");
    } else if (body_len + 1 < output_size - *used) {
        memcpy(output + *used, body, body_len);
        *used += body_len;
    } else {
        size_t keep = claw_utils_utf8_prefix_len(body, output_size - *used - 1);
        memcpy(output + *used, body, keep);
        *used += keep;
    }

    if (*used + 1 < output_size) {
        output[(*used)++] = '\n';
    }
    output[*used] = '\0';
}

/* versioned writes (added): defined further down; declared here because the file
 * executors below snapshot before they overwrite. */
static esp_err_t cap_files_snapshot(const char *resolved_path,
                                    char *out_version_path, size_t out_size);

/* Line-numbered, paged file reader (added): returns a window [offset, offset+limit)
 * of lines with "N: " prefixes, plus a continuation hint when the window ends
 * before EOF. Replaces the previous whole-file byte dump. */
static esp_err_t cap_files_read_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *j_offset = NULL;
    cJSON *j_limit = NULL;
    const char *path = NULL;
    char resolved_path[256];
    struct stat st = {0};
    FILE *file = NULL;
    char *linebuf = NULL;
    uint32_t offset = 0;
    uint32_t limit = 0;
    uint32_t total_lines = 0;
    bool count_capped = false;
    uint32_t lineno = 0;
    uint32_t emitted = 0;
    size_t used = 0;
    bool window_closed = false;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }

    j_offset = cJSON_GetObjectItem(root, "offset");
    j_limit = cJSON_GetObjectItem(root, "limit");
    if (cJSON_IsNumber(j_offset) && j_offset->valueint > 0) {
        offset = (uint32_t)j_offset->valueint;
    }
    if (cJSON_IsNumber(j_limit) && j_limit->valueint > 0) {
        limit = (uint32_t)j_limit->valueint;
    }

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "stat %s: errno=%d", resolved_path, errno);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    file = fopen(resolved_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "fopen %s: errno=%d", resolved_path, errno);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    linebuf = malloc(CAP_FILES_READ_LINE_MAX_BYTES + 1);
    if (!linebuf) {
        fclose(file);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    /* Single pass: emit the [offset, offset+limit) window, then keep counting
     * lines to EOF so the hint can report the total. A second pass is avoided
     * because rewind() on this VFS does not reliably reset an at-EOF stream. */
    output[0] = '\0';
    while (fgets(linebuf, CAP_FILES_READ_LINE_MAX_BYTES + 1, file)) {
        size_t len = strlen(linebuf);
        bool has_nl = (len > 0 && linebuf[len - 1] == '\n');
        bool line_truncated = (!has_nl && !feof(file));
        size_t body_len = len - (has_nl ? 1u : 0u);

        if (!window_closed && lineno >= offset) {
            if (used + CAP_FILES_READ_HINT_RESERVE >= output_size) {
                window_closed = true;
            } else {
                cap_files_emit_line(output, output_size, &used, lineno + 1, linebuf, body_len, line_truncated);
                emitted++;
                if (limit != 0 && emitted >= limit) {
                    window_closed = true;
                }
            }
        }

        if (line_truncated) {
            cap_files_drain_line(file);
        }
        lineno++;
        if (window_closed && lineno >= CAP_FILES_READ_COUNT_SCAN_MAX_LINES) {
            count_capped = true;
            break;
        }
    }
    total_lines = lineno;

    free(linebuf);
    fclose(file);
    cJSON_Delete(root);

    if (emitted == 0) {
        if (!count_capped && offset >= total_lines) {
            snprintf(output, output_size, "[read_file: offset %u is beyond end of file (%u lines)]",
                     (unsigned)offset, (unsigned)total_lines);
        } else if (offset > 0) {
            snprintf(output, output_size, "[read_file: no lines at offset %u; file may be shorter]",
                     (unsigned)offset);
        } else {
            snprintf(output, output_size, "[read_file: file is empty]");
        }
        return ESP_OK;
    }

    if (window_closed && (count_capped || offset + emitted < total_lines)) {
        uint32_t first = offset + 1;
        uint32_t last = offset + emitted;
        size_t tail = used;

        if (tail < output_size - 1) {
            if (count_capped) {
                snprintf(output + tail, output_size - tail,
                         "\n[read_file: showing lines %u-%u; call read_file with offset=%u to continue]",
                         (unsigned)first, (unsigned)last, (unsigned)last);
            } else {
                snprintf(output + tail, output_size - tail,
                         "\n[read_file: showing lines %u-%u of %u; call read_file with offset=%u to continue]",
                         (unsigned)first, (unsigned)last, (unsigned)total_lines, (unsigned)last);
            }
        }
    }

    return ESP_OK;
}


static esp_err_t cap_files_write_file_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *content = NULL;
    char resolved_path[256];
    char saved_version[CAP_FILES_VERSION_PATH_MAX] = {0};
    FILE *file = NULL;
    size_t content_len;
    size_t written;
    bool had_previous = false;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing content");
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_files_path_is_writable(resolved_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only path: %s", resolved_path);
        return ESP_ERR_INVALID_ARG;
    }

    if (cap_files_ensure_parent_dirs(resolved_path) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to create parent directories for %s", resolved_path);
        return ESP_FAIL;
    }

    /* Snapshot whatever is being replaced, and say so in the result. */
    had_previous = (cap_files_snapshot(resolved_path, saved_version, sizeof(saved_version)) == ESP_OK);

    file = fopen(resolved_path, "wb");
    if (!file) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open file for writing: %s", resolved_path);
        return ESP_FAIL;
    }

    content_len = strlen(content);
    written = fwrite(content, 1, content_len, file);
    fclose(file);
    cJSON_Delete(root);

    if (written != content_len) {
        ESP_LOGE(TAG, "fwrite %s: %d/%d bytes, errno=%d", resolved_path, (int)written, (int)content_len, errno);
        snprintf(output, output_size, "Error: wrote %d of %d bytes to %s",
                 (int)written,
                 (int)content_len,
                 resolved_path);
        return ESP_FAIL;
    }

    if (had_previous) {
        snprintf(output, output_size, "OK: wrote %d bytes to %s (previous version saved: %s)",
                 (int)written, resolved_path, saved_version);
    } else {
        snprintf(output, output_size, "OK: wrote %d bytes to %s (new file)", (int)written, resolved_path);
    }
    cap_files_append_size_hint(output, output_size, resolved_path, (size_t)written);
    return ESP_OK;
}

/* Count non-overlapping occurrences of needle (which must be non-empty). */
static size_t cap_files_count_matches(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t count = 0;
    const char *cursor = haystack;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_len;
    }
    return count;
}

/* Replace the first occurrence of old_text with new_text, returning a newly
 * allocated string, or NULL on allocation failure. */
static char *cap_files_replace_first(const char *src, const char *old_text, const char *new_text)
{
    const char *hit = strstr(src, old_text);
    size_t src_len = strlen(src);
    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t pre_len;
    size_t total;
    char *result;

    if (!hit) {
        return NULL;
    }
    pre_len = (size_t)(hit - src);
    total = src_len - old_len + new_len;

    result = malloc(total + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, src, pre_len);
    memcpy(result + pre_len, new_text, new_len);
    memcpy(result + pre_len + new_len, hit + old_len, src_len - pre_len - old_len);
    result[total] = '\0';
    return result;
}

/* edit_file (added): anchored old_text -> new_text replacement on an existing
 * file. Prefer this over write_file for edits: the model emits only the changed
 * span instead of re-emitting the whole file. Each old_text must match exactly
 * once so an ambiguous anchor cannot corrupt the file. Writes via a temp file +
 * rename so a failed write never leaves a truncated file behind. */
static esp_err_t cap_files_edit_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *edits = NULL;
    cJSON *item = NULL;
    const char *path = NULL;
    char resolved_path[256];
    char tmp_path[264];
    char saved_version[CAP_FILES_VERSION_PATH_MAX] = {0};
    struct stat st = {0};
    FILE *file = NULL;
    char *content = NULL;
    size_t content_len = 0;
    size_t applied = 0;
    esp_err_t result = ESP_OK;
    bool had_previous = false;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }

    edits = cJSON_GetObjectItem(root, "edits");
    if (!cJSON_IsArray(edits) || cJSON_GetArraySize(edits) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'edits' must be a non-empty array of {old_text,new_text}");
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_files_path_is_writable(resolved_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only path: %s", resolved_path);
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: file not found: %s (edit_file only edits existing files; use write_file for new files)",
                 resolved_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size > CAP_FILES_EDIT_MAX_FILE_SIZE) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: file too large to edit (%ld bytes > %d); use read_file with offset/limit and edit in smaller files",
                 (long)st.st_size, CAP_FILES_EDIT_MAX_FILE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(resolved_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "fopen %s: errno=%d", resolved_path, errno);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open file: %s", resolved_path);
        return ESP_FAIL;
    }
    content_len = (size_t)st.st_size;
    content = malloc(content_len + 1);
    if (!content) {
        fclose(file);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (content_len > 0 && fread(content, 1, content_len, file) != content_len) {
        ESP_LOGE(TAG, "fread %s: errno=%d", resolved_path, errno);
        fclose(file);
        free(content);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to read file: %s", resolved_path);
        return ESP_FAIL;
    }
    content[content_len] = '\0';
    fclose(file);

    cJSON_ArrayForEach(item, edits) {
        const char *old_text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "old_text"));
        const char *new_text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "new_text"));
        size_t matches;
        char *next;

        applied++;  /* 1-based index of the edit being applied */
        if (!old_text || !old_text[0] || !new_text) {
            snprintf(output, output_size,
                     "Error: edit #%u is missing old_text/new_text (old_text must be non-empty)",
                     (unsigned)applied);
            result = ESP_ERR_INVALID_ARG;
            goto done;
        }
        matches = cap_files_count_matches(content, old_text);
        if (matches == 0) {
            snprintf(output, output_size,
                     "Error: edit #%u old_text not found; use read_file to see the exact current text",
                     (unsigned)applied);
            result = ESP_ERR_NOT_FOUND;
            goto done;
        }
        if (matches > 1) {
            snprintf(output, output_size,
                     "Error: edit #%u old_text matches %u locations; include more surrounding context to make it unique",
                     (unsigned)applied, (unsigned)matches);
            result = ESP_ERR_INVALID_ARG;
            goto done;
        }
        next = cap_files_replace_first(content, old_text, new_text);
        if (!next) {
            snprintf(output, output_size, "Error: out of memory applying edit #%u", (unsigned)applied);
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        free(content);
        content = next;
        content_len = strlen(content);
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s%s", resolved_path, CAP_FILES_EDIT_TMP_SUFFIX) >= (int)sizeof(tmp_path)) {
        snprintf(output, output_size, "Error: path too long: %s", resolved_path);
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    /* Snapshot the pre-edit content and report it, so a bad edit is recoverable
     * without the model first hunting for a backup. */
    had_previous = (cap_files_snapshot(resolved_path, saved_version, sizeof(saved_version)) == ESP_OK);

    file = fopen(tmp_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "fopen %s: errno=%d", tmp_path, errno);
        snprintf(output, output_size, "Error: cannot open temp file for writing: %s", tmp_path);
        result = ESP_FAIL;
        goto done;
    }
    if (content_len > 0 && fwrite(content, 1, content_len, file) != content_len) {
        ESP_LOGE(TAG, "fwrite %s: errno=%d", tmp_path, errno);
        fclose(file);
        unlink(tmp_path);
        snprintf(output, output_size, "Error: failed to write %s", tmp_path);
        result = ESP_FAIL;
        goto done;
    }
    fclose(file);

    if (rename(tmp_path, resolved_path) != 0) {
        int saved_errno = errno;
        /* FATFS rename() fails with EEXIST when the destination exists; fall back
         * to unlink + rename. Loses atomicity but still never leaves a partial file. */
        if (saved_errno == EEXIST && unlink(resolved_path) == 0 && rename(tmp_path, resolved_path) == 0) {
            /* replaced via unlink + rename */
        } else {
            ESP_LOGE(TAG, "rename %s -> %s: errno=%d", tmp_path, resolved_path, saved_errno);
            unlink(tmp_path);
            snprintf(output, output_size, "Error: failed to replace %s", resolved_path);
            result = ESP_FAIL;
            goto done;
        }
    }

    if (had_previous) {
        snprintf(output, output_size, "OK: replaced %u edit(s) in %s (previous version saved: %s)",
                 (unsigned)applied, resolved_path, saved_version);
    } else {
        snprintf(output, output_size, "OK: replaced %u edit(s) in %s", (unsigned)applied, resolved_path);
    }
    cap_files_append_size_hint(output, output_size, resolved_path, strlen(content));

done:
    free(content);
    cJSON_Delete(root);
    return result;
}

static esp_err_t cap_files_delete_file_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[256];
    struct stat st = {0};

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (!cap_files_path_is_writable(resolved_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only path: %s", resolved_path);
        return ESP_ERR_INVALID_ARG;
    }

    if (unlink(resolved_path) != 0) {
        ESP_LOGE(TAG, "unlink %s: errno=%d", resolved_path, errno);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to delete file: %s", resolved_path);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "OK: deleted %s", resolved_path);
    return ESP_OK;
}

static esp_err_t cap_files_copy_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *src_path = NULL;
    const char *dst_path = NULL;
    char resolved_src_path[256];
    char resolved_dst_path[256];
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    src_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "src_path"));
    dst_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dst_path"));
    if (cap_files_resolve_path(src_path, resolved_src_path, sizeof(resolved_src_path)) != ESP_OK
        || cap_files_resolve_path(dst_path, resolved_dst_path, sizeof(resolved_dst_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be absolute paths within allowed directories");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(resolved_src_path, resolved_dst_path) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be different");
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_files_path_is_writable(resolved_dst_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only destination: %s", resolved_dst_path);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_files_copy_file_internal(resolved_src_path, resolved_dst_path);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: file not found: %s", resolved_src_path);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to copy %s to %s", resolved_src_path, resolved_dst_path);
        return err;
    }

    snprintf(output, output_size, "OK: copied %s to %s", resolved_src_path, resolved_dst_path);
    return ESP_OK;
}

static esp_err_t cap_files_move_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *src_path = NULL;
    const char *dst_path = NULL;
    char resolved_src_path[256];
    char resolved_dst_path[256];
    struct stat st = {0};

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    src_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "src_path"));
    dst_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dst_path"));
    if (cap_files_resolve_path(src_path, resolved_src_path, sizeof(resolved_src_path)) != ESP_OK
        || cap_files_resolve_path(dst_path, resolved_dst_path, sizeof(resolved_dst_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be absolute paths within allowed directories");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(resolved_src_path, resolved_dst_path) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be different");
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(resolved_src_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_src_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Move deletes the source, so both ends must be writable. To pull a file out
     * of a read-only root (e.g. /system), use copy_file instead. */
    if (!cap_files_path_is_writable(resolved_src_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only source: %s", resolved_src_path);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_files_path_is_writable(resolved_dst_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only destination: %s", resolved_dst_path);
        return ESP_ERR_INVALID_ARG;
    }

    if (cap_files_ensure_parent_dirs(resolved_dst_path) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to create parent directories for %s", resolved_dst_path);
        return ESP_FAIL;
    }

    if (rename(resolved_src_path, resolved_dst_path) != 0) {
        esp_err_t err;

        /* rename() across mount points is not supported; fall back to copy+delete */
        ESP_LOGW(TAG, "rename errno=%d, fallback copy: %s -> %s", errno, resolved_src_path, resolved_dst_path);
        err = cap_files_copy_file_internal(resolved_src_path, resolved_dst_path);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "copy failed: %s -> %s", resolved_src_path, resolved_dst_path);
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: failed to move %s to %s", resolved_src_path, resolved_dst_path);
            return err;
        }
        if (unlink(resolved_src_path) != 0) {
            ESP_LOGE(TAG, "unlink src %s: errno=%d", resolved_src_path, errno);
            unlink(resolved_dst_path);
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: failed to remove source after moving: %s", resolved_src_path);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "OK: moved %s to %s", resolved_src_path, resolved_dst_path);
    return ESP_OK;
}

static esp_err_t cap_files_list_dir_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *keyword = NULL;
    size_t offset = 0;
    int count = 0;

    (void)ctx;

    output[0] = '\0';
    root = cJSON_Parse(input_json);
    if (root) {
        keyword = cJSON_GetStringValue(cJSON_GetObjectItem(root, "keyword"));
    }

    /* List every configured root in turn. A writable root may not exist yet on
     * a fresh device, so create it first; read-only roots are mounted already. */
    for (size_t i = 0; i < CAP_FILES_ROOT_SPEC_COUNT; i++) {
        const char *base = claw_paths_get(s_root_specs[i].key);
        esp_err_t   err;

        if (!base) {
            continue;
        }
        if (s_root_specs[i].writable && cap_files_ensure_dir(base) != ESP_OK) {
            ESP_LOGW(TAG, "cannot open %s, skipping", base);
            continue;
        }

        err = cap_files_list_recursive(base, keyword, output, output_size, &offset, &count);
        if (err == ESP_ERR_INVALID_SIZE) {
            break;  /* Output buffer is full; stop walking further roots. */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "list %s: err=0x%x, skipping", base, err);
        }
    }

    cJSON_Delete(root);

    if (count == 0) {
        snprintf(output, output_size, "(no files found)");
    }
    return ESP_OK;
}

/* Case-insensitive bounded substring test (no copy of the needle). */
static bool cap_files_grep_ci_contains(const char *haystack, const char *needle, size_t needle_len)
{
    if (!haystack || needle_len == 0) {
        return false;
    }
    for (const char *cursor = haystack; *cursor; cursor++) {
        size_t i;

        for (i = 0; i < needle_len; i++) {
            unsigned char hc = (unsigned char)cursor[i];
            unsigned char nc = (unsigned char)needle[i];

            if (hc == '\0') {
                return false;
            }
            if (hc >= 'A' && hc <= 'Z') {
                hc = (unsigned char)(hc - 'A' + 'a');
            }
            if (nc >= 'A' && nc <= 'Z') {
                nc = (unsigned char)(nc - 'A' + 'a');
            }
            if (hc != nc) {
                break;
            }
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

/* A '|' in the pattern means "any of these alternatives" - the grep convention
 * the model expects. There is no regex engine on this platform, so this is the
 * only metacharacter; everything else is matched literally. */
static bool cap_files_grep_match(const char *line, const char *pattern, bool case_sensitive)
{
    const char *alt = pattern;

    if (!line || !pattern || !pattern[0]) {
        return false;
    }
    while (alt && *alt) {
        const char *bar = strchr(alt, '|');
        size_t alt_len = bar ? (size_t)(bar - alt) : strlen(alt);
        bool hit = false;

        if (alt_len > 0) {
            if (case_sensitive) {
                for (const char *cursor = line; *cursor; cursor++) {
                    if (strncmp(cursor, alt, alt_len) == 0) {
                        hit = true;
                        break;
                    }
                }
            } else {
                hit = cap_files_grep_ci_contains(line, alt, alt_len);
            }
        }
        if (hit) {
            return true;
        }
        alt = bar ? bar + 1 : NULL;
    }
    return false;
}

/* Emit one "path:lineno: text" hit, trimming the line ending. */
static void cap_files_grep_emit(char *output, size_t output_size, size_t *used,
                                const char *path, unsigned lineno, const char *line)
{
    size_t len = strlen(line);
    int written;

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len > CAP_FILES_GREP_LINE_MAX_BYTES) {
        len = CAP_FILES_GREP_LINE_MAX_BYTES;
    }

    written = snprintf(output + *used, output_size - *used, "%s:%u: %.*s\n",
                       path, lineno, (int)len, line);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= output_size - *used) {
        *used = output_size - 1;
        return;
    }
    *used += (size_t)written;
    output[*used] = '\0';
}

static esp_err_t cap_files_grep_file(const char *file_path, const char *pattern, bool case_sensitive,
                                     char *output, size_t output_size, size_t *used,
                                     size_t *match_count, size_t max_results, bool *stopped)
{
    FILE *file = NULL;
    char *line = NULL;
    unsigned lineno = 0;

    file = fopen(file_path, "rb");
    if (!file) {
        return ESP_OK;  /* unreadable file: skip it rather than fail the search */
    }
    line = malloc(CAP_FILES_GREP_LINE_MAX_BYTES + 1);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    while (fgets(line, CAP_FILES_GREP_LINE_MAX_BYTES + 1, file)) {
        size_t len = strlen(line);
        bool has_nl = (len > 0 && line[len - 1] == '\n');

        lineno++;
        if (cap_files_grep_match(line, pattern, case_sensitive)) {
            cap_files_grep_emit(output, output_size, used, file_path, lineno, line);
            (*match_count)++;
            if (*match_count >= max_results) {
                *stopped = true;
                break;
            }
        }
        if (!has_nl && !feof(file)) {
            cap_files_drain_line(file);  /* long line: skip the remainder */
        }
    }

    free(line);
    fclose(file);
    return ESP_OK;
}

static esp_err_t cap_files_grep_recursive(const char *dir_path, const char *pattern,
                                          const char *file_filter, bool case_sensitive,
                                          char *output, size_t output_size, size_t *used,
                                          size_t *match_count, size_t max_results, bool *stopped)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    dir = opendir(dir_path);
    if (!dir) {
        return ESP_OK;
    }

    while (!*stopped && (entry = readdir(dir)) != NULL) {
        char full_path[256];
        struct stat st = {0};
        esp_err_t err;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        /* Skip dot-prefixed entries: also keeps internal dirs (.toolout,
         * .versions) out of search results. */
        if (cap_files_is_hidden_name(entry->d_name)) {
            continue;
        }
        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(full_path)) {
            continue;
        }
        if (!cap_files_path_is_valid(full_path) || stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            err = cap_files_grep_recursive(full_path, pattern, file_filter, case_sensitive,
                                           output, output_size, used, match_count, max_results, stopped);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (file_filter && file_filter[0] && !cap_files_text_contains_ci(entry->d_name, file_filter)) {
            continue;
        }
        if (st.st_size > CAP_FILES_GREP_MAX_FILE_SIZE) {
            continue;
        }

        err = cap_files_grep_file(full_path, pattern, case_sensitive,
                                  output, output_size, used, match_count, max_results, stopped);
        if (err != ESP_OK) {
            closedir(dir);
            return err;
        }
    }

    closedir(dir);
    return ESP_OK;
}

/* grep_files (added): search file contents for a plain-text pattern. */
static esp_err_t cap_files_grep_files_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    cJSON *cs = NULL;
    cJSON *mr = NULL;
    const char *path = NULL;
    const char *pattern = NULL;
    const char *file_filter = NULL;
    char resolved_path[256];
    /* pattern/file_filter point into the parsed cJSON tree, which is deleted
     * before the tail of this function still echoes the pattern; copy them so
     * the echo cannot read freed memory (this produced garbled output). */
    char pattern_copy[160] = {0};
    char filter_copy[96] = {0};
    struct stat st = {0};
    bool case_sensitive = false;
    size_t max_results = CAP_FILES_GREP_DEFAULT_MAX_RESULTS;
    size_t used = 0;
    size_t matches = 0;
    bool stopped = false;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    pattern = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pattern"));
    file_filter = cJSON_GetStringValue(cJSON_GetObjectItem(root, "file_filter"));

    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pattern || !pattern[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing pattern");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(pattern_copy, pattern, sizeof(pattern_copy));
    pattern = pattern_copy;
    if (file_filter && file_filter[0]) {
        strlcpy(filter_copy, file_filter, sizeof(filter_copy));
        file_filter = filter_copy;
    }

    cs = cJSON_GetObjectItem(root, "case_sensitive");
    case_sensitive = cJSON_IsTrue(cs);
    mr = cJSON_GetObjectItem(root, "max_results");
    if (cJSON_IsNumber(mr) && mr->valueint > 0) {
        max_results = (size_t)mr->valueint;
        if (max_results > CAP_FILES_GREP_MAX_RESULTS_LIMIT) {
            max_results = CAP_FILES_GREP_MAX_RESULTS_LIMIT;
        }
    }

    if (stat(resolved_path, &st) != 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    output[0] = '\0';
    if (S_ISDIR(st.st_mode)) {
        err = cap_files_grep_recursive(resolved_path, pattern, file_filter, case_sensitive,
                                       output, output_size, &used, &matches, max_results, &stopped);
    } else if (S_ISREG(st.st_mode)) {
        err = cap_files_grep_file(resolved_path, pattern, case_sensitive,
                                  output, output_size, &used, &matches, max_results, &stopped);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: grep failed: %s", esp_err_to_name(err));
        return err;
    }
    if (matches == 0) {
        snprintf(output, output_size, "(no matches for \"%s\" under %s)", pattern, resolved_path);
        return ESP_OK;
    }
    if (used < output_size - 1) {
        snprintf(output + used, output_size - used,
                 "[grep_files: %u match(es)%s]",
                 (unsigned)matches,
                 stopped ? "; stopped at the result limit - narrow the pattern or raise max_results" : "");
    }
    return ESP_OK;
}

/* versioned writes (added): see the defines at the top of this file. */
/* "<data_root>/.versions" + the file's own absolute path (which already starts
 * with the data root), so versions mirror the tree and are human-readable. */
static esp_err_t cap_files_versions_dir_for(const char *resolved_path, char *dir, size_t dir_size)
{
    const char *data_root = claw_paths_get(CLAW_PATH_DATA);
    int written;

    if (!data_root) {
        return ESP_ERR_INVALID_STATE;
    }
    written = snprintf(dir, dir_size, "%s/%s%s", data_root, CAP_FILES_VERSIONS_DIRNAME, resolved_path);
    if (written <= 0 || (size_t)written >= dir_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t cap_files_ensure_dir_recursive(const char *dir)
{
    char probe[CAP_FILES_VERSION_PATH_MAX];

    if (snprintf(probe, sizeof(probe), "%s/v", dir) >= (int)sizeof(probe)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return cap_files_ensure_parent_dirs(probe);
}

/* Delete the lowest-numbered version in dir. Returns true if one was removed. */
static bool cap_files_delete_lowest_version(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    unsigned lowest = 0;
    bool found = false;

    if (!d) {
        return false;
    }
    while ((e = readdir(d)) != NULL) {
        char *end = NULL;
        unsigned long v = strtoul(e->d_name, &end, 10);

        if (end == e->d_name || *end != '\0') {
            continue;
        }
        if (!found || v < lowest) {
            lowest = (unsigned)v;
            found = true;
        }
    }
    closedir(d);
    if (!found) {
        return false;
    }

    {
        char victim[CAP_FILES_VERSION_PATH_MAX];

        if (snprintf(victim, sizeof(victim), "%s/%u", dir, lowest) >= (int)sizeof(victim)) {
            return false;
        }
        return unlink(victim) == 0;
    }
}

/* Snapshot the current content of resolved_path as the next version. Returns
 * ESP_ERR_NOT_FOUND when the file does not exist yet (nothing to snapshot). */
static esp_err_t cap_files_snapshot(const char *resolved_path,
                                    char *out_version_path, size_t out_size)
{
    char dir[CAP_FILES_VERSION_PATH_MAX];
    struct stat st = {0};
    DIR *d = NULL;
    struct dirent *e;
    unsigned max_v = 0;
    unsigned count = 0;
    FILE *in = NULL;
    FILE *out = NULL;
    uint8_t buf[512];
    size_t n;
    esp_err_t err = ESP_OK;

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (cap_files_versions_dir_for(resolved_path, dir, sizeof(dir)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (cap_files_ensure_dir_recursive(dir) != ESP_OK) {
        return ESP_FAIL;
    }

    d = opendir(dir);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char *end = NULL;
            unsigned long v = strtoul(e->d_name, &end, 10);

            if (end == e->d_name || *end != '\0') {
                continue;
            }
            count++;
            if (v > max_v) {
                max_v = (unsigned)v;
            }
        }
        closedir(d);
    }
    if (snprintf(out_version_path, out_size, "%s/%u", dir, max_v + 1) >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    in = fopen(resolved_path, "rb");
    if (!in) {
        return ESP_FAIL;
    }
    out = fopen(out_version_path, "wb");
    if (!out) {
        fclose(in);
        return ESP_FAIL;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            err = ESP_FAIL;
            break;
        }
    }
    if (ferror(in)) {
        err = ESP_FAIL;
    }
    fclose(in);
    fclose(out);
    if (err != ESP_OK) {
        unlink(out_version_path);
        return err;
    }

    count++;  /* the version just written */
    while (count > CAP_FILES_VERSIONS_KEEP) {
        if (!cap_files_delete_lowest_version(dir)) {
            break;
        }
        count--;
    }
    return ESP_OK;
}

/* restore_file (added): put a snapshotted version back, snapshotting the current
 * content first so the restore itself is undoable. */
static esp_err_t cap_files_restore_file_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    cJSON *jv = NULL;
    const char *path = NULL;
    char resolved_path[256];
    char dir[CAP_FILES_VERSION_PATH_MAX];
    char version_path[CAP_FILES_VERSION_PATH_MAX];
    char list[160];
    struct stat st = {0};
    DIR *d = NULL;
    struct dirent *e;
    unsigned want = 0;
    unsigned max_v = 0;
    bool found_want = false;
    size_t list_used = 0;
    FILE *in = NULL;
    FILE *out = NULL;
    uint8_t buf[512];
    size_t n;
    esp_err_t err = ESP_OK;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    jv = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(jv) && jv->valueint > 0) {
        want = (unsigned)jv->valueint;
    }
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must be an absolute path within an allowed directory");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_files_path_is_writable(resolved_path)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: read-only path: %s", resolved_path);
        return ESP_ERR_INVALID_ARG;
    }
    if (cap_files_versions_dir_for(resolved_path, dir, sizeof(dir)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path too long for versioning");
        return ESP_ERR_INVALID_SIZE;
    }

    list[0] = '\0';
    d = opendir(dir);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char *end = NULL;
            unsigned long v = strtoul(e->d_name, &end, 10);

            if (end == e->d_name || *end != '\0') {
                continue;
            }
            if (v > max_v) {
                max_v = (unsigned)v;
            }
            if (want != 0 && v == want) {
                found_want = true;
            }
            if (list_used + 12 < sizeof(list)) {
                list_used += (size_t)snprintf(list + list_used, sizeof(list) - list_used, "%lu,", v);
            }
        }
        closedir(d);
    }
    if (max_v == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: no versions saved for %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (want == 0) {
        want = max_v;  /* default: the latest version */
    } else if (!found_want) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: no version %u for %s; available: %s",
                 want, resolved_path, list);
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_Delete(root);

    if (snprintf(version_path, sizeof(version_path), "%s/%u", dir, want) >= (int)sizeof(version_path)) {
        snprintf(output, output_size, "Error: version path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(version_path, &st) != 0) {
        snprintf(output, output_size, "Error: version file missing: %s", version_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Keep the current content recoverable too (ignore when the file is absent).
     * Uses its own buffer: the snapshot call writes the new version's path, which
     * must not clobber version_path (that is the version being restored). */
    {
        char discard_version[CAP_FILES_VERSION_PATH_MAX];

        (void)cap_files_snapshot(resolved_path, discard_version, sizeof(discard_version));
    }

    in = fopen(version_path, "rb");
    if (!in) {
        snprintf(output, output_size, "Error: cannot open %s", version_path);
        return ESP_FAIL;
    }
    out = fopen(resolved_path, "wb");
    if (!out) {
        fclose(in);
        snprintf(output, output_size, "Error: cannot open %s for writing", resolved_path);
        return ESP_FAIL;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            err = ESP_FAIL;
            break;
        }
    }
    if (ferror(in)) {
        err = ESP_FAIL;
    }
    fclose(in);
    fclose(out);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to restore %s", resolved_path);
        return err;
    }

    snprintf(output, output_size, "OK: restored version %u of %s", want, resolved_path);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_files_descriptors[] = {
    {
        .id = "read_file",
        .name = "read_file",
        .family = "files",
        .description = "Read a text file as numbered lines. Read the whole file in one call whenever it fits; use offset/limit only to continue past a truncation or to re-check a known region.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under an allowed directory\"},\"offset\":{\"type\":\"integer\",\"description\":\"First line to show, 0-based (default 0)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return (default: as many as fit)\"}},\"required\":[\"path\"]}",
        .execute = cap_files_read_file_execute,
    },
    {
        .id = "grep_files",
        .name = "grep_files",
        .family = "files",
        .description = "Search file contents for plain text (no regex); returns path:line: text hits. Use | for alternatives, e.g. TIME_X|NET_Y. Locate code with this instead of reading whole files.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute file or directory path under an allowed directory\"},\"pattern\":{\"type\":\"string\",\"description\":\"Plain text to find (case-insensitive by default)\"},\"file_filter\":{\"type\":\"string\",\"description\":\"Optional substring to match against file names, e.g. .lua\"},\"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Default false\"},\"max_results\":{\"type\":\"integer\",\"description\":\"Default 100, max 200\"}},\"required\":[\"path\",\"pattern\"]}",
        .execute = cap_files_grep_files_execute,
    },
    {
        .id = "write_file",
        .name = "write_file",
        .family = "files",
        .description = "Create a new file, or fully overwrite one. For changing an existing file prefer edit_file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under a writable directory\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        .execute = cap_files_write_file_execute,
    },
    {
        .id = "edit_file",
        .name = "edit_file",
        .family = "files",
        .description = "Edit an existing file by replacing exact old_text with new_text. Each old_text must match exactly once. Prefer this over write_file for changes.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under a writable directory\"},\"edits\":{\"type\":\"array\",\"description\":\"Replacements to apply in order\",\"items\":{\"type\":\"object\",\"properties\":{\"old_text\":{\"type\":\"string\",\"description\":\"Exact existing text, must match exactly once\"},\"new_text\":{\"type\":\"string\",\"description\":\"Replacement text\"}},\"required\":[\"old_text\",\"new_text\"]}}},\"required\":[\"path\",\"edits\"]}",
        .execute = cap_files_edit_file_execute,
    },
    {
        .id = "restore_file",
        .name = "restore_file",
        .family = "files",
        .description = "Restore a previously saved version of a file. write_file and edit_file save the previous content automatically; omit version for the latest.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under a writable directory\"},\"version\":{\"type\":\"integer\",\"description\":\"Optional; default is the latest saved version\"}},\"required\":[\"path\"]}",
        .execute = cap_files_restore_file_execute,
    },
    {
        .id = "delete_file",
        .name = "delete_file",
        .family = "files",
        .description = "Delete a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under a writable directory\"}},\"required\":[\"path\"]}",
        .execute = cap_files_delete_file_execute,
    },
    {
        .id = "copy_file",
        .name = "copy_file",
        .family = "files",
        .description = "Copy a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"src_path\":{\"type\":\"string\",\"description\":\"Absolute source path\"},\"dst_path\":{\"type\":\"string\",\"description\":\"Absolute destination path under a writable directory\"}},\"required\":[\"src_path\",\"dst_path\"]}",
        .execute = cap_files_copy_file_execute,
    },
    {
        .id = "move_file",
        .name = "move_file",
        .family = "files",
        .description = "Move a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"src_path\":{\"type\":\"string\",\"description\":\"Absolute source path under a writable directory\"},\"dst_path\":{\"type\":\"string\",\"description\":\"Absolute destination path under a writable directory\"}},\"required\":[\"src_path\",\"dst_path\"]}",
        .execute = cap_files_move_file_execute,
    },
    {
        .id = "list_dir",
        .name = "list_dir",
        .family = "files",
        .description = "Recursively list files, optionally filtered by case-insensitive path keyword.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"keyword\":{\"type\":\"string\"}}}",
        .execute = cap_files_list_dir_execute,
    },
};

static const claw_cap_group_t s_files_group = {
    .group_id = "cap_files",
    .descriptors = s_files_descriptors,
    .descriptor_count = sizeof(s_files_descriptors) / sizeof(s_files_descriptors[0]),
};

esp_err_t cap_files_register_group(void)
{
    if (!cap_files_any_root_configured()) {
        ESP_LOGE(TAG, "no sandbox root configured in claw_paths");
        return ESP_ERR_INVALID_STATE;
    }
    if (claw_cap_group_exists(s_files_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_files_group);
}
