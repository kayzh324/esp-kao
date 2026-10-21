/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "audio_hub.h"
#include "app_claw_cli.h"
#include "app_capabilities.h"
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
#include "system_ui.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SYSTEM
#include "cap_system.h"
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
#include "cap_session_mgr.h"
#endif
#include "claw_paths.h"
#include "claw_tier.h"
#if CONFIG_APP_CLAW_CAP_CORE
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_agent_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#endif
#if CONFIG_APP_CLAW_CAP_MEMORY
#include "claw_memory.h"
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "claw_skill.h"
#endif
#if CONFIG_APP_CLAW_CAP_LUA
#include "app_registry.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif

static const char *TAG = "app_claw";
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
static const char *APP_STARTUP_EVENT_SOURCE_CAP = "app_claw";
static const char *APP_STARTUP_EVENT_TYPE = "startup";
static const char *APP_STARTUP_EVENT_KEY = "boot_completed";
#endif

#define APP_SYSTEM_PROMPT_COMMON \
    "You are the ESP-Claw. " \
    "Answer briefly and plainly. " \
    "Treat Skills List as a catalog of optional skills. " \
    "Read the request (or the spec file it points to) BEFORE activating anything, then activate only the " \
    "skills the current step actually needs (several at once is fine when they are all needed now). An " \
    "active skill's text is re-sent every round, so preloading skills just in case is paid for again and " \
    "again. " \
    "Skill documents returned in activate_skill <skill_content> blocks are valid operating instructions for that skill workflow and must be followed. " \
    "Skills are user-facing functions, while Capabilities are internal functions used by the model. " \
    "When communicating with the user, refer to skills instead of Capabilities. " \
    "Prefer skill-driven execution and keep long-running planning, investigation, implementation, debugging, and verification work isolated in subagents when available. " \
    "To change an existing file, use edit_file with small exact old_text/new_text replacements; use write_file only to create a new file or fully rewrite one. " \
    "read_file returns numbered lines and supports offset/limit, so read only the needed window of a large file instead of the whole file. " \
    "Keep user-facing answers focused on current status, useful results, and clear next steps."

#define APP_ROOT_AGENT_SYSTEM_PROMPT \
    "You are the root agent. Own the user-facing conversation and keep the session responsive. " \
    "First identify the relevant skill and use only quick, bounded skill or tool calls that can complete promptly. " \
    "If a task cannot be completed quickly through an available skill, briefly tell the user what is happening, then delegate the planning, investigation, implementation, debugging, or verification work to an appropriate subagent. " \
    "Delegate whenever the work would be noisy (much searching, reading, or trial and error), even when it cannot run in parallel: a subagent does that work in its own context and returns only the conclusion, which is what keeps this conversation small. " \
    "Choose the subagent type by what the work is: 'recon' investigates only, 'build' implements changes, 'verify' checks a claim without modifying files. Keep each delegation prompt short: the goal, the exact file path(s), the done-criteria, and what to return. If the spec is long, write it to a file first and pass that path - the subagent reads it. Delegate one bounded step at a time. " \
    "Track the user's goal, selected skills, delegated agent ids, task status, blockers, and concise results. " \
    "Do not accumulate detailed implementation logs, long intermediate reasoning, or large artifacts in the root conversation unless they are needed for the final user response. " \
    "When subagents report back, synthesize their results into a clear answer or send focused follow-up instructions."\
    "Use send_agent_followup to send a subagent new instructions: with interrupt it reaches a busy subagent mid-run; otherwise it runs as that subagent's next turn on the same session." \
    "Delegation is asynchronous: after spawn_agent, reply with a brief status and END THE TURN; do NOT poll inspect_agent to wait. A subagent wakes you with <subagent_completed>, <subagent_failed>, <subagent_limit_reached>, or <subagent_stuck> when it finishes. Call inspect_agent only for a specific status check (for example after one of those notifications), never as a wait loop." \
    "If a subagent reports <subagent_failed> or <subagent_limit_reached>, use inspect_agent to check its status and last_error, then either resume it with send_agent_followup or close_agent to stop it and finish the work yourself. " \
    "A failure usually means the subagent died mid-step (for example 'Out of memory serializing request' on a very long session), so its on-disk writes may be PARTIAL: before retrying, check what actually landed (grep/read the touched files and PROGRESS.md), then close it and either resume it or spawn a bounded recovery subagent whose task is written to a file first. " \
    "Treat repeated failures, OOM, or timeouts as resource-tension signals: reduce the work per subagent and the size of what it must hold, rather than retrying the same thing unchanged. " \
    "Keep a short cold-start handover file on disk (state, what changed, exact re-run command) so a fresh subagent can pick up without your session."

#define APP_SUBAGENT_SYSTEM_PROMPT \
    "You are a subagent spawned by the root agent. Handle the scoped work delegated by the root agent. " \
    "Own the detailed planning, investigation, implementation, tool use, and verification for your assigned task. " \
    "Keep your work bounded to the delegation prompt and available tools. " \
    "Do not manage other agents or broaden the task unless the root explicitly asks. " \
    "Return a structured result: status, the conclusion or what changed, the files touched, the verification you ran, and any blocker or open question. " \
    "Return conclusions and evidence, not raw logs or a transcript: your own context holds the detail, the root only needs what it must act on."

