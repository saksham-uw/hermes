export type ActionsConfig = {
  port: number;
  deviceToken: string;
  githubToken: string;
  slackToken: string;
  slackDestination: string;
  idempotencyFile: string;
};

function required(name: string): string {
  const value = process.env[name]?.trim();
  if (!value) throw new Error(`${name} is required`);
  return value;
}

function userSlackToken(): string {
  const token = required("HERMES_SLACK_USER_TOKEN");
  if (token.startsWith("xoxb-")) {
    throw new Error(
      "HERMES_SLACK_USER_TOKEN must be a user token (xoxp-), not a bot token",
    );
  }
  return token;
}

export function loadConfig(): ActionsConfig {
  const rawPort = process.env.HERMES_ACTIONS_PORT ?? "8787";
  const port = Number.parseInt(rawPort, 10);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error("HERMES_ACTIONS_PORT must be a valid TCP port");
  }
  return {
    port,
    deviceToken: required("HERMES_ACTIONS_DEVICE_TOKEN"),
    githubToken: required("HERMES_GITHUB_TOKEN"),
    slackToken: userSlackToken(),
    slackDestination: required("HERMES_SLACK_DESTINATION"),
    idempotencyFile:
      process.env.HERMES_ACTIONS_IDEMPOTENCY_FILE ??
      ".hermes-actions-idempotency.json",
  };
}
