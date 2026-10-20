/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "claw_cap.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_agent_mgr_register_group(void);

/**
 * @brief  LLM authorizer implementing subagent type capability boundaries (added)
 *
 *         Register with claw_cap_set_llm_authorizer(). Returns false when the
 *         calling subagent's type must not use this capability.
 */
bool cap_agent_mgr_llm_allow(const char *group_id,
                             const claw_cap_descriptor_t *descriptor,
                             const claw_cap_call_context_t *ctx,
                             void *user_ctx);

#ifdef __cplusplus
}
#endif