#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
#define APP_SYSTEM_PROMPT_SUFFIX \
    "When long-term memory is needed, activate the 'memory_ops' skill first and follow its instructions. " \
    "Do not activate or use the memory skill for ordinary self-introductions or casual preferences unless the user explicitly asks to remember, save, update, or forget something. Automatic extraction will handle durable facts silently after the reply when appropriate. " \
    "Use memory tools only through that skill. " \
    "Auto-injected memory context contains summary labels, not full memory bodies. " \
    "When detailed long-term memory is needed, use exact summary labels with memory_recall. " \
    "Do not ask whether the user wants you to remember ordinary profile or preference statements when automatic extraction can handle them. Do not offer memory-save help unless the user explicitly asks about memory management. " \
    "Do not use memory_records.jsonl, memory_index.json, memory_digest.log, or MEMORY.md as direct decision input.\n"
#else
#define APP_SYSTEM_PROMPT_SUFFIX "\n"
#endif

/* always-on conventions (added): invariants that must not depend on the model
 * choosing to load a skill. Task-shaped know-how stays in skills; these do not. */
#define APP_SYSTEM_PROMPT_CONVENTIONS \
    "Environment conventions: " \
    "before operating hardware, assigning a GPIO, or writing board-specific code, activate the " \
    "board_hardware_info skill: it is this board's device inventory (device names, chip models, bus " \
    "addresses and occupied GPIOs). Never guess a chip model, bus address or pin - read it there. " \
    "the writable data root is storage.get_root_dir() and /system is read-only; build paths with " \
    "storage.get_root_dir() + storage.join_path(...) and never hardcode /fatfs. " \
    "A program lives in <data_root>/apps/<id>/ with launcher.json (schema_version 1, id, entry) and its " \
    "entry script (main.lua by convention); the manifest is what makes a directory an app, and " \
    "publish_app must be called after creating or updating it so the system launcher shows it. " \
    "Throwaway scripts go in <data_root>/tmp/. " \
    "Do not read sessions/*.log, memory_records.jsonl, memory_index.json or MEMORY.md as task input - " \
    "they are the agent's own plumbing, not task material. " \
    "Do not keep duplicate backup copies: write_file and edit_file save the previous version automatically " \
    "and report its path, and restore_file brings it back. But a file rewritten from inside a script is NOT " \
    "saved - so before letting a script overwrite something you will need to compare against, copy_file it " \
    "to tmp/ first. Reading it is not enough: the write replaces it and your reads get evicted. " \
    "For work that runs over many rounds, first pin_note your task card (goal, constraints, API notes, " \
    "draft outline) - it is replayed every round and survives context compaction. " \
    "Write code drafts to a file with write_file as soon as you have them; a draft that only lives in your " \
    "reasoning is lost when the context is compacted. " \
    "After a compaction, re-read the pinned card before re-reading any file. " \
    "Deliver multi-step work in observable stages: after each stage that can be checked on the device, " \
    "stop and report what to look at (what is on screen, the log line, the number) instead of continuing " \
    "until everything is done. A stage nobody can observe is not a stage; if several tool rounds have " \
    "passed without producing anything observable, stop and report what you have and what is left. " \
    "Waiting on a long-running Lua job is free if you wait inside the tool: pass wait_ms to " \
    "lua_tail_async_job (wait for new output) or lua_get_async_job (wait for it to finish). Calling " \
    "lua_list_async_jobs or either reader in a loop re-sends your whole context for every check - " \
    "never poll: wait once, with a deadline. " \
    "Keep a program modular, not one growing file: split by responsibility into <data_root>/apps/_lib/ " \
    "helpers (require them from main.lua) BEFORE any file gets large - write_file/edit_file report each " \
    "file's size and what it costs per read, and a file past roughly a third of your round budget is already " \
    "too big. A main.lua that keeps getting longer is a defect, not progress. " \
    "Authority: when you delegate, state what the task may take and must restore; if you hit something " \
    "only the user can decide, call report_blocked and stop - never take what you were not given, and " \
    "never work around a block."

#define APP_SYSTEM_PROMPT \
    APP_SYSTEM_PROMPT_COMMON \
    APP_SYSTEM_PROMPT_CONVENTIONS \
    APP_SYSTEM_PROMPT_SUFFIX

#define APP_CLAW_LAUNCHER_OUTPUT_LEN 128
#define APP_CLAW_UI_JOBS_OUTPUT_LEN 4096
#define APP_CLAW_UI_JOB_STOP_WAIT_MS 50

/* Max tool-calling rounds per turn (added). This is a runaway-loop backstop, not
 * a planning budget: it is deliberately high so ordinary long autonomous work
 * never hits it, and the model is no longer told a round count. Context growth
 * is bounded separately by session projection, so long turns stay affordable.
 * The Kconfig default follows the ESP KAO performance tiers (P4/S31 > S3), so a
 * module with more RAM still runs a longer turn. */
#define APP_CLAW_MAX_TOOL_ITERATIONS CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS

#if CONFIG_APP_CLAW_CAP_CORE && CONFIG_APP_CLAW_CAP_SKILL_MGR
typedef struct {
    char *text;
    size_t length;
} app_claw_skills_list_builder_t;

