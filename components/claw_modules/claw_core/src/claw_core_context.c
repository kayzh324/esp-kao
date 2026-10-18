/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "claw_paths.h"
#include "claw_utils_string.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "claw_core";

void claw_core_free_cached_contexts(claw_core_cached_context_t *contexts, size_t count)
{
    size_t i;

    if (!contexts) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(contexts[i].content);
        contexts[i].content = NULL;
        contexts[i].valid = false;
    }
    free(contexts);
}

esp_err_t claw_core_append_user_message(cJSON *messages, const char *text)
{
    cJSON *user_msg = NULL;

    if (!messages || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    user_msg = cJSON_CreateObject();
    if (!user_msg) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(messages, user_msg);
    return ESP_OK;
}

static esp_err_t append_message_array_json(cJSON *messages, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!messages || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static esp_err_t append_message_array(cJSON *messages, const cJSON *items)
{
    const cJSON *item = NULL;

    if (!messages || !items || !cJSON_IsArray((cJSON *)items)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(item, items) {
        cJSON *dup = cJSON_Duplicate((cJSON *)item, true);

        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    return ESP_OK;
}

static esp_err_t append_tool_array_json(cJSON *tools, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!tools || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(tools, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static char *build_current_turn_prompt(const claw_core_request_t *request)
{
#define CLAW_CORE_TURN_PROMPT_FMT \
    "## Current Turn Context\n" \
    "- source_channel: %s\n" \
    "- source_chat_id: %s\n"
#define CLAW_CORE_TURN_PROMPT_ARGS(req) \
    (req)->source_channel ? (req)->source_channel : "(unknown)", \
    (req)->source_chat_id ? (req)->source_chat_id : "(unknown)"

    int needed;
    char *text = NULL;

    if (!request) {
        text = NULL;
        goto cleanup;
    }

    needed = snprintf(NULL, 0, CLAW_CORE_TURN_PROMPT_FMT, CLAW_CORE_TURN_PROMPT_ARGS(request));
    if (needed < 0) {
        ESP_LOGE(TAG, "failed to size current turn prompt");
        text = NULL;
        goto cleanup;
    }

    text = calloc(1, (size_t)needed + 1);
    if (!text) {
        goto cleanup;
    }

    if (snprintf(text, (size_t)needed + 1, CLAW_CORE_TURN_PROMPT_FMT, CLAW_CORE_TURN_PROMPT_ARGS(request)) < 0) {
        ESP_LOGE(TAG, "failed to build current turn prompt");
        free(text);
        text = NULL;
    }

cleanup:
#undef CLAW_CORE_TURN_PROMPT_ARGS
#undef CLAW_CORE_TURN_PROMPT_FMT
    return text;
}

static esp_err_t append_prompt_section(char **prompt,
                                       const char *section_name,
                                       const char *content)
{
    char *grown = NULL;
    size_t current_len;
    size_t extra_len;

    if (!prompt || !*prompt || !section_name || !content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    current_len = strlen(*prompt);
    extra_len = strlen("\n\n## \n") + strlen(section_name) + strlen(content);
    grown = realloc(*prompt, current_len + extra_len + 1);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    *prompt = grown;
    snprintf((*prompt) + current_len,
             extra_len + 1,
             "\n\n## %s\n%s",
             section_name,
             content);
    return ESP_OK;
}

esp_err_t claw_core_append_assistant_tool_calls(cJSON *messages,
                                                const claw_core_llm_response_t *response)
{
    cJSON *assistant = NULL;

    if (!messages || !response) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!response->raw_message_json || !response->raw_message_json[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    assistant = cJSON_ParseWithOpts(response->raw_message_json, NULL, 1);
    if (!assistant) {
        cJSON_Delete(assistant);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON_AddItemToArray(messages, assistant);
    return ESP_OK;
}

void claw_core_finish_from_plain_text(uint32_t request_id,
                                      const claw_core_llm_response_t *llm_response,
                                      claw_core_response_t *response)
{
    const char *text = (llm_response && llm_response->text) ? llm_response->text : "";

    response->completion_type = CLAW_CORE_COMPLETION_DONE;
    free(response->text);
    response->text = claw_utils_string_dup(text);
    free(response->error_message);
    response->error_message = NULL;

    ESP_LOGI(TAG, "completion request=%" PRIu32 " status=done raw=%.*s%s",
             request_id,
             claw_core_log_snippet_len(text),
             claw_core_log_snippet(text),
             claw_core_log_snippet_suffix(text));
}

/* bounded tool results (added): one tool result can be as large as the cap output
 * buffer. When it exceeds CLAW_TIER_TOOL_RESULT_MAX_BYTES the full text is written
 * under the writable DATA root and the model sees only a UTF-8-safe head plus the
 * file path, which it can re-read with read_file offset/limit. Bounds the
 * per-result context cost without discarding information.
 * Threshold and head size are per-tier (claw_tier.h). */
#define CLAW_CORE_TOOL_RESULT_SPILL_SUBDIR ".toolout"
#define CLAW_CORE_TOOL_RESULT_HINT_RESERVE 448
#define CLAW_CORE_TOOL_RESULT_SPILL_MAX_AGE_MS (10 * 60 * 1000ULL)
#define CLAW_CORE_TOOL_RESULT_DIR_MAX 96
#define CLAW_CORE_TOOL_RESULT_PATH_MAX 192

static void claw_core_toolout_sanitize(const char *name, char *out, size_t out_size)
{
    size_t j = 0;

    for (size_t i = 0; name && name[i] && j + 1 < out_size; i++) {
        char c = name[i];

        out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    out[j] = '\0';
    if (j == 0 && out_size > 4) {
        strlcpy(out, "tool", out_size);
    }
}

/* Remove spill files older than the max age; the leading name field is the
 * admission time in millis. Cheap enough to run on each spill. */
static void claw_core_toolout_prune(const char *dir, uint64_t now_ms)
{
    DIR *d = opendir(dir);
    struct dirent *entry;

    if (!d) {
        return;
    }
    while ((entry = readdir(d)) != NULL) {
        char path[CLAW_CORE_TOOL_RESULT_PATH_MAX];
        char *end = NULL;
        unsigned long long ts;

        if (entry->d_name[0] == '.') {
            continue;
        }
        ts = strtoull(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '_') {
            continue;
        }
        if (now_ms > ts && now_ms - ts > CLAW_CORE_TOOL_RESULT_SPILL_MAX_AGE_MS &&
                snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)) {
            unlink(path);
        }
    }
    closedir(d);
}

/* Takes ownership of tool_output; returns the model-visible string to use. */
static char *claw_core_bound_tool_output(const claw_core_request_t *request,
                                         const char *tool_name,
                                         char *tool_output)
{
    char dir[CLAW_CORE_TOOL_RESULT_DIR_MAX];
    char path[CLAW_CORE_TOOL_RESULT_PATH_MAX];
    char safe_name[40];
    char *bounded = NULL;
    struct stat st = {0};
    uint64_t now_ms;
    size_t len;
    size_t head;
    FILE *file;

    if (!tool_output) {
        return NULL;
    }
    len = strlen(tool_output);
    if (len <= CLAW_TIER_TOOL_RESULT_MAX_BYTES) {
        return tool_output;
    }

    /* tool-result spill: do NOT spill results that are meant to be consumed
     * inline. read_file already pages its own output, and activate_skill exists
     * precisely to hand the model the skill document. More importantly, a spilled
     * read_file result is re-read by the model with read_file, whose result would
     * spill again - an unbounded read/spill/read loop (observed on device: the
     * agent re-reading the spill directory and thrashing). */
    if (tool_name && (strcmp(tool_name, "read_file") == 0 ||
                      strcmp(tool_name, "activate_skill") == 0)) {
        return tool_output;
    }

    if (claw_paths_join(CLAW_PATH_DATA, CLAW_CORE_TOOL_RESULT_SPILL_SUBDIR, dir, sizeof(dir)) != ESP_OK) {
        return tool_output;
    }
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "tool-out spill dir unavailable: %s", dir);
        return tool_output;
    }

    now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    claw_core_toolout_prune(dir, now_ms);
    claw_core_toolout_sanitize(tool_name, safe_name, sizeof(safe_name));
    if (snprintf(path, sizeof(path), "%s/%llu_%" PRIu32 "_%s.txt",
                 dir, (unsigned long long)now_ms, request ? request->request_id : 0U, safe_name)
            >= (int)sizeof(path)) {
        return tool_output;
    }

    file = fopen(path, "wb");
    if (!file) {
        return tool_output;
    }
    if (fwrite(tool_output, 1, len, file) != len) {
        fclose(file);
        unlink(path);
        return tool_output;
    }
    fclose(file);

    head = claw_utils_utf8_prefix_len(tool_output, CLAW_TIER_TOOL_RESULT_HEAD_BYTES);
    bounded = malloc(head + CLAW_CORE_TOOL_RESULT_HINT_RESERVE + 1);
    if (!bounded) {
        return tool_output;
    }
    memcpy(bounded, tool_output, head);
    bounded[head] = '\0';
    snprintf(bounded + head,
             CLAW_CORE_TOOL_RESULT_HINT_RESERVE + 1,
             "\n[tool output truncated: showing first %u of %u bytes; full output saved to %s; "
             "use read_file with offset/limit to read the rest]",
             (unsigned)head, (unsigned)len, path);
    free(tool_output);
    return bounded;
}

/* in-turn window (added): runtime_messages accumulates every tool round and the
 * whole array is re-sent on each iteration, so a long turn grows the request body
 * without bound. Trim ONLY when the accumulated bytes exceed a budget, then shrink
 * the oldest rounds just enough to get back under it; the newest rounds are never
 * touched. Deliberately conservative: dropping recent in-turn detail makes the
 * model redo work it already did. Full content stays in the session.
 * Budget/keep/arg limits are per-tier (claw_tier.h). */
/* The note must name an action the model can take. "Full content is in the session
 * history" was true but unusable - session logs are not task input - and it sent one
 * real run hunting for content it could not recover. */
#define CLAW_CORE_RUNTIME_OMITTED_NOTE \
    "[older tool result omitted to bound context - run the same tool call again if you still need it]"

static bool claw_core_message_has_tool_calls(const cJSON *message)
{
    const cJSON *tool_calls;

    if (!cJSON_IsObject(message)) {
        return false;
    }
    tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    return cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0;
}

/* Approximate the bytes a message contributes to the request body.
 * reasoning_content is included (added): it is re-sent verbatim on every round
 * like any other field, and on a measured device task it was 28% of the replay.
 * It used to be invisible to the budget below, so the window could believe it
 * had trimmed while the body kept growing. */
static size_t claw_core_message_bytes(const cJSON *message)
{
    const cJSON *content;
    const cJSON *reasoning;
    const cJSON *tool_calls;
    const cJSON *call = NULL;
    size_t total = 0;

    if (!cJSON_IsObject(message)) {
        return 0;
    }
    content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        total += strlen(content->valuestring);
    }
    reasoning = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
    if (cJSON_IsString(reasoning) && reasoning->valuestring) {
        total += strlen(reasoning->valuestring);
    }
    tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    cJSON_ArrayForEach(call, tool_calls) {
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;

        if (cJSON_IsString(args) && args->valuestring) {
            total += strlen(args->valuestring);
        }
    }
    return total;
}

/* Which tool call produced this tool result, and with what path? Needed so the
 * omission note can name a place the model can ACTUALLY go back to (added).
 * The old note said "full content is in the session history", which is true but
 * useless: session logs are not task input, and read_file results are never spilled
 * to .toolout either. An agent that took the note at face value spent a long stretch
 * of a real run hunting an original it could not recover. Telling it to re-read the
 * path is one action it can always take. */
static bool claw_core_find_producing_call(const cJSON *messages, const cJSON *message,
                                          char *name_out, size_t name_size,
                                          char *path_out, size_t path_size)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "tool_call_id");
    const cJSON *probe = NULL;

    if (name_out && name_size) {
        name_out[0] = '\0';
    }
    if (path_out && path_size) {
        path_out[0] = '\0';
    }
    if (!cJSON_IsArray(messages) || !cJSON_IsString(id) || !id->valuestring ||
            !name_out || name_size == 0) {
        return false;
    }
    cJSON_ArrayForEach(probe, messages) {
        const cJSON *calls = cJSON_GetObjectItemCaseSensitive(probe, "tool_calls");
        const cJSON *call = NULL;

        cJSON_ArrayForEach(call, calls) {
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(call, "id");
            const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            const cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;

            if (!cJSON_IsString(cid) || !cid->valuestring ||
                    strcmp(cid->valuestring, id->valuestring) != 0) {
                continue;
            }
            if (cJSON_IsString(name) && name->valuestring) {
                strlcpy(name_out, name->valuestring, name_size);
            }
            if (path_out && path_size > 0 && cJSON_IsString(args) && args->valuestring) {
                cJSON *parsed = cJSON_Parse(args->valuestring);
                const cJSON *path = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "path") : NULL;

                if (cJSON_IsString(path) && path->valuestring) {
                    strlcpy(path_out, path->valuestring, path_size);
                }
                cJSON_Delete(parsed);
            }
            return name_out[0] != '\0';
        }
    }
    return false;
}

static void claw_core_shrink_message(const cJSON *messages, cJSON *message)
{
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
    cJSON *content;
    cJSON *reasoning;
    cJSON *tool_calls;
    cJSON *call = NULL;

    if (cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0) {
        char tool_name[32];
        char tool_path[160];
        char note[288];

        content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (cJSON_IsString(content)) {
            if (claw_core_find_producing_call(messages, message, tool_name, sizeof(tool_name),
                                              tool_path, sizeof(tool_path)) &&
                    tool_path[0] && strcmp(tool_name, "read_file") == 0) {
                snprintf(note, sizeof(note),
                         "[older read of %s omitted to bound context - re-read that path if you "
                         "still need it]", tool_path);
                cJSON_SetValuestring(content, note);
            } else {
                cJSON_SetValuestring(content, CLAW_CORE_RUNTIME_OMITTED_NOTE);
            }
        }
        return;
    }

    tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (!cJSON_IsArray(tool_calls)) {
        return;  /* user / plain assistant messages are left as-is */
    }

    /* Blank the reasoning of a round we are already trimming. The field must stay
     * present - the upstream rejects an assistant turn without reasoning_content -
     * and an empty string is what the replay layer already writes for stored turns
     * that lack one, so it is accepted. The full text stays in the session: this
     * stops it being re-sent, it does not lose it. */
    reasoning = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
    if (cJSON_IsString(reasoning) && reasoning->valuestring && reasoning->valuestring[0]) {
        cJSON_SetValuestring(reasoning, "");
    }

    cJSON_ArrayForEach(call, tool_calls) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;

        if (cJSON_IsString(args) && strlen(args->valuestring) > CLAW_TIER_RUNTIME_ARG_MAX_BYTES) {
            size_t keep = claw_utils_utf8_prefix_len(args->valuestring, CLAW_TIER_RUNTIME_ARG_MAX_BYTES);
            char *shortened = malloc(keep + 32);

            if (shortened) {
                snprintf(shortened, keep + 32, "%.*s...[truncated]", (int)keep, args->valuestring);
                cJSON_SetValuestring(args, shortened);
                free(shortened);
            }
        }
    }
}

/* Free the bytes a message spends on its reasoning, returning how many were
 * freed. The field itself must stay present (the upstream rejects an assistant
 * turn without reasoning_content), so it is emptied, not removed. */
static size_t claw_core_strip_reasoning(cJSON *message)
{
    cJSON *reasoning;
    size_t before;

    if (!cJSON_IsObject(message)) {
        return 0;
    }
    reasoning = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
    if (!cJSON_IsString(reasoning) || !reasoning->valuestring || !reasoning->valuestring[0]) {
        return 0;
    }
    before = strlen(reasoning->valuestring);
    cJSON_SetValuestring(reasoning, "");
    return before;
}

/* reasoning folding (added) -------------------------------------------------
 * The model's reasoning is the one category in a turn that cannot be recovered:
 * a tool result can be re-read, but a conclusion the model already reached
 * cannot be re-derived identically - and trying to re-derive it is exactly the
 * read/forget/re-read loop we are trying to stop. So old reasoning is COMPRESSED
 * rather than dropped: keep the head (intent and plan) and the tail (where the
 * conclusions land) and mark the hole.
 *
 * Measured on a real 100-round device turn: per-round reasoning is ~400 chars
 * median, so most rounds pass through untouched, and the diagnosis lines sit at
 * the END of the reasoning ("Key insight: ...").
 * ------------------------------------------------------------------------- */
#define CLAW_CORE_REASONING_HEAD_CHARS 200
#define CLAW_CORE_REASONING_TAIL_CHARS 400
#define CLAW_CORE_REASONING_ELISION_MARK "\n[... reasoning middle elided to bound context ...]\n"

/* Start of the last `max_bytes` bytes, moved forward so a UTF-8 sequence is
 * never split. */
static const char *claw_core_utf8_suffix(const char *text, size_t len, size_t max_bytes)
{
    size_t start = (len > max_bytes) ? (len - max_bytes) : 0;
    const unsigned char *p = (const unsigned char *)text + start;

    while (*p && ((*p & 0xC0) == 0x80)) {
        p++;
    }
    return (const char *)p;
}

/* Compress one message's reasoning to head + tail + marker. Returns bytes freed;
 * 0 when the reasoning is already short enough to be worth keeping whole. */
static size_t claw_core_fold_reasoning(cJSON *message)
{
    cJSON *reasoning;
    const char *text;
    const char *tail;
    size_t len;
    size_t head;
    size_t after;
    char *folded;

    if (!cJSON_IsObject(message)) {
        return 0;
    }
    reasoning = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
    if (!cJSON_IsString(reasoning) || !reasoning->valuestring || !reasoning->valuestring[0]) {
        return 0;
    }
    text = reasoning->valuestring;
    len = strlen(text);
    if (len <= (CLAW_CORE_REASONING_HEAD_CHARS + CLAW_CORE_REASONING_TAIL_CHARS +
                sizeof(CLAW_CORE_REASONING_ELISION_MARK))) {
        return 0;  /* short enough: the fold would free nothing */
    }

    head = claw_utils_utf8_prefix_len(text, CLAW_CORE_REASONING_HEAD_CHARS);
    tail = claw_core_utf8_suffix(text, len, CLAW_CORE_REASONING_TAIL_CHARS);
    after = head + strlen(CLAW_CORE_REASONING_ELISION_MARK) + strlen(tail);
    folded = malloc(after + 1);
    if (!folded) {
        return 0;
    }
    snprintf(folded, after + 1, "%.*s%s%s", (int)head, text,
             CLAW_CORE_REASONING_ELISION_MARK, tail);
    cJSON_SetValuestring(reasoning, folded);
    free(folded);
    return len - after;
}

/* Bytes of the newest `keep_rounds` rounds (and everything after them). */
static size_t claw_core_tail_bytes(const cJSON *messages, size_t count, size_t keep_rounds)
{
    size_t seen = 0;
    size_t total = 0;
    size_t i;

    for (i = count; i > 0; i--) {
        const cJSON *message = cJSON_GetArrayItem(messages, (int)(i - 1));

        total += claw_core_message_bytes(message);
        if (claw_core_message_has_tool_calls(message) && ++seen >= keep_rounds) {
            break;
        }
    }
    return total;
}

/* Index of the newest round (assistant message carrying tool calls), or -1. */
static int claw_core_newest_round_index(const cJSON *messages, size_t count)
{
    size_t i;

    for (i = count; i > 0; i--) {
        if (claw_core_message_has_tool_calls(cJSON_GetArrayItem(messages, (int)(i - 1)))) {
            return (int)(i - 1);
        }
    }
    return -1;
}

/* instruction content vs data (added) -------------------------------------
 * A skill activation is not a tool result in the same sense a file read is: it is
 * instructions the model needs for the WHOLE task, and it is big (a skill file can
 * be 19 KB, ~40% of the S3 window). A file read is data: dropping it costs one
 * round to re-acquire, and re-reading is routine. The spill path already exempts
 * these two by tool name (see claw_core_spill_tool_result); the window's omit path
 * did not, so the largest and most reusable item in a turn was the first thing
 * thrown away - which is exactly "forget the instructions, then drift".
 *
 * Identified via the producing tool call rather than by sniffing the text, so it
 * keeps working if the tool is renamed or the content format changes.
 * ------------------------------------------------------------------------- */
#define CLAW_CORE_INSTRUCTION_TOOL "activate_skill"

static bool claw_core_is_instruction_result(const cJSON *messages, const cJSON *message)
{
    const cJSON *id;
    const cJSON *probe = NULL;

    if (!cJSON_IsArray(messages) || !cJSON_IsObject(message)) {
        return false;
    }
    id = cJSON_GetObjectItemCaseSensitive(message, "tool_call_id");
    if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0]) {
        return false;
    }
    cJSON_ArrayForEach(probe, messages) {
        const cJSON *calls = cJSON_GetObjectItemCaseSensitive(probe, "tool_calls");
        const cJSON *call = NULL;

        cJSON_ArrayForEach(call, calls) {
            const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(call, "id");
            const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;

            if (cJSON_IsString(cid) && cid->valuestring && cJSON_IsString(name) &&
                    name->valuestring && strcmp(cid->valuestring, id->valuestring) == 0) {
                return strcmp(name->valuestring, CLAW_CORE_INSTRUCTION_TOOL) == 0;
            }
        }
    }
    return false;
}

