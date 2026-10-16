/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"
#include "claw_task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "llm/claw_llm_http_transport.h"

static const char *TAG = "claw_core";

static esp_err_t handle_pending_user_interrupts(claw_core_state_t *core,
                                                const claw_core_request_item_t *request,
                                                const char *timing_point,
                                                cJSON **runtime_messages,
                                                bool *out_drained)
{
    char *texts[CLAW_CORE_INSERT_QUEUE_LEN] = {0};
    const char *persist_texts[CLAW_CORE_INSERT_QUEUE_LEN] = {0};
    size_t text_count = 0;
    size_t i;
    bool persisted = false;
    esp_err_t err;

    if (out_drained) {
        *out_drained = false;
    }
    if (!core || !request || !runtime_messages || !out_drained) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!claw_core_ingress_dequeue_inserted_user_inputs(core,
                                                        request->view.session_id,
                                                        texts,
                                                        CLAW_CORE_INSERT_QUEUE_LEN,
                                                        &text_count)) {
        return ESP_OK;
    }
    claw_core_control_clear_user_interrupt_abort(core, request->view.request_id);

    for (i = 0; i < text_count; i++) {
        persist_texts[i] = texts[i];
    }

    err = claw_core_persist_context_user_messages_if_configured(core,
                                                                &request->view,
                                                                persist_texts,
                                                                text_count,
                                                                &persisted);
    if (err != ESP_OK) {
        claw_core_log_context_persist_failure(&request->view,
                                              "persist_context_user_interrupt",
                                              err);
        persisted = false;
    }
    ESP_LOGI(TAG,
             "user_interrupt_triggered request=%" PRIu32 " timing=%s count=%u persisted=%s",
             request->view.request_id,
             timing_point ? timing_point : "unknown",
             (unsigned)text_count,
             persisted ? "true" : "false");

    if (!*runtime_messages) {
        *runtime_messages = cJSON_CreateArray();
        if (!*runtime_messages) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    for (i = 0; i < text_count; i++) {
        err = claw_core_append_user_message(*runtime_messages, texts[i]);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }
    *out_drained = true;
    err = ESP_OK;

cleanup:
    for (i = 0; i < text_count; i++) {
        free(texts[i]);
    }
    return err;
}

