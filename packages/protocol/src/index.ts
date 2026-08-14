/** Shared Hermes MQTT message types (ESP32 <-> bridge via AWS IoT Core). */

export type AgentKind = "codex" | "cursor";

export type AgentState =
  | "idle"
  | "thinking"
  | "running"
  | "waiting_approval"
  | "error"
  | "offline";

/** Compact CLI status shown on the device chat pane. */
export type CliStatus = "idle" | "running" | "waiting";

export type ApprovalKind =
  | "command"
  | "file_change"
  | "permissions"
  | "network"
  | "other";

export type ChatSummary = {
  id: string;
  title: string;
  preview: string;
  cwd?: string;
  live?: boolean;
};

/** Topics under hermes/{userId}/... */
export function topics(userId: string) {
  const base = `hermes/${userId}`;
  return {
    base,
    deviceUp: `${base}/device/up`,
    deviceDown: `${base}/device/down`,
  } as const;
}

export type DeviceUpMessage =
  | { type: "hello"; deviceId: string; firmware?: string }
  | { type: "ping"; ts?: number }
  | { type: "approve"; id: string; always?: boolean }
  | { type: "deny"; id: string }
  | { type: "prompt"; agent: "codex"; text: string }
  | { type: "select_chat"; agent: AgentKind; id: string }
  | { type: "refresh_chats"; agent?: AgentKind }
  | { type: "voice_begin"; id: string; hz: number; bytes?: number }
  | { type: "voice_chunk"; id: string; seq: number; d: string }
  | { type: "voice_end"; id: string; error?: string };

export type DeviceDownMessage =
  | {
      type: "status";
      agent: AgentKind;
      state: AgentState;
      summary: string;
      updatedAt: string;
    }
  | {
      type: "log";
      agent: AgentKind;
      text: string;
      truncated?: boolean;
      chatId?: string;
    }
  | {
      type: "chats";
      agent: AgentKind;
      chats: ChatSummary[];
    }
  | {
      type: "chat_view";
      agent: AgentKind;
      id: string;
      cwd: string;
      quotaLeft: string;
      model: string;
      reasoning: string;
      cli: CliStatus;
      last: string;
      approvalId?: string;
      approvalTitle?: string;
    }
  | {
      type: "approval";
      id: string;
      agent: AgentKind;
      kind: ApprovalKind;
      title: string;
      detail: string;
    }
  | { type: "ack"; of: string; ok: boolean; error?: string }
  | { type: "error"; message: string }
  | { type: "transcript"; text: string; append?: boolean }
  | { type: "transcript_error"; message: string };

export function parseDeviceUp(raw: string): DeviceUpMessage | null {
  try {
    const msg = JSON.parse(raw) as DeviceUpMessage;
    if (!msg || typeof msg !== "object" || !("type" in msg)) return null;
    return msg;
  } catch {
    return null;
  }
}

export function truncateForDisplay(text: string, max = 240): {
  text: string;
  truncated: boolean;
} {
  const cleaned = text.replace(/\s+/g, " ").trim();
  if (cleaned.length <= max) return { text: cleaned, truncated: false };
  return { text: cleaned.slice(0, max - 1) + "…", truncated: true };
}
