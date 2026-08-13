import { parseDeviceUp } from "@hermes/protocol";
import { loadConfig } from "./config.js";
import { connectIot } from "./iot/client.js";
import { startCodexAdapter } from "./adapters/codex.js";
import { startCursorInbox } from "./inbox/server.js";

async function main() {
  const config = loadConfig();
  console.log(
    `[hermes] starting bridge user=${config.userId} endpoint=${config.iotEndpoint}`,
  );

  const iot = await connectIot(config);
  const publishDown = iot.publishDown;

  let codex: Awaited<ReturnType<typeof startCodexAdapter>> | null = null;
  try {
    codex = await startCodexAdapter(config, publishDown);
  } catch (err) {
    console.error("[hermes] Codex adapter failed to start:", err);
    await publishDown({
      type: "status",
      agent: "codex",
      state: "error",
      summary: "Codex unavailable — check app-server",
      updatedAt: new Date().toISOString(),
    });
  }

  const cursor = await startCursorInbox(config, publishDown);

  await publishDown({
    type: "status",
    agent: "codex",
    state: codex ? "idle" : "offline",
    summary: codex ? "Hermes bridge online" : "Bridge online (Codex offline)",
    updatedAt: new Date().toISOString(),
  });

  iot.onUp((raw) => {
    const msg = parseDeviceUp(raw);
    if (!msg) {
      console.warn("[hermes] bad device up payload", raw.slice(0, 200));
      return;
    }

    void (async () => {
      switch (msg.type) {
        case "hello":
          console.log(`[hermes] device hello ${msg.deviceId}`);
          await publishDown({
            type: "ack",
            of: "hello",
            ok: true,
          });
          await publishDown({
            type: "status",
            agent: "codex",
            state: codex ? "idle" : "offline",
            summary: `Device ${msg.deviceId} linked`,
            updatedAt: new Date().toISOString(),
          });
          break;
        case "ping":
          await publishDown({ type: "ack", of: "ping", ok: true });
          break;
        case "approve":
          if (!codex) {
            await publishDown({
              type: "ack",
              of: "approve",
              ok: false,
              error: "codex offline",
            });
            break;
          }
          {
            const ok = await codex.handleApprove(msg.id);
            if (!ok) {
              await publishDown({
                type: "ack",
                of: "approve",
                ok: false,
                error: "unknown approval id",
              });
            }
          }
          break;
        case "deny":
          if (!codex) {
            await publishDown({
              type: "ack",
              of: "deny",
              ok: false,
              error: "codex offline",
            });
            break;
          }
          {
            const ok = await codex.handleDeny(msg.id);
            if (!ok) {
              await publishDown({
                type: "ack",
                of: "deny",
                ok: false,
                error: "unknown approval id",
              });
            }
          }
          break;
        case "prompt":
          if (!codex) {
            await publishDown({
              type: "ack",
              of: "prompt",
              ok: false,
              error: "codex offline",
            });
            break;
          }
          try {
            await codex.handlePrompt(msg.text);
            await publishDown({ type: "ack", of: "prompt", ok: true });
          } catch (err) {
            await publishDown({
              type: "ack",
              of: "prompt",
              ok: false,
              error: err instanceof Error ? err.message : String(err),
            });
          }
          break;
        default:
          break;
      }
    })();
  });

  const shutdown = async () => {
    console.log("[hermes] shutting down");
    await codex?.close();
    await cursor.close();
    await iot.close();
    process.exit(0);
  };
  process.on("SIGINT", () => void shutdown());
  process.on("SIGTERM", () => void shutdown());
}

main().catch((err) => {
  console.error("[hermes] fatal", err);
  process.exit(1);
});
