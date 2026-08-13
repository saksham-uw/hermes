export type BridgeConfig = {
  awsRegion: string;
  iotEndpoint: string;
  userId: string;
  bridgeClientId: string;
  codexWsUrl: string;
  codexAuthToken: string;
  codexSpawn: boolean;
  codexCwd: string;
  inboxHost: string;
  inboxPort: number;
};

function env(name: string, fallback = ""): string {
  return process.env[name]?.trim() || fallback;
}

function envBool(name: string, fallback: boolean): boolean {
  const v = process.env[name]?.trim().toLowerCase();
  if (v === undefined || v === "") return fallback;
  return v === "1" || v === "true" || v === "yes";
}

export function loadConfig(): BridgeConfig {
  return {
    awsRegion: env("AWS_REGION", "us-east-1"),
    iotEndpoint: env(
      "HERMES_IOT_ENDPOINT",
      "acp99wijypwjp-ats.iot.us-east-1.amazonaws.com",
    ),
    userId: env("HERMES_USER_ID", "saksham"),
    bridgeClientId: env("HERMES_BRIDGE_CLIENT_ID", `hermes-bridge-${process.pid}`),
    codexWsUrl: env("HERMES_CODEX_WS_URL", "ws://127.0.0.1:4520"),
    codexAuthToken: env("HERMES_CODEX_AUTH_TOKEN", ""),
    codexSpawn: envBool("HERMES_CODEX_SPAWN", true),
    codexCwd: env("HERMES_CODEX_CWD", process.cwd()),
    inboxHost: env("HERMES_INBOX_HOST", "127.0.0.1"),
    inboxPort: Number(env("HERMES_INBOX_PORT", "8741")) || 8741,
  };
}
