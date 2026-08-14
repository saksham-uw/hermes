import { spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";
import WebSocket from "ws";
import {
  truncateForDisplay,
  type ApprovalKind,
  type ChatSummary,
  type CliStatus,
} from "@hermes/protocol";
import type { BridgeConfig } from "../config.js";
import type { DownPublisher } from "../iot/client.js";

type JsonRpcResponse = {
  id?: number | string | null;
  result?: unknown;
  error?: { message?: string };
  method?: string;
  params?: unknown;
};

type PendingApproval = {
  rpcId: number | string;
  kind: ApprovalKind;
  title: string;
  detail: string;
};

type CodexThread = {
  id: string;
  name?: string | null;
  preview?: string | null;
  cwd?: string | null;
  updatedAt?: number | null;
  createdAt?: number | null;
  status?: { type?: string } | null;
  turns?: unknown[];
};

export type CodexAdapter = {
  handleApprove: (id: string, always?: boolean) => Promise<boolean>;
  handleDeny: (id: string) => Promise<boolean>;
  handlePrompt: (text: string) => Promise<void>;
  selectChat: (id: string) => Promise<boolean>;
  syncChats: () => Promise<void>;
  close: () => Promise<void>;
};

function tokenPath(): string {
  const dir = join(homedir(), ".hermes");
  mkdirSync(dir, { recursive: true });
  return join(dir, "codex-app-server.token");
}

function ensureToken(config: BridgeConfig): string {
  if (config.codexAuthToken) return config.codexAuthToken;
  const path = tokenPath();
  if (existsSync(path)) return readFileSync(path, "utf8").trim();
  const generated =
    randomUUID().replace(/-/g, "") + randomUUID().replace(/-/g, "");
  writeFileSync(path, generated, { mode: 0o600 });
  return generated;
}

async function sleep(ms: number) {
  await new Promise((r) => setTimeout(r, ms));
}

function asRecord(v: unknown): Record<string, unknown> | null {
  return v && typeof v === "object" ? (v as Record<string, unknown>) : null;
}

function extractText(node: unknown): string {
  if (!node) return "";
  if (typeof node === "string") return node;
  const obj = asRecord(node);
  if (!obj) return "";
  if (typeof obj.text === "string") return obj.text;
  if (typeof obj.content === "string") return obj.content;
  if (Array.isArray(obj.content)) {
    return obj.content
      .map((c) => extractText(c))
      .filter(Boolean)
      .join(" ");
  }
  if (Array.isArray(obj.parts)) {
    return obj.parts
      .map((c) => extractText(c))
      .filter(Boolean)
      .join(" ");
  }
  return "";
}

function linesFromThread(thread: CodexThread): string[] {
  const lines: string[] = [];
  const turns = Array.isArray(thread.turns) ? thread.turns : [];
  for (const turn of turns) {
    const t = asRecord(turn);
    if (!t) continue;
    const items = Array.isArray(t.items)
      ? t.items
      : Array.isArray(t.content)
        ? t.content
        : [];
    for (const item of items) {
      const it = asRecord(item);
      if (!it) continue;
      const typ = typeof it.type === "string" ? it.type : "";
      const text = extractText(it);
      if (!text) continue;
      if (
        typ === "userMessage" ||
        typ === "user" ||
        (typ === "message" && it.role === "user")
      ) {
        lines.push(`> ${truncateForDisplay(text, 110).text}`);
      } else if (
        typ === "agentMessage" ||
        typ === "agent" ||
        typ === "assistant" ||
        (typ === "message" && it.role === "assistant")
      ) {
        lines.push(truncateForDisplay(text, 110).text);
      }
    }
    // Some payloads put input on the turn itself.
    if (Array.isArray(t.input)) {
      for (const inp of t.input) {
        const text = extractText(inp);
        if (text) lines.push(`> ${truncateForDisplay(text, 110).text}`);
      }
    }
  }
  return lines.slice(-36);
}

function threadTitle(t: CodexThread): string {
  const name = (t.name || "").trim();
  if (name) return truncateForDisplay(name, 36).text;
  const preview = (t.preview || "").trim();
  if (preview) return truncateForDisplay(preview, 36).text;
  const cwd = (t.cwd || "").trim();
  if (cwd) {
    const base = cwd.split("/").filter(Boolean).pop() || cwd;
    return truncateForDisplay(base, 36).text;
  }
  return truncateForDisplay(t.id, 36).text;
}

function lastMessageFromThread(thread: CodexThread): string {
  const lines = linesFromThread(thread);
  return lines.length ? lines[lines.length - 1] : "";
}

function shortCwd(cwd?: string | null): string {
  if (!cwd) return "-";
  const parts = cwd.split("/").filter(Boolean);
  if (parts.length <= 2) return truncateForDisplay(cwd, 28).text;
  return truncateForDisplay(parts.slice(-2).join("/"), 28).text;
}

function weeklyQuotaLeft(rateLimits: unknown): string {
  const root = asRecord(rateLimits);
  if (!root) return "-";
  const byId = asRecord(root.rateLimitsByLimitId) || asRecord(root);
  const buckets: Array<{ used?: number; mins?: number }> = [];
  const collect = (node: unknown) => {
    const o = asRecord(node);
    if (!o) return;
    const primary = asRecord(o.primary) || o;
    const used =
      typeof primary.usedPercent === "number"
        ? primary.usedPercent
        : typeof o.usedPercent === "number"
          ? o.usedPercent
          : undefined;
    const mins =
      typeof primary.windowDurationMins === "number"
        ? primary.windowDurationMins
        : typeof o.windowDurationMins === "number"
          ? o.windowDurationMins
          : undefined;
    if (typeof used === "number") buckets.push({ used, mins });
  };
  collect(root.rateLimits);
  if (byId) {
    for (const v of Object.values(byId)) collect(v);
  }
  if (!buckets.length) return "-";
  const weekly = buckets.find((b) => (b.mins || 0) >= 10080);
  const day = buckets.find((b) => (b.mins || 0) >= 1440);
  const pick = weekly || day || buckets.reduce((a, b) =>
    (b.mins || 0) > (a.mins || 0) ? b : a,
  );
  const left = Math.max(0, Math.min(100, Math.round(100 - (pick.used || 0))));
  return `${left}%`;
}

function contextLabel(usage: unknown): string {
  const o = asRecord(usage);
  if (!o) return "-";
  const used =
    (typeof o.lastTotalTokens === "number" && o.lastTotalTokens) ||
    (typeof o.totalTokens === "number" && o.totalTokens) ||
    (typeof o.usedTokens === "number" && o.usedTokens) ||
    (typeof o.inputTokens === "number" && o.inputTokens) ||
    0;
  const window =
    (typeof o.contextWindow === "number" && o.contextWindow) ||
    (typeof o.contextTokens === "number" && o.contextTokens) ||
    (typeof o.maxContextTokens === "number" && o.maxContextTokens) ||
    0;
  if (window > 0 && used >= 0) {
    const pct = Math.max(0, Math.min(100, Math.round((used / window) * 100)));
    return `${pct}%`;
  }
  if (used > 0) {
    if (used >= 1000) return `${(used / 1000).toFixed(used >= 10000 ? 0 : 1)}k`;
    return `${used}`;
  }
  return "-";
}

export async function startCodexAdapter(
  config: BridgeConfig,
  publishDown: DownPublisher,
): Promise<CodexAdapter> {
  let child: ChildProcess | null = null;
  const token = ensureToken(config);
  const pendingApprovals = new Map<string, PendingApproval>();
  let nextId = 1;
  const rpcWaiters = new Map<
    number | string,
    { resolve: (v: unknown) => void; reject: (e: Error) => void }
  >();

  let threadId: string | null = null;
  let threadTitleCached = "";
  let threadCwd = "";
  let lastMessage = "";
  let cli: CliStatus = "idle";
  let quotaLeft = "-";
  let contextUsed = "-";
  let lastViewJson = "";
  let tokenUsage: unknown = null;
  let ws: WebSocket | null = null;
  let closed = false;

  const setStatus = async (
    state: "idle" | "thinking" | "running" | "waiting_approval" | "error" | "offline",
    summary: string,
  ) => {
    await publishDown({
      type: "status",
      agent: "codex",
      state,
      summary,
      updatedAt: new Date().toISOString(),
    });
  };

  if (config.codexSpawn) {
    const url = new URL(config.codexWsUrl);
    const listen = `ws://${url.hostname}:${url.port || "4520"}`;
    const tokenFile = tokenPath();
    writeFileSync(tokenFile, token, { mode: 0o600 });

    const args = [
      "app-server",
      "--listen",
      listen,
      "--ws-auth",
      "capability-token",
      "--ws-token-file",
      tokenFile,
    ];
    console.log(`[codex] spawning: codex ${args.join(" ")}`);
    child = spawn("codex", args, {
      cwd: config.codexCwd || undefined,
      stdio: ["ignore", "pipe", "pipe"],
      env: {
        ...process.env,
        // Ensure /usr/bin/bwrap is findable without shadowing nvm's node.
        PATH: (() => {
          const base = process.env.PATH ?? "";
          return base.split(":").includes("/usr/bin")
            ? base
            : `${base}:/usr/bin:/bin`;
        })(),
      },
    });
    child.stdout?.on("data", (d) => process.stdout.write(`[codex-server] ${d}`));
    child.stderr?.on("data", (d) => process.stderr.write(`[codex-server] ${d}`));
    child.on("exit", (code) => {
      console.warn(`[codex] app-server exited code=${code}`);
      void setStatus("offline", `app-server exited (${code})`);
    });
  }

  const headers: Record<string, string> = {};
  if (token) headers.Authorization = `Bearer ${token}`;

  const maxAttempts = 30;
  let lastErr: unknown;
  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    try {
      ws = await new Promise<WebSocket>((resolve, reject) => {
        const socket = new WebSocket(config.codexWsUrl, { headers });
        const timer = setTimeout(() => {
          try {
            socket.terminate();
          } catch {
            /* ignore */
          }
          reject(new Error("codex ws connect timeout"));
        }, 3_000);
        socket.once("open", () => {
          clearTimeout(timer);
          resolve(socket);
        });
        socket.once("error", (err) => {
          clearTimeout(timer);
          reject(err);
        });
      });
      break;
    } catch (err) {
      lastErr = err;
      console.warn(
        `[codex] waiting for app-server (${attempt}/${maxAttempts}):`,
        err instanceof Error ? err.message : err,
      );
      await sleep(500);
    }
  }
  if (!ws) {
    throw lastErr instanceof Error
      ? lastErr
      : new Error("codex ws connect failed");
  }
  const socket = ws;
  console.log(`[codex] connected ${config.codexWsUrl}`);

  const send = (method: string, params?: unknown): Promise<unknown> => {
    const id = nextId++;
    return new Promise((resolve, reject) => {
      rpcWaiters.set(id, { resolve, reject });
      socket.send(JSON.stringify({ id, method, params }));
      setTimeout(() => {
        if (rpcWaiters.has(id)) {
          rpcWaiters.delete(id);
          reject(new Error(`RPC timeout: ${method}`));
        }
      }, 60_000);
    });
  };

  const notify = (method: string, params?: unknown) => {
    socket.send(JSON.stringify({ method, params }));
  };

  const respond = (id: number | string, result: unknown) => {
    socket.send(JSON.stringify({ id, result }));
  };

  const listThreads = async (): Promise<CodexThread[]> => {
    const attempts: Array<Record<string, unknown>> = [
      {
        limit: 25,
        sortKey: "updated_at",
        sortDirection: "desc",
        archived: false,
      },
      {
        limit: 25,
        sortKey: "created_at",
        sortDirection: "desc",
        archived: false,
        sourceKinds: ["cli", "vscode", "appServer", "unknown"],
      },
      { limit: 25 },
    ];

    let lastErr: unknown;
    for (const params of attempts) {
      try {
        const result = (await send("thread/list", params)) as {
          data?: CodexThread[];
        };
        const data = Array.isArray(result?.data) ? result.data : [];
        console.log(
          `[codex] thread/list ok count=${data.length} params=${JSON.stringify(params)}`,
        );
        if (data.length || params === attempts[attempts.length - 1]) return data;
      } catch (err) {
        lastErr = err;
        console.warn(
          `[codex] thread/list failed params=${JSON.stringify(params)}:`,
          err instanceof Error ? err.message : err,
        );
      }
    }
    throw lastErr instanceof Error ? lastErr : new Error("thread/list failed");
  };

  const publishChatList = async (threads: CodexThread[]) => {
    const chats: ChatSummary[] = threads.slice(0, 6).map((t) => ({
      id: t.id,
      title: truncateForDisplay(threadTitle(t), 28).text,
      preview: truncateForDisplay(
        (t.preview || t.cwd || t.id || "").toString(),
        40,
      ).text,
      cwd: t.cwd ? truncateForDisplay(t.cwd, 40).text : undefined,
      live: t.id === threadId || t.status?.type === "active",
    }));
    const payload = { type: "chats" as const, agent: "codex" as const, chats };
    console.log(`[codex] publishing chats n=${chats.length}`);
    await publishDown(payload);
  };

  const refreshQuota = async () => {
    try {
      const res = await send("account/rateLimits/read", {});
      quotaLeft = weeklyQuotaLeft(res);
    } catch (err) {
      console.warn(
        "[codex] rateLimits/read failed:",
        err instanceof Error ? err.message : err,
      );
    }
  };

  const publishChatView = async (force = false) => {
    if (!threadId) return;
    const pending = [...pendingApprovals.values()][0];
    const waiting = cli === "waiting" && pending;
    const payload = {
      type: "chat_view" as const,
      agent: "codex" as const,
      id: threadId,
      cwd: threadCwd || "-",
      quotaLeft,
      context: contextUsed,
      cli,
      last: cli === "running" ? "" : lastMessage,
      approvalId: waiting ? String(pending.rpcId) : undefined,
      approvalTitle: waiting ? pending.title : undefined,
    };
    const json = JSON.stringify(payload);
    if (!force && json === lastViewJson) return;
    lastViewJson = json;
    console.log(
      `[codex] chat_view cli=${cli} cwd=${payload.cwd} quota=${quotaLeft} ctx=${contextUsed} bytes=${json.length}`,
    );
    await publishDown(payload);
  };

  const loadLastFromThread = async (id: string) => {
    try {
      const read = (await send("thread/read", {
        threadId: id,
        includeTurns: true,
      })) as { thread?: CodexThread };
      const thread = read?.thread;
      if (thread?.cwd) threadCwd = shortCwd(thread.cwd);
      lastMessage = thread ? lastMessageFromThread(thread) : lastMessage;
    } catch (err) {
      console.warn("[codex] thread/read failed:", err);
    }
  };

  const resumeThread = async (id: string, title?: string, cwd?: string | null) => {
    const resumed = (await send("thread/resume", {
      threadId: id,
    })) as { thread?: CodexThread };
    threadId = resumed?.thread?.id || id;
    threadTitleCached = title || threadTitle(resumed?.thread || { id });
    threadCwd = shortCwd(cwd || resumed?.thread?.cwd);
    if (cli !== "waiting" && cli !== "running") cli = "idle";
    console.log(`[codex] resumed ${threadId} (${threadTitleCached})`);
    await refreshQuota();
    await loadLastFromThread(threadId);
    contextUsed = contextLabel(tokenUsage);
    await publishChatView(true);
  };

  const pickFollowThread = (threads: CodexThread[]): CodexThread | null => {
    if (!threads.length) return null;
    const active = threads.find((t) => t.status?.type === "active");
    if (active) return active;
    return threads[0];
  };

  const runSyncChats = async (opts?: { follow?: boolean }) => {
    if (closed) return;
    try {
      const threads = await listThreads();
      console.log(`[codex] listed ${threads.length} thread(s)`);
      await publishChatList(threads);
      if (opts?.follow === false) return;
      const target = pickFollowThread(threads);
      if (!target) {
        threadId = null;
        return;
      }
      if (target.id !== threadId) {
        await resumeThread(target.id, threadTitle(target), target.cwd);
        await publishChatList(threads);
      } else {
        await publishChatView();
      }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      console.warn("[codex] syncChats failed:", msg);
      await setStatus("error", `chat sync: ${truncateForDisplay(msg, 60).text}`);
      await publishDown({ type: "chats", agent: "codex", chats: [] });
    }
  };

  const handleServerMessage = async (raw: string) => {
    let msg: JsonRpcResponse;
    try {
      msg = JSON.parse(raw) as JsonRpcResponse;
    } catch {
      return;
    }

    if (
      msg.id !== undefined &&
      msg.id !== null &&
      (msg.result !== undefined || msg.error)
    ) {
      const waiter = rpcWaiters.get(msg.id);
      if (waiter) {
        rpcWaiters.delete(msg.id);
        if (msg.error) waiter.reject(new Error(msg.error.message || "RPC error"));
        else waiter.resolve(msg.result);
      }
      return;
    }

    if (!msg.method) return;
    const method = msg.method;
    const params = (msg.params ?? {}) as Record<string, unknown>;

    if (method.endsWith("/requestApproval") && msg.id !== undefined && msg.id !== null) {
      const approvalId = String(msg.id);
      let kind: ApprovalKind = "other";
      if (method.includes("commandExecution")) kind = "command";
      else if (method.includes("fileChange")) kind = "file_change";
      else if (method.includes("permissions")) kind = "permissions";
      else if (method.includes("network")) kind = "network";

      const command = typeof params.command === "string" ? params.command : "";
      const reason = typeof params.reason === "string" ? params.reason : "";
      const title = truncateForDisplay(
        kind === "command"
          ? command || "Shell command"
          : kind === "file_change"
            ? "File change approval"
            : kind === "permissions"
              ? "Permissions approval"
              : "Approval needed",
        80,
      ).text;
      const detail = truncateForDisplay(
        reason || command || JSON.stringify(params).slice(0, 400),
        400,
      ).text;

      pendingApprovals.set(approvalId, {
        rpcId: msg.id,
        kind,
        title,
        detail,
      });

      await setStatus("waiting_approval", title);
      cli = "waiting";
      await publishChatView(true);
      return;
    }

    if (method === "thread/started") {
      const thr = asRecord(params.thread);
      if (thr && typeof thr.id === "string") {
        void runSyncChats({ follow: true });
      }
      return;
    }

    if (method === "thread/tokenUsage/updated") {
      tokenUsage = params.tokenUsage || params.usage || params;
      contextUsed = contextLabel(tokenUsage);
      await publishChatView();
      return;
    }

    if (method === "account/rateLimits/updated") {
      quotaLeft = weeklyQuotaLeft(params);
      await publishChatView();
      return;
    }

    if (method === "turn/started") {
      cli = "running";
      await setStatus("running", "Turn started");
      await publishChatView(true);
      return;
    }
    if (method === "turn/completed") {
      cli = pendingApprovals.size ? "waiting" : "idle";
      await setStatus(cli === "waiting" ? "waiting_approval" : "idle", "Turn completed");
      if (threadId) await loadLastFromThread(threadId);
      await refreshQuota();
      await publishChatView(true);
      return;
    }
    if (method === "item/completed") {
      const item = asRecord(params.item);
      if (item) {
        const typ = typeof item.type === "string" ? item.type : "";
        const text = extractText(item);
        if (text && (typ === "userMessage" || typ === "agentMessage")) {
          const prefix = typ === "userMessage" ? "> " : "";
          lastMessage = truncateForDisplay(prefix + text, 280).text;
          if (cli !== "running") await publishChatView();
        }
      }
      return;
    }
  };

  socket.on("message", (data) => {
    void handleServerMessage(data.toString());
  });
  socket.on("close", () => {
    void setStatus("offline", "Codex websocket closed");
  });

  await send("initialize", {
    clientInfo: { name: "hermes-bridge", version: "0.1.0" },
    capabilities: { experimentalApi: true },
  });
  notify("initialized", {});

  // Attach to existing Codex chats — snapshots only, no stream, no poll.
  await refreshQuota();
  await runSyncChats({ follow: true });

  return {
    async handleApprove(id, always) {
      const pending = pendingApprovals.get(id);
      if (!pending) return false;
      pendingApprovals.delete(id);
      const decision = always ? "acceptForSession" : "accept";
      respond(pending.rpcId, { decision, accept: true, approved: true });
      cli = "running";
      await setStatus("running", always ? "Always allowed" : "Approved");
      await publishChatView(true);
      return true;
    },
    async handleDeny(id) {
      const pending = pendingApprovals.get(id);
      if (!pending) return false;
      pendingApprovals.delete(id);
      respond(pending.rpcId, {
        decision: "decline",
        accept: false,
        approved: false,
      });
      cli = "idle";
      await setStatus("idle", "Denied");
      await publishChatView(true);
      return true;
    },
    async handlePrompt(text) {
      const trimmed = text.trim();
      if (!trimmed) return;
      if (!threadId) {
        const started = (await send("thread/start", {
          cwd: config.codexCwd || undefined,
        })) as { thread?: { id?: string }; id?: string };
        threadId = started?.thread?.id || started?.id || null;
        threadTitleCached = "new session";
      }
      if (!threadId) throw new Error("No Codex thread");
      lastMessage = `> ${truncateForDisplay(trimmed, 280).text}`;
      cli = "running";
      await setStatus("running", truncateForDisplay(trimmed, 80).text);
      await send("turn/start", {
        threadId,
        input: [{ type: "text", text: trimmed }],
      });
      await publishChatView(true);
    },
    async selectChat(id) {
      const threads = await listThreads();
      const target = threads.find((t) => t.id === id);
      if (!target) return false;
      await resumeThread(id, threadTitle(target), target.cwd);
      return true;
    },
    async syncChats() {
      await runSyncChats({ follow: false });
    },
    async close() {
      closed = true;
      try {
        socket.close();
      } catch {
        /* ignore */
      }
      if (child && !child.killed) child.kill("SIGTERM");
    },
  };
}
