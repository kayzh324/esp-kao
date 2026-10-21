import { For, Show, createSignal, onCleanup, onMount } from 'solid-js';
import type { Component } from 'solid-js';
import { fetchAgentStates, type AgentState } from '../../api/client';
import { t } from '../../i18n';

/** The subagent types the root agent can dispatch, with their display labels. */
const AGENT_TYPES = {
  subagent: 'agentType_subagent',
  recon: 'agentType_recon',
  build: 'agentType_build',
  verify: 'agentType_verify',
  bench: 'agentType_bench',
} as const;

const AGENT_TYPE_KEYS = ['subagent', 'recon', 'build', 'verify', 'bench'] as const;

const POLL_MS = 2000;

const COLOR_NEVER = 'var(--color-text-muted)';
const COLOR_IDLE = 'var(--color-green)';
const COLOR_BUSY = '#f5a623';
const COLOR_ERROR = '#e5484d';

/**
 * Subagent status lights: one dot per dispatchable agent type.
 * gray = never dispatched, green = idle after working, amber = working, red = error.
 */
export const AgentLights: Component<{ chatId?: string }> = (props) => {
  const [states, setStates] = createSignal<AgentState[]>([]);

  const poll = async () => {
    try {
      const res = await fetchAgentStates(props.chatId);
      setStates(res.agents ?? []);
    } catch {
      /* informational only: keep the last known state */
    }
  };

  onMount(() => {
    void poll();
    const timer = window.setInterval(() => void poll(), POLL_MS);
    onCleanup(() => window.clearInterval(timer));
  });

  const colorFor = (type: string): string => {
    const list = states().filter((a) => a.type === type);
    if (list.length === 0) return COLOR_NEVER;
    if (list.some((a) => a.status === 'error')) return COLOR_ERROR;
    if (list.some((a) => a.busy)) return COLOR_BUSY;
    return COLOR_IDLE;
  };

  /* Highest in-flight round for this agent type; 0 when none is busy. */
  const roundFor = (type: string): number => {
    const busy = states().filter((a) => a.type === type && a.busy);
    return busy.length ? Math.max(...busy.map((a) => a.round ?? 0)) : 0;
  };

  return (
    <div class="flex items-center gap-2.5" title={t('agentLightsHint') as string}>
      <For each={AGENT_TYPE_KEYS}>
        {(type) => (
          <span class="inline-flex items-center gap-1.5">
            <span class="h-1.5 w-1.5 rounded-full" style={{ background: colorFor(type) }} />
            <span class="text-[0.72rem] text-[var(--color-text-muted)]">
              {t(AGENT_TYPES[type]) as string}
            </span>
            <Show when={roundFor(type) > 0}>
              <span class="rounded bg-white/8 px-1 text-[0.62rem] text-[var(--color-text-secondary)]">
                R{roundFor(type)}
              </span>
            </Show>
          </span>
        )}
      </For>
    </div>
  );
};
