/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/claw_llm_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "claw_llm";

#define CLAW_LLM_DEFAULT_TIMEOUT_MS (120 * 1000)
#define CLAW_LLM_DEFAULT_MAX_TOKENS 8192
#define CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES (512 * 1024)

struct claw_llm_runtime {
    claw_llm_runtime_config_t config;
    claw_llm_model_profile_t profile;
    const claw_llm_backend_vtable_t *backend;
    void *backend_ctx;
};

static char *dup_or_null(const char *value)
{
    return value ? strdup(value) : NULL;
}

static bool string_is_empty(const char *value)
{
    return !value || value[0] == '\0';
}

static void runtime_config_free(claw_llm_runtime_config_t *config)
{
    if (!config) {
        return;
    }

    free((char *)config->api_key);
    free((char *)config->backend_type);
    free((char *)config->model);
    free((char *)config->base_url);
    free((char *)config->auth_type);
    free((char *)config->max_tokens_field);
    free((char *)config->reasoning_effort);
    memset(config, 0, sizeof(*config));
}

static esp_err_t runtime_config_copy(claw_llm_runtime_config_t *dst,
                                     const claw_llm_runtime_config_t *src)
{
    dst->api_key = dup_or_null(src->api_key);
    dst->backend_type = dup_or_null(src->backend_type);
    dst->model = dup_or_null(src->model);
    dst->base_url = dup_or_null(src->base_url);
    dst->auth_type = dup_or_null(src->auth_type);
    dst->max_tokens_field = dup_or_null(src->max_tokens_field);
    dst->reasoning_effort = dup_or_null(src->reasoning_effort);
    dst->timeout_ms = src->timeout_ms;
    dst->max_tokens = src->max_tokens;
    dst->image_max_bytes = src->image_max_bytes;
    dst->supports_tools = src->supports_tools;
    dst->supports_vision = src->supports_vision;
    dst->image_remote_url_only = src->image_remote_url_only;

    if ((src->api_key && !dst->api_key) ||
            (src->backend_type && !dst->backend_type) ||
            (src->model && !dst->model) ||
            (src->base_url && !dst->base_url) ||
            (src->auth_type && !dst->auth_type) ||
            (src->max_tokens_field && !dst->max_tokens_field) ||
            (src->reasoning_effort && !dst->reasoning_effort)) {
        runtime_config_free(dst);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t claw_llm_runtime_init(claw_llm_runtime_t **out_runtime,
                                const claw_llm_runtime_config_t *config,
                                char **out_error_message)
{
    claw_llm_runtime_t *runtime;
    const claw_llm_backend_registration_t *backend_registration;
    esp_err_t err;

    if (out_runtime) {
        *out_runtime = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!out_runtime || !config || !config->api_key || !config->model ||
            !config->backend_type || !config->backend_type[0] || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    backend_registration = claw_llm_find_backend_registration(config->backend_type);
    if (!backend_registration || !backend_registration->vtable) {
        *out_error_message = strdup("Unknown LLM backend type");
        return ESP_ERR_NOT_SUPPORTED;
    }

    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        *out_error_message = strdup("Out of memory allocating runtime");
        return ESP_ERR_NO_MEM;
    }

    err = runtime_config_copy(&runtime->config, config);
    if (err != ESP_OK) {
        free(runtime);
        *out_error_message = strdup("Out of memory copying runtime config");
        return err;
    }
    if (string_is_empty(runtime->config.auth_type)) {
        free((char *)runtime->config.auth_type);
        runtime->config.auth_type = dup_or_null(backend_registration->defaults.auth_type);
    }
    if (!runtime->config.timeout_ms) {
        runtime->config.timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS;
    }
    if (!runtime->config.max_tokens) {
        runtime->config.max_tokens = CLAW_LLM_DEFAULT_MAX_TOKENS;
    }
    if (!runtime->config.image_max_bytes) {
        runtime->config.image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES;
    }
    if (string_is_empty(runtime->config.max_tokens_field)) {
        free((char *)runtime->config.max_tokens_field);
        runtime->config.max_tokens_field = dup_or_null(backend_registration->defaults.max_tokens_field);
    }
    if (!runtime->config.backend_type ||
            (backend_registration->defaults.auth_type && !runtime->config.auth_type) ||
            (backend_registration->defaults.max_tokens_field && !runtime->config.max_tokens_field)) {
        runtime_config_free(&runtime->config);
        free(runtime);
        *out_error_message = strdup("Out of memory finalizing runtime config");
        return ESP_ERR_NO_MEM;
    }

    runtime->profile.chat_path = backend_registration->defaults.chat_path ?
                                 backend_registration->defaults.chat_path : "";
    runtime->profile.max_tokens_field = runtime->config.max_tokens_field ?
                                        runtime->config.max_tokens_field : "";
    runtime->profile.supports_tools = runtime->config.supports_tools;
    runtime->profile.supports_vision = runtime->config.supports_vision;
    runtime->profile.image_remote_url_only = runtime->config.image_remote_url_only;
    /* opencode-go provider support (added): optional per-session header name. */
    runtime->profile.session_header_name = backend_registration->defaults.session_header_name;
    runtime->backend = backend_registration->vtable;

    err = runtime->backend->init(&runtime->config, &runtime->profile, &runtime->backend_ctx, out_error_message);
    if (err != ESP_OK) {
        runtime_config_free(&runtime->config);
        free(runtime);
        return err;
    }

    *out_runtime = runtime;
    return ESP_OK;
}

esp_err_t claw_llm_runtime_chat(claw_llm_runtime_t *runtime,
                                const claw_llm_chat_request_t *request,
                                claw_llm_response_t *out_response,
                                char **out_error_message)
{
    if (!runtime || !runtime->backend || !runtime->backend->chat) {
        return ESP_ERR_INVALID_STATE;
    }

    return runtime->backend->chat(runtime->backend_ctx,
                                  &runtime->profile,
                                  request,
                                  out_response,
                                  out_error_message);
}

/* llm-retry (added): shared with the agent loop, which used to own the only
 * copy of this policy. A transient failure produced no model output, so the
 * identical request can be resent safely. */
bool claw_llm_failure_is_transient(esp_err_t err, int http_status)
{
    switch (err) {
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_TIMEOUT:
        return true;
    default:
        break;
    }
    /* A gateway answered but could not serve the request. A 4xx is our request's
     * fault and will fail the same way, so it is not retried. */
    return http_status >= 500 || http_status == 429;
}

uint32_t claw_llm_retry_delay_ms(uint32_t attempt)
{
    uint32_t delay = CLAW_LLM_RETRY_BASE_MS;
    uint32_t jitter;

    for (uint32_t i = 0; i < attempt && delay < CLAW_LLM_RETRY_MAX_MS; i++) {
        delay <<= 1;
    }
    if (delay > CLAW_LLM_RETRY_MAX_MS) {
        delay = CLAW_LLM_RETRY_MAX_MS;
    }
    /* +/-25%: 0..half of the delay, minus a quarter. */
    jitter = (delay * (esp_random() % 51)) / 100;
    return delay - (delay / 4) + jitter;
}

esp_err_t claw_llm_runtime_chat_with_retry(claw_llm_runtime_t *runtime,
                                           const claw_llm_chat_request_t *request,
                                           claw_llm_response_t *out_response,
                                           char **out_error_message)
{
    uint32_t retry_count = 0;

    if (!runtime || !request || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    for (;;) {
        esp_err_t err = claw_llm_runtime_chat(runtime, request, out_response, out_error_message);

        if (err == ESP_OK) {
            return ESP_OK;
        }

        bool aborted = request->abort_flag && *request->abort_flag;

        if (aborted ||
                retry_count >= CLAW_LLM_RETRY_MAX_RETRIES ||
                !claw_llm_failure_is_transient(err, out_response->http_status)) {
            if (retry_count > 0 && *out_error_message) {
                char *with_attempts = NULL;
                int needed = snprintf(NULL, 0, "%s (gave up after %u attempts)",
                                      *out_error_message, (unsigned)(retry_count + 1));

                if (needed > 0) {
                    with_attempts = malloc((size_t)needed + 1);
                    if (with_attempts) {
                        snprintf(with_attempts, (size_t)needed + 1,
                                 "%s (gave up after %u attempts)",
                                 *out_error_message, (unsigned)(retry_count + 1));
                        free(*out_error_message);
                        *out_error_message = with_attempts;
                    }
                }
            }
            return err;
        }

        uint32_t delay_ms = claw_llm_retry_delay_ms(retry_count);

        retry_count++;
        ESP_LOGW(TAG,
                 "LLM call failed (%s, http=%d); retry %u/%u in %" PRIu32 " ms",
                 *out_error_message ? *out_error_message : esp_err_to_name(err),
                 out_response->http_status,
                 (unsigned)retry_count,
                 (unsigned)CLAW_LLM_RETRY_MAX_RETRIES,
                 delay_ms);
        free(*out_error_message);
        *out_error_message = NULL;
        out_response->http_status = 0;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t claw_llm_runtime_infer_media(claw_llm_runtime_t *runtime,
                                       const claw_llm_media_request_t *request,
                                       char **out_text,
                                       char **out_error_message)
{
    if (!runtime || !runtime->backend || !runtime->backend->infer_media) {
        return ESP_ERR_INVALID_STATE;
    }

    return runtime->backend->infer_media(runtime->backend_ctx,
                                         &runtime->profile,
                                         request,
                                         out_text,
                                         out_error_message);
}

void claw_llm_runtime_deinit(claw_llm_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    if (runtime->backend && runtime->backend->deinit) {
        runtime->backend->deinit(runtime->backend_ctx);
    }
    runtime_config_free(&runtime->config);
    free(runtime);
}

void claw_llm_response_free(claw_llm_response_t *response)
{
    size_t i;

    if (!response) {
        return;
    }

    free(response->text);
    free(response->reasoning_content);
    free(response->raw_message_json);
    for (i = 0; i < response->tool_call_count; i++) {
        free(response->tool_calls[i].id);
        free(response->tool_calls[i].name);
        free(response->tool_calls[i].arguments_json);
    }
    free(response->tool_calls);
    memset(response, 0, sizeof(*response));
}