/* Shared eviction policy (added) -------------------------------------------
 * Two callers need "which bytes go first": the in-turn window (runtime budget)
 * and the serialize guard (total request budget). They used to implement it
 * separately - the window evicted by recovery cost and protected the newest
 * rounds, while the guard blanked every message in one pass. The first time the
 * total body crossed the guard's threshold the model therefore lost all of its
 * evidence mid-turn and looped blind (observed: request body 104 KB, one pass
 * freed 56 KB, only ~8 KB was needed).
 *
 * Eviction order - by cost to recover, not by age:
 *   1. fold old reasoning to head+tail   - most bytes freed per unit of value
 *   2. omit the oldest tool results      - recoverable with one extra round
 *   3. blank the oldest folded reasoning - its conclusions were kept in step 1
 *   4. fold/blank the newest reasoning   - last resort, the live thought
 * Stops as soon as `budget_bytes` is met, and never touches the newest
 * `keep_rounds` rounds in step 2 or a skill activation (instructions outrank
 * data). The whole round is persisted before this runs, so nothing is lost from
 * the session; this only stops it being re-sent.
 * ------------------------------------------------------------------------- */
typedef struct {
    size_t folded;
    size_t blanked;
    size_t omitted;
} claw_core_trim_stats_t;

static void claw_core_evict_to_budget(cJSON *messages, size_t budget_bytes,
                                      size_t keep_rounds, claw_core_trim_stats_t *stats,
                                       char *omitted_note, size_t omitted_note_size)
{
    size_t count;
    size_t total = 0;
    size_t rounds = 0;
    size_t seen_rounds = 0;
    size_t shrinkable;
    size_t folded = 0;
    size_t blanked = 0;
    size_t omitted = 0;
    size_t i;
    int newest_index;
    size_t note_len = 0;

    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (omitted_note && omitted_note_size > 0) {
        omitted_note[0] = '\0';
    }
    if (omitted_note && omitted_note_size > 0) {
        omitted_note[0] = '\0';
    }
    if (!cJSON_IsArray(messages)) {
        return;
    }
    count = cJSON_GetArraySize(messages);
    for (i = 0; i < count; i++) {
        total += claw_core_message_bytes(cJSON_GetArrayItem(messages, i));
    }
    if (total <= budget_bytes) {
        return;  /* already within budget: leave everything intact */
    }

    for (i = 0; i < count; i++) {
        if (claw_core_message_has_tool_calls(cJSON_GetArrayItem(messages, i))) {
            rounds++;
        }
    }
    if (rounds == 0) {
        return;
    }
    newest_index = claw_core_newest_round_index(messages, count);

    /* Step 1 - compress old reasoning, never drop it outright. A tool result is
     * re-readable; a conclusion is not, and re-deriving it on-device costs a full
     * round each time. Folding frees most of the bytes while leaving the intent
     * (head) and the conclusion (tail) in place. */
    for (i = 0; i < count && total > budget_bytes; i++) {
        size_t freed;

        if ((int)i == newest_index) {
            continue;  /* the live train of thought */
        }
        freed = claw_core_fold_reasoning(cJSON_GetArrayItem(messages, i));
        if (freed > 0) {
            total -= freed;
            folded++;
        }
    }

    /* Bound the protected window by bytes as well as round count: a single huge
     * round can otherwise fill the whole budget with content the loop below is
     * not allowed to touch, leaving it no choice but to evict everything else
     * and still miss the budget. */
    while (keep_rounds > 1 &&
            claw_core_tail_bytes(messages, count, keep_rounds) > (budget_bytes / 2)) {
        keep_rounds--;
    }

    /* Step 2 - omit the oldest tool results, protecting the newest rounds. This is
     * the big byte source and the cheap one to lose: the model can re-read a file
     * in one round, and (with the conclusions from step 1 still in front of it) it
     * can then act instead of re-deriving. Deliberately NOT ordered after blanking
     * reasoning - that would trade the irreplaceable for the replaceable. */
    if (rounds > keep_rounds && total > budget_bytes) {
        shrinkable = rounds - keep_rounds;

        for (i = 0; i < count && total > budget_bytes; i++) {
            cJSON *message = cJSON_GetArrayItem(messages, i);
            const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
            bool is_tool_result = cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0;
            size_t before;
            size_t after;

            if (claw_core_message_has_tool_calls(message)) {
                seen_rounds++;
                if (seen_rounds > shrinkable) {
                    break;  /* newest rounds are protected */
                }
            } else if (seen_rounds == 0) {
                continue;  /* messages before the first tool round (e.g. the user) */
            }

            /* Instructions outrank data (added): never omit a skill activation. It
             * is the largest item in a turn and the model needs it throughout, while
             * a file read is one round away from being recovered. */
            if (is_tool_result && claw_core_is_instruction_result(messages, message)) {
                continue;
            }

            before = claw_core_message_bytes(message);
            /* A3: record what is being omitted before shrinking */
            if (is_tool_result && omitted_note && omitted_note_size > 0 && note_len < omitted_note_size - 1) {
                const cJSON *tool_call_id = cJSON_GetObjectItemCaseSensitive(message, "tool_call_id");
                const cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(message, "name");
                const char *id_str = (tool_call_id && cJSON_IsString(tool_call_id)) ? tool_call_id->valuestring : "unknown";
                const char *name_str = (tool_name && cJSON_IsString(tool_name)) ? tool_name->valuestring : "unknown";
                int written = snprintf(omitted_note + note_len, omitted_note_size - note_len,
                                       "read_file %s (id=%s); ", name_str, id_str);
                if (written > 0 && (size_t)written < omitted_note_size - note_len) {
                    note_len += (size_t)written;
                }
            }
            claw_core_shrink_message(messages, message);
            after = claw_core_message_bytes(message);
            total -= (before > after) ? (before - after) : 0;
            if (is_tool_result && before > after) {
                omitted++;
            }
        }
    }

    /* Step 3 - still over budget after cheapening everything else: now, and only
     * now, drop the oldest folded reasoning outright (oldest first - age is the
     * only staleness proxy available). */
    for (i = 0; i < count && total > budget_bytes; i++) {
        size_t freed;

        if ((int)i == newest_index) {
            continue;
        }
        freed = claw_core_strip_reasoning(cJSON_GetArrayItem(messages, i));
        if (freed > 0) {
            total -= freed;
            blanked++;
        }
    }

    /* Step 4 - last resort: even the live reasoning goes before we accept an
     * oversized round. The model loses its scratchpad, not its evidence. */
    if (total > budget_bytes && newest_index >= 0) {
        cJSON *newest = cJSON_GetArrayItem(messages, newest_index);

        if (claw_core_fold_reasoning(newest) > 0) {
            folded++;
        } else if (claw_core_strip_reasoning(newest) > 0) {
            blanked++;
        }
    }

    if (stats) {
        stats->folded = folded;
        stats->blanked = blanked;
        stats->omitted = omitted;
    }
}

