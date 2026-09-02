import assert from "node:assert/strict";
import { once } from "node:events";
import test from "node:test";
import { createActionsServer } from "./server.js";
import { InputError, type GitWorkflowService } from "./service.js";

const deviceToken = "device-test-token";

function fakeService(): GitWorkflowService {
  return {
    async listBranches() {
      return [{ name: "feature", sha: "abcdef1", committedAt: "2026-09-01T12:00:00Z" }];
    },
    async listPulls() {
      return [];
    },
    async listDeployments() {
      return [{
        repo: "ultracontract",
        state: "running",
        runNumber: 256,
        candidateTag: "12345678",
        latestTag: "78f1f616",
        latestRunNumber: 255,
        startedAtEpoch: 1788343200,
        completedAtEpoch: 1788339600,
      }];
    },
    parseRepo(value?: string) {
      if (value === "ultracontract" || value === "ultrachatbot") return value;
      throw new InputError("repo must be ultracontract or ultrachatbot");
    },
    async getPull() {
      throw new Error("not used");
    },
    async createPull() {
      throw new Error("not used");
    },
    async mergePull() {
      throw new Error("not used");
    },
  } as unknown as GitWorkflowService;
}

async function withServer(
  run: (baseUrl: string) => Promise<void>,
): Promise<void> {
  const server = createActionsServer(fakeService(), deviceToken);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert(address && typeof address === "object");
  try {
    await run(`http://127.0.0.1:${address.port}`);
  } finally {
    server.close();
    await once(server, "close");
  }
}

test("every route requires the device bearer token", async () => {
  await withServer(async (baseUrl) => {
    const response = await fetch(`${baseUrl}/v1/health`);
    assert.equal(response.status, 401);
    assert.deepEqual(await response.json(), {
      error: { code: "device_auth", message: "Device authentication failed" },
    });
  });
});

test("health and branch responses follow the bounded device contract", async () => {
  await withServer(async (baseUrl) => {
    const headers = { Authorization: `Bearer ${deviceToken}` };
    const health = await fetch(`${baseUrl}/v1/health`, { headers });
    assert.equal(health.status, 200);
    assert.deepEqual(await health.json(), {
      ok: true,
      service: "hermes-actions",
      version: 1,
    });

    const branches = await fetch(
      `${baseUrl}/v1/git/branches?repo=ultracontract&recent_hours=48`,
      { headers },
    );
    assert.equal(branches.status, 200);
    assert.deepEqual(await branches.json(), {
      branches: [
        {
          name: "feature",
          sha: "abcdef1",
          committedAt: "2026-09-01T12:00:00Z",
        },
      ],
    });
  });
});

test("unknown routes return a stable machine error", async () => {
  await withServer(async (baseUrl) => {
    const response = await fetch(`${baseUrl}/not-a-route`, {
      headers: { Authorization: `Bearer ${deviceToken}` },
    });
    assert.equal(response.status, 404);
    assert.deepEqual(await response.json(), {
      error: { code: "not_found", message: "Route not found" },
    });
  });
});

test("git routes require a known repo", async () => {
  await withServer(async (baseUrl) => {
    const headers = { Authorization: `Bearer ${deviceToken}` };
    const response = await fetch(`${baseUrl}/v1/git/branches?recent_hours=48`, {
      headers,
    });
    assert.equal(response.status, 400);
    assert.deepEqual(await response.json(), {
      error: {
        code: "invalid_input",
        message: "repo must be ultracontract or ultrachatbot",
      },
    });
  });
});

test("deployment summary uses a bounded normalized contract", async () => {
  await withServer(async (baseUrl) => {
    const response = await fetch(`${baseUrl}/v1/git/deployments`, {
      headers: { Authorization: `Bearer ${deviceToken}` },
    });
    assert.equal(response.status, 200);
    const body = await response.json() as { deployments: Array<Record<string, unknown>> };
    assert.equal(body.deployments[0].state, "running");
    assert.equal(body.deployments[0].latestTag, "78f1f616");
  });
});
