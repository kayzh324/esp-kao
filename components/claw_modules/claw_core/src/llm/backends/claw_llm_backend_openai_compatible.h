/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_runtime.h"

#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_ID "openai_compatible"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_CHAT_PATH "/chat/completions"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_AUTH_TYPE "bearer"
#define CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_DEFAULT_MAX_TOKENS_FIELD "max_tokens"
#define CLAW_LLM_BACKEND_TRIAL_ID "trial"

/*
 * opencode-go provider support (added) - do not drop on project update.
 * Standard OpenAI-compatible protocol plus an "x-opencode-session" header
 * carrying the ongoing conversation's session id on every LLM request.
 * See the block comment in claw_llm_backend_openai_compatible.c for details.
 */
#define CLAW_LLM_BACKEND_OPENCODE_GO_ID "opencode_go"
#define CLAW_LLM_BACKEND_OPENCODE_GO_CHAT_PATH "/chat/completions"
#define CLAW_LLM_BACKEND_OPENCODE_GO_AUTH_TYPE "bearer"
#define CLAW_LLM_BACKEND_OPENCODE_GO_DEFAULT_MAX_TOKENS_FIELD "max_tokens"
#define CLAW_LLM_BACKEND_OPENCODE_GO_SESSION_HEADER "x-opencode-session"
#define CLAW_LLM_BACKEND_OPENCODE_GO_USER_AGENT "esp-claw/1.1.0"

const claw_llm_backend_vtable_t *claw_llm_backend_openai_compatible_vtable(void);
const claw_llm_backend_registration_t *claw_llm_backend_openai_compatible_registration(void);
const claw_llm_backend_registration_t *claw_llm_backend_opencode_go_registration(void);
const claw_llm_backend_registration_t *claw_llm_backend_trial_registration(void);
