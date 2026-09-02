import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { handleActionsRequest } from "./http.js";
import type { GitWorkflowService } from "./service.js";
import { HomeDataService } from "./home.js";

const BODY_LIMIT = 16 * 1024;

function headerValue(value: string | string[] | undefined): string | undefined {
  if (Array.isArray(value)) return value[0];
  return value;
}

function collectHeaders(request: IncomingMessage): Record<string, string | undefined> {
  const headers: Record<string, string | undefined> = {};
  for (const [key, value] of Object.entries(request.headers)) {
    headers[key] = headerValue(value);
  }
  return headers;
}

async function readBody(request: IncomingMessage): Promise<{ body: string; tooLarge: boolean }> {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const chunk of request) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    size += buffer.length;
    if (size > BODY_LIMIT) return { body: "", tooLarge: true };
    chunks.push(buffer);
  }
  return { body: Buffer.concat(chunks).toString("utf8"), tooLarge: false };
}

export function createActionsServer(
  service: GitWorkflowService,
  deviceToken: string,
  home = new HomeDataService(),
) {
  return createServer(async (request, response) => {
    const method = request.method ?? "GET";
    const needsBody = method === "POST" || method === "PUT" || method === "PATCH";
    const { body, tooLarge } = needsBody
      ? await readBody(request)
      : { body: "", tooLarge: false };
    const result = tooLarge
      ? {
          status: 413,
          headers: {
            "content-type": "application/json; charset=utf-8",
            "cache-control": "no-store",
          },
          body: JSON.stringify({
            error: { code: "body_too_large", message: "Request is too large" },
          }),
        }
      : await handleActionsRequest(service, deviceToken, {
          method,
          url: request.url ?? "/",
          headers: collectHeaders(request),
          body,
        }, home);
    response.writeHead(result.status, {
      ...result.headers,
      "Content-Length": Buffer.byteLength(result.body),
    });
    response.end(result.body);
  });
}