void claw_core_agent_loop_task(void *arg)
{
    claw_core_state_t *core = (claw_core_state_t *)arg;

    if (!core) {
        ESP_LOGE(TAG, "agent_loop_task started without core state");
        vTaskDelete(NULL);
        return;
    }

    while (!core->stop_requested) {
        claw_core_request_item_t request = {0};
        claw_core_response_item_t response = {0};
        claw_core_cached_context_t *request_start_contexts = NULL;
        size_t request_start_context_count = 0;
        cJSON *runtime_messages = NULL;
        cJSON *messages = NULL;
        char *system_prompt = NULL;
        char *tools_json = NULL;
        char tool_summary[CLAW_CORE_TOOL_SUMMARY_MAX_LEN] = {0};
        claw_core_llm_response_t llm_response = {0};
        uint32_t iteration = 0;
        esp_err_t err = ESP_OK;
        char obs_providers_csv[CLAW_CORE_OBS_CSV_MAX] = {0};
        char obs_tool_calls_csv[CLAW_CORE_OBS_CSV_MAX] = {0};
        bool original_user_persisted = false;
        bool inject_active_user = true;
        /* terminal-agent-notification (added): every terminal outcome must reach
         * registered observers (subagent parents), including failures and the
         * tool-call round limit, so a parent never sees a silently stuck child. */
        claw_core_completion_type_t completion_outcome = CLAW_CORE_COMPLETION_FAILED;
        bool completion_notified = false;
        /* llm-retry (added): one retry of the first LLM call of a turn. */
        bool llm_retry_used = false;
        /* in-turn window report (added): what the previous round's trim did, so
         * the model is told about lost context instead of silently forgetting. */
        char trim_note[CLAW_CORE_TRIM_NOTE_MAX] = {0};

        if (xQueueReceive(core->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (core->stop_requested) {
            claw_core_free_request_item(&request);
            break;
        }

        if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            core->inflight_request_id = request.view.request_id;
            if (request.view.session_id && request.view.session_id[0]) {
                strlcpy(core->inflight_session_id,
                        request.view.session_id,
                        sizeof(core->inflight_session_id));
            } else {
                core->inflight_session_id[0] = '\0';
            }
            core->agent_loop_phase = CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT;
            core->inflight_abort = false;
            core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_NONE;
            claw_core_ingress_clear_insert_queue_locked(core);
            xSemaphoreGive(core->inflight_lock);
        }
        response.view.request_id = request.view.request_id;
        response.view.status = CLAW_CORE_RESPONSE_STATUS_ERROR;
        response.view.completion_type = CLAW_CORE_COMPLETION_DONE;
        response.view.target_channel = claw_utils_string_dup(request.view.target_channel);
        response.view.target_chat_id = claw_utils_string_dup(request.view.target_chat_id);
        if ((request.view.target_channel && request.view.target_channel[0] &&
                !response.view.target_channel) ||
                (request.view.target_chat_id && request.view.target_chat_id[0] &&
                 !response.view.target_chat_id)) {
            response.view.error_message = claw_utils_string_dup("Failed to allocate response target");
            goto finish_request;
        }
        {
            char llm_unavailable_message[192] = {0};

            if (!claw_core_llm_config_ready(core,
                                           llm_unavailable_message,
                                           sizeof(llm_unavailable_message))) {
                response.view.status = CLAW_CORE_RESPONSE_STATUS_OK;
                response.view.text = claw_utils_string_dup(llm_unavailable_message);
                if (!response.view.text) {
                    response.view.status = CLAW_CORE_RESPONSE_STATUS_ERROR;
                    response.view.error_message = claw_utils_string_dup("Failed to allocate LLM config message");
                    err = ESP_ERR_NO_MEM;
                } else {
                    err = ESP_OK;
                }
                goto finish_request;
            }
        }
        if (core->request_gate) {
            char reject_message[192] = {0};
            esp_err_t gate_err = core->request_gate(&request.view,
                                                    reject_message,
                                                    sizeof(reject_message),
                                                    core->request_gate_user_ctx);
            if (gate_err != ESP_OK) {
                if (reject_message[0]) {
                    response.view.status = CLAW_CORE_RESPONSE_STATUS_OK;
                    response.view.text = claw_utils_string_dup(reject_message);
                    if (!response.view.text) {
                        response.view.status = CLAW_CORE_RESPONSE_STATUS_ERROR;
                        response.view.error_message = claw_utils_string_dup("Failed to allocate reject message");
                        err = ESP_ERR_NO_MEM;
                    } else {
                        err = ESP_OK;
                    }
                } else {
                    response.view.error_message = claw_utils_string_dup(esp_err_to_name(gate_err));
                    err = gate_err;
                }
                goto finish_request;
            }
        }
        if (core->on_request_start) {
            err = core->on_request_start(&request.view, core->on_request_start_user_ctx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "request_start request=%" PRIu32 " failed: %s",
                         request.view.request_id,
                         esp_err_to_name(err));
            }
        }

        {
            const char *user_texts[1] = {request.view.user_text};
            esp_err_t persist_err = claw_core_persist_context_user_messages_if_configured(
                                        core,
                                        &request.view,
                                        user_texts,
                                        1,
                                        &original_user_persisted);

            claw_core_log_context_persist_failure(&request.view,
                                                  "persist_context_user",
                                                  persist_err);
        }

        runtime_messages = cJSON_CreateArray();
        if (!runtime_messages) {
            response.view.error_message = claw_utils_string_dup("Failed to allocate runtime messages");
            goto finish_request;
        }

        err = claw_core_collect_request_start_only_contexts(core,
                                                            &request,
                                                            &request_start_contexts,
                                                            &request_start_context_count);
        if (err != ESP_OK) {
            response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
            goto finish_request;
        }
        if (original_user_persisted &&
                claw_core_cached_contexts_have_messages(request_start_contexts, request_start_context_count)) {
            inject_active_user = false;
        }

        while (true) {
            claw_core_llm_response_free(&llm_response);
            free(system_prompt);
            free(tools_json);
            cJSON_Delete(messages);
            system_prompt = NULL;
            tools_json = NULL;
            messages = NULL;

            claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT);
            {
                bool drained = false;

                err = handle_pending_user_interrupts(core,
                                                     &request,
                                                     "before_build_iteration_context",
                                                     &runtime_messages,
                                                     &drained);
                if (err != ESP_OK) {
                    response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                    goto finish_request;
                }
                if (drained) {
                    continue;
                }
            }

            claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT);
            err = claw_core_build_iteration_context(core,
                                                    &request,
                                                    runtime_messages,
                                                    request_start_contexts,
                                                    request_start_context_count,
                                                    inject_active_user,
                                                    &system_prompt,
                                                    &messages,
                                                    &tools_json,
                                                    obs_providers_csv,
                                                    sizeof(obs_providers_csv));
            if (err != ESP_OK) {
                response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                goto finish_request;
            }

            /* Tell the model what the in-turn window did to its own history. A
             * silently trimmed context is what makes a long turn degrade into a
             * read/forget/re-read loop: the model cannot tell "I never read this"
             * from "this was taken away". */
            if (trim_note[0]) {
                char *prompt_with_note = claw_utils_string_dup_printf("%s\n%s",
                                                                      system_prompt,
                                                                      trim_note);

                if (prompt_with_note) {
                    free(system_prompt);
                    system_prompt = prompt_with_note;
                }
            }

            /* serialize guard (added): the budgets inside the context builder each
             * bound their own part; nothing bounded their sum. Estimate the body
             * here, before the transport allocates it, so an oversized round fails
             * cheaply and with advice instead of as "Out of memory serializing
             * request" halfway through building the JSON. */
            ESP_LOGI(TAG,
                     "request=%" PRIu32 " iteration=%u built context: system=%u tools=%u messages=%u "
                     "free_int=%u largest_int=%u free_spiram=%u largest_spiram=%u",
                     request.view.request_id, (unsigned)iteration,
                     (unsigned)(system_prompt ? strlen(system_prompt) : 0),
                     (unsigned)(tools_json ? strlen(tools_json) : 0),
                     (unsigned)cJSON_GetArraySize(messages),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            err = claw_core_context_guard_request(system_prompt,
                                                  messages,
                                                  tools_json,
                                                  &response.view.error_message);
            if (err != ESP_OK) {
                if (!response.view.error_message) {
                    response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                }
                goto finish_request;
            }
            {
                bool drained = false;

                err = handle_pending_user_interrupts(core,
                                                     &request,
                                                     "before_llm_http",
                                                     &runtime_messages,
                                                     &drained);
                if (err != ESP_OK) {
                    response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                    goto finish_request;
                }
                if (drained) {
                    continue;
                }
            }

            claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP);
            /*
             * opencode-go provider support (added): pass the borrowed session
             * id so the opencode_go backend can attach the
             * "x-opencode-session" header; other backends ignore it.
             */
            err = claw_core_llm_chat_messages_for_session(core,
                                                           request.view.session_id,
                                                           system_prompt,
                                                           messages,
                                                           tools_json,
                                                           &llm_response,
                                                           &response.view.error_message);
            if (err != ESP_OK) {
                bool drained = false;

                if (claw_core_control_take_user_interrupt_http_abort(core, request.view.request_id)) {
                    free(response.view.error_message);
                    response.view.error_message = NULL;
                    err = handle_pending_user_interrupts(core,
                                                         &request,
                                                         "in_llm_http_abort",
                                                         &runtime_messages,
                                                         &drained);
                    if (err != ESP_OK) {
                        response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                        goto finish_request;
                    }
                    if (drained) {
                        continue;
                    }
                }
                /* llm-retry (added): a transient upstream/transport failure on the
                 * first LLM call (no tool has run yet) should not kill the turn;
                 * retry once. Later rounds are not retried because tools may have
                 * already produced side effects. */
                if (!llm_retry_used && iteration == 0 && !core->stop_requested) {
                    llm_retry_used = true;
                    ESP_LOGW(TAG, "request=%" PRIu32 " retrying first LLM call after failure: %s",
                             request.view.request_id,
                             response.view.error_message ? response.view.error_message : esp_err_to_name(err));
                    free(response.view.error_message);
                    response.view.error_message = NULL;
                    continue;
                }
                goto finish_request;
            }

            if (llm_response.tool_call_count == 0) {
                claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING);
                claw_core_publish_stage_note_for_round(core, &request.view, iteration);
                claw_core_finish_from_plain_text(request.view.request_id,
                                                 &llm_response,
                                                 &response.view);
                err = ESP_OK;
                break;
            }

            claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_AFTER_LLM_BEFORE_TOOL);
            {
                bool drained = false;

                err = handle_pending_user_interrupts(core,
                                                     &request,
                                                     "after_llm_before_tool",
                                                     &runtime_messages,
                                                     &drained);
                if (err != ESP_OK) {
                    response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                    goto finish_request;
                }
                if (drained) {
                    continue;
                }
            }

            claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL);
            claw_core_log_tool_call_names(request.view.request_id, &llm_response);
            claw_core_publish_stage_tool_calls(&request.view, &llm_response, iteration);
            for (size_t tc = 0; tc < llm_response.tool_call_count; tc++) {
                claw_core_obs_csv_append(obs_tool_calls_csv,
                                         sizeof(obs_tool_calls_csv),
                                         llm_response.tool_calls[tc].name,
                                         false);
            }

            char *tool_results_json = NULL;
            const char *assistant_tool_message_json = llm_response.raw_message_json;

            err = claw_core_append_assistant_tool_calls(runtime_messages, &llm_response);
            if (err != ESP_OK) {
                response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                goto finish_request;
            }

            err = claw_core_append_tool_results_messages(core,
                                                         runtime_messages,
                                                         &llm_response,
                                                         &request.view,
                                                         tool_summary,
                                                         sizeof(tool_summary),
                                                         &tool_results_json);
            if (err != ESP_OK) {
                response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
                goto finish_request;
            }

            if (tool_results_json && tool_results_json[0]) {
                esp_err_t persist_err = claw_core_persist_context_tool_round_if_configured(
                                            core,
                                            &request.view,
                                            assistant_tool_message_json,
                                            tool_results_json);

                if (persist_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "persist_context_tool_round failed for request=%" PRIu32
                             " iteration=%" PRIu32 ": %s",
                             request.view.request_id,
                             iteration,
                             esp_err_to_name(persist_err));
                }
            }
            if (tool_results_json) {
                cJSON_free(tool_results_json);
            }

            /* in-turn window (added): keep the request body bounded as rounds
             * accumulate (the full round is already persisted above). The note it
             * leaves is attached to the next round's system prompt. */
            claw_core_trim_runtime_messages(runtime_messages, trim_note, sizeof(trim_note));

            iteration++;
            if (iteration >= core->max_tool_iterations) {
                /* terminal-agent-notification (added): report the round limit as a
                 * distinct terminal outcome with an actionable message rather than
                 * a generic failure, so the parent can follow up or close the agent. */
                response.view.error_message = claw_utils_string_dup(
                    "reached tool-call round limit; partial work is retained in this session");
                completion_outcome = CLAW_CORE_COMPLETION_LIMIT_REACHED;
                err = ESP_ERR_INVALID_STATE;
                goto finish_request;
            }
        }

        if (err == ESP_OK && response.view.text) {
            esp_err_t persist_err;

            response.view.status = CLAW_CORE_RESPONSE_STATUS_OK;
            persist_err = claw_core_persist_context_final_if_configured(core,
                                                                        &request.view,
                                                                        llm_response.raw_message_json,
                                                                        response.view.text);
            claw_core_log_context_persist_failure(&request.view,
                                                  "persist_context_final",
                                                  persist_err);
            if (core->completion_observer_count > 0) {
                claw_core_completion_summary_t summary = {
                    .request_id = request.view.request_id,
                    .session_id = request.view.session_id,
                    .final_text = response.view.text,
                    .context_providers_csv = obs_providers_csv,
                    .tool_calls_csv = obs_tool_calls_csv,
                    .completion_type = CLAW_CORE_COMPLETION_DONE,
                    .error_message = NULL,
                };
                for (size_t i = 0; i < core->completion_observer_count; i++) {
                    core->completion_observers[i].fn(&summary,
                                                     core->completion_observers[i].user_ctx);
                }
            }
            completion_outcome = CLAW_CORE_COMPLETION_DONE;
            completion_notified = true;
        } else if (!response.view.error_message) {
            response.view.error_message = claw_utils_string_dup(esp_err_to_name(err));
        }