static esp_err_t app_claw_append_skill(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    app_claw_skills_list_builder_t *builder = user_ctx;

    if (!entry || !builder) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *id = entry->id ? entry->id : "";
    const char *summary = entry->summary ? entry->summary : "";
    int added = snprintf(NULL, 0, "- %s: %s\n", id, summary);

    if (added < 0 || SIZE_MAX - builder->length <= (size_t)added) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *grown = realloc(builder->text, builder->length + (size_t)added + 1);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    builder->text = grown;
    snprintf(builder->text + builder->length, (size_t)added + 1, "- %s: %s\n", id, summary);
    builder->length += (size_t)added;
    return ESP_OK;
}

static esp_err_t app_claw_skills_list_collect(const claw_core_request_t *request, claw_core_context_t *out_context, void *user_ctx)
{
    static const char heading[] = "Available skills:\n";
    app_claw_skills_list_builder_t builder = {
        .text = strdup(heading),
        .length = sizeof(heading) - 1,
    };

    (void)request;
    (void)user_ctx;
    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_context, 0, sizeof(*out_context));
    if (!builder.text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = claw_skill_foreach_catalog_entry(app_claw_append_skill, &builder);
    if (err != ESP_OK) {
        free(builder.text);
        return err;
    }
    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = builder.text;
    return ESP_OK;
}

static const claw_core_context_provider_t s_app_claw_skills_list_provider = {
    .name = "Skills List",
    .collect = app_claw_skills_list_collect,
    /* request-start snapshot (added): the catalog is stable within a request. */
    .flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY,
};
#endif

#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE && CONFIG_APP_CLAW_CAP_LUA
static char *app_claw_split_job_field(char **cursor)
{
    char *field = NULL;
    char *sep = NULL;

    if (!cursor || !*cursor) {
        return NULL;
    }

    field = *cursor;
    sep = strstr(field, " | ");
    if (sep) {
        *sep = '\0';
        *cursor = sep + 3;
    } else {
        *cursor = NULL;
    }
    return field;
}

static const char *app_claw_job_field_value(const char *field, const char *prefix)
{
    size_t prefix_len = 0;

    if (!field || !prefix) {
        return "";
    }
    prefix_len = strlen(prefix);
    if (strncmp(field, prefix, prefix_len) == 0) {
        return field + prefix_len;
    }
    return field;
}

static bool app_claw_job_value_is_empty(const char *value)
{
    return !value || !value[0] || strcmp(value, "(unnamed)") == 0 || strcmp(value, "none") == 0;
}

static void app_claw_set_job_title(system_ui_task_item_t *item,
                                   const char *path,
                                   const char *name,
                                   const char *job_id)
{
    const char *title = NULL;
    const char *slash = NULL;
    size_t len = 0;

    if (!item) {
        return;
    }

    if (!app_claw_job_value_is_empty(path)) {
        slash = strrchr(path, '/');
        title = slash ? slash + 1 : path;
    }
    if (app_claw_job_value_is_empty(title) && !app_claw_job_value_is_empty(name)) {
        title = name;
    }
    if (app_claw_job_value_is_empty(title)) {
        title = job_id ? job_id : "";
    }

    len = strlen(title);
    if (len > 4 && strcmp(title + len - 4, ".lua") == 0) {
        len -= 4;
    }
    snprintf(item->title, sizeof(item->title), "%.*s", (int)len, title);
}

static size_t app_claw_parse_lua_jobs(char *text, system_ui_task_item_t *out, size_t max)
{
    size_t count = 0;
    char *line = text;

    if (!text || !out || max == 0 || strcmp(text, "(no Lua async jobs)") == 0) {
        return 0;
    }

    while (line && line[0] && count < max) {
        char *next_line = strchr(line, '\n');
        char *cursor = line;
        char *job_id = NULL;
        char *status = NULL;
        char *name_field = NULL;
        char *exclusive_field = NULL;
        char *runtime_field = NULL;
        char *path_field = NULL;
        const char *name = NULL;
        const char *exclusive = NULL;
        const char *runtime = NULL;
        const char *path = NULL;

        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        job_id = app_claw_split_job_field(&cursor);
        status = app_claw_split_job_field(&cursor);
        name_field = app_claw_split_job_field(&cursor);
        exclusive_field = app_claw_split_job_field(&cursor);
        runtime_field = app_claw_split_job_field(&cursor);
        path_field = cursor;

        if (!job_id || !status || !name_field || !runtime_field || !path_field) {
            line = next_line;
            continue;
        }

        name = app_claw_job_field_value(name_field, "name=");
        exclusive = app_claw_job_field_value(exclusive_field, "exclusive=");
        runtime = app_claw_job_field_value(runtime_field, "runtime=");
        path = app_claw_job_field_value(path_field, "path=");

        strlcpy(out[count].id, job_id, sizeof(out[count].id));
        app_claw_set_job_title(&out[count], path, name, job_id);
        strlcpy(out[count].status, status, sizeof(out[count].status));
        if (app_claw_job_value_is_empty(exclusive)) {
            snprintf(out[count].detail, sizeof(out[count].detail), "%s", runtime);
        } else {
            snprintf(out[count].detail, sizeof(out[count].detail),
                     "%s  exclusive=%s", runtime, exclusive);
        }
        count++;
        line = next_line;
    }
    return count;
}

