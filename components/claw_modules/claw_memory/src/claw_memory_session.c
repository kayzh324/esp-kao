/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"
#include "claw_task.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "claw_utils_string.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "claw_memory";

#define CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN 4
#define CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE (6 * 1024)
#define CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY 5
#define CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS pdMS_TO_TICKS(60000)

typedef struct claw_memory_pending_summary {
    char *session_id;
    char *summary_list;
    struct claw_memory_pending_summary *next;
} claw_memory_pending_summary_t;

typedef struct claw_memory_async_extract_job {
    uint32_t request_id;
    char *session_id;
    char *user_text;
    char *llm_text;
    claw_memory_message_intent_t message_intent;
    TickType_t created_ticks;
    TickType_t completed_ticks;
    esp_err_t result;
    bool completed;
    /* session-projection (added): when compact is set this job summarizes the
     * history up to keep_from instead of extracting memory facts. */
    bool compact;
    uint32_t keep_from;
    SemaphoreHandle_t done_sem;
    struct claw_memory_async_extract_job *next;
} claw_memory_async_extract_job_t;

typedef struct claw_memory_request_state {
    uint32_t request_id;
    bool manual_write;
    struct claw_memory_request_state *next;
} claw_memory_request_state_t;

typedef struct {
    bool enabled;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    TaskHandle_t task_handle;
    claw_llm_runtime_t *runtime;
    claw_memory_async_extract_job_t *jobs;
} claw_memory_async_extract_state_t;

static claw_memory_pending_summary_t *s_pending_summaries = NULL;
static claw_memory_async_extract_state_t s_async_extract = {0};
static claw_memory_request_state_t *s_request_states = NULL;

/* session-projection (added): runs on the async task to summarize old history. */
static esp_err_t claw_memory_session_compact_run(const char *session_id, uint32_t keep_from);

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint8_t record_type;
    uint8_t backend_format;
} __attribute__((packed)) claw_memory_session_index_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
} __attribute__((packed)) claw_memory_session_index_header_t;

typedef struct {
    claw_memory_session_index_entry_t *entries;
    size_t count;
} claw_memory_session_index_t;

typedef struct {
    size_t start;
    size_t end;
    bool completed;
    bool has_tool_records;
    bool keep_tool_records;
} claw_memory_session_turn_t;

#define CLAW_MEMORY_SESSION_IDX_MAGIC 0x58444843u /* CHDX */
#define CLAW_MEMORY_SESSION_IDX_VERSION 1
#define CLAW_MEMORY_SESSION_COMPACT_TOOL_TURNS 1
#define CLAW_MEMORY_SESSION_SIZE_WARNING \
    "Session history is still too large after compaction. Please create a new conversation by sending \
    the command `/session new [name]`, and delete the old session by `/session delete <name>` due to limited storage space."

_Static_assert(sizeof(claw_memory_session_index_header_t) == 8,
               "session history index header size must remain fixed");
_Static_assert(sizeof(claw_memory_session_index_entry_t) == 10,
               "session history index entry must contain only offset, length, type, and backend format");

static claw_memory_pending_summary_t *claw_memory_find_pending_summary(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

static char *claw_memory_pending_summary_take_summary_list(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;
    claw_memory_pending_summary_t *prev = NULL;
    char *summary_list = NULL;

    if (!session_id || !session_id[0]) {
        return NULL;
    }

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return NULL;
    }

    summary_list = node->summary_list;
    node->summary_list = NULL;
    if (prev) {
        prev->next = node->next;
    } else {
        s_pending_summaries = node->next;
    }
    free(node->session_id);
    free(node);
    return summary_list;
}

static claw_memory_request_state_t *claw_memory_find_request_state(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;

    while (node) {
        if (node->request_id == request_id) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

esp_err_t claw_memory_request_mark_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = NULL;

    if (request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    node = claw_memory_find_request_state(request_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->request_id = request_id;
        node->next = s_request_states;
        s_request_states = node;
    }

    node->manual_write = true;
    return ESP_OK;
}

static bool claw_memory_request_take_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;
    claw_memory_request_state_t *prev = NULL;
    bool manual_write = false;

    if (request_id == 0) {
        return false;
    }

    while (node) {
        if (node->request_id == request_id) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return false;
    }

    manual_write = node->manual_write;
    if (prev) {
        prev->next = node->next;
    } else {
        s_request_states = node->next;
    }
    free(node);
    return manual_write;
}

static claw_memory_async_extract_job_t *claw_memory_async_extract_find_job_locked(uint32_t request_id)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    while (job) {
        if (job->request_id == request_id) {
            return job;
        }
        job = job->next;
    }
    return NULL;
}

static void claw_memory_async_extract_free_job(claw_memory_async_extract_job_t *job)
{
    if (!job) {
        return;
    }
    if (job->done_sem) {
        vSemaphoreDelete(job->done_sem);
    }
    free(job->session_id);
    free(job->user_text);
    free(job->llm_text);
    free(job);
}

static void claw_memory_async_extract_sweep_locked(TickType_t now_ticks)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;
    claw_memory_async_extract_job_t *prev = NULL;

    while (job) {
        claw_memory_async_extract_job_t *next = job->next;
        bool expired = job->completed &&
                       (now_ticks - job->completed_ticks) >= CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS;

        if (expired) {
            if (prev) {
                prev->next = next;
            } else {
                s_async_extract.jobs = next;
            }
            claw_memory_async_extract_free_job(job);
        } else {
            prev = job;
        }
        job = next;
    }
}

static void claw_memory_async_extract_task(void *arg)
{
    (void)arg;

    while (true) {
        claw_memory_async_extract_job_t *job = NULL;
        char *llm_text = NULL;
        claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;
        esp_err_t err;

        if (xQueueReceive(s_async_extract.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!job) {
            continue;
        }

        /* session-projection (added): compaction jobs are self-contained and are
         * freed here; they are not tracked by the extract consumer path. */
        if (job->compact) {
            esp_err_t cerr = claw_memory_session_compact_run(job->session_id, job->keep_from);

            if (cerr != ESP_OK) {
                ESP_LOGW(TAG, "compaction job failed session=%s: %s",
                         job->session_id ? job->session_id : "?", esp_err_to_name(cerr));
            }
            claw_memory_async_extract_free_job(job);
            continue;
        }

        /*
         * opencode-go provider support (added): pass the job's session id so
         * the async extract LLM call also carries the x-opencode-session
         * header when the opencode_go backend is active.
         */
        err = claw_memory_auto_extract_prepare_with_runtime(s_async_extract.runtime,
                                                            job->session_id,
                                                            job->user_text,
                                                            &message_intent,
                                                            &llm_text);

        if (s_async_extract.lock && xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            if (job->llm_text) {
                free(job->llm_text);
            }
            job->llm_text = llm_text;
            job->message_intent = message_intent;
            job->result = err;
            job->completed = true;
            job->completed_ticks = xTaskGetTickCount();
            /* Signal completion while still holding the lock. Once `completed`
             * is visible, the consumer (take_summary_list) may detach and free
             * this job; doing the give after releasing the lock would be a
             * use-after-free on `job`/`job->done_sem`. Holding the lock across
             * the give keeps the consumer's free serialized after us. */
            if (job->done_sem) {
                xSemaphoreGive(job->done_sem);
            }
            xSemaphoreGive(s_async_extract.lock);
        } else {
            free(llm_text);
        }
    }
}

static char *claw_memory_async_extract_take_summary_list(const claw_core_request_t *request,
                                                        bool apply_result)
{
    claw_memory_async_extract_job_t *job = NULL;
    claw_memory_async_extract_job_t *prev = NULL;
    SemaphoreHandle_t done_sem = NULL;
    char *llm_text = NULL;
    char *summary_list = NULL;
    claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;

    if (!request || !request->request_id || !s_async_extract.enabled || !s_async_extract.lock) {
        return NULL;
    }

    while (true) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }

        prev = NULL;
        job = s_async_extract.jobs;
        while (job) {
            if (job->request_id == request->request_id) {
                break;
            }
            prev = job;
            job = job->next;
        }

        if (!job) {
            claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
            xSemaphoreGive(s_async_extract.lock);
            return NULL;
        }

        if (job->completed) {
            llm_text = job->llm_text;
            job->llm_text = NULL;
            message_intent = job->message_intent;
            if (prev) {
                prev->next = job->next;
            } else {
                s_async_extract.jobs = job->next;
            }
            xSemaphoreGive(s_async_extract.lock);
            claw_memory_async_extract_free_job(job);
            if (!apply_result) {
                free(llm_text);
                return NULL;
            }
            if (claw_memory_auto_extract_apply_result(llm_text,
                                                      message_intent,
                                                      &summary_list) != ESP_OK) {
                free(llm_text);
                free(summary_list);
                return NULL;
            }
            free(llm_text);
            return summary_list;
        }

        done_sem = job->done_sem;
        xSemaphoreGive(s_async_extract.lock);
        ESP_LOGI(TAG, "stage note provider waiting request=%" PRIu32, request->request_id);
        if (!done_sem || xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }
    }
}