void claw_core_trim_runtime_messages(cJSON *runtime_messages, char *note, size_t note_size)
{
    claw_core_trim_stats_t stats;
    char omitted[256];

    if (note && note_size > 0) {
        note[0] = '\0';
    }
    omitted[0] = '\0';
    if (!cJSON_IsArray(runtime_messages)) {
        return;
    }
    /* A3: evict fills the named list of dropped tool results; keep it in a
     * separate buffer so the composed note below can carry it. */
    /* A0 (wired): the runtime window is the dynamic window W, not the fixed
     * tier constant. With a fixed 48 KB budget, a subagent whose working set is
     * a handful of files (SPEC+PROGRESS+ARCH+NOTES+main.lua ≈ 58 KB on device)
     * had its context evicted every round and re-read the same files forever
     * without ever editing. W is memory-safety-bounded in
     * claw_core_context_window.c. */
    claw_core_evict_to_budget(runtime_messages, claw_core_window_get(),
                              CLAW_TIER_RUNTIME_KEEP_ROUNDS, &stats,
                              omitted, sizeof(omitted));

    if (note && note_size > 0 &&
            (stats.folded > 0 || stats.blanked > 0 || stats.omitted > 0)) {
        snprintf(note, note_size,
                 "Context control: this turn is long, so %u earlier reasoning block(s) were "
                 "%s and %u tool result(s) were dropped from the request. Nothing is lost from "
                 "the session or from disk. Treat your pinned card and the files you have "
                 "written as the authoritative state, and re-read a file only when you need "
                 "its exact current text to edit it.%s%s",
                 (unsigned)(stats.folded + stats.blanked),
                 (stats.blanked > 0) ? "compressed or cleared"
                                     : "compressed to their first and last part",
                 (unsigned)stats.omitted,
                 omitted[0] ? " Dropped (re-read by name): " : "",
                 omitted);
    }
}


