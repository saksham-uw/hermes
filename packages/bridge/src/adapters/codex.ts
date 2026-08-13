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
  handleApprove: (id: string) => Promise<boolean>;
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

function isLiveStatus(status?: { type?: string } | null): boolean {
  const typ = status?.type || "";
  return typ === "active" || typ === "idle";
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
  let ws: WebSocket | null = null;
  let closed = false;
  let syncTimer: NodeJS.Timeout | null = null;

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
      env: { ...process.env },
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
      live: t.id === threadId || isLiveStatus(t.status),
    }));
    const payload = { type: "chats" as const, agent: "codex" as const, chats };
    const bytes = JSON.stringify(payload).length;
    console.log(`[codex] publishing chats n=${chats.length} bytes=${bytes}`);
    await publishDown(payload);
    await setStatus(
      "idle",
      threads.length
        ? `${threads.length} chat${threads.length === 1 ? "" : "s"}`
        : "No Codex chats yet",
    );
  };

  const publishHistory = async (id: string, title: string, lines: string[]) => {
    await publishDown({
      type: "chat_lines",
      agent: "codex",
      id,
      title,
      lines,
      replace: true,
    });
    if (lines.length) {
      await publishDown({
        type: "log",
        agent: "codex",
        text: lines[lines.length - 1],
        chatId: id,
      });
    }
  };

  const readAndPublishHistory = async (id: string, title: string) => {
    try {
      const read = (await send("thread/read", {
        threadId: id,
        includeTurns: true,
      })) as { thread?: CodexThread };
      const thread = read?.thread;
      const lines = thread ? linesFromThread(thread) : [];
      await publishHistory(id, title, lines);
      if (!lines.length) {
        await publishDown({
          type: "log",
          agent: "codex",
          text: "(thread open — waiting for messages)",
          chatId: id,
        });
      }
    } catch (err) {
      console.warn("[codex] thread/read failed:", err);
      await publishHistory(id, title, []);
    }
  };

  const resumeThread = async (id: string, title?: string) => {
    const resumed = (await send("thread/resume", {
      threadId: id,
    })) as { thread?: CodexThread };
    threadId = resumed?.thread?.id || id;
    threadTitleCached = title || threadTitle(resumed?.thread || { id });
    console.log(`[codex] resumed ${threadId} (${threadTitleCached})`);
    await setStatus("idle", threadTitleCached);
    await readAndPublishHistory(threadId, threadTitleCached);
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

      const follow = opts?.follow !== false;
      if (!follow) return;

      const target = pickFollowThread(threads);
      if (!target) {
        threadId = null;
        return;
      }
      if (target.id !== threadId) {
        await resumeThread(target.id, threadTitle(target));
        await publishChatList(threads);
      }
      // Avoid republishing full history every poll — only on resume/select.
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      console.warn("[codex] syncChats failed:", msg);
      await setStatus("error", `chat sync: ${truncateForDisplay(msg, 60).text}`);
      // Still clear the list so the device isn't stuck on a stale stub forever.
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
      await publishDown({
        type: "approval",
        id: approvalId,
        agent: "codex",
        kind,
        title,
        detail,
      });
      return;
    }

    if (method === "thread/started") {
      const thr = asRecord(params.thread);
      if (thr && typeof thr.id === "string") {
        void runSyncChats({ follow: true });
      }
      return;
    }

    if (method === "turn/started") {
      await setStatus("thinking", "Turn started");
      return;
    }
    if (method === "turn/completed") {
      await setStatus("idle", "Turn completed");
      if (threadId) {
        void readAndPublishHistory(threadId, threadTitleCached || threadId);
      }
      return;
    }
    if (
      method === "item/agentMessage/delta" ||
      method === "item/commandExecution/outputDelta"
    ) {
      const delta = typeof params.delta === "string" ? params.delta : "";
      if (delta) {
        const { text, truncated } = truncateForDisplay(delta, 200);
        await publishDown({
          type: "log",
          agent: "codex",
          text,
          truncated,
          chatId: threadId || undefined,
        });
        await setStatus("running", text);
      }
      return;
    }
    if (method === "item/completed") {
      const item = asRecord(params.item);
      if (item) {
        const typ = typeof item.type === "string" ? item.type : "";
        const text = extractText(item);
        if (text && (typ === "userMessage" || typ === "agentMessage")) {
          const prefix = typ === "userMessage" ? "> " : "";
          await publishDown({
            type: "log",
            agent: "codex",
            text: truncateForDisplay(prefix + text, 200).text,
            chatId: threadId || undefined,
          });
        }
      }
      return;
    }
    if (method.startsWith("item/") && method.endsWith("/started")) {
      await setStatus("running", method);
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

  // Attach to existing Codex chats — do NOT invent a blank thread/start.
  await runSyncChats({ follow: true });

  syncTimer = setInterval(() => {
    void runSyncChats({ follow: true });
  }, 12_000);

  return {
    async handleApprove(id) {
      const pending = pendingApprovals.get(id);
      if (!pending) return false;
      pendingApprovals.delete(id);
      respond(pending.rpcId, { decision: "accept", accept: true, approved: true });
      await setStatus("running", "Approved");
      await publishDown({ type: "ack", of: "approve", ok: true });
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
      await setStatus("idle", "Denied");
      await publishDown({ type: "ack", of: "deny", ok: true });
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
      await setStatus("thinking", truncateForDisplay(trimmed, 80).text);
      await publishDown({
        type: "log",
        agent: "codex",
        text: `> ${truncateForDisplay(trimmed, 110).text}`,
        chatId: threadId,
      });
      await send("turn/start", {
        threadId,
        input: [{ type: "text", text: trimmed }],
      });
    },
    async selectChat(id) {
      const threads = await listThreads();
      const target = threads.find((t) => t.id === id);
      if (!target) return false;
      await resumeThread(id, threadTitle(target));
      await publishChatList(threads);
      return true;
    },
    async syncChats() {
      await runSyncChats({ follow: true });
    },
    async close() {
      closed = true;
      if (syncTimer) clearInterval(syncTimer);
      try {
        socket.close();
      } catch {
        /* ignore */
      }
      if (child && !child.killed) child.kill("SIGTERM");
    },
  };
}
