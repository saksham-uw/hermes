import assert from "node:assert/strict";
import test from "node:test";
import type { RawPull } from "./github.js";
import { IdempotencyConflictError, IdempotencyStore } from "./idempotency.js";
import { GitWorkflowService, InputError } from "./service.js";
import type { MergeResult, PullSummary, WorkflowId } from "./types.js";

function pullSummary(overrides: Partial<PullSummary> = {}): PullSummary {
  return {
    number: 42,
    title: "Merge feature into main",
    url: "https://github.com/UltraWording/ultracontract/pull/42",
    head: "feature",
    headSha: "abcdef1234567890",
    base: "main",
    mergeable: true,
    mergeableState: "clean",
    requestedReviewers: ["Ultrassure", "CAI-yw"],
    reviews: [],
    approvedBy: [],
    ready: false,
    ...overrides,
  };
}

class FakeGitHub {
  createCalls = 0;
  reviewerCalls = 0;
  mergeCalls = 0;
  lastRepo = "";
  pull = pullSummary();

  async listRecentBranches() {
    return [{ name: "feature", sha: "abcdef1", committedAt: new Date().toISOString() }];
  }
  async createPull(repo: string, _workflow: WorkflowId, head: string, base: string) {
    this.createCalls++;
    this.lastRepo = repo;
    this.pull = pullSummary({ head, base });
    return { pull: { number: this.pull.number } as RawPull, created: true };
  }
  async requestReviewers() {
    this.reviewerCalls++;
  }
  async summarizePull() {
    return this.pull;
  }
  async listHermesPulls() {
    return [this.pull];
  }
  async getHermesPull() {
    return this.pull;
  }
  async mergePull(): Promise<MergeResult> {
    this.mergeCalls++;
    return { merged: true, sha: "deadbeef", message: "Pull Request successfully merged" };
  }
}

class FakeSlack {
  calls = 0;
  failures = 0;
  async announcePull() {
    this.calls++;
    if (this.failures-- > 0) throw new Error("temporary_unavailable");
  }
}

test("create is idempotent across device retries", async () => {
  const github = new FakeGitHub();
  const slack = new FakeSlack();
  const service = new GitWorkflowService(github, slack, new IdempotencyStore(null));

  const first = await service.createPull("ultracontract", "prod-from-main", undefined, "create-prod-0001");
  const second = await service.createPull("ultracontract", "prod-from-main", undefined, "create-prod-0001");

  assert.equal(first.created, true);
  assert.deepEqual(second, first);
  assert.equal(github.createCalls, 1);
  assert.equal(github.reviewerCalls, 1);
  assert.equal(slack.calls, 1);
});

test("partial Slack failure retries only the missing announcement", async () => {
  const github = new FakeGitHub();
  const slack = new FakeSlack();
  slack.failures = 1;
  const service = new GitWorkflowService(github, slack, new IdempotencyStore(null));

  const first = await service.createPull("ultracontract", "main-from-branch", "feature", "create-main-0001");
  const second = await service.createPull("ultracontract", "main-from-branch", "feature", "create-main-0001");

  assert.equal(first.warnings.length, 1);
  assert.equal(second.warnings.length, 0);
  assert.equal(github.createCalls, 1);
  assert.equal(github.reviewerCalls, 1);
  assert.equal(slack.calls, 2);
});

test("idempotency key cannot be reused for another operation", async () => {
  const service = new GitWorkflowService(
    new FakeGitHub(),
    new FakeSlack(),
    new IdempotencyStore(null),
  );
  await service.createPull("ultracontract", "prod-from-main", undefined, "shared-key-0001");
  await assert.rejects(
    service.createPull("ultracontract", "main-from-branch", "feature", "shared-key-0001"),
    IdempotencyConflictError,
  );
});

test("branch and SHA inputs are bounded", async () => {
  const service = new GitWorkflowService(
    new FakeGitHub(),
    new FakeSlack(),
    new IdempotencyStore(null),
  );
  await assert.rejects(
    service.createPull("ultracontract", "main-from-branch", "../secret", "create-main-0002"),
    InputError,
  );
  await assert.rejects(
    service.mergePull("ultracontract", 42, "not-a-sha", "merge-pr-0042"),
    InputError,
  );
});

test("merge retries return the recorded result", async () => {
  const github = new FakeGitHub();
  const service = new GitWorkflowService(
    github,
    new FakeSlack(),
    new IdempotencyStore(null),
  );
  const first = await service.mergePull("ultracontract", 42, "abcdef1234567890", "merge-pr-0042");
  const second = await service.mergePull("ultracontract", 42, "abcdef1234567890", "merge-pr-0042");
  assert.deepEqual(second, first);
  assert.equal(github.mergeCalls, 1);
});

test("repo must be ultracontract or ultrachatbot", () => {
  const service = new GitWorkflowService(
    new FakeGitHub(),
    new FakeSlack(),
    new IdempotencyStore(null),
  );
  assert.equal(service.parseRepo("ultrachatbot"), "ultrachatbot");
  assert.throws(() => service.parseRepo("hermes"), InputError);
});

test("createPull forwards the selected repository", async () => {
  const github = new FakeGitHub();
  const service = new GitWorkflowService(github, new FakeSlack(), new IdempotencyStore(null));
  await service.createPull("ultrachatbot", "prod-from-main", undefined, "create-prod-bot");
  assert.equal(github.lastRepo, "ultrachatbot");
});
