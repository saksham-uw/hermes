import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import {
  truncateForDisplay,
  type AgentState,
  type DeviceDownMessage,
} from "@hermes/protocol";
import type { BridgeConfig } from "../config.js";
import type { DownPublisher } from "../iot/client.js";

export type CursorObserveAdapter = {
  close: () => Promise<void>;
};

type InboxEvent = {
  source?: string;
  event?: string;
  summary?: string;
  text?: string;
  command?: string;
  state?: AgentState;
};

/**
 * Loopback HTTP inbox for Cursor observe hooks.
 * POST /event  { event, summary?, text?, command?, state? }
 */
export async function startCursorInbox(
  config: BridgeConfig,
  publishDown: DownPublisher,
): Promise<CursorObserveAdapter> {
  const publish = async (msg: DeviceDownMessage) => {
    try {
      await publishDown(msg);
    } catch (err) {
      console.error("[cursor-inbox] publish failed", err);
    }
  };

  const server = createServer(async (req: IncomingMessage, res: ServerResponse) => {
    if (req.method === "GET" && req.url === "/health") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === "POST" && (req.url === "/event" || req.url === "/")) {
      const chunks: Buffer[] = [];
      for await (const chunk of req) chunks.push(chunk as Buffer);
      let body: InboxEvent = {};
      try {
        body = JSON.parse(Buffer.concat(chunks).toString("utf8")) as InboxEvent;
      } catch {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ ok: false, error: "invalid json" }));
        return;
      }

      const event = body.event || "update";
      const rawText =
        body.text || body.summary || body.command || event;
      const { text, truncated } = truncateForDisplay(String(rawText), 220);
      const state: AgentState =
        body.state ||
        (event.includes("start")
          ? "running"
          : event.includes("end") || event.includes("stop")
            ? "idle"
            : "running");

      await publish({
        type: "status",
        agent: "cursor",
        state,
        summary: text,
        updatedAt: new Date().toISOString(),
      });
      await publish({
        type: "log",
        agent: "cursor",
        text: `[${event}] ${text}`,
        truncated,
      });

      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    res.writeHead(404);
    res.end();
  });

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(config.inboxPort, config.inboxHost, () => resolve());
  });
  console.log(
    `[cursor-inbox] listening http://${config.inboxHost}:${config.inboxPort}`,
  );

  return {
    async close() {
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}