static size_t app_claw_lua_jobs_provider(system_ui_task_item_t *out, size_t max, void *user_ctx)
{
    char *output = NULL;
    size_t count = 0;

    (void)user_ctx;
    if (!out || max == 0) {
        return 0;
    }

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return 0;
    }

    if (cap_lua_list_jobs("running", output, APP_CLAW_UI_JOBS_OUTPUT_LEN) == ESP_OK) {
        count = app_claw_parse_lua_jobs(output, out, max);
    }
    free(output);
    return count;
}

static void app_claw_lua_job_event_cb(const cap_lua_job_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (!event) {
        return;
    }
    switch (event->type) {
    case CAP_LUA_JOB_EVENT_CREATED:
    case CAP_LUA_JOB_EVENT_RUNNING:
    case CAP_LUA_JOB_EVENT_STOP_REQUESTED:
    case CAP_LUA_JOB_EVENT_TERMINAL:
        (void)system_ui_refresh_tasks();
        break;
    default:
        break;
    }
}

static esp_err_t app_claw_lua_job_stop_cb(const char *job_id, void *user_ctx)
{
    char *output = NULL;
    esp_err_t err;

    (void)user_ctx;
    if (!job_id || !job_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_lua_stop_job(job_id, APP_CLAW_UI_JOB_STOP_WAIT_MS, output, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop Lua job failed: job_id=%s err=%s output=%s",
                 job_id, esp_err_to_name(err), output);
        if (err == ESP_ERR_TIMEOUT) {
            err = ESP_OK;
        }
    } else {
        ESP_LOGI(TAG, "stop Lua job: job_id=%s output=%s", job_id, output);
    }
    free(output);
    return err;
}

static esp_err_t app_claw_lua_jobs_stop_all_cb(void *user_ctx)
{
    char *output = NULL;
    esp_err_t err;

    (void)user_ctx;

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_lua_stop_all_jobs(NULL, APP_CLAW_UI_JOB_STOP_WAIT_MS, output, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop all Lua jobs failed: err=%s output=%s",
                 esp_err_to_name(err), output);
    } else {
        ESP_LOGI(TAG, "stop all Lua jobs: output=%s", output);
    }
    free(output);
    return err;
}

static bool app_claw_app_action_is_lua_script(const char *action)
{
    size_t len;

    if (action == NULL || action[0] != '/' || strstr(action, "..") != NULL) {
        return false;
    }
    len = strlen(action);
    return len > 4 && strcmp(action + len - 4, ".lua") == 0;
}

static void app_claw_app_select_cb(const system_ui_launcher_item_t *selection, void *user_ctx)
{
    char output[APP_CLAW_LAUNCHER_OUTPUT_LEN] = {0};

    (void)user_ctx;
    if (selection == NULL || !app_claw_app_action_is_lua_script(selection->action)) {
        ESP_LOGW(TAG, "invalid launcher action: %s",
                 selection && selection->action ? selection->action : "(null)");
        return;
    }

    esp_err_t err = cap_lua_run_script_async(selection->action,
                                             selection->args_json,
                                             0,
                                             selection->id,
                                             "display",
                                             true,
                                             output,
                                             sizeof(output));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "launcher action failed: title=%s action=%s err=%s output=%s",
                 selection->title ? selection->title : "(null)",
                 selection->action,
                 esp_err_to_name(err),
                 output);
        return;
    }
    ESP_LOGI(TAG, "launcher action started: title=%s action=%s output=%s",
             selection->title ? selection->title : "(null)",
             selection->action,
             output);
}
#endif

static SemaphoreHandle_t s_config_lock;
static app_claw_config_t s_current_config;
static bool s_current_config_valid;
static app_claw_save_config_fn s_save_config;
static void *s_save_config_user_ctx;
static app_claw_network_ready_fn s_network_ready;
static void *s_network_ready_user_ctx;

