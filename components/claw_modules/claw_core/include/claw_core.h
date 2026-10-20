/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_CORE_RESPONSE_STATUS_OK = 0,
    CLAW_CORE_RESPONSE_STATUS_ERROR = 1,
} claw_core_response_status_t;

typedef enum {
    CLAW_CORE_COMPLETION_DONE = 0,
    CLAW_CORE_COMPLETION_FAILED = 1,
    CLAW_CORE_COMPLETION_LIMIT_REACHED = 2,
    /* blocked (added): the turn stopped because it needs something only the user
     * can decide (an authorization, a product choice). It is NOT a failure: the
     * question has been put to the user and the answer resumes the work. */
    CLAW_CORE_COMPLETION_BLOCKED = 3,
} claw_core_completion_type_t;

typedef enum {
    CLAW_CORE_AGENT_LOOP_PHASE_IDLE = 0,
    CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT,
    CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT,
    CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_LLM_HTTP,
    CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP,
    CLAW_CORE_AGENT_LOOP_PHASE_AFTER_LLM_BEFORE_TOOL,
    CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL,
    CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING,
} claw_core_agent_loop_phase_t;

#define CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE (1U << 0)
#define CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE (1U << 1)
#define CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE  (1U << 3)

#define CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY (1U << 0)

typedef struct claw_core_state *claw_core_handle_t;

typedef struct {
    uint32_t request_id;
    uint32_t flags;
    const char *session_id;
    const char *user_text;
    const char *source_channel;
    const char *source_chat_id;
    const char *source_sender_id;
    const char *source_message_id;
    const char *source_cap;
    const char *target_channel;
    const char *target_chat_id;
} claw_core_request_t;

typedef enum {
    /** The message starts a new run, immediately or after an older run. */
    CLAW_CORE_MESSAGE_QUEUED_NEW_RUN = 0,
    /** The message was appended to the inbox of the active run. */
    CLAW_CORE_MESSAGE_APPENDED_TO_RUN,
} claw_core_message_disposition_t;

typedef struct {
    claw_core_message_disposition_t disposition;
    /** Run that owns the message. This can differ from request.request_id. */
    uint32_t run_id;
} claw_core_message_receipt_t;

typedef enum {
    CLAW_CORE_CONTEXT_RECORD_USER = 1,
    CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL = 2,
    CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL = 3,
    CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT = 4,
    /* session-projection (added): a structured summary that replaces all records
     * before its keep_from index when history is replayed. Never sent verbatim;
     * the loader converts it into a leading system message. */
    CLAW_CORE_CONTEXT_RECORD_SUMMARY = 5,
} claw_core_context_record_type_t;

typedef struct {
    claw_core_context_record_type_t type;
    const char *message_json;
    const char *text;
} claw_core_context_record_t;

typedef struct {
    const char *session_id;
    const claw_core_request_t *request;
    const claw_core_context_record_t *records;
    size_t record_count;
    bool turn_completed;
} claw_core_context_persist_batch_t;

typedef esp_err_t (*claw_core_persist_context_fn)(
    const claw_core_context_persist_batch_t *batch,
    void *user_ctx);

typedef esp_err_t (*claw_core_request_start_fn)(const claw_core_request_t *request,
                                                void *user_ctx);

typedef esp_err_t (*claw_core_request_gate_fn)(const claw_core_request_t *request,
                                               char *reject_message,
                                               size_t reject_message_size,
                                               void *user_ctx);

typedef esp_err_t (*claw_core_stage_note_fn)(const claw_core_request_t *request,
                                             char **out_note,
                                             void *user_ctx);

typedef struct claw_core_response claw_core_response_t;

typedef enum {
    CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT = 0,
    CLAW_CORE_CONTEXT_KIND_MESSAGES = 1,
    CLAW_CORE_CONTEXT_KIND_TOOLS = 2,
} claw_core_context_kind_t;

typedef struct {
    claw_core_context_kind_t kind;
    char *content;
} claw_core_context_t;

typedef esp_err_t (*claw_core_context_provider_collect_fn)(
    const claw_core_request_t *request,
    claw_core_context_t *out_context,
    void *user_ctx);

typedef struct {
    const char *name;
    claw_core_context_provider_collect_fn collect;
    void *user_ctx;
    uint32_t flags;
} claw_core_context_provider_t;

typedef esp_err_t (*claw_core_call_cap_fn)(const char *cap_name,
                                           const char *input_json,
                                           const claw_core_request_t *request,
                                           char **out_output,
                                           void *user_ctx);

typedef struct {
    uint32_t instance_id;
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    const char *reasoning_effort;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
    const char *system_prompt;
    claw_core_persist_context_fn persist_context;
    void *persist_context_user_ctx;
    claw_core_request_gate_fn request_gate;
    void *request_gate_user_ctx;
    claw_core_request_start_fn on_request_start;
    void *on_request_start_user_ctx;
    claw_core_stage_note_fn collect_stage_note;
    void *collect_stage_note_user_ctx;
    claw_core_call_cap_fn call_cap;
    void *cap_user_ctx;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core;
    uint32_t max_tool_iterations;
    uint32_t request_queue_len;
    uint32_t response_queue_len;
    size_t max_context_providers;
} claw_core_config_t;

