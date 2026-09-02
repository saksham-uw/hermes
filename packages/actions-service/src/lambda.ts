import { loadConfig } from "./config.js";
import { GitHubClient } from "./github.js";
import { handleActionsRequest } from "./http.js";
import { IdempotencyStore } from "./idempotency.js";
import { GitWorkflowService } from "./service.js";
import { SlackClient } from "./slack.js";

type FunctionUrlEvent = {
  rawPath?: string;
  rawQueryString?: string;
  headers?: Record<string, string | undefined>;
  requestContext?: { http?: { method?: string } };
  body?: string | null;
  isBase64Encoded?: boolean;
};

type FunctionUrlResult = {
  statusCode: number;
  headers: Record<string, string>;
  body: string;
};

let cached:
  | { service: GitWorkflowService; deviceToken: string }
  | undefined;

function runtime(): { service: GitWorkflowService; deviceToken: string } {
  if (!cached) {
    const config = loadConfig();
    cached = {
      deviceToken: config.deviceToken,
      service: new GitWorkflowService(
        new GitHubClient(config.githubToken),
        new SlackClient(config.slackToken, config.slackDestination),
        new IdempotencyStore(config.idempotencyFile),
      ),
    };
  }
  return cached;
}

export async function handler(event: FunctionUrlEvent): Promise<FunctionUrlResult> {
  const { service, deviceToken } = runtime();
  const path = event.rawPath || "/";
  const query = event.rawQueryString ? `?${event.rawQueryString}` : "";
  const body = event.isBase64Encoded
    ? Buffer.from(event.body ?? "", "base64").toString("utf8")
    : (event.body ?? "");
  const result = await handleActionsRequest(service, deviceToken, {
    method: event.requestContext?.http?.method ?? "GET",
    url: `${path}${query}`,
    headers: event.headers ?? {},
    body,
  });
  return {
    statusCode: result.status,
    headers: result.headers,
    body: result.body,
  };
}
