import { timingSafeEqual } from "node:crypto";
import { GitHubApiError } from "./github.js";
import { IdempotencyConflictError } from "./idempotency.js";
import { InputError, type GitWorkflowService } from "./service.js";
import type { ServiceErrorBody, WorkflowId } from "./types.js";
import { HomeDataError, type HomeDataService } from "./home.js";

const BODY_LIMIT = 16 * 1024;

export type ActionsHttpRequest = {
  method: string;
  url: string;
  headers: Record<string, string | undefined>;
  body: string;
};

export type ActionsHttpResponse = {
  status: number;
  headers: Record<string, string>;
  body: string;
};

class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
  ) {
    super(message);
  }
}

function header(request: ActionsHttpRequest, name: string): string | undefined {
  const wanted = name.toLowerCase();
  for (const [key, value] of Object.entries(request.headers)) {
    if (key.toLowerCase() === wanted) return value;
  }
  return undefined;
}

function jsonResponse(status: number, body: unknown): ActionsHttpResponse {
  const payload = JSON.stringify(body);
  return {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
    body: payload,
  };
}

function tokenMatches(suppliedHeader: string | undefined, expected: string): boolean {
  if (!suppliedHeader?.startsWith("Bearer ")) return false;
  const supplied = Buffer.from(suppliedHeader.slice(7), "utf8");
  const wanted = Buffer.from(expected, "utf8");
  return supplied.length === wanted.length && timingSafeEqual(supplied, wanted);
}

function parseJsonObject(body: string): Record<string, unknown> {
  if (body.length > BODY_LIMIT) {
    throw new HttpError(413, "body_too_large", "Request is too large");
  }
  if (!body) return {};
  try {
    const value = JSON.parse(body) as unknown;
    if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error();
    return value as Record<string, unknown>;
  } catch {
    throw new HttpError(400, "invalid_json", "Request body must be a JSON object");
  }
}

function idempotencyKey(request: ActionsHttpRequest): string {
  const value = header(request, "idempotency-key");
  if (typeof value !== "string" || !/^[A-Za-z0-9._:-]{8,128}$/.test(value)) {
    throw new HttpError(
      400,
      "invalid_idempotency_key",
      "A valid Idempotency-Key is required",
    );
  }
  return value;
}

function mappedError(error: unknown): HttpError {
  if (error instanceof HttpError) return error;
  if (error instanceof InputError) return new HttpError(400, "invalid_input", error.message);
  if (error instanceof IdempotencyConflictError) {
    return new HttpError(409, "idempotency_conflict", error.message);
  }
  if (error instanceof GitHubApiError) {
    const status = error.status >= 400 && error.status < 500 ? error.status : 502;
    const code = error.status === 401 || error.status === 403 ? "github_auth" : "github_error";
    return new HttpError(status, code, error.message.slice(0, 180));
  }
  if (error instanceof HomeDataError) {
    return new HttpError(502, "home_data_error", error.message.slice(0, 180));
  }
  return new HttpError(502, "upstream_error", "Action service request failed");
}

export async function handleActionsRequest(
  service: GitWorkflowService,
  deviceToken: string,
  request: ActionsHttpRequest,
  home?: HomeDataService,
): Promise<ActionsHttpResponse> {
  try {
    if (!tokenMatches(header(request, "authorization"), deviceToken)) {
      throw new HttpError(401, "device_auth", "Device authentication failed");
    }
    const method = (request.method || "GET").toUpperCase();
    const url = new URL(request.url || "/", "http://hermes.local");

    if (method === "GET" && url.pathname === "/v1/health") {
      return jsonResponse(200, { ok: true, service: "hermes-actions", version: 1 });
    }
    if (method === "GET" && url.pathname === "/v1/home/weather" && home) {
      return jsonResponse(200, await home.weather());
    }
    if (method === "GET" && url.pathname === "/v1/home/markets/crypto" && home) {
      return jsonResponse(200, await home.crypto());
    }
    if (method === "GET" && url.pathname === "/v1/home/markets/stocks" && home) {
      return jsonResponse(200, await home.stocks());
    }
    if (method === "GET" && url.pathname === "/v1/git/branches") {
      const repo = service.parseRepo(url.searchParams.get("repo") ?? undefined);
      const rawHours = url.searchParams.get("recent_hours") ?? "48";
      return jsonResponse(200, {
        branches: await service.listBranches(repo, Number(rawHours)),
      });
    }
    if (method === "GET" && url.pathname === "/v1/git/pulls") {
      const repo = service.parseRepo(url.searchParams.get("repo") ?? undefined);
      return jsonResponse(200, { pulls: await service.listPulls(repo) });
    }

    const pullMatch = url.pathname.match(/^\/v1\/git\/pulls\/(\d+)$/);
    if (method === "GET" && pullMatch) {
      const repo = service.parseRepo(url.searchParams.get("repo") ?? undefined);
      return jsonResponse(200, {
        pull: await service.getPull(repo, Number(pullMatch[1])),
      });
    }
    const mergeMatch = url.pathname.match(/^\/v1\/git\/pulls\/(\d+)\/merge$/);
    if (method === "POST" && mergeMatch) {
      const body = parseJsonObject(request.body);
      if (typeof body.headSha !== "string") {
        throw new HttpError(400, "invalid_input", "headSha is required");
      }
      const repo = service.parseRepo(
        typeof body.repo === "string" ? body.repo : url.searchParams.get("repo") ?? undefined,
      );
      const result = await service.mergePull(
        repo,
        Number(mergeMatch[1]),
        body.headSha,
        idempotencyKey(request),
      );
      return jsonResponse(200, result);
    }

    const workflowMatch = url.pathname.match(
      /^\/v1\/git\/workflows\/(main-from-branch|prod-from-main)\/pulls$/,
    );
    if (method === "POST" && workflowMatch) {
      const workflow = workflowMatch[1] as WorkflowId;
      const body = parseJsonObject(request.body);
      const branch = typeof body.branch === "string" ? body.branch : undefined;
      const repo = service.parseRepo(
        typeof body.repo === "string" ? body.repo : url.searchParams.get("repo") ?? undefined,
      );
      const result = await service.createPull(
        repo,
        workflow,
        branch,
        idempotencyKey(request),
      );
      return jsonResponse(result.created ? 201 : 200, result);
    }

    throw new HttpError(404, "not_found", "Route not found");
  } catch (error) {
    const mapped = mappedError(error);
    const body: ServiceErrorBody = {
      error: { code: mapped.code, message: mapped.message },
    };
    return jsonResponse(mapped.status, body);
  }
}