struct claw_core_response {
    uint32_t request_id;
    claw_core_response_status_t status;
    claw_core_completion_type_t completion_type;
    char *target_channel;
    char *target_chat_id;
    char *text;
    char *error_message;
};

typedef struct {
    uint32_t request_id;
    const char *session_id;            /* may be NULL */
    const char *final_text;            /* may be NULL or empty */
    const char *context_providers_csv; /* providers that injected non-empty content */
    const char *tool_calls_csv;        /* tool calls invoked across all rounds */
    /* terminal-agent-notification (added): observers now fire on every terminal
     * outcome, not only success, so a parent agent always learns how a child
     * ended. completion_type distinguishes normal completion from failure or
     * hitting the tool-call round limit; error_message is set for failures. */
    claw_core_completion_type_t completion_type;
    const char *error_message;         /* may be NULL; set when completion_type != DONE */
} claw_core_completion_summary_t;

typedef void (*claw_core_completion_observer_fn)(const claw_core_completion_summary_t *summary,
                                                 void *user_ctx);

esp_err_t claw_core_create(const claw_core_config_t *config, claw_core_handle_t *out_core);
esp_err_t claw_core_start(claw_core_handle_t core);
esp_err_t claw_core_stop(claw_core_handle_t core, uint32_t timeout_ms);
esp_err_t claw_core_destroy(claw_core_handle_t core);
esp_err_t claw_core_update_llm_config(claw_core_handle_t core,
                                      const claw_core_config_t *config);
esp_err_t claw_core_add_context_provider(claw_core_handle_t core,
                                         const claw_core_context_provider_t *provider);
esp_err_t claw_core_add_completion_observer(claw_core_handle_t core,
                                            claw_core_completion_observer_fn observer,
                                            void *user_ctx);
esp_err_t claw_core_publish_stage_text(const claw_core_request_t *request, const char *text);
/* agent notice (added): let a capability speak to the user's own conversation.
 * This is how a subagent gets a voice - a progress line or a blocking question -
 * without routing everything through the root agent. `payload_json` is optional
 * structured attribution (agent id/type) for a front end that can render it. */
esp_err_t claw_core_publish_agent_notice(const char *channel,
                                         const char *chat_id,
                                         const char *text,
                                         const char *payload_json);

/* blocked outcome (added): a capability declares that this turn should end as
 * BLOCKED rather than DONE, with the reason, because it needs something only the
 * user can decide. Cleared at the start of every request. */
esp_err_t claw_core_mark_blocked(claw_core_handle_t core, const char *reason);

/** Queue an explicitly independent Agent run. */
esp_err_t claw_core_start_run(claw_core_handle_t core,
                              const claw_core_request_t *request,
                              uint32_t timeout_ms);
/**
 * Deliver a conversation message.
 *
 * A message for the session currently owned by an inbox-accepting run is put in
 * that run's inbox. Otherwise it starts a new run. Callers therefore never
 * need a separate submit/append decision or a racy active-state query.
 */
esp_err_t claw_core_post_message(claw_core_handle_t core,
                                 const claw_core_request_t *request,
                                 uint32_t timeout_ms,
                                 claw_core_message_receipt_t *out_receipt);
/* Returns idle, queued or running; includes requests not yet picked by the worker. */
const char *claw_core_session_state(claw_core_handle_t core, const char *session_id);
esp_err_t claw_core_cancel_request(claw_core_handle_t core, uint32_t request_id);
claw_core_agent_loop_phase_t claw_core_get_agent_loop_phase(claw_core_handle_t core);
esp_err_t claw_core_receive(claw_core_handle_t core,
                            claw_core_response_t *response,
                            uint32_t timeout_ms);
esp_err_t claw_core_receive_for(claw_core_handle_t core,
                                uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms);
void claw_core_response_free(claw_core_response_t *response);

/**
 * @brief  Capacity / serialization statistics of the running core
 *
 *         (added, C4) The capacity decisions - window size, request budget, and
 *         whether the device can actually serialize the request - used to be
 *         visible only as warnings in the log, so judging a device run meant
 *         inferring them from failure strings. These counters make the same
 *         judgement a single call.
 */
typedef struct {
    size_t window_bytes;          /**< Dynamic window W in force */
    size_t est_budget_bytes;      /**< Byte budget for the whole request body */
    size_t refuse_limit_bytes;    /**< Where the guard refuses instead of trimming */
    size_t block_have_bytes;      /**< Largest contiguous block available right now */
    size_t last_body_bytes;       /**< Last request body the guard estimated */
    size_t last_block_need_bytes; /**< Contiguous block that body needs */
    size_t last_block_have_bytes; /**< Contiguous block available at that moment */
    uint32_t trim_count;          /**< Guards that had to degrade, this boot */
    uint32_t refuse_count;        /**< Guards that refused a round, this boot */
    uint32_t last_folded;         /**< Last trim: reasoning blocks folded */
    uint32_t last_omitted;        /**< Last trim: tool results omitted */
    uint32_t last_blanked;        /**< Last trim: tool arguments blanked */
} claw_core_context_stats_t;

/**
 * @brief  Read the capacity / serialization statistics
 *
 * @param[out]  out  Receives the statistics
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if out is NULL
 */
esp_err_t claw_core_get_context_stats(claw_core_context_stats_t *out);

#ifdef __cplusplus
}
#endif