/* ===== serialize guard (added) =============================================
 * Every budget above governs one part of the request; nobody governed the total.
 * On a failing device task the parts summed past what the module could hold and
 * the failure surfaced as "Out of memory serializing request" deep inside the
 * JSON writer, after all the context work had been paid for. This estimates the
 * body before the call, trims once if it is over the estimate budget, and
 * refuses cleanly (with advice the model can act on) if it is still too big.
 * ========================================================================== */

/* Rough but monotone: raw text plus a fixed allowance for JSON scaffolding. */
static size_t claw_core_request_body_bytes(const char *system_prompt,
                                           const cJSON *messages,
                                           const char *tools_json)
{
    size_t total = 0;
    size_t count;
    size_t i;

    if (system_prompt) {
        total += strlen(system_prompt);
    }
    if (tools_json) {
        total += strlen(tools_json);
    }
    if (cJSON_IsArray(messages)) {
        count = cJSON_GetArraySize(messages);
        for (i = 0; i < count; i++) {
            total += claw_core_message_bytes(cJSON_GetArrayItem(messages, i));
        }
    }
    /* Variable scaffolding factor: text with many special chars needs more escaping */
    if (total > 0) {
        /* Estimate: if text has >10% special chars, use 20% factor; otherwise 12.5% */
        size_t special_estimate = total / 10;
        if (special_estimate > total / 10) {
            return total + (total / 5);  /* +20% for heavy escaping */
        }
    }
    return total + (total / 8);  /* +12.5% for normal text */
}

