/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_agent_mgr_report.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "claw_paths.h"

static const char *TAG = "claw_agent_mgr_report";

/* Report spill (added, C1). Files are named "<admission_ms>_<safe>_<req>_<kind>.md"
 * so the leading field doubles as the prune timestamp, exactly like .toolout. */
#define CLAW_AGENT_MGR_REPORT_SUBDIR   ".reports"
#define CLAW_AGENT_MGR_REPORT_DIR_MAX  96
#define CLAW_AGENT_MGR_REPORT_SAFE_MAX 40
/* Sanitized copy of a full session id before its tail is kept. */
#define CLAW_AGENT_MGR_REPORT_FULL_MAX 272
/* Reports are the parent's durable evidence, so they must outlive the 10-minute
 * tool-output window: the parent may only read it many rounds later. */
#define CLAW_AGENT_MGR_REPORT_MAX_AGE_MS (60 * 60 * 1000ULL)

static void report_sanitize_tail(const char *session_id, char *out, size_t out_size)
{
    char full[CLAW_AGENT_MGR_REPORT_FULL_MAX];
    size_t j = 0;
    size_t len;

    for (size_t i = 0; session_id && session_id[i] && j + 1 < sizeof(full); i++) {
        char c = session_id[i];

        full[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    full[j] = '\0';

    /* Keep the TAIL of the id: subagent ids differ at the end (:subagent_07), and
     * truncating the head instead would make every report collide on the prefix. */
    len = strlen(full);
    const char *src = full;
    if (len >= out_size) {
        src = full + (len - (out_size - 1));
    }
    strlcpy(out, src, out_size);
}

/* Drop report files whose admission timestamp is older than the max age. Cheap
 * enough to run on each spill; a stat-per-file scan would not be. */
static void report_prune(const char *dir, uint64_t now_ms)
{
    DIR *d = opendir(dir);
    struct dirent *entry;

    if (!d) {
        return;
    }
    while ((entry = readdir(d)) != NULL) {
        char path[CLAW_AGENT_MGR_REPORT_DIR_MAX + 64];
        char *end = NULL;
        unsigned long long ts;

        if (entry->d_name[0] == '.') {
            continue;
        }
        ts = strtoull(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '_') {
            continue;
        }
        if (now_ms > ts && now_ms - ts > CLAW_AGENT_MGR_REPORT_MAX_AGE_MS &&
                snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)) {
            unlink(path);
        }
    }
    closedir(d);
}

esp_err_t claw_agent_mgr_report_spill(const char *session_id,
                                      uint32_t request_id,
                                      const char *kind,
                                      const char *body,
                                      char *out_path,
                                      size_t out_path_size)
{
    char dir[CLAW_AGENT_MGR_REPORT_DIR_MAX];
    char safe[CLAW_AGENT_MGR_REPORT_SAFE_MAX];
    struct stat st = {0};
    uint64_t now_ms;
    FILE *file;
    size_t body_len;

    if (!session_id || !session_id[0] || !kind || !kind[0] || !body || !out_path || out_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_paths_join(CLAW_PATH_DATA, CLAW_AGENT_MGR_REPORT_SUBDIR, dir, sizeof(dir)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stat(dir, &st) != 0) {
        /* Retry once: the SD on this board intermittently fails a write with a
         * timeout (measured: a plain mkdir returned an error twice and then
         * succeeded). Losing the report loses the parent's only evidence of how
         * the subagent ended, so a second attempt is worth 50 ms. */
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "report dir unavailable: %s (errno=%d)", dir, errno);
                return ESP_FAIL;
            }
        }
    }

    now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    report_prune(dir, now_ms);
    report_sanitize_tail(session_id, safe, sizeof(safe));

    if (snprintf(out_path, out_path_size, "%s/%llu_%s_%" PRIu32 "_%s.md",
                 dir, (unsigned long long)now_ms, safe, request_id, kind) >= (int)out_path_size) {
        out_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(out_path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "cannot open report file %s", out_path);
        out_path[0] = '\0';
        return ESP_FAIL;
    }
    body_len = strlen(body);
    if (body_len > 0 && fwrite(body, 1, body_len, file) != body_len) {
        fclose(file);
        unlink(out_path);
        out_path[0] = '\0';
        return ESP_FAIL;
    }
    fclose(file);
    ESP_LOGI(TAG, "spilled %u B %s to %s", (unsigned)body_len, kind, out_path);
    return ESP_OK;
}
