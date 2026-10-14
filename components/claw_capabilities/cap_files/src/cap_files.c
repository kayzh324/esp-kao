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
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "claw_utils_string.h"
#include "esp_log.h"

static const char *TAG = "cap_files";

#define CAP_FILES_MAX_FILE_SIZE (32 * 1024)

/* read_file paging (added): line-numbered output with offset/limit so the model
 * can page through large files instead of re-reading whole files every time.
 * A single physical line longer than the per-line cap is truncated (UTF-8 safe)
 * and marked, so one minified/base64 line cannot consume the whole output. */
#define CAP_FILES_READ_LINE_MAX_BYTES 2048
#define CAP_FILES_READ_HINT_RESERVE 256
#define CAP_FILES_READ_COUNT_SCAN_MAX_LINES 200000

/* edit_file (added): anchored old_text -> new_text replacement. Bounds: whole
 * file must fit the edit buffer; replacements must be unique per old_text. */
#define CAP_FILES_EDIT_MAX_FILE_SIZE (32 * 1024)
#define CAP_FILES_EDIT_TMP_SUFFIX ".tmp"

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
    FILE *file = NULL;
    size_t content_len;
    size_t written;

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

    snprintf(output, output_size, "OK: wrote %d bytes to %s", (int)written, resolved_path);
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
    struct stat st = {0};
    FILE *file = NULL;
    char *content = NULL;
    size_t content_len = 0;
    size_t applied = 0;
    esp_err_t result = ESP_OK;

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

    snprintf(output, output_size, "OK: replaced %u edit(s) in %s", (unsigned)applied, resolved_path);

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

static const claw_cap_descriptor_t s_files_descriptors[] = {
    {
        .id = "read_file",
        .name = "read_file",
        .family = "files",
        .description = "Read a text file as numbered lines. Use offset (0-based line) and limit (line count) to page through large files.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path under an allowed directory\"},\"offset\":{\"type\":\"integer\",\"description\":\"First line to show, 0-based (default 0)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to return (default: as many as fit)\"}},\"required\":[\"path\"]}",
        .execute = cap_files_read_file_execute,
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