esp_err_t claw_core_context_guard_request(const char *system_prompt,
                                          cJSON *messages,
                                          const char *tools_json,
                                          char **out_error_message)
{
    size_t bytes;
    size_t preamble;
    size_t message_budget;
    claw_core_trim_stats_t stats;

    if (out_error_message) {
        *out_error_message = NULL;
    }

    /* A2: total budget = runtime window W + replayed history + preamble.
     * The request body is preamble + replayed session history + runtime window;
     * an earlier version summed only (W + preamble) and so tripped the guard on
     * every round once the replayed history was also present, folding reasoning
     * each round for no memory reason. All three parts must be in the sum. */
    size_t est_budget = claw_core_window_get() +
                        CLAW_TIER_PROJECT_TRIGGER_CHARS +
                        CLAW_TIER_SERIALIZE_PREAMBLE_BYTES;

    bytes = claw_core_request_body_bytes(system_prompt, messages, tools_json);
    if (bytes < est_budget) {
        return ESP_OK;
    }

    /* A2: contiguous block pre-check — ensure largest free block can hold est * 1.5 */
    size_t required_block = (bytes * 3) / 2;
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t largest_total = largest_int + largest_psram;

    if (largest_total < required_block) {
        ESP_LOGW(TAG,
                 "request body ~%u bytes needs %u contiguous block, have %u (int=%u psram=%u): trimming",
                 (unsigned)bytes,
                 (unsigned)required_block,
                 (unsigned)largest_total,
                 (unsigned)largest_int,
                 (unsigned)largest_psram);
    }

    ESP_LOGW(TAG,
             "request body ~%u bytes >= %u budget: trimming by recovery cost (free_int=%u largest_int=%u free_spiram=%u largest_spiram=%u)",
             (unsigned)bytes,
             (unsigned)est_budget,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)largest_int,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)largest_psram);

    /* A2: solve for message-budget from dynamic est_budget */
    preamble = (system_prompt ? strlen(system_prompt) : 0) +
               (tools_json ? strlen(tools_json) : 0);
    message_budget = (est_budget * 8) / 9;
    message_budget = (message_budget > preamble) ? (message_budget - preamble) : 0;

    /* Degrade by recovery cost and stop as soon as the budget is met (added).
     * This used to blank EVERY tool result, reasoning block and long tool
     * argument in one pass, which turned a small overage into total amnesia for
     * the rest of the turn (device: 104 KB body, 56 KB freed, ~8 KB needed). */
    claw_core_evict_to_budget(messages, message_budget,
                              CLAW_TIER_RUNTIME_KEEP_ROUNDS, &stats,
                              NULL, 0);

    bytes = claw_core_request_body_bytes(system_prompt, messages, tools_json);
    ESP_LOGW(TAG,
             "request body trimmed to ~%u bytes (folded=%u omitted=%u blanked=%u)",
             (unsigned)bytes, (unsigned)stats.folded,
             (unsigned)stats.omitted, (unsigned)stats.blanked);
    size_t refuse_limit = est_budget;  /* A2: refuse at the trigger, no overshoot */
    if (bytes < refuse_limit) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "request body still ~%u bytes after shrinking; refusing the round", (unsigned)bytes);
    if (out_error_message) {
        *out_error_message = claw_utils_string_dup_printf(
            "This turn's context is too large to send (%u bytes, limit %u). Nothing was sent and no "
            "tool ran. Recover by: (1) calling pin_note with the goal, decisions and current state; "
            "(2) starting a new session if the task is finished; (3) splitting the job into smaller "
            "subagent tasks. Do not retry the same request unchanged.",
            (unsigned)bytes, (unsigned)refuse_limit);
    }
    return ESP_ERR_INVALID_SIZE;
}