static void app_claw_audio_start_service(void)
{
    static const audio_hub_config_t config = {
        .mixer = {
            .sample_rate = 16000, .channels = 1, .bits = 16, .frame_ms = 20,
            .system_full_gain = 1.0f, .app_full_gain = 1.0f, .app_ducked_gain = 0.3f,
            .duck_release_ms = 300, .output_volume = 80,
        },
        .capture = {
            .sample_rate = 16000, .channels = 1, .bits = 16, .frame_ms = 20, .ring_frames = 8,
        },
    };
    esp_err_t err = audio_hub_start(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_hub_start skipped: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_claw_ensure_config_lock(void)
{
    if (!s_config_lock) {
        s_config_lock = xSemaphoreCreateMutex();
        if (!s_config_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t app_claw_store_current_config(const app_claw_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_current_config = *config;
    s_current_config_valid = true;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_set_save_config_callback(app_claw_save_config_fn save_config,
                                            void *user_ctx)
{
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_save_config = save_config;
    s_save_config_user_ctx = user_ctx;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_set_network_ready_callback(app_claw_network_ready_fn network_ready,
                                              void *user_ctx)
{
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_network_ready = network_ready;
    s_network_ready_user_ctx = user_ctx;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_get_config(app_claw_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG, "out_config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (!s_current_config_valid) {
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_config = s_current_config;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

claw_core_handle_t app_claw_get_core(void)
{
#if CONFIG_APP_CLAW_CAP_CORE
    return claw_agent_mgr_get_root_core();
#else
    return NULL;
#endif
}

#if CONFIG_APP_CLAW_CAP_MEMORY
static esp_err_t app_claw_persist_context(const claw_core_context_persist_batch_t *batch, void *ctx)
{
    esp_err_t err = claw_memory_persist_context_callback(batch, ctx);
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    if (err == ESP_OK) {
        for (size_t i = 0; i < batch->record_count; i++) {
            if (batch->records[i].type == CLAW_CORE_CONTEXT_RECORD_USER || batch->records[i].type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL) {
                esp_err_t catalog_err = claw_session_mgr_catalog_touch(batch->session_id, batch->records[i].text);
                if (catalog_err != ESP_OK) ESP_LOGW(TAG, "Session metadata update failed: %s", esp_err_to_name(catalog_err));
                break;
            }
        }
    }
#endif
    return err;
}
#endif

#if CONFIG_APP_CLAW_CAP_SESSION_MGR && (CONFIG_APP_CLAW_CAP_MEMORY || CONFIG_APP_CLAW_CAP_SKILL_MGR)
static esp_err_t app_claw_delete_session_history(const char *session_id,
                                                 bool *out_deleted_any,
                                                 void *user_ctx)
{
    bool memory_deleted = false;
    bool skill_deleted = false;
    esp_err_t err;

    (void)user_ctx;

    if (!out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_deleted_any = false;

#if CONFIG_APP_CLAW_CAP_MEMORY
    err = claw_memory_delete_session_history(session_id, &memory_deleted);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    err = claw_skill_delete_session_state(session_id, &skill_deleted);
    if (err != ESP_OK) {
        return err;
    }
#endif

    *out_deleted_any = memory_deleted || skill_deleted;
    return ESP_OK;
}
#endif

esp_err_t app_claw_ui_start(void)
{
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    ESP_RETURN_ON_ERROR(system_ui_start(NULL), TAG, "start system UI failed");
#if CONFIG_APP_CLAW_CAP_LUA
    const system_ui_callbacks_t callbacks = {
        .on_launcher_select = app_claw_app_select_cb,
        .get_tasks = app_claw_lua_jobs_provider,
        .on_stop_task = app_claw_lua_job_stop_cb,
        .on_stop_all_tasks = app_claw_lua_jobs_stop_all_cb,
    };
    ESP_RETURN_ON_ERROR(system_ui_set_callbacks(&callbacks, NULL), TAG, "set system UI callbacks failed");
    ESP_RETURN_ON_ERROR(cap_lua_register_job_event_cb(app_claw_lua_job_event_cb, NULL),
                        TAG, "register Lua job event callback failed");
#endif
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/* agent-monitor (added): subagent states for the local web UI's status lights. */
esp_err_t app_claw_get_agents_json(const char *chat_id_filter, char *out_json, size_t out_json_size)
{
    /* Heap, not stack: one agent_info_t is ~850 bytes (three 256-byte ids), so a
     * small array overflows the caller's task stack (httpd runs with 8 KB; a
     * stack array here corrupted the HTTP server and crashed it). */
    claw_agent_mgr_agent_info_t *infos = NULL;
    const size_t max_infos = 8;
    size_t count = 0;
    size_t used;
    int written;
    esp_err_t err;

    if (!out_json || out_json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_json[0] = '\0';

    infos = calloc(max_infos, sizeof(*infos));
    if (!infos) {
        snprintf(out_json, out_json_size, "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return ESP_ERR_NO_MEM;
    }

    err = claw_agent_mgr_agent_snapshot(chat_id_filter, infos, max_infos, &count);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Manager not up yet (early boot): report "no agents" rather than an error. */
        snprintf(out_json, out_json_size, "{\"ok\":true,\"agents\":[]}");
        free(infos);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        snprintf(out_json, out_json_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        free(infos);
        return err;
    }

    written = snprintf(out_json, out_json_size, "{\"ok\":true,\"agents\":[");
    if (written < 0 || (size_t)written >= out_json_size) {
        out_json[out_json_size - 1] = '\0';
        return ESP_OK;
    }
    used = (size_t)written;

    for (size_t i = 0; i < count && used + 96 < out_json_size; i++) {
        const char *status;
        bool busy;

        switch (infos[i].status) {
        case CLAW_AGENT_MGR_STATUS_RUNNING: status = "running"; break;
        case CLAW_AGENT_MGR_STATUS_IDLE: status = "idle"; break;
        case CLAW_AGENT_MGR_STATUS_COMPLETED: status = "completed"; break;
        case CLAW_AGENT_MGR_STATUS_CLOSED: status = "closed"; break;
        case CLAW_AGENT_MGR_STATUS_ERROR: status = "error"; break;
        default: status = "empty"; break;
        }
        busy = infos[i].status == CLAW_AGENT_MGR_STATUS_RUNNING &&
               infos[i].phase != CLAW_CORE_AGENT_LOOP_PHASE_IDLE;

        written = snprintf(out_json + used, out_json_size - used,
                           "%s{\"type\":\"%s\",\"status\":\"%s\",\"busy\":%s}",
                           i > 0 ? "," : "",
                           infos[i].agent_type,
                           status,
                           busy ? "true" : "false");
        if (written < 0 || (size_t)written >= out_json_size - used) {
            out_json[used] = '\0';
            break;
        }
        used += (size_t)written;
    }

    snprintf(out_json + used, out_json_size - used, "]}");
    free(infos);
    return ESP_OK;
}

esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid)
{
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    const system_ui_network_state_t state = {
        .sta_connected = sta_connected,
        .ap_ssid = ap_ssid,
    };
    return system_ui_update_network(&state);
#else
    (void)sta_connected;
    (void)ap_ssid;
    return ESP_OK;
#endif
}

#if CONFIG_APP_CLAW_CAP_MEMORY

static bool app_claw_bool_is_true(const char *value)
{
    return value &&
           (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
}

static esp_err_t init_memory(const app_claw_config_t *config, const app_claw_storage_paths_t *paths)
{
    claw_memory_config_t memory_config = {
        .session_root_dir = paths->memory_session_root,
        .memory_root_dir = paths->memory_root_dir,
        .max_message_chars = 4096,
        .max_tool_iterations = CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS,
        .llm = {
            .api_key = config->llm_api_key,
            .backend_type = config->llm_backend_type,
            .model = config->llm_model,
            .base_url = config->llm_base_url,
            .auth_type = config->llm_auth_type,
            .max_tokens_field = config->llm_max_tokens_field,
            .reasoning_effort = config->llm_reasoning_effort,
            .timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10),
            .max_tokens = (uint32_t)strtoul(config->llm_max_tokens, NULL, 10),
            .image_max_bytes = (size_t)strtoul(config->llm_default_image_max_bytes, NULL, 10),
            .supports_tools = app_claw_bool_is_true(config->llm_supports_tools),
            .supports_vision = app_claw_bool_is_true(config->llm_supports_vision),
            .image_remote_url_only = app_claw_bool_is_true(config->llm_image_remote_url_only),
        },
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
        .enable_async_extract_stage_note = true,
#else
        .enable_async_extract_stage_note = false,
#endif
    };
    esp_err_t err = claw_memory_init(&memory_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init claw_memory: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE && CONFIG_APP_CLAW_CAP_LUA
static void app_claw_registry_changed_cb(void *user_ctx)
{
    (void)user_ctx;
    if (!system_ui_is_started()) {
        return;
    }
    esp_err_t err = system_ui_reload_home();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reload UI after launcher change failed: %s", esp_err_to_name(err));
    }
}
#endif

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
static esp_err_t init_skills(const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t) {
                            .session_state_root_dir = paths->memory_session_root,
                            .max_file_bytes = 20 * 1024,
                        }),
                        TAG, "Failed to init claw_skill");
    /* Writable skills take priority over firmware-baked skills. */
    ESP_RETURN_ON_ERROR(claw_skill_add_directory(paths->skills_root_dir), TAG, "Failed to add skills directory");
    ESP_RETURN_ON_ERROR(claw_skill_add_directory(paths->system_skills_root_dir), TAG, "Failed to add system skills directory");
    return claw_skill_reload_registry();
}
#endif

#if CONFIG_APP_CLAW_CAP_LUA
static esp_err_t init_apps(const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(app_registry_init(), TAG, "Failed to init App registry");
    ESP_RETURN_ON_ERROR(app_registry_add_directory(paths->apps_root_dir), TAG, "Failed to add writable App directory");
    ESP_RETURN_ON_ERROR(app_registry_add_directory(paths->system_apps_root_dir), TAG, "Failed to add system App directory");
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    ESP_RETURN_ON_ERROR(app_registry_register_changed_cb(app_claw_registry_changed_cb, NULL), TAG, "Failed to register App listener");
#endif
    return app_registry_reload();
}
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
static esp_err_t app_claw_publish_startup_event(void)
{
    static const char *payload_json =
        "{\"phase\":\"boot_completed\"}";

    ESP_LOGI(TAG, "Publishing startup trigger event: %s/%s",
             APP_STARTUP_EVENT_TYPE, APP_STARTUP_EVENT_KEY);
    return claw_event_router_publish_trigger(APP_STARTUP_EVENT_SOURCE_CAP,
                                             APP_STARTUP_EVENT_TYPE,
                                             APP_STARTUP_EVENT_KEY,
                                             payload_json);
}
#endif

#if CONFIG_APP_CLAW_CAP_CORE
static void app_claw_fill_core_config(const app_claw_config_t *config, claw_core_config_t *core_config)
{
    memset(core_config, 0, sizeof(*core_config));
    core_config->api_key = config->llm_api_key;
    core_config->backend_type = config->llm_backend_type;
    core_config->model = config->llm_model;
    core_config->base_url = config->llm_base_url;
    core_config->auth_type = config->llm_auth_type;
    core_config->max_tokens_field = config->llm_max_tokens_field;
    core_config->reasoning_effort = config->llm_reasoning_effort;
    core_config->timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10);
    core_config->max_tokens = (uint32_t)strtoul(config->llm_max_tokens, NULL, 10);
    core_config->image_max_bytes = (size_t)strtoul(config->llm_default_image_max_bytes, NULL, 10);
    core_config->supports_tools = app_claw_bool_is_true(config->llm_supports_tools);
    core_config->supports_vision = app_claw_bool_is_true(config->llm_supports_vision);
    core_config->image_remote_url_only = app_claw_bool_is_true(config->llm_image_remote_url_only);
    core_config->instance_id = 0;
    core_config->system_prompt = APP_SYSTEM_PROMPT;
#if CONFIG_APP_CLAW_CAP_MEMORY
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
    core_config->persist_context = app_claw_persist_context;
    core_config->request_gate = claw_memory_request_gate_callback;
    core_config->on_request_start = claw_memory_request_start_callback;
    core_config->collect_stage_note = claw_memory_stage_note_callback;
#else
    core_config->persist_context = app_claw_persist_context;
    core_config->request_gate = claw_memory_request_gate_callback;
#endif
#endif
    core_config->call_cap = claw_cap_call_from_core;
    core_config->cap_user_ctx = NULL;
    core_config->task_stack_size = CLAW_TIER_APP_CORE_STACK_SIZE;
    core_config->task_priority = 5;
    core_config->task_core = tskNO_AFFINITY;
    core_config->max_tool_iterations = APP_CLAW_MAX_TOOL_ITERATIONS;
    core_config->request_queue_len = CLAW_TIER_APP_CORE_QUEUE_LEN;
    core_config->response_queue_len = CLAW_TIER_APP_CORE_QUEUE_LEN;
    core_config->max_context_providers = 8;
}
#endif

#if CONFIG_APP_CLAW_CAP_SCHEDULER && CONFIG_APP_CLAW_CAP_SYSTEM
static void app_time_sync_success(bool had_valid_time, void *ctx)
{
    (void)ctx;

    if (!had_valid_time) {
        esp_err_t err = cap_scheduler_handle_time_sync();

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Scheduler rebase after first time sync failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Scheduler rebased after first successful time sync");
        }
    }
}
#endif

// Resolve the storage paths threaded through the capability framework from the
// logical homes registered in claw_paths. This is where app_claw owns the data
// layout (the subdirectory convention); main only decides the mount points.
static esp_err_t build_storage_paths(app_claw_storage_paths_t *paths)
{
    memset(paths, 0, sizeof(*paths));

    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, NULL, paths->fatfs_base_path, sizeof(paths->fatfs_base_path)),
                        TAG, "data home unavailable");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "sessions", paths->memory_session_root, sizeof(paths->memory_session_root)),
                        TAG, "session root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "memory", paths->memory_root_dir, sizeof(paths->memory_root_dir)),
                        TAG, "memory root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "skills", paths->skills_root_dir, sizeof(paths->skills_root_dir)),
                        TAG, "skills root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "apps", paths->apps_root_dir, sizeof(paths->apps_root_dir)),
                        TAG, "App root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "scripts", paths->lua_root_dir, sizeof(paths->lua_root_dir)),
                        TAG, "lua root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "router_rules/router_rules.json", paths->router_rules_path, sizeof(paths->router_rules_path)),
                        TAG, "router rules path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_SYSTEM, ".recovery/router_rules/router_rules.json", paths->router_recovery_path, sizeof(paths->router_recovery_path)),
                        TAG, "router recovery path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "scheduler/schedules.json", paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path)),
                        TAG, "scheduler rules path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_SYSTEM, ".recovery/scheduler/schedules.json", paths->scheduler_recovery_path, sizeof(paths->scheduler_recovery_path)),
                        TAG, "scheduler recovery path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "inbox", paths->im_attachment_root, sizeof(paths->im_attachment_root)),
                        TAG, "inbox path too long");

    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_SYSTEM, "skills", paths->system_skills_root_dir, sizeof(paths->system_skills_root_dir)),
                        TAG, "system skills root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_SYSTEM, "apps", paths->system_apps_root_dir, sizeof(paths->system_apps_root_dir)),
                        TAG, "system App root path too long");

    return ESP_OK;
}

esp_err_t app_claw_start(const app_claw_config_t *config, claw_hw_registry_handle_t registry)
{
    app_claw_storage_paths_t paths;
#if CONFIG_APP_CLAW_CAP_CORE
    claw_core_config_t core_config = {0};
#endif
#if CONFIG_APP_CLAW_CAP_CORE || CONFIG_APP_CLAW_CAP_MEMORY
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    claw_event_router_config_t router_config = {
        .rules_path = NULL,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .agent_submit_timeout_ms = 1000,
        .default_route_messages_to_agent = false,
    };
#endif
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    ESP_RETURN_ON_ERROR(build_storage_paths(&paths), TAG, "Failed to resolve storage paths");

    ESP_RETURN_ON_FALSE(registry != NULL, ESP_ERR_INVALID_ARG, TAG, "hardware registry is required");
#if CONFIG_APP_CLAW_CAP_LUA
    ESP_RETURN_ON_ERROR(cap_lua_set_hw_registry(registry), TAG, "bind Lua hardware registry failed");
#endif
    app_claw_audio_start_service();

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    router_config.default_route_messages_to_agent = true;
    router_config.rules_path = paths.router_rules_path;
    router_config.recovery_rules_path = paths.router_recovery_path;
#endif

#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir(paths.memory_session_root),
                        TAG, "Failed to configure session manager");
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config), TAG, "Failed to init event router");
#endif

