import type { GitHubClient, RawPull } from "./github.js";
import { IdempotencyStore } from "./idempotency.js";
import type { SlackClient } from "./slack.js";
import type {
  BranchSummary,
  MergeResult,
  PullCreation,
  PullSummary,
  RepoId,
  WorkflowId,
} from "./types.js";
import { isRepoId } from "./types.js";

type GitHubOperations = Pick<
  GitHubClient,
  | "listRecentBranches"
  | "createPull"
  | "requestReviewers"
  | "summarizePull"
  | "listHermesPulls"
  | "getHermesPull"
  | "mergePull"
>;

type SlackOperations = Pick<SlackClient, "announcePull">;

type CreateProgress = {
  pullNumber?: number;
  created?: boolean;
  reviewersRequested?: boolean;
  slackSent?: boolean;
  result?: PullCreation;
};

export class InputError extends Error {}

function errorMessage(prefix: string, error: unknown): string {
  const detail = error instanceof Error ? error.message : "unknown error";
  return `${prefix}: ${detail}`.slice(0, 180);
}

export class GitWorkflowService {
  constructor(
    private readonly github: GitHubOperations,
    private readonly slack: SlackOperations,
    private readonly idempotency: IdempotencyStore,
  ) {}

  parseRepo(value: string | undefined): RepoId {
    const repo = value?.trim() ?? "";
    if (!isRepoId(repo)) {
      throw new InputError("repo must be ultracontract or ultrachatbot");
    }
    return repo;
  }

  listBranches(repo: RepoId, hours: number): Promise<BranchSummary[]> {
    if (!Number.isInteger(hours) || hours < 1 || hours > 168) {
      throw new InputError("recent_hours must be between 1 and 168");
    }
    return this.github.listRecentBranches(repo, hours).then((branches) => branches.slice(0, 8));
  }

  listPulls(repo: RepoId): Promise<PullSummary[]> {
    return this.github.listHermesPulls(repo).then((pulls) => pulls.slice(0, 8));
  }

  getPull(repo: RepoId, number: number): Promise<PullSummary> {
    if (!Number.isInteger(number) || number < 1) {
      throw new InputError("Pull request number is invalid");
    }
    return this.github.getHermesPull(repo, number);
  }

  async createPull(
    repo: RepoId,
    workflow: WorkflowId,
    branch: string | undefined,
    idempotencyKey: string,
  ): Promise<PullCreation> {
    const normalizedBranch = branch?.trim();
    if (workflow === "main-from-branch" && !normalizedBranch) {
      throw new InputError("branch is required for main-from-branch");
    }
    if (normalizedBranch && (!/^[A-Za-z0-9._/-]{1,120}$/.test(normalizedBranch) || normalizedBranch.includes(".."))) {
      throw new InputError("branch contains unsupported characters");
    }
    const head = workflow === "prod-from-main" ? "main" : normalizedBranch!;
    const base = workflow === "prod-from-main" ? "prod" : "main";
    const fingerprint = `create:${repo}:${workflow}:${head}:${base}`;
    const progress =
      (await this.idempotency.get<CreateProgress>(idempotencyKey, fingerprint)) ?? {};
    if (progress.result) return progress.result;

    let pull: PullSummary;
    if (!progress.pullNumber) {
      const created = await this.github.createPull(repo, workflow, head, base);
      progress.pullNumber = created.pull.number;
      progress.created = created.created;
      await this.idempotency.put(idempotencyKey, fingerprint, progress);
      pull = await this.github.summarizePull(repo, created.pull as RawPull);
    } else {
      pull = await this.github.getHermesPull(repo, progress.pullNumber);
    }

    const warnings: string[] = [];
    if (!progress.reviewersRequested) {
      try {
        await this.github.requestReviewers(repo, pull.number);
        progress.reviewersRequested = true;
        await this.idempotency.put(idempotencyKey, fingerprint, progress);
        pull = await this.github.getHermesPull(repo, pull.number);
      } catch (error) {
        warnings.push(errorMessage("reviewer request failed", error));
      }
    }

    if (progress.created && !progress.slackSent) {
      try {
        await this.slack.announcePull(pull);
        progress.slackSent = true;
        await this.idempotency.put(idempotencyKey, fingerprint, progress);
      } catch (error) {
        warnings.push(errorMessage("Slack announcement failed", error));
      }
    }

    const result = { pull, created: progress.created === true, warnings };
    if (warnings.length === 0) {
      progress.result = result;
      await this.idempotency.put(idempotencyKey, fingerprint, progress);
    }
    return result;
  }

  async mergePull(
    repo: RepoId,
    number: number,
    expectedSha: string,
    idempotencyKey: string,
  ): Promise<MergeResult> {
    if (!Number.isInteger(number) || number < 1) {
      throw new InputError("Pull request number is invalid");
    }
    if (!/^[0-9a-f]{7,64}$/i.test(expectedSha)) {
      throw new InputError("Current head SHA is required");
    }
    const fingerprint = `merge:${repo}:${number}:${expectedSha}`;
    const previous = await this.idempotency.get<MergeResult>(
      idempotencyKey,
      fingerprint,
    );
    if (previous) return previous;
    const result = await this.github.mergePull(repo, number, expectedSha);
    await this.idempotency.put(idempotencyKey, fingerprint, result);
    return result;
  }
}