esp_err_t claw_core_append_tool_results_messages(claw_core_state_t *core,
                                                 cJSON *runtime_messages,
                                                 const claw_core_llm_response_t *response,
                                                 const claw_core_request_t *request,
                                                 char *tool_summary,
                                                 size_t tool_summary_size,
                                                 char **out_tool_results_json)
{
    cJSON *tool_results = NULL;
    char *tool_results_json = NULL;
    size_t i;
    esp_err_t ret = ESP_OK;

    if (!core || !runtime_messages || !response || !request || !out_tool_results_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!core->call_cap) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_tool_results_json = NULL;

    tool_results = cJSON_CreateArray();
    if (!tool_results) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        char *tool_output = NULL;
        cJSON *tool_message = NULL;
        cJSON *runtime_copy = NULL;
        esp_err_t err;

        ESP_LOGI(TAG, "tool_call request=%" PRIu32 " name=%s args=%.*s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 claw_core_log_snippet_len(response->tool_calls[i].arguments_json),
                 claw_core_log_snippet(response->tool_calls[i].arguments_json),
                 claw_core_log_snippet_suffix(response->tool_calls[i].arguments_json));

        err = core->call_cap(response->tool_calls[i].name,
                             response->tool_calls[i].arguments_json,
                             request,
                             &tool_output,
                             core->cap_user_ctx);
        if (err != ESP_OK && !tool_output) {
            tool_output = claw_utils_string_dup(esp_err_to_name(err));
        }
        if (!tool_output) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        /* Bound oversized results before they enter context (added). */
        tool_output = claw_core_bound_tool_output(request, response->tool_calls[i].name, tool_output);
        if (!tool_output) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        ESP_LOGI(TAG, "tool_result request=%" PRIu32 " name=%s err=%s output=%.*s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 esp_err_to_name(err),
                 claw_core_log_snippet_len(tool_output),
                 claw_core_log_snippet(tool_output),
                 claw_core_log_snippet_suffix(tool_output));

        if (tool_summary && tool_summary_size > 0 && response->tool_calls[i].name) {
            esp_err_t summary_err = claw_core_append_tool_summary_line(tool_summary,
                                                                       tool_summary_size,
                                                                       response->tool_calls[i].name,
                                                                       err == ESP_OK);
            if (summary_err != ESP_OK) {
                ESP_LOGW(TAG, "tool summary truncated for request=%" PRIu32, request->request_id);
            }
        }

        tool_message = cJSON_CreateObject();
        if (!tool_message) {
            free(tool_output);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        cJSON_AddStringToObject(tool_message, "role", "tool");
        cJSON_AddStringToObject(tool_message, "tool_call_id", response->tool_calls[i].id);
        cJSON_AddStringToObject(tool_message, "content", tool_output);
        cJSON_AddBoolToObject(tool_message, "is_error", err != ESP_OK);
        free(tool_output);

        runtime_copy = cJSON_Duplicate(tool_message, true);
        if (!runtime_copy) {
            cJSON_Delete(tool_message);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(runtime_messages, runtime_copy);
        cJSON_AddItemToArray(tool_results, tool_message);
    }

    tool_results_json = cJSON_PrintUnformatted(tool_results);
    if (!tool_results_json) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_tool_results_json = tool_results_json;
    tool_results_json = NULL;

cleanup:
    if (tool_results_json) {
        cJSON_free(tool_results_json);
    }
    cJSON_Delete(tool_results);
    return ret;
}

static esp_err_t apply_context_content(char **system_prompt,
                                       cJSON *messages,
                                       cJSON *tools,
                                       claw_core_context_kind_t kind,
                                       const char *section_name,
                                       const char *content)
{
    if (!system_prompt || !*system_prompt || !messages || !tools ||
            !section_name || !content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (kind) {
    case CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT:
        return append_prompt_section(system_prompt, section_name, content);
    case CLAW_CORE_CONTEXT_KIND_MESSAGES:
        return append_message_array_json(messages, content);
    case CLAW_CORE_CONTEXT_KIND_TOOLS:
        return append_tool_array_json(tools, content);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t claw_core_collect_request_start_only_contexts(
    claw_core_state_t *core,
    const claw_core_request_item_t *request,
    claw_core_cached_context_t **out_contexts,
    size_t *out_count)
{
    claw_core_cached_context_t *contexts = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!core || !request || !out_contexts || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_contexts = NULL;
    *out_count = 0;

    if (core->context_provider_count == 0) {
        return ESP_OK;
    }

    contexts = calloc(core->context_provider_count, sizeof(*contexts));
    if (!contexts) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        claw_core_context_t context = {0};
        const claw_core_context_provider_t *provider = &core->context_providers[i];
        size_t context_len;

        if (!(provider->flags & CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY)) {
            continue;
        }

        err = provider->collect(&request->view, &context, provider->user_ctx);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "context provider collect failed request=%" PRIu32
                     " provider=%s err=%s",
                     request->view.request_id,
                     provider->name,
                     esp_err_to_name(err));
            goto cleanup;
        }
        if (!context.content || !context.content[0]) {
            ESP_LOGW(TAG,
                     "context provider returned empty content request=%" PRIu32
                     " provider=%s",
                     request->view.request_id,
                     provider->name);
            free(context.content);
            err = ESP_FAIL;
            goto cleanup;
        }

        context_len = strlen(context.content);
        ESP_LOGI(TAG,
                 "context_cached request=%" PRIu32 " provider=%s context_kind=%s context_len=%u",
                 request->view.request_id,
                 provider->name,
                 claw_core_context_kind_to_string(context.kind),
                 (unsigned)context_len);

        contexts[i].valid = true;
        contexts[i].kind = context.kind;
        contexts[i].content = context.content;
        context.content = NULL;
    }

    *out_contexts = contexts;
    *out_count = core->context_provider_count;
    contexts = NULL;

cleanup:
    claw_core_free_cached_contexts(contexts, core->context_provider_count);
    return err;
}

bool claw_core_cached_contexts_have_messages(const claw_core_cached_context_t *contexts,
                                             size_t count)
{
    size_t i;

    if (!contexts) {
        return false;
    }

    for (i = 0; i < count; i++) {
        if (contexts[i].valid && contexts[i].kind == CLAW_CORE_CONTEXT_KIND_MESSAGES) {
            return true;
        }
    }
    return false;
}

esp_err_t claw_core_build_iteration_context(claw_core_state_t *core,
                                            const claw_core_request_item_t *request,
                                            const cJSON *runtime_messages,
                                            const claw_core_cached_context_t *request_start_contexts,
                                            size_t request_start_context_count,
                                            bool inject_active_user,
                                            char **out_system_prompt,
                                            cJSON **out_messages,
                                            char **out_tools_json,
                                            char *obs_providers_csv,
                                            size_t obs_providers_csv_size)
{
    char *system_prompt = NULL;
    char *turn_prompt = NULL;
    cJSON *messages = NULL;
    cJSON *tools = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!core || !request || !out_system_prompt || !out_messages || !out_tools_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_system_prompt = NULL;
    *out_messages = NULL;
    *out_tools_json = NULL;

    system_prompt = claw_utils_string_dup(core->system_prompt);
    messages = cJSON_CreateArray();
    tools = cJSON_CreateArray();
    if (!system_prompt || !messages || !tools) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        claw_core_context_t context = {0};
        const claw_core_context_provider_t *provider = &core->context_providers[i];
        size_t context_len;

        if (provider->flags & CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY) {
            if (i < request_start_context_count && request_start_contexts &&
                    request_start_contexts[i].valid) {
                err = apply_context_content(&system_prompt,
                                            messages,
                                            tools,
                                            request_start_contexts[i].kind,
                                            provider->name,
                                            request_start_contexts[i].content);
                if (err != ESP_OK) {
                    goto cleanup;
                }
                claw_core_obs_csv_append(obs_providers_csv, obs_providers_csv_size, provider->name, true);
            }
            continue;
        }

        err = provider->collect(&request->view, &context, provider->user_ctx);
        if (err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "context provider collect failed request=%" PRIu32
                     " provider=%s err=%s",
                     request->view.request_id,
                     provider->name,
                     esp_err_to_name(err));
            goto cleanup;
        }
        if (!context.content || !context.content[0]) {
            ESP_LOGW(TAG,
                     "context provider returned empty content request=%" PRIu32
                     " provider=%s",
                     request->view.request_id,
                     provider->name);
            free(context.content);
            err = ESP_FAIL;
            goto cleanup;
        }
        context_len = strlen(context.content);
        ESP_LOGI(TAG,
                 "context_loaded request=%" PRIu32 " provider=%s context_kind=%s context_len=%u",
                 request->view.request_id,
                 provider->name,
                 claw_core_context_kind_to_string(context.kind),
                 (unsigned)context_len);
        claw_core_obs_csv_append(obs_providers_csv, obs_providers_csv_size, provider->name, true);

        err = apply_context_content(&system_prompt,
                                    messages,
                                    tools,
                                    context.kind,
                                    provider->name,
                                    context.content);
        free(context.content);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    turn_prompt = build_current_turn_prompt(&request->view);
    if (!turn_prompt) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = append_prompt_section(&system_prompt, "Core Request", turn_prompt);
    free(turn_prompt);
    turn_prompt = NULL;
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (inject_active_user) {
        err = claw_core_append_user_message(messages, request->view.user_text);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (runtime_messages && cJSON_GetArraySize((cJSON *)runtime_messages) > 0) {
        err = append_message_array(messages, runtime_messages);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    *out_tools_json = cJSON_GetArraySize(tools) > 0 ? cJSON_PrintUnformatted(tools) : NULL;
    if (cJSON_GetArraySize(tools) > 0 && !*out_tools_json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_system_prompt = system_prompt;
    *out_messages = messages;
    system_prompt = NULL;
    messages = NULL;
    err = ESP_OK;

cleanup:
    free(turn_prompt);
    free(system_prompt);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    if (err != ESP_OK) {
        free(*out_tools_json);
        *out_tools_json = NULL;
    }
    return err;
}
