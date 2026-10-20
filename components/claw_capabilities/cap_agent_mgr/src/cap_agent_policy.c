/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Subagent type policy (added).
 *
 * A subagent type is a capability boundary, not just a prompt:
 *   - recon investigates only; it must not modify files or start Lua jobs.
 *   - verify may run things but must not modify files, otherwise it can "fix"
 *     the thing under test until it passes.
 *   - build and the default subagent are unrestricted apart from the existing
 *     root-only rules.
 *
 * The policy is registered through claw_cap_set_llm_authorizer(), so it applies
 * to both the tool list the model sees and the actual invocation.
 */

#include "cap_agent_mgr.h"

#include <string.h>

static bool cap_agent_policy_is_denied(const char *const *denied,
                                       size_t denied_count,
                                       const char *tool_id)
{
    for (size_t i = 0; i < denied_count; i++) {
        if (strcmp(denied[i], tool_id) == 0) {
            return true;
        }
    }
    return false;
}

bool cap_agent_mgr_llm_allow(const char *group_id,
                             const claw_cap_descriptor_t *descriptor,
                             const claw_cap_call_context_t *ctx,
                             void *user_ctx)
{
    static const char *const recon_denied[] = {
        "write_file", "edit_file", "delete_file", "copy_file", "move_file", "restore_file",
        "lua_run_script", "lua_run_script_async", "lua_stop_async_job",
    };
    static const char *const verify_denied[] = {
        "write_file", "edit_file", "delete_file", "copy_file", "move_file", "restore_file",
    };
    const char *agent_type;

    (void)group_id;
    (void)user_ctx;

    if (!descriptor || !descriptor->id || !ctx) {
        return true;
    }
    if (ctx->caller != CLAW_CAP_CALLER_SUB_AGENT) {
        return true;
    }
    agent_type = ctx->agent_type;
    if (!agent_type || !agent_type[0]) {
        return true;
    }

    if (strcmp(agent_type, "recon") == 0) {
        return !cap_agent_policy_is_denied(recon_denied,
                                           sizeof(recon_denied) / sizeof(recon_denied[0]),
                                           descriptor->id);
    }
    if (strcmp(agent_type, "verify") == 0) {
        return !cap_agent_policy_is_denied(verify_denied,
                                           sizeof(verify_denied) / sizeof(verify_denied[0]),
                                           descriptor->id);
    }
    return true;
}