finish_request:
        claw_core_control_set_phase(core, CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING);
        if (xSemaphoreTake(core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            bool was_cancelled = core->inflight_abort &&
                                 core->inflight_abort_reason == CLAW_CORE_CONTROL_ABORT_REASON_CANCEL;
            core->inflight_request_id = 0;
            core->inflight_session_id[0] = '\0';
            core->agent_loop_phase = CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
            core->inflight_abort = false;
            core->inflight_abort_reason = CLAW_CORE_CONTROL_ABORT_REASON_NONE;
            claw_core_ingress_clear_insert_queue_locked(core);
            xSemaphoreGive(core->inflight_lock);
            if (was_cancelled && err != ESP_OK && response.view.error_message) {
                /* Replace the generic transport error with a clearer one. */
                free(response.view.error_message);
                response.view.error_message = claw_utils_string_dup("request cancelled");
            }
        }

        response.view.completion_type = completion_outcome;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request=%" PRIu32 " failed: %s",
                     request.view.request_id,
                     response.view.error_message ? response.view.error_message : esp_err_to_name(err));
            /* failure-note writeback removed (added): a failed turn used to be
             * persisted into the session as a synthetic assistant message, which
             * permanently poisoned history (replayed on every later turn) and
             * pushed raw upstream error text into context. The error still reaches
             * the caller via the outbound response; the session tail stays the
             * unanswered user message, so the next turn retries naturally. */
        }
        /* terminal-agent-notification (added): if the success path did not already
         * notify, fire observers for the failure/limit/empty outcome here. This
         * covers every early `goto finish_request` exit as well as loop failures. */
        if (!completion_notified && core->completion_observer_count > 0) {
            claw_core_completion_summary_t summary = {
                .request_id = request.view.request_id,
                .session_id = request.view.session_id,
                .final_text = response.view.text,
                .context_providers_csv = obs_providers_csv,
                .tool_calls_csv = obs_tool_calls_csv,
                .completion_type = completion_outcome,
                .error_message = response.view.error_message,
            };
            for (size_t i = 0; i < core->completion_observer_count; i++) {
                core->completion_observers[i].fn(&summary,
                                                 core->completion_observers[i].user_ctx);
            }
        }

        claw_core_publish_out_message_if_requested(&request, &response);
        if (request.view.flags & CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE) {
            claw_core_free_response_item(&response);
        } else if (claw_core_response_push(core, &response) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enqueue response for request_id=%" PRIu32, request.view.request_id);
            claw_core_free_response_item(&response);
        }

        claw_core_llm_response_free(&llm_response);
        cJSON_Delete(runtime_messages);
        cJSON_Delete(messages);
        free(system_prompt);
        free(tools_json);
        claw_core_free_cached_contexts(request_start_contexts, request_start_context_count);
        claw_core_free_request_item(&request);
    }

    ESP_LOGI(core->log_tag, "Stopped worker task");
    core->task_handle = NULL;
    core->started = false;
    claw_task_delete(NULL);
}
