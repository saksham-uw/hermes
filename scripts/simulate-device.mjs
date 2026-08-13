#!/usr/bin/env node
/**
 * Simulate the ESP32 over AWS IoT using the provisioned device certificate.
 * Usage: node scripts/simulate-device.mjs
 *        node scripts/simulate-device.mjs approve <id>
 *        node scripts/simulate-device.mjs prompt "do the thing"
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import mqtt from "mqtt";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(__dirname, "..");
const endpoint =
  process.env.HERMES_IOT_ENDPOINT ||
  "acp99wijypwjp-ats.iot.us-east-1.amazonaws.com";
const userId = process.env.HERMES_USER_ID || "saksham";
const thing = process.env.HERMES_THING_NAME || "hermes-device";
const certs = path.join(root, "firmware/certs");

const up = `hermes/${userId}/device/up`;
const down = `hermes/${userId}/device/down`;

const client = mqtt.connect({
  host: endpoint,
  port: 8883,
  protocol: "mqtts",
  clientId: thing,
  key: fs.readFileSync(path.join(certs, "device.private.key")),
  cert: fs.readFileSync(path.join(certs, "device.cert.pem")),
  ca: fs.readFileSync(path.join(certs, "AmazonRootCA1.pem")),
  rejectUnauthorized: true,
});

const cmd = process.argv[2] || "hello";

client.on("connect", () => {
  console.log("connected", endpoint);
  client.subscribe(down, { qos: 1 });
  if (cmd === "hello") {
    client.publish(
      up,
      JSON.stringify({ type: "hello", deviceId: thing, firmware: "sim" }),
      { qos: 1 },
    );
  } else if (cmd === "approve") {
    client.publish(
      up,
      JSON.stringify({ type: "approve", id: process.argv[3] }),
      { qos: 1 },
    );
  } else if (cmd === "deny") {
    client.publish(
      up,
      JSON.stringify({ type: "deny", id: process.argv[3] }),
      { qos: 1 },
    );
  } else if (cmd === "prompt") {
    client.publish(
      up,
      JSON.stringify({
        type: "prompt",
        agent: "codex",
        text: process.argv.slice(3).join(" ") || "status",
      }),
      { qos: 1 },
    );
  }
});

client.on("message", (topic, payload) => {
  console.log("<", topic, payload.toString());
});

client.on("error", (err) => {
  console.error(err);
  process.exit(1);
});