#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_init(&(cap_scheduler_config_t) {
                            .schedules_path = paths.scheduler_rules_path,
                            .recovery_schedules_path = paths.scheduler_recovery_path,
                            .tick_ms = 1000,
                            .max_items = 32,
                            .task_stack_size = 6144,
                            .task_priority = 5,
                            .task_core = tskNO_AFFINITY,
                            .publish_event = claw_event_router_publish,
                            .persist_after_fire = true,
                        }),
                        TAG, "Failed to init scheduler");
#endif
#if CONFIG_APP_CLAW_CAP_MEMORY
    ESP_RETURN_ON_ERROR(init_memory(config, &paths), TAG, "Failed to init memory");
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR && (CONFIG_APP_CLAW_CAP_MEMORY || CONFIG_APP_CLAW_CAP_SKILL_MGR)
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_delete_session_handler(app_claw_delete_session_history, NULL),
                        TAG, "Failed to register session delete handler");
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    ESP_RETURN_ON_ERROR(init_skills(&paths), TAG, "Failed to init skills");
#endif
#if CONFIG_APP_CLAW_CAP_LUA
    ESP_RETURN_ON_ERROR(init_apps(&paths), TAG, "Failed to init Apps");
#endif
    ESP_RETURN_ON_ERROR(app_capabilities_init(config, &paths), TAG, "Failed to init capabilities");
