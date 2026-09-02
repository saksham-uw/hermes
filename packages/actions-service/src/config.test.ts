import assert from "node:assert/strict";
import test from "node:test";
import { loadConfig } from "./config.js";

const requiredEnv = {
  HERMES_ACTIONS_DEVICE_TOKEN: "device-token-test",
  HERMES_GITHUB_TOKEN: "github_pat_test",
  HERMES_SLACK_USER_TOKEN: "xoxp-user-token",
  HERMES_SLACK_DESTINATION: "C1234567890",
} as const;

const managedKeys = [
  ...Object.keys(requiredEnv),
  "HERMES_ACTIONS_PORT",
  "HERMES_ACTIONS_IDEMPOTENCY_FILE",
] as const;

function withEnv(env: Record<string, string>, run: () => void): void {
  const previous = new Map<string, string | undefined>();
  for (const key of managedKeys) {
    previous.set(key, process.env[key]);
    delete process.env[key];
  }
  Object.assign(process.env, env);
  try {
    run();
  } finally {
    for (const [key, value] of previous) {
      if (value === undefined) delete process.env[key];
      else process.env[key] = value;
    }
  }
}

test("accepts a Slack user token", () => {
  withEnv(requiredEnv, () => {
    const config = loadConfig();
    assert.equal(config.slackToken, "xoxp-user-token");
  });
});

test("rejects a Slack bot token", () => {
  withEnv({ ...requiredEnv, HERMES_SLACK_USER_TOKEN: "xoxb-bot-token" }, () => {
    assert.throws(() => loadConfig(), /user token \(xoxp-\), not a bot token/);
  });
});
