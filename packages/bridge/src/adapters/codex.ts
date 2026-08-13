import { spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";
import WebSocket from "ws";
import { truncateForDisplay, type ApprovalKind } from "@hermes/protocol";
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

export type CodexAdapter = {
  handleApprove: (id: string) => Promise<boolean>;
  handleDeny: (id: string) => Promise<boolean>;
  handlePrompt: (text: string) => Promise<void>;
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
  let ws: WebSocket | null = null;

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

  // app-server can take a few seconds (sandbox bootstrap). Retry WS connect.
  const maxAttempts = 30;
  let lastErr: unknown;
  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    try {
      ws = await new Promise<WebSocket>((resolve, reject) => {
        const socket = new WebSocket(config.codexWsUrl, { headers });
        const timer = setTimeout(
          () => {
            try {
              socket.terminate();
            } catch {
              /* ignore */
            }
            reject(new Error("codex ws connect timeout"));
          },
          3_000,
        );
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

    if (method === "turn/started") {
      await setStatus("thinking", "Turn started");
      return;
    }
    if (method === "turn/completed") {
      await setStatus("idle", "Turn completed");
      return;
    }
    if (
      method === "item/agentMessage/delta" ||
      method === "item/commandExecution/outputDelta"
    ) {
      const delta = typeof params.delta === "string" ? params.delta : "";
      if (delta) {
        const { text, truncated } = truncateForDisplay(delta, 200);
        await publishDown({ type: "log", agent: "codex", text, truncated });
        await setStatus("running", text);
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

  try {
    const started = (await send("thread/start", {
      cwd: config.codexCwd || undefined,
    })) as { thread?: { id?: string }; id?: string; threadId?: string };
    threadId = started?.thread?.id || started?.id || started?.threadId || null;
    console.log(`[codex] thread ${threadId}`);
    await setStatus(
      "idle",
      threadId ? `Thread ${threadId.slice(0, 8)}…` : "Ready",
    );
  } catch (err) {
    console.warn("[codex] thread/start failed, will retry on prompt:", err);
    await setStatus("idle", "Connected (no thread yet)");
  }

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
      }
      if (!threadId) throw new Error("No Codex thread");
      await setStatus("thinking", truncateForDisplay(trimmed, 80).text);
      await send("turn/start", {
        threadId,
        input: [{ type: "text", text: trimmed }],
      });
    },
    async close() {
      try {
        socket.close();
      } catch {
        /* ignore */
      }
      if (child && !child.killed) child.kill("SIGTERM");
    },
  };
}