#if CONFIG_APP_CLAW_CAP_IM_QQ
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("qq", "qq_send_message"),
                        TAG, "Failed to bind QQ outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("feishu", "feishu_send_message"),
                        TAG, "Failed to bind Feishu outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"),
                        TAG, "Failed to bind Telegram outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("wechat", "wechat_send_message"),
                        TAG, "Failed to bind WeChat outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("web", "local_send_message"),
                        TAG, "Failed to bind Web / local IM outbound");
#endif

#if CONFIG_APP_CLAW_CAP_CORE
    app_claw_fill_core_config(config, &core_config);
    {
        claw_core_context_provider_t base_providers[] = {
            claw_memory_profile_provider,
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
            claw_memory_long_term_provider,
#else
            claw_memory_long_term_lightweight_provider,
#endif
            claw_memory_pinned_core_provider,
            claw_memory_session_history_provider,
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
            s_app_claw_skills_list_provider,
#endif
        };
        const char *root_agent_id = NULL;

        ESP_LOGI(TAG, "Starting root agent backend=%s base_url=%s model=%s token=%s",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(default)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model[0] ? config->llm_model : "(empty)",
                 config->llm_api_key[0] ? "configured" : "missing");
        ESP_RETURN_ON_ERROR(claw_agent_mgr_init(&(claw_agent_mgr_config_t) {
                                .core_config = &core_config,
                                .base_context_providers = base_providers,
                                .base_context_provider_count = sizeof(base_providers) / sizeof(base_providers[0]),
                                .root_agent_system_prompt = APP_ROOT_AGENT_SYSTEM_PROMPT,
                                .subagent_system_prompt = APP_SUBAGENT_SYSTEM_PROMPT,
                            }),
                            TAG, "Failed to init claw_agent_mgr");
        ESP_RETURN_ON_ERROR(claw_agent_mgr_create_root_agent(&root_agent_id),
                            TAG, "Failed to create root agent");
        ESP_LOGI(TAG, "Root agent ready id=%s", root_agent_id ? root_agent_id : "?");
    }
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_start(), TAG, "Failed to start scheduler");
#endif