static void claw_memory_async_extract_deinit(void)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    s_async_extract.jobs = NULL;
    while (job) {
        claw_memory_async_extract_job_t *next = job->next;

        claw_memory_async_extract_free_job(job);
        job = next;
    }
    if (s_async_extract.task_handle) {
        claw_task_delete(s_async_extract.task_handle);
        s_async_extract.task_handle = NULL;
    }
    if (s_async_extract.queue) {
        vQueueDelete(s_async_extract.queue);
        s_async_extract.queue = NULL;
    }
    if (s_async_extract.lock) {
        vSemaphoreDelete(s_async_extract.lock);
        s_async_extract.lock = NULL;
    }
    if (s_async_extract.runtime) {
        claw_llm_runtime_deinit(s_async_extract.runtime);
        s_async_extract.runtime = NULL;
    }
    s_async_extract.enabled = false;
}

esp_err_t claw_memory_async_extract_init(const claw_memory_config_t *config)
{
    BaseType_t task_result;
    const claw_memory_llm_config_t *llm = NULL;
    char *error_message = NULL;
    esp_err_t err;

    claw_memory_async_extract_deinit();

    if (!config || !config->enable_async_extract_stage_note) {
        return ESP_OK;
    }
    llm = &config->llm;

    if (!llm->api_key || !llm->api_key[0] ||
        !llm->model || !llm->model[0] ||
        !llm->backend_type || !llm->backend_type[0]) {
        ESP_LOGI(TAG, "Async memory extract disabled: LLM config incomplete");
        return ESP_OK;
    }

    err = claw_llm_runtime_init(&s_async_extract.runtime,
                                &(claw_llm_runtime_config_t) {
                                    .api_key = llm->api_key,
                                    .backend_type = llm->backend_type,
                                    .model = llm->model,
                                    .base_url = llm->base_url,
                                    .auth_type = llm->auth_type,
                                    .max_tokens_field = llm->max_tokens_field,
                                    .timeout_ms = llm->timeout_ms,
                                    .max_tokens = llm->max_tokens,
                                    .image_max_bytes = llm->image_max_bytes,
                                    .supports_tools = llm->supports_tools,
                                    .supports_vision = llm->supports_vision,
                                    .image_remote_url_only = llm->image_remote_url_only,
                                },
                                &error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to init async memory extract runtime: %s",
                 error_message ? error_message : esp_err_to_name(err));
        free(error_message);
        claw_memory_async_extract_deinit();
        return err;
    }
    free(error_message);

    s_async_extract.lock = xSemaphoreCreateMutex();
    s_async_extract.queue = xQueueCreate(CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN,
                                         sizeof(claw_memory_async_extract_job_t *));
    if (!s_async_extract.lock || !s_async_extract.queue) {
        claw_memory_async_extract_deinit();
        return ESP_ERR_NO_MEM;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = "claw_mem_extract",
                                        .stack_size = CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE,
                                        .priority = CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY,
                                        .core_id = tskNO_AFFINITY,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_memory_async_extract_task,
                                    NULL,
                                    &s_async_extract.task_handle);
    if (task_result != pdPASS) {
        claw_memory_async_extract_deinit();
        return ESP_FAIL;
    }

    s_async_extract.enabled = true;
    ESP_LOGI(TAG, "Async memory extract worker ready");
    return ESP_OK;
}

esp_err_t claw_memory_async_extract_ensure_started(const claw_core_request_t *request)
{
    claw_memory_async_extract_job_t *job = NULL;

    if (!request || !request->request_id || !request->session_id || !request->session_id[0] ||
        !request->user_text || !request->user_text[0] || !s_async_extract.enabled) {
        return ESP_OK;
    }
    if (!s_async_extract.lock || !s_async_extract.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
    if (claw_memory_async_extract_find_job_locked(request->request_id)) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_OK;
    }

    job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_ERR_NO_MEM;
    }

    job->request_id = request->request_id;
    job->session_id = dup_printf("%s", request->session_id);
    job->user_text = dup_printf("%s", request->user_text);
    job->done_sem = xSemaphoreCreateBinary();
    job->created_ticks = xTaskGetTickCount();
    if (!job->session_id || !job->user_text || !job->done_sem) {
        xSemaphoreGive(s_async_extract.lock);
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_NO_MEM;
    }

    job->next = s_async_extract.jobs;
    s_async_extract.jobs = job;
    xSemaphoreGive(s_async_extract.lock);

    if (xQueueSend(s_async_extract.queue, &job, 0) != pdTRUE) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            claw_memory_async_extract_job_t *node = s_async_extract.jobs;
            claw_memory_async_extract_job_t *prev = NULL;

            while (node) {
                if (node == job) {
                    if (prev) {
                        prev->next = node->next;
                    } else {
                        s_async_extract.jobs = node->next;
                    }
                    break;
                }
                prev = node;
                node = node->next;
            }
            xSemaphoreGive(s_async_extract.lock);
        }
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "async extract job created request=%" PRIu32 " session=%s",
             request->request_id,
             request->session_id);
    return ESP_OK;
}

static bool session_history_record_type_valid(uint8_t record_type)
{
    switch ((claw_core_context_record_type_t)record_type) {
    case CLAW_CORE_CONTEXT_RECORD_USER:
    case CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL:
    case CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL:
    case CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT:
    case CLAW_CORE_CONTEXT_RECORD_SUMMARY:
        return true;
    default:
        return false;
    }
}

static bool session_history_backend_format_valid(uint8_t backend_format)
{
    switch ((claw_memory_backend_format_t)backend_format) {
    case CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN:
    case CLAW_MEMORY_BACKEND_FORMAT_OPENAI:
    case CLAW_MEMORY_BACKEND_FORMAT_ANTHROPIC:
        return true;
    default:
        return false;
    }
}

static char *session_history_idx_path_dup(const char *data_path)
{
    if (!data_path || !data_path[0]) {
        return NULL;
    }
    return dup_printf("%s.idx", data_path);
}

static char *session_history_blocked_path_dup(const char *data_path)
{
    if (!data_path || !data_path[0]) {
        return NULL;
    }
    return dup_printf("%s.blocked", data_path);
}

static bool session_history_path_exists(const char *path)
{
    struct stat st = {0};

    if (!path) {
        return false;
    }
    return stat(path, &st) == 0;
}

