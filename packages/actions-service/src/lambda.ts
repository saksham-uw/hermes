import { loadConfig } from "./config.js";
import { GitHubClient } from "./github.js";
import { handleActionsRequest } from "./http.js";
import { IdempotencyStore } from "./idempotency.js";
import { GitWorkflowService } from "./service.js";
import { SlackClient } from "./slack.js";
import { HomeDataService } from "./home.js";

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
  | { service: GitWorkflowService; deviceToken: string; home: HomeDataService }
  | undefined;

function runtime(): { service: GitWorkflowService; deviceToken: string; home: HomeDataService } {
  if (!cached) {
    const config = loadConfig();
    cached = {
      deviceToken: config.deviceToken,
      home: new HomeDataService(),
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
  const { service, deviceToken, home } = runtime();
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
  }, home);
  return {
    statusCode: result.status,
    headers: result.headers,
    body: result.body,
  };
}
