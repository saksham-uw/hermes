import assert from "node:assert/strict";
import test from "node:test";
import { GitHubClient } from "./github.js";

test("maps a running production workflow without losing the last successful tag", async () => {
  let requestedUrl = "";
  const fakeFetch = async (input: string | URL | Request) => {
    requestedUrl = String(input);
    return new Response(JSON.stringify({
      workflow_runs: [
        {
          run_number: 256,
          status: "in_progress",
          conclusion: null,
          head_sha: "1234567890abcdef",
          run_started_at: "2026-09-02T10:00:00Z",
          created_at: "2026-09-02T09:59:00Z",
          updated_at: "2026-09-02T10:01:00Z",
        },
        {
          run_number: 255,
          status: "completed",
          conclusion: "success",
          head_sha: "78f1f616abcdef00",
          run_started_at: "2026-09-02T08:00:00Z",
          created_at: "2026-09-02T08:00:00Z",
          updated_at: "2026-09-02T08:15:00Z",
        },
      ],
    }), { status: 200 });
  };
  const client = new GitHubClient("test-token", fakeFetch as typeof fetch);
  const deployment = await client.getDeployment("ultracontract");
  assert.match(requestedUrl, /actions\/workflows\/deploy\.yml\/runs/);
  assert.match(requestedUrl, /branch=prod/);
  assert.match(requestedUrl, /event=push/);
  assert.equal(deployment.state, "running");
  assert.equal(deployment.candidateTag, "12345678");
  assert.equal(deployment.latestTag, "78f1f616");
  assert.equal(deployment.runNumber, 256);
});

test("uses the ultrachatbot production workflow and maps a successful tag", async () => {
  let requestedUrl = "";
  const fakeFetch = async (input: string | URL | Request) => {
    requestedUrl = String(input);
    return new Response(JSON.stringify({
      workflow_runs: [{
        run_number: 36,
        status: "completed",
        conclusion: "success",
        head_sha: "aadbb9ae11223344",
        run_started_at: "2026-09-01T10:00:00Z",
        created_at: "2026-09-01T10:00:00Z",
        updated_at: "2026-09-01T10:01:00Z",
      }],
    }), { status: 200 });
  };
  const client = new GitHubClient("test-token", fakeFetch as typeof fetch);
  const deployment = await client.getDeployment("ultrachatbot");
  assert.match(requestedUrl, /actions\/workflows\/push-ecr-prod\.yml\/runs/);
  assert.equal(deployment.state, "ready");
  assert.equal(deployment.latestTag, "aadbb9ae");
});