#if CONFIG_APP_CLAW_CAP_SYSTEM
    ESP_ERROR_CHECK(cap_system_time_sync_service_start(&(cap_system_time_sync_service_config_t) {
                        .network_ready = s_network_ready,
                        .network_ready_ctx = s_network_ready_user_ctx,
#if CONFIG_APP_CLAW_CAP_SCHEDULER
                        .on_sync_success = app_time_sync_success,
#else
                        .on_sync_success = NULL,
#endif
                    }));
#endif

#if CONFIG_APP_CLAW_ENABLE_CLI
    ESP_RETURN_ON_ERROR(app_claw_cli_start(registry), TAG, "Failed to start CLI");
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(app_claw_publish_startup_event(), TAG,
                        "Failed to publish startup event");
#endif
#if CONFIG_APP_CLAW_LUA_DRIVER_USB_HOST_HID && CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    {
        char input_out[128] = {0};
        esp_err_t input_err = cap_lua_run_script_async("/system/scripts/hid_input.lua",
                                                       NULL, 0, "hid_input", "input", true,
                                                       input_out, sizeof(input_out));
        if (input_err != ESP_OK) {
            ESP_LOGW(TAG, "USB HID input bridge not started: %s (%s)",
                     esp_err_to_name(input_err), input_out);
        }
    }
#endif
    ESP_LOGI(TAG, "App Claw runtime started");

    return ESP_OK;
}

esp_err_t app_claw_update_config(const app_claw_config_t *config)
{
#if CONFIG_APP_CLAW_CAP_CORE
    claw_core_config_t core_config = {0};

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    app_claw_fill_core_config(config, &core_config);
    return claw_agent_mgr_update_core_config(&core_config);
#else
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    return ESP_OK;
#endif
}

esp_err_t app_claw_apply_config(const app_claw_config_t *config)
{
    app_claw_save_config_fn save_config = NULL;
    void *save_user_ctx = NULL;
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    save_config = s_save_config;
    save_user_ctx = s_save_config_user_ctx;
    xSemaphoreGive(s_config_lock);

    if (save_config) {
        err = save_config(config, save_user_ctx);
        if (err != ESP_OK) {
            return err;
        }
    }
    return app_claw_update_config(config);
}