static esp_err_t session_history_unlink_path(const char *path, bool *out_deleted_any)
{
    if (!path || !path[0] || !out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }

    if (unlink(path) == 0) {
        *out_deleted_any = true;
        return ESP_OK;
    }
    if (errno == ENOENT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "delete session history artifact %s failed: errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t session_history_close_file(FILE *file);

static esp_err_t session_history_mark_blocked(const char *data_path)
{
    char *blocked_path = NULL;
    FILE *file = NULL;
    esp_err_t err = ESP_OK;

    if (!data_path || !data_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    blocked_path = session_history_blocked_path_dup(data_path);
    if (!blocked_path) {
        return ESP_ERR_NO_MEM;
    }

    file = fopen(blocked_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "create session history blocked marker %s failed: errno=%d",
                 blocked_path,
                 errno);
        err = ESP_FAIL;
    } else if (session_history_close_file(file) != ESP_OK) {
        err = ESP_FAIL;
    }

    free(blocked_path);
    return err;
}

static bool session_history_session_blocked(const char *session_id)
{
    char *data_path = NULL;
    char *blocked_path = NULL;
    bool blocked = false;

    if (!session_id || !session_id[0] || !s_memory.initialized) {
        return false;
    }

    data_path = claw_memory_session_path_dup(session_id);
    if (!data_path) {
        return false;
    }
    blocked_path = session_history_blocked_path_dup(data_path);
    if (blocked_path) {
        blocked = session_history_path_exists(blocked_path);
    }

    free(blocked_path);
    free(data_path);
    return blocked;
}

static void session_history_index_header_init(claw_memory_session_index_header_t *header)
{
    memset(header, 0, sizeof(*header));
    header->magic = CLAW_MEMORY_SESSION_IDX_MAGIC;
    header->version = CLAW_MEMORY_SESSION_IDX_VERSION;
    header->entry_size = sizeof(claw_memory_session_index_entry_t);
}

static bool session_history_index_header_valid(const claw_memory_session_index_header_t *header)
{
    if (!header) {
        return false;
    }
    if (header->magic != CLAW_MEMORY_SESSION_IDX_MAGIC) {
        ESP_LOGW(TAG, "Invalid session history idx magic");
        return false;
    }
    if (header->version != CLAW_MEMORY_SESSION_IDX_VERSION) {
        ESP_LOGW(TAG,
                 "Unsupported session history idx version %u",
                 (unsigned)header->version);
        return false;
    }
    if (header->entry_size != sizeof(claw_memory_session_index_entry_t)) {
        ESP_LOGW(TAG,
                 "Invalid session history idx entry size %u",
                 (unsigned)header->entry_size);
        return false;
    }
    return true;
}

static size_t session_history_record_object_len(const claw_memory_session_index_entry_t *entry)
{
    if (!entry || entry->length == 0) {
        return 0;
    }
    return entry->length - 1;
}

static esp_err_t session_history_write_index_header(FILE *file)
{
    claw_memory_session_index_header_t header;

    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    session_history_index_header_init(&header);
    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history idx header failed");
        return ESP_FAIL;
    }
    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(TAG, "write session history idx header failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t session_history_entry_count_from_idx_size(size_t idx_size,
                                                           size_t *out_count)
{
    size_t payload_size;

    if (!out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (idx_size < sizeof(claw_memory_session_index_header_t)) {
        ESP_LOGW(TAG, "Session history idx file is too small");
        return ESP_ERR_INVALID_STATE;
    }
    payload_size = idx_size - sizeof(claw_memory_session_index_header_t);
    if ((payload_size % sizeof(claw_memory_session_index_entry_t)) != 0) {
        ESP_LOGW(TAG, "Session history idx file has a partial entry");
        return ESP_ERR_INVALID_STATE;
    }
    *out_count = payload_size / sizeof(claw_memory_session_index_entry_t);
    return ESP_OK;
}

static void session_history_index_free(claw_memory_session_index_t *index)
{
    if (!index) {
        return;
    }
    free(index->entries);
    memset(index, 0, sizeof(*index));
}

static esp_err_t session_history_read_index_file(const char *idx_path,
                                                 claw_memory_session_index_t *out_index)
{
    FILE *file = NULL;
    claw_memory_session_index_header_t header;
    size_t idx_size;
    size_t entry_count = 0;
    size_t i;
    esp_err_t err;

    if (!idx_path || !out_index) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_index, 0, sizeof(*out_index));

    file = fopen(idx_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "open session history idx %s failed: errno=%d", idx_path, errno);
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGW(TAG, "read session history idx header failed");
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (!session_history_index_header_valid(&header)) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    idx_size = file_size_bytes(idx_path);
    err = session_history_entry_count_from_idx_size(idx_size, &entry_count);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (entry_count > 0) {
        out_index->entries = calloc(entry_count, sizeof(*out_index->entries));
        if (!out_index->entries) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        if (fread(out_index->entries,
                  sizeof(*out_index->entries),
                  entry_count,
                  file) != entry_count) {
            ESP_LOGW(TAG, "read session history idx entries failed");
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
    }

    for (i = 0; i < entry_count; i++) {
        if (!session_history_record_type_valid(out_index->entries[i].record_type)) {
            ESP_LOGW(TAG,
                     "Invalid session history record_type entry=%u type=%u",
                     (unsigned)i,
                     (unsigned)out_index->entries[i].record_type);
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        if (!session_history_backend_format_valid(out_index->entries[i].backend_format)) {
            ESP_LOGW(TAG,
                     "Invalid session history backend_format entry=%u format=%u",
                     (unsigned)i,
                     (unsigned)out_index->entries[i].backend_format);
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
    }

    out_index->count = entry_count;
    err = ESP_OK;

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        session_history_index_free(out_index);
    }
    return err;
}

static esp_err_t session_history_validate_pair(const char *data_path,
                                               const char *idx_path,
                                               claw_memory_session_index_t *out_index)
{
    claw_memory_session_index_t index = {0};
    size_t data_size;
    size_t i;
    esp_err_t err;

    if (!data_path || !idx_path || !out_index) {
        return ESP_ERR_INVALID_ARG;
    }

    err = session_history_read_index_file(idx_path, &index);
    if (err != ESP_OK) {
        return err;
    }

    if (!session_history_path_exists(data_path)) {
        session_history_index_free(&index);
        return ESP_ERR_NOT_FOUND;
    }

    data_size = file_size_bytes(data_path);
    if (index.count == 0 && data_size > 0) {
        ESP_LOGW(TAG, "Session history data has no matching idx entries");
        session_history_index_free(&index);
        return ESP_ERR_INVALID_STATE;
    }
    for (i = 0; i < index.count; i++) {
        const claw_memory_session_index_entry_t *entry = &index.entries[i];

        if (entry->length == 0 ||
                entry->offset > data_size ||
                entry->length > data_size - entry->offset) {
            ESP_LOGW(TAG,
                     "Invalid session history entry=%u offset=%" PRIu32 " length=%" PRIu32 " data_size=%u",
                     (unsigned)i,
                     entry->offset,
                     entry->length,
                     (unsigned)data_size);
            session_history_index_free(&index);
            return ESP_ERR_INVALID_STATE;
        }
    }

    *out_index = index;
    return ESP_OK;
}

static esp_err_t session_history_read_record_text(FILE *file,
                                                  const claw_memory_session_index_entry_t *entry,
                                                  char **out_text)
{
    char *text = NULL;
    size_t object_len;

    if (!file || !entry || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    object_len = session_history_record_object_len(entry);
    if (object_len == 0) {
        ESP_LOGW(TAG,
                 "Invalid session history entry offset=%" PRIu32 " length=%" PRIu32,
                 entry->offset,
                 entry->length);
        return ESP_ERR_INVALID_STATE;
    }

    text = calloc(1, object_len + 1);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    if (fseek(file, (long)entry->offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history record failed");
        free(text);
        return ESP_FAIL;
    }
    if (fread(text, 1, object_len, file) != object_len) {
        ESP_LOGE(TAG, "read session history record failed");
        free(text);
        return ESP_FAIL;
    }
    if (fgetc(file) != '\n') {
        ESP_LOGW(TAG, "session history record missing newline separator");
        free(text);
        return ESP_ERR_INVALID_STATE;
    }

    *out_text = text;
    return ESP_OK;
}

static esp_err_t session_history_append_loaded_record(cJSON *records,
                                                      cJSON *record,
                                                      bool expand_array)
{
    cJSON *item;

    if (!records || !record) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!expand_array || !cJSON_IsArray(record)) {
        if (!cJSON_AddItemToArray(records, record)) {
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    while (cJSON_GetArraySize(record) > 0) {
        item = cJSON_DetachItemFromArray(record, 0);
        if (!item) {
            cJSON_Delete(record);
            return ESP_ERR_INVALID_STATE;
        }
        if (!cJSON_AddItemToArray(records, item)) {
            cJSON_Delete(item);
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(record);
    return ESP_OK;
}

static bool session_history_backend_mismatch(uint8_t record_format);
static esp_err_t session_history_degrade_assistant_final(cJSON *record,
                                                         cJSON **out_record);

static esp_err_t session_history_load_indexed_json(FILE *file,
                                                   const claw_memory_session_index_t *index,
                                                   char **out_json)
{
    cJSON *records = NULL;
    char *json = NULL;
    size_t i;
    esp_err_t err = ESP_OK;
    /* session-projection (added): the latest summary record replaces every record
     * before its keep_from index when history is replayed. */
    size_t keep_from = 0;
    char *summary_text = NULL;

    if (!file || !index || !out_json || index->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    records = cJSON_CreateArray();
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    /* Pass 1: find the latest summary record. */
    for (i = 0; i < index->count; i++) {
        const claw_memory_session_index_entry_t *entry = &index->entries[i];
        char *record_text = NULL;
        cJSON *record = NULL;
        cJSON *sum = NULL;
        cJSON *kf = NULL;

        if (entry->record_type != CLAW_CORE_CONTEXT_RECORD_SUMMARY) {
            continue;
        }
        err = session_history_read_record_text(file, entry, &record_text);
        if (err != ESP_OK) {
            goto cleanup;
        }
        record = cJSON_ParseWithOpts(record_text, NULL, 1);
        free(record_text);
        if (!record) {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        sum = cJSON_GetObjectItemCaseSensitive(record, "summary");
        kf = cJSON_GetObjectItemCaseSensitive(record, "keep_from");
        if (cJSON_IsString(sum) && cJSON_IsNumber(kf)) {
            char *dup = dup_printf("%s", sum->valuestring);

            if (!dup) {
                cJSON_Delete(record);
                err = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            free(summary_text);
            summary_text = dup;
            keep_from = (size_t)kf->valueint;
        }
        cJSON_Delete(record);
    }

    if (summary_text) {
        cJSON *summary_msg = cJSON_CreateObject();

        if (!summary_msg) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddStringToObject(summary_msg, "role", "system");
        cJSON_AddStringToObject(summary_msg, "content", summary_text);
        cJSON_AddItemToArray(records, summary_msg);
    }

    for (i = 0; i < index->count; i++) {
        const claw_memory_session_index_entry_t *entry;
        claw_core_context_record_type_t type;
        char *record_text = NULL;
        cJSON *record = NULL;

        entry = &index->entries[i];
        type = (claw_core_context_record_type_t)entry->record_type;

        /* Records before the keep point are represented by the summary; summary
         * records themselves never become messages. */
        if (i < keep_from || type == CLAW_CORE_CONTEXT_RECORD_SUMMARY) {
            continue;
        }

        err = session_history_read_record_text(file, entry, &record_text);
        if (err != ESP_OK) {
            goto cleanup;
        }

        record = cJSON_ParseWithOpts(record_text, NULL, 1);
        free(record_text);
        if (!record) {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        /* reasoning-content (added): the upstream provider requires EVERY replayed
         * assistant turn to carry a reasoning_content field in thinking mode, even
         * when the model produced no reasoning. A stored turn can lack the field
         * either because the provider omitted it, or because an earlier build
         * stripped it; either way the request is rejected with HTTP 400
         * ("reasoning_content ... must be passed back to the API"). Normalize it
         * to an empty string rather than dropping or inventing content. */
        {
            const cJSON *role = cJSON_GetObjectItemCaseSensitive(record, "role");

            if (cJSON_IsString(role) && strcmp(role->valuestring, "assistant") == 0 &&
                    !cJSON_GetObjectItemCaseSensitive(record, "reasoning_content")) {
                cJSON_AddStringToObject(record, "reasoning_content", "");
            }
        }

        if (session_history_backend_mismatch(entry->backend_format)) {
            if (type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL ||
                    type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT) {
                cJSON_Delete(record);
                continue;
            }
            if (type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL) {
                cJSON *degraded = NULL;

                err = session_history_degrade_assistant_final(record, &degraded);
                if (err == ESP_ERR_NOT_FOUND) {
                    err = ESP_OK;
                    continue;
                }
                if (err != ESP_OK) {
                    goto cleanup;
                }
                record = degraded;
            }
        }

        err = session_history_append_loaded_record(records,
                                                   record,
                                                   type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    json = cJSON_PrintUnformatted(records);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_json = json;
    json = NULL;

cleanup:
    free(summary_text);
    if (json) {
        cJSON_free(json);
    }
    if (records) {
        cJSON_Delete(records);
    }
    return err;
}

static esp_err_t session_history_close_file(FILE *file)
{
    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "close session history failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t session_history_recreate_file(const char *data_path,
                                               const char *idx_path)
{
    FILE *data_file = NULL;
    FILE *idx_file = NULL;
    esp_err_t err;

    if (!data_path || !idx_path) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_parent_dir(data_path);
    if (err != ESP_OK) {
        return err;
    }

    data_file = fopen(data_path, "w+b");
    if (!data_file) {
        ESP_LOGE(TAG, "create session history %s failed: errno=%d", data_path, errno);
        return ESP_FAIL;
    }
    err = session_history_close_file(data_file);
    data_file = NULL;
    if (err != ESP_OK) {
        return err;
    }

    idx_file = fopen(idx_path, "w+b");
    if (!idx_file) {
        ESP_LOGE(TAG, "create session history idx %s failed: errno=%d", idx_path, errno);
        return ESP_FAIL;
    }
    err = session_history_write_index_header(idx_file);
    if (err != ESP_OK) {
        fclose(idx_file);
        return err;
    }

    return session_history_close_file(idx_file);
}

static bool session_history_backend_mismatch(uint8_t record_format)
{
    if (s_memory.backend_format == CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN ||
            record_format == CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN) {
        return false;
    }
    return record_format != (uint8_t)s_memory.backend_format;
}

static esp_err_t session_history_degrade_assistant_final(cJSON *record,
                                                        cJSON **out_record)
{
    cJSON *content = NULL;
    cJSON *block = NULL;
    cJSON *fallback = NULL;
    char *text = NULL;
    size_t total_len = 0;
    size_t offset = 0;

    if (!record || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_record = NULL;

    content = cJSON_GetObjectItem(record, "content");
    if (cJSON_IsString(content)) {
        *out_record = record;
        return ESP_OK;
    }
    if (!cJSON_IsArray(content)) {
        cJSON_Delete(record);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        cJSON *block_text = cJSON_GetObjectItem(block, "text");

        if (cJSON_IsString(type) && type->valuestring &&
                strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(block_text) && block_text->valuestring) {
            total_len += strlen(block_text->valuestring);
        }
    }
    if (total_len == 0) {
        cJSON_Delete(record);
        return ESP_ERR_NOT_FOUND;
    }

    text = calloc(1, total_len + 1);
    if (!text) {
        cJSON_Delete(record);
        return ESP_ERR_NO_MEM;
    }
    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        cJSON *block_text = cJSON_GetObjectItem(block, "text");

        if (cJSON_IsString(type) && type->valuestring &&
                strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(block_text) && block_text->valuestring) {
            size_t len = strlen(block_text->valuestring);

            memcpy(text + offset, block_text->valuestring, len);
            offset += len;
        }
    }

    fallback = cJSON_CreateObject();
    if (!fallback ||
            !cJSON_AddStringToObject(fallback, "role", "assistant") ||
            !cJSON_AddStringToObject(fallback, "content", text)) {
        cJSON_Delete(fallback);
        free(text);
        cJSON_Delete(record);
        return ESP_ERR_NO_MEM;
    }

    free(text);
    cJSON_Delete(record);
    *out_record = fallback;
    return ESP_OK;
}

static esp_err_t claw_memory_session_load_json_alloc(const char *session_id, char **out_json)
{
    char *data_path = NULL;
    char *idx_path = NULL;
    FILE *file = NULL;
    claw_memory_session_index_t index = {0};
    char *json = NULL;
    esp_err_t err;
    bool data_exists;
    bool idx_exists;
    bool reset_file = false;
    const char *reset_reason = NULL;

    if (!session_id || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_json = NULL;
    data_path = claw_memory_session_path_dup(session_id);
    if (!data_path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        return ESP_ERR_NO_MEM;
    }
    idx_path = session_history_idx_path_dup(data_path);
    if (!idx_path) {
        free(data_path);
        return ESP_ERR_NO_MEM;
    }

    data_exists = session_history_path_exists(data_path);
    idx_exists = session_history_path_exists(idx_path);
    if (!data_exists && !idx_exists) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (!data_exists || !idx_exists) {
        reset_file = true;
        reset_reason = "missing data/index pair";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = session_history_validate_pair(data_path, idx_path, &index);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "invalid data/index pair";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (index.count == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    file = fopen(data_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "open session history %s failed: errno=%d", data_path, errno);
        err = ESP_FAIL;
        goto cleanup;
    }

    err = session_history_load_indexed_json(file, &index, &json);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "read indexed records failed";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (reset_file) {
        esp_err_t reset_err;

        ESP_LOGW(TAG, "Resetting session history %s: %s", data_path, reset_reason);
        reset_err = session_history_recreate_file(data_path, idx_path);
        if (reset_err != ESP_OK) {
            ESP_LOGE(TAG, "reset session history %s failed: %s",
                     data_path,
                     esp_err_to_name(reset_err));
            err = reset_err;
        }
    }
    session_history_index_free(&index);
    free(data_path);
    free(idx_path);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    *out_json = json;
    return ESP_OK;
}

static esp_err_t session_history_open_pair_for_append(const char *data_path,
                                                      const char *idx_path,
                                                      FILE **out_data_file,
                                                      FILE **out_idx_file)
{
    FILE *data_file = NULL;
    FILE *idx_file = NULL;
    claw_memory_session_index_t index = {0};
    esp_err_t err = ESP_OK;

    if (!data_path || !idx_path || !out_data_file || !out_idx_file) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data_file = NULL;
    *out_idx_file = NULL;

    err = session_history_validate_pair(data_path, idx_path, &index);
    session_history_index_free(&index);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            bool data_exists = session_history_path_exists(data_path);
            bool idx_exists = session_history_path_exists(idx_path);

            if (data_exists != idx_exists) {
                ESP_LOGW(TAG, "Reinitializing mismatched session history pair %s", data_path);
            }
        } else {
            ESP_LOGW(TAG, "Reinitializing legacy or invalid session history pair %s", data_path);
        }
        err = session_history_recreate_file(data_path, idx_path);
        if (err != ESP_OK) {
            return err;
        }
    }

    data_file = fopen(data_path, "ab");
    if (!data_file) {
        ESP_LOGE(TAG, "open session history %s for append failed: errno=%d", data_path, errno);
        return ESP_FAIL;
    }
    idx_file = fopen(idx_path, "ab");
    if (!idx_file) {
        ESP_LOGE(TAG, "open session history idx %s for append failed: errno=%d", idx_path, errno);
        fclose(data_file);
        return ESP_FAIL;
    }

    *out_data_file = data_file;
    *out_idx_file = idx_file;
    return ESP_OK;
}

static esp_err_t session_history_append_indexed_record(FILE *data_file,
                                                       FILE *idx_file,
                                                       claw_core_context_record_type_t record_type,
                                                       const char *json_text,
                                                       const char *role,
                                                       const char *text)
{
    cJSON *record = NULL;
    char *normalized = NULL;
    char *record_text = NULL;
    claw_memory_session_index_entry_t entry = {0};
    uint32_t offset = 0;
    uint32_t length = 0;
    size_t max_chars = s_memory.max_message_chars;
    size_t normalized_size;
    esp_err_t err;

    if (!data_file || !idx_file ||
            ((!json_text || !json_text[0]) && (!role || !text)) ||
            !session_history_record_type_valid((uint8_t)record_type)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(data_file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "seek session history EOF failed");
        return ESP_FAIL;
    }

    if (!json_text || !json_text[0]) {
        normalized_size = claw_memory_text_buffer_size(max_chars);
        normalized = calloc(1, normalized_size);
        if (!normalized) {
            return ESP_ERR_NO_MEM;
        }
        claw_memory_normalize_session_text(text, normalized, normalized_size, max_chars);

        record = cJSON_CreateObject();
        if (!record ||
                !cJSON_AddStringToObject(record, "role", role) ||
                !cJSON_AddStringToObject(record, "content", normalized)) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        record_text = cJSON_PrintUnformatted(record);
        if (!record_text) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        json_text = record_text;
    }

    err = claw_memory_write_session_raw_record(data_file,
                                               json_text,
                                               &offset,
                                               &length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "write session history %s record failed: %s",
                 role ? role : "raw",
                 esp_err_to_name(err));
        goto cleanup;
    }

    entry.offset = offset;
    entry.length = length;
    entry.record_type = (uint8_t)record_type;
    entry.backend_format = (uint8_t)s_memory.backend_format;
    if (fwrite(&entry, 1, sizeof(entry), idx_file) != sizeof(entry)) {
        ESP_LOGE(TAG, "write session history idx entry failed");
        err = ESP_FAIL;
        goto cleanup;
    }
    err = ESP_OK;

cleanup:
    if (record_text) {
        cJSON_free(record_text);
    }
    cJSON_Delete(record);
    free(normalized);
    return err;
}

static esp_err_t session_history_analyze_turns(const claw_memory_session_index_t *index,
                                               claw_memory_session_turn_t **out_turns,
                                               size_t *out_turn_count)
{
    claw_memory_session_turn_t *turns = NULL;
    size_t turn_count = 0;
    size_t kept_tool_turns = 0;
    size_t i;
    size_t t;

    if (!index || !out_turns || !out_turn_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_turns = NULL;
    *out_turn_count = 0;
    if (index->count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    turns = calloc(index->count, sizeof(*turns));
    if (!turns) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < index->count; i++) {
        claw_core_context_record_type_t type =
            (claw_core_context_record_type_t)index->entries[i].record_type;

        if (i == 0 || type == CLAW_CORE_CONTEXT_RECORD_USER) {
            if (turn_count > 0) {
                turns[turn_count - 1].end = i;
            }
            turns[turn_count].start = i;
            turn_count++;
        }
    }
    if (turn_count == 0) {
        free(turns);
        return ESP_ERR_NOT_FOUND;
    }
    turns[turn_count - 1].end = index->count;

    for (t = 0; t < turn_count; t++) {
        for (i = turns[t].start; i < turns[t].end; i++) {
            claw_core_context_record_type_t type =
                (claw_core_context_record_type_t)index->entries[i].record_type;

            if (type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL) {
                turns[t].completed = true;
            } else if (type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL ||
                       type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT) {
                turns[t].has_tool_records = true;
            }
        }
    }

    for (t = turn_count; t > 0; t--) {
        size_t turn_index = t - 1;

        if (turns[turn_index].completed &&
                turns[turn_index].has_tool_records &&
                kept_tool_turns < CLAW_MEMORY_SESSION_COMPACT_TOOL_TURNS) {
            turns[turn_index].keep_tool_records = true;
            kept_tool_turns++;
        }
    }

    *out_turns = turns;
    *out_turn_count = turn_count;
    return ESP_OK;
}

static bool session_history_compact_keep_record(const claw_memory_session_turn_t *turns,
                                                size_t turn_count,
                                                size_t turn_index,
                                                claw_core_context_record_type_t type)
{
    if (!turns || turn_index >= turn_count) {
        return false;
    }
    if (type == CLAW_CORE_CONTEXT_RECORD_USER ||
            type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL) {
        return true;
    }
    if (turn_index + 1 == turn_count && !turns[turn_index].completed) {
        return true;
    }
    return turns[turn_index].keep_tool_records &&
           (type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL ||
            type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT);
}

static esp_err_t session_history_plan_compaction(const claw_memory_session_index_t *index,
                                                 const claw_memory_session_turn_t *turns,
                                                 size_t turn_count,
                                                 size_t *out_data_size,
                                                 size_t *out_entry_count)
{
    size_t compacted_data_size = 0;
    size_t compacted_entry_count = 0;
    size_t turn_index = 0;
    size_t i;

    if (!index || !turns || turn_count == 0 || !out_data_size || !out_entry_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data_size = 0;
    *out_entry_count = 0;

    for (i = 0; i < index->count; i++) {
        claw_core_context_record_type_t type =
            (claw_core_context_record_type_t)index->entries[i].record_type;

        while (turn_index + 1 < turn_count && i >= turns[turn_index].end) {
            turn_index++;
        }
        if (!session_history_compact_keep_record(turns,
                                                 turn_count,
                                                 turn_index,
                                                 type)) {
            continue;
        }
        if (SIZE_MAX - compacted_data_size < index->entries[i].length) {
            return ESP_ERR_INVALID_SIZE;
        }
        compacted_data_size += index->entries[i].length;
        compacted_entry_count++;
    }

    *out_data_size = compacted_data_size;
    *out_entry_count = compacted_entry_count;
    return ESP_OK;
}

static void session_history_publish_size_warning(const claw_core_request_t *request,
                                                 const char *session_id)
{
    esp_err_t err;

    if (!request) {
        ESP_LOGW(TAG,
                 "session history blocked for %s: %s",
                 session_id ? session_id : "(null)",
                 CLAW_MEMORY_SESSION_SIZE_WARNING);
        return;
    }

    err = claw_core_publish_stage_text(request, CLAW_MEMORY_SESSION_SIZE_WARNING);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "publish session history size warning failed for %s: %s",
                 session_id ? session_id : "(null)",
                 esp_err_to_name(err));
    }
}

static esp_err_t session_history_rewrite_compacted(const char *session_id,
                                                   const claw_core_request_t *request,
                                                   const char *data_path,
                                                   const char *idx_path,
                                                   const claw_memory_session_index_t *index)
{
    FILE *data_file = NULL;
    FILE *idx_file = NULL;
    claw_memory_session_turn_t *turns = NULL;
    size_t compacted_data_size = 0;
    size_t compacted_entry_count = 0;
    size_t turn_count = 0;
    size_t turn_index = 0;
    uint32_t write_offset = 0;
    size_t i;
    esp_err_t err;

    if (!session_id || !data_path || !idx_path || !index) {
        return ESP_ERR_INVALID_ARG;
    }

    err = session_history_analyze_turns(index, &turns, &turn_count);
    if (err != ESP_OK) {
        return err;
    }

    err = session_history_plan_compaction(index,
                                          turns,
                                          turn_count,
                                          &compacted_data_size,
                                          &compacted_entry_count);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (compacted_data_size > CLAW_MEMORY_SESSION_SIZE_LIMIT) {
        esp_err_t block_err = session_history_mark_blocked(data_path);

        if (block_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mark session history blocked failed for %s: %s",
                     session_id,
                     esp_err_to_name(block_err));
        }
        session_history_publish_size_warning(request, session_id);
        goto cleanup;
    }

    data_file = fopen(data_path, "r+b");
    idx_file = fopen(idx_path, "r+b");
    if (!data_file || !idx_file) {
        ESP_LOGE(TAG, "open session history compaction files failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = session_history_write_index_header(idx_file);
    if (err != ESP_OK) {
        goto cleanup;
    }

    for (i = 0; i < index->count; i++) {
        claw_core_context_record_type_t type =
            (claw_core_context_record_type_t)index->entries[i].record_type;

        while (turn_index + 1 < turn_count && i >= turns[turn_index].end) {
            turn_index++;
        }
        if (!session_history_compact_keep_record(turns,
                                                 turn_count,
                                                 turn_index,
                                                 type)) {
            continue;
        }

        const claw_memory_session_index_entry_t *entry = &index->entries[i];
        claw_memory_session_index_entry_t new_entry = {
            .offset = write_offset,
            .length = entry->length,
            .record_type = entry->record_type,
            .backend_format = entry->backend_format,
        };

        if (entry->offset != write_offset) {
            char *record_text = NULL;
            uint32_t offset = 0;
            uint32_t length = 0;

            err = session_history_read_record_text(data_file, entry, &record_text);
            if (err != ESP_OK) {
                goto cleanup;
            }
            if (fseek(data_file, (long)write_offset, SEEK_SET) != 0) {
                ESP_LOGE(TAG, "seek compacted session history write offset failed");
                free(record_text);
                err = ESP_FAIL;
                goto cleanup;
            }
            err = claw_memory_write_session_raw_record(data_file,
                                                       record_text,
                                                       &offset,
                                                       &length);
            free(record_text);
            if (err != ESP_OK) {
                goto cleanup;
            }
            if (offset != write_offset || length != entry->length) {
                ESP_LOGW(TAG,
                         "compacted session history record size mismatch offset=%" PRIu32
                         " length=%" PRIu32 " expected_offset=%" PRIu32
                         " expected_length=%" PRIu32,
                         offset,
                         length,
                         write_offset,
                         entry->length);
                err = ESP_ERR_INVALID_STATE;
                goto cleanup;
            }
        }

        if (fwrite(&new_entry, 1, sizeof(new_entry), idx_file) != sizeof(new_entry)) {
            err = ESP_FAIL;
            goto cleanup;
        }
        write_offset += entry->length;
    }

    if (write_offset != compacted_data_size) {
        ESP_LOGW(TAG,
                 "compacted session history size mismatch write_offset=%" PRIu32 " planned=%u",
                 write_offset,
                 (unsigned)compacted_data_size);
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (fflush(data_file) != 0 || fflush(idx_file) != 0) {
        ESP_LOGE(TAG, "flush compacted session history files failed: errno=%d", errno);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (ftruncate(fileno(data_file), (off_t)compacted_data_size) != 0) {
        ESP_LOGE(TAG, "truncate compacted session history failed: errno=%d", errno);
        err = ESP_FAIL;
        goto cleanup;
    }
    if (ftruncate(fileno(idx_file),
                  (off_t)(sizeof(claw_memory_session_index_header_t) +
                          compacted_entry_count * sizeof(claw_memory_session_index_entry_t))) != 0) {
        ESP_LOGE(TAG, "truncate compacted session history idx failed: errno=%d", errno);
        err = ESP_FAIL;
        goto cleanup;
    }

cleanup:
    if (data_file && session_history_close_file(data_file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (idx_file && session_history_close_file(idx_file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(turns);
    return err;
}

static esp_err_t session_history_compact_if_needed(const char *session_id,
                                                   const claw_core_request_t *request,
                                                   const char *data_path,
                                                   const char *idx_path)
{
    claw_memory_session_index_t index = {0};
    esp_err_t err;

    /* session-projection (added): RETIRED. This mechanical >300KB compaction
     * rewrote the file and DROPPED older tool records, which shifted record
     * indices and invalidated the summary record's keep_from pointer, and it
     * could mark the session .blocked and kill it outright (12 such files exist
     * on the device). Session projection now bounds replay instead, so this is a
     * no-op; the code below is kept for reference. */
    (void)session_id;
    (void)request;
    (void)data_path;
    (void)idx_path;
    return ESP_OK;

    if (!session_id || !data_path || !idx_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (file_size_bytes(data_path) <= CLAW_MEMORY_SESSION_SIZE_LIMIT) {
        return ESP_OK;
    }

    err = session_history_validate_pair(data_path, idx_path, &index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Resetting invalid oversized session history %s", data_path);
        return session_history_recreate_file(data_path, idx_path);
    }

    err = session_history_rewrite_compacted(session_id, request, data_path, idx_path, &index);
    session_history_index_free(&index);
    return err;
}

static esp_err_t claw_memory_session_validate_batch(const claw_core_context_persist_batch_t *batch)
{
    size_t i;

    if (!batch || !batch->session_id || !batch->session_id[0] ||
            !batch->records || batch->record_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < batch->record_count; i++) {
        if (!session_history_record_type_valid((uint8_t)batch->records[i].type) ||
                ((!batch->records[i].message_json || !batch->records[i].message_json[0]) &&
                 (!batch->records[i].text || !batch->records[i].text[0]))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static const char *claw_memory_session_record_text_role(claw_core_context_record_type_t type)
{
    switch (type) {
    case CLAW_CORE_CONTEXT_RECORD_USER:
        return "user";
    case CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL:
        return "assistant";
    default:
        return NULL;
    }
}

/* ===== session-projection (added) ==========================================
 * The stored transcript stays append-only and complete. When the replayed part
 * grows past a trigger, an async job summarizes everything before a keep point
 * into one structured record; replay then sends [summary] + records after the
 * keep point, so long sessions keep their decisions without replaying everything.
 * ========================================================================== */

#define CLAW_MEMORY_PROJECT_TRIGGER_CHARS (48 * 1024)
#define CLAW_MEMORY_PROJECT_KEEP_CHARS    (24 * 1024)
#define CLAW_MEMORY_PROJECT_TOOL_MAX_CHARS 1500
#define CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS (40 * 1024)

static const char *CLAW_MEMORY_PROJECT_SYSTEM_PROMPT =
    "You compress an ongoing conversation into a compact working summary so an assistant can continue "
    "without the full transcript. Preserve: the user's goal and constraints; decisions made and why; "
    "what has been done; files created or modified; the current state; unresolved problems; and the "
    "next steps. Drop greetings, restated tool output, and reasoning. Output plain text with short "
    "labelled sections. Be specific and factual; never invent details.";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} claw_memory_sbuf_t;

static bool claw_memory_sbuf_append(claw_memory_sbuf_t *sb, const char *s, size_t max_total)
{
    size_t n;
    size_t need;
    size_t cap;
    char *grown;

    if (!sb || !s) {
        return true;
    }
    n = strlen(s);
    if (max_total && sb->len + n > max_total) {
        n = (sb->len < max_total) ? (max_total - sb->len) : 0;
    }
    if (n == 0) {
        return true;
    }
    need = sb->len + n + 1;
    if (need > sb->cap) {
        cap = sb->cap ? sb->cap : 4096;
        while (cap < need) {
            cap *= 2;
        }
        grown = realloc(sb->buf, cap);
        if (!grown) {
            return false;
        }
        sb->buf = grown;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return true;
}

/* Render records [from, to) as a plain-text transcript for the summary prompt. */
static esp_err_t session_history_serialize_range(FILE *file,
                                                 const claw_memory_session_index_t *index,
                                                 size_t from,
                                                 size_t to,
                                                 char **out_text)
{
    claw_memory_sbuf_t sb = {0};
    esp_err_t err = ESP_OK;
    size_t i;

    if (!file || !index || !out_text || from >= to) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    for (i = from; i < to && i < index->count; i++) {
        const claw_memory_session_index_entry_t *entry = &index->entries[i];
        claw_core_context_record_type_t type = (claw_core_context_record_type_t)entry->record_type;
        char *record_text = NULL;
        cJSON *record = NULL;
        const char *content = NULL;

        if (type == CLAW_CORE_CONTEXT_RECORD_SUMMARY ||
                sb.len >= CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS) {
            continue;
        }

        err = session_history_read_record_text(file, entry, &record_text);
        if (err != ESP_OK) {
            goto done;
        }
        record = cJSON_ParseWithOpts(record_text, NULL, 1);
        free(record_text);
        if (!record) {
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }

        switch (type) {
        case CLAW_CORE_CONTEXT_RECORD_USER:
            claw_memory_sbuf_append(&sb, "[User]: ", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "content"));
            claw_memory_sbuf_append(&sb, content ? content : "", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            claw_memory_sbuf_append(&sb, "\n", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            break;
        case CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL:
            claw_memory_sbuf_append(&sb, "[Assistant]: ", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "content"));
            claw_memory_sbuf_append(&sb, content ? content : "", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            claw_memory_sbuf_append(&sb, "\n", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            break;
        case CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL: {
            cJSON *calls = cJSON_GetObjectItemCaseSensitive(record, "tool_calls");
            cJSON *call = NULL;

            claw_memory_sbuf_append(&sb, "[Assistant tool calls]: ", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            cJSON_ArrayForEach(call, calls) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
                const char *name = fn ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fn, "name")) : NULL;

                if (name) {
                    claw_memory_sbuf_append(&sb, name, CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
                    claw_memory_sbuf_append(&sb, " ", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
                }
            }
            claw_memory_sbuf_append(&sb, "\n", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            break;
        }
        case CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT: {
            cJSON *item = NULL;

            /* Tool results are stored as an array of {role:"tool",content}. */
            cJSON_ArrayForEach(item, record) {
                char head[32];
                const char *body = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "content"));

                snprintf(head, sizeof(head), "[Tool result]: ");
                claw_memory_sbuf_append(&sb, head, CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
                if (body) {
                    char *truncated = NULL;
                    size_t keep = claw_utils_utf8_prefix_len(body, CLAW_MEMORY_PROJECT_TOOL_MAX_CHARS);

                    truncated = dup_printf("%.*s", (int)keep, body);
                    claw_memory_sbuf_append(&sb, truncated ? truncated : body, CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
                    free(truncated);
                }
                claw_memory_sbuf_append(&sb, "\n", CLAW_MEMORY_PROJECT_TRANSCRIPT_MAX_CHARS);
            }
            break;
        }
        default:
            break;
        }
        cJSON_Delete(record);
    }

    if (!sb.buf) {
        sb.buf = dup_printf("");
        if (!sb.buf) {
            err = ESP_ERR_NO_MEM;
            goto done;
        }
    }
    *out_text = sb.buf;
    sb.buf = NULL;

done:
    free(sb.buf);
    return err;
}

static esp_err_t session_history_append_summary_record(const char *session_id,
                                                       const char *summary,
                                                       uint32_t keep_from)
{
    char *data_path = NULL;
    char *idx_path = NULL;
    FILE *data_file = NULL;
    FILE *idx_file = NULL;
    cJSON *record = NULL;
    char *json = NULL;
    esp_err_t err;

    data_path = claw_memory_session_path_dup(session_id);
    if (!data_path) {
        return ESP_ERR_NO_MEM;
    }
    idx_path = session_history_idx_path_dup(data_path);
    if (!idx_path) {
        free(data_path);
        return ESP_ERR_NO_MEM;
    }

    err = session_history_open_pair_for_append(data_path, idx_path, &data_file, &idx_file);
    if (err != ESP_OK) {
        goto done;
    }

    record = cJSON_CreateObject();
    if (!record || !cJSON_AddStringToObject(record, "summary", summary) ||
            !cJSON_AddNumberToObject(record, "keep_from", (double)keep_from)) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    json = cJSON_PrintUnformatted(record);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    err = session_history_append_indexed_record(data_file, idx_file,
                                                CLAW_CORE_CONTEXT_RECORD_SUMMARY,
                                                json, NULL, NULL);

done:
    if (json) {
        cJSON_free(json);
    }
    cJSON_Delete(record);
    if (data_file) {
        session_history_close_file(data_file);
    }
    if (idx_file) {
        session_history_close_file(idx_file);
    }
    free(data_path);
    free(idx_path);
    return err;
}

/* Summarize records [prev_keep, keep_from) with the LLM and append the summary
 * record. Runs on the async extract task. */
static esp_err_t claw_memory_session_compact_run(const char *session_id, uint32_t keep_from)
{
    char *data_path = NULL;
    char *idx_path = NULL;
    FILE *file = NULL;
    claw_memory_session_index_t index = {0};
    char *transcript = NULL;
    char *prev_summary = NULL;
    size_t prev_keep = 0;
    cJSON *messages = NULL;
    cJSON *user_msg = NULL;
    cJSON *content = NULL;
    claw_llm_response_t response = {0};
    char *error_message = NULL;
    char *user_prompt = NULL;
    esp_err_t err;

    if (!session_id || !s_async_extract.runtime) {
        return ESP_ERR_INVALID_STATE;
    }

    data_path = claw_memory_session_path_dup(session_id);
    idx_path = data_path ? session_history_idx_path_dup(data_path) : NULL;
    if (!data_path || !idx_path) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    err = session_history_validate_pair(data_path, idx_path, &index);
    if (err != ESP_OK) {
        goto done;
    }
    file = fopen(data_path, "rb");
    if (!file) {
        err = ESP_FAIL;
        goto done;
    }

    /* Previous summary (if any) so summaries compound. */
    for (size_t i = 0; i < index.count; i++) {
        if (index.entries[i].record_type != CLAW_CORE_CONTEXT_RECORD_SUMMARY) {
            continue;
        }
        char *text = NULL;
        cJSON *rec = NULL;

        if (session_history_read_record_text(file, &index.entries[i], &text) != ESP_OK) {
            continue;
        }
        rec = cJSON_ParseWithOpts(text, NULL, 1);
        free(text);
        if (!rec) {
            continue;
        }
        cJSON *s = cJSON_GetObjectItemCaseSensitive(rec, "summary");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(rec, "keep_from");

        if (cJSON_IsString(s) && cJSON_IsNumber(k)) {
            free(prev_summary);
            prev_summary = dup_printf("%s", s->valuestring);
            prev_keep = (size_t)k->valueint;
        }
        cJSON_Delete(rec);
    }

    if (prev_keep >= keep_from) {
        err = ESP_OK;  /* nothing new to summarize */
        goto done;
    }

    err = session_history_serialize_range(file, &index, prev_keep, keep_from, &transcript);
    if (err != ESP_OK) {
        goto done;
    }

    user_prompt = dup_printf("Previous summary (may be empty):\n%s\n\nConversation to summarize:\n%s",
                             prev_summary ? prev_summary : "(none)",
                             transcript ? transcript : "");
    if (!user_prompt) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    messages = cJSON_CreateArray();
    user_msg = cJSON_CreateObject();
    if (!messages || !user_msg) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_prompt);
    cJSON_AddItemToArray(messages, user_msg);
    user_msg = NULL;

    err = claw_llm_runtime_chat(s_async_extract.runtime,
                                &(claw_llm_chat_request_t) {
                                    .system_prompt = CLAW_MEMORY_PROJECT_SYSTEM_PROMPT,
                                    .messages = messages,
                                    .tools_json = NULL,
                                    .abort_flag = NULL,
                                    .session_id = session_id,
                                },
                                &response,
                                &error_message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "compaction llm failed session=%s: %s", session_id,
                 error_message ? error_message : esp_err_to_name(err));
        goto done;
    }
    if (!response.text || !response.text[0]) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    err = session_history_append_summary_record(session_id, response.text, keep_from);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "compaction wrote summary session=%s keep_from=%" PRIu32 " chars=%u",
                 session_id, keep_from, (unsigned)strlen(response.text));
    }

done:
    if (content) {
        cJSON_Delete(content);
    }
    if (user_msg) {
        cJSON_Delete(user_msg);
    }
    if (messages) {
        cJSON_Delete(messages);
    }
    claw_llm_response_free(&response);
    free(error_message);
    free(user_prompt);
    free(transcript);
    free(prev_summary);
    session_history_index_free(&index);
    if (file) {
        session_history_close_file(file);
    }
    free(data_path);
    free(idx_path);
    return err;
}

/* Enqueue an async compaction job (not tracked by the consumer path). */
static esp_err_t claw_memory_enqueue_compact_job(const char *session_id, uint32_t keep_from)
{
    claw_memory_async_extract_job_t *job = NULL;

    if (!session_id || !s_async_extract.queue) {
        return ESP_ERR_INVALID_STATE;
    }
    job = calloc(1, sizeof(*job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->session_id = dup_printf("%s", session_id);
    job->compact = true;
    job->keep_from = keep_from;
    if (!job->session_id) {
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_async_extract.queue, &job, 0) != pdTRUE) {
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Decide whether the replayed history is too large and, if so, pick a keep point
 * that never splits an assistant-tool/tool-result pair, then enqueue a summary. */
static esp_err_t session_history_project_if_needed(const char *session_id,
                                                   const claw_core_request_t *request,
                                                   const char *data_path,
                                                   const char *idx_path)
{
    claw_memory_session_index_t index = {0};
    size_t prev_keep = 0;
    size_t replay_chars = 0;
    size_t keep_from;
    size_t i;
    esp_err_t err;

    (void)request;

    err = session_history_validate_pair(data_path, idx_path, &index);
    if (err != ESP_OK) {
        return err;
    }

    /* Use the last summary record's keep_from (later ones supersede). */
    for (i = 0; i < index.count; i++) {
        FILE *file;
        char *text = NULL;
        cJSON *rec = NULL;
        cJSON *k = NULL;

        if (index.entries[i].record_type != CLAW_CORE_CONTEXT_RECORD_SUMMARY) {
            continue;
        }
        file = fopen(data_path, "rb");
        if (!file) {
            continue;
        }
        if (session_history_read_record_text(file, &index.entries[i], &text) == ESP_OK) {
            rec = cJSON_ParseWithOpts(text, NULL, 1);
        }
        free(text);
        session_history_close_file(file);
        if (!rec) {
            continue;
        }
        k = cJSON_GetObjectItemCaseSensitive(rec, "keep_from");
        if (cJSON_IsNumber(k)) {
            prev_keep = (size_t)k->valueint;
        }
        cJSON_Delete(rec);
    }

    for (i = prev_keep; i < index.count; i++) {
        if (index.entries[i].record_type != CLAW_CORE_CONTEXT_RECORD_SUMMARY) {
            replay_chars += session_history_record_object_len(&index.entries[i]);
        }
    }
    if (replay_chars <= CLAW_MEMORY_PROJECT_TRIGGER_CHARS) {
        err = ESP_OK;
        goto done;
    }

    /* Keep a recent tail; cut point must not be a bare tool result. */
    {
        size_t acc = 0;

        keep_from = index.count;
        for (i = index.count; i > prev_keep; i--) {
            acc += session_history_record_object_len(&index.entries[i - 1]);
            if (acc > CLAW_MEMORY_PROJECT_KEEP_CHARS) {
                keep_from = i - 1;
                break;
            }
        }
        while (keep_from < index.count &&
                index.entries[keep_from].record_type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT) {
            keep_from++;
        }
        if (keep_from <= prev_keep || keep_from >= index.count) {
            err = ESP_OK;  /* nothing meaningful to summarize yet */
            goto done;
        }
    }

    err = claw_memory_enqueue_compact_job(session_id, (uint32_t)keep_from);

done:
    session_history_index_free(&index);
    return err;
}

/* reasoning-content (added): reasoning_content is stored and replayed verbatim.
 * The upstream provider requires the model's reasoning to be passed back in
 * thinking mode, so it must not be stripped. */

esp_err_t claw_memory_persist_context_callback(const claw_core_context_persist_batch_t *batch,
                                               void *user_ctx)
{
    char *data_path = NULL;
    char *idx_path = NULL;
    FILE *data_file = NULL;
    FILE *idx_file = NULL;
    esp_err_t err = ESP_OK;
    size_t i;

    (void)user_ctx;

    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    err = claw_memory_session_validate_batch(batch);
    if (err != ESP_OK) {
        return err;
    }
    if (session_history_session_blocked(batch->session_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    data_path = claw_memory_session_path_dup(batch->session_id);
    if (!data_path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        return ESP_ERR_NO_MEM;
    }
    idx_path = session_history_idx_path_dup(data_path);
    if (!idx_path) {
        free(data_path);
        return ESP_ERR_NO_MEM;
    }
    if (ensure_parent_dir(data_path) != ESP_OK) {
        err = ESP_FAIL;
        goto cleanup;
    }

    err = session_history_open_pair_for_append(data_path,
                                               idx_path,
                                               &data_file,
                                               &idx_file);
    if (err != ESP_OK) {
        goto cleanup;
    }

    for (i = 0; i < batch->record_count; i++) {
        const char *role = NULL;
        const char *text = NULL;
        const char *message_json = batch->records[i].message_json;

        if (!message_json || !message_json[0]) {
            role = claw_memory_session_record_text_role(batch->records[i].type);
            text = batch->records[i].text;
            if (!role || !text || !text[0]) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
        }
        err = session_history_append_indexed_record(data_file,
                                                    idx_file,
                                                    batch->records[i].type,
                                                    message_json,
                                                    role,
                                                    text);
        if (err != ESP_OK) {
            break;
        }
    }

cleanup:
    if (data_file && session_history_close_file(data_file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (idx_file && session_history_close_file(idx_file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        /* Retired mechanical >300KB compaction (documented no-op, see its body). */
        (void)session_history_compact_if_needed(batch->session_id, batch->request, data_path, idx_path);

        /* session-projection (added): evaluate on EVERY persist, not only at turn
         * end. A long turn that never completes (the user's interrupts keep it
         * alive) would otherwise never compact, so the whole history keeps being
         * replayed on each new request until the body runs out of memory
         * ("Out of memory serializing request"). */
        esp_err_t proj = session_history_project_if_needed(batch->session_id,
                                                           batch->request,
                                                           data_path,
                                                           idx_path);

        if (proj != ESP_OK) {
            ESP_LOGW(TAG, "projection planning failed session=%s: %s",
                     batch->session_id, esp_err_to_name(proj));
        }
    }
    free(data_path);
    free(idx_path);
    return err;
}

esp_err_t claw_memory_delete_session_history(const char *session_id,
                                             bool *out_deleted_any)
{
    char *data_path = NULL;
    char *idx_path = NULL;
    char *blocked_path = NULL;
    bool deleted_any = false;
    esp_err_t err = ESP_OK;

    if (!session_id || !session_id[0] || !out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_deleted_any = false;

    data_path = claw_memory_session_path_dup(session_id);
    if (!data_path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        return ESP_ERR_NO_MEM;
    }
    idx_path = session_history_idx_path_dup(data_path);
    blocked_path = session_history_blocked_path_dup(data_path);
    if (!idx_path || !blocked_path) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = session_history_unlink_path(data_path, &deleted_any);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = session_history_unlink_path(idx_path, &deleted_any);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = session_history_unlink_path(blocked_path, &deleted_any);
    if (err != ESP_OK) {
        goto cleanup;
    }

cleanup:
    free(data_path);
    free(idx_path);
    free(blocked_path);
    if (err == ESP_OK) {
        *out_deleted_any = deleted_any;
    }
    return err;
}

esp_err_t claw_memory_note_session_summary(const char *session_id,
                                           const char *summary_list)
{
    claw_memory_pending_summary_t *node = NULL;

    if (!session_id || !session_id[0] || !summary_list || !summary_list[0]) {
        return ESP_OK;
    }

    node = claw_memory_find_pending_summary(session_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->session_id = dup_printf("%s", session_id);
        if (!node->session_id) {
            free(node);
            return ESP_ERR_NO_MEM;
        }
        node->next = s_pending_summaries;
        s_pending_summaries = node;
    }

    return line_list_merge_unique(&node->summary_list, summary_list);
}

esp_err_t claw_memory_request_gate_callback(const claw_core_request_t *request,
                                            char *reject_message,
                                            size_t reject_message_size,
                                            void *user_ctx)
{
    (void)user_ctx;

    if (!request || !request->session_id || !request->session_id[0]) {
        return ESP_OK;
    }
    if (!reject_message || reject_message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reject_message[0] = '\0';

    if (!session_history_session_blocked(request->session_id)) {
        return ESP_OK;
    }

    strlcpy(reject_message, CLAW_MEMORY_SESSION_SIZE_WARNING, reject_message_size);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t claw_memory_request_start_callback(const claw_core_request_t *request,
                                             void *user_ctx)
{
    (void)user_ctx;
    return claw_memory_async_extract_ensure_started(request);
}

esp_err_t claw_memory_stage_note_callback(const claw_core_request_t *request,
                                          char **out_note,
                                          void *user_ctx)
{
    char *summary_list = NULL;
    char *async_summary = NULL;
    bool manual_write = false;

    (void)user_ctx;

    if (!out_note) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_note = NULL;
    if (!request || !request->session_id || !request->session_id[0]) {
        return ESP_OK;
    }

    manual_write = claw_memory_request_take_manual_write(request->request_id);
    summary_list = claw_memory_pending_summary_take_summary_list(request->session_id);
    async_summary = claw_memory_async_extract_take_summary_list(request, !manual_write);
    if (line_list_merge_unique(&summary_list, async_summary) != ESP_OK) {
        ESP_LOGW(TAG, "merge async extract summary failed for request=%" PRIu32, request->request_id);
    }
    free(async_summary);
    *out_note = claw_memory_format_update_stage_note(summary_list);
    free(summary_list);
    return ESP_OK;
}

static esp_err_t claw_memory_session_history_collect(const claw_core_request_t *request,
                                                     claw_core_context_t *out_context,
                                                     void *user_ctx)
{
    char *content = NULL;
    esp_err_t err;

    (void)user_ctx;

    if (!request || !out_context || !request->session_id || !request->session_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_context, 0, sizeof(*out_context));

    err = claw_memory_session_load_json_alloc(request->session_id, &content);
    if (err != ESP_OK) {
        return err;
    }
    if (!content || !content[0] || strcmp(content, "[]") == 0) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_memory_session_history_provider = {
    .name = "Session History",
    .collect = claw_memory_session_history_collect,
    .user_ctx = NULL,
    .flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY,
};
