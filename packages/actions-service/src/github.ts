import type {
  BranchSummary,
  DeploymentState,
  DeploymentStatus,
  MergeResult,
  PullSummary,
  RepoId,
  ReviewSummary,
  WorkflowId,
} from "./types.js";

const OWNER = "UltraWording";
const API_VERSION = "2026-03-10";
const REVIEWERS = ["Ultrassure", "CAI-yw"] as const;
const EXPECTED_ACTOR = "saksham-uw";
const MARKER = "<!-- git-workflow:";

type Fetch = typeof fetch;

type RawUser = { login: string };
type RawBranch = { name: string; commit: { sha: string } };
type RawCommit = {
  sha: string;
  commit: {
    author?: { date?: string | null } | null;
    committer?: { date?: string | null } | null;
  };
};
type RawReview = {
  user: RawUser | null;
  state: string;
  submitted_at: string | null;
};
type RawWorkflowRun = {
  run_number: number;
  status: "queued" | "in_progress" | "completed" | string;
  conclusion: string | null;
  head_sha: string;
  run_started_at: string | null;
  created_at: string;
  updated_at: string;
};
export type RawPull = {
  number: number;
  title: string;
  html_url: string;
  body: string | null;
  head: { ref: string; sha: string };
  base: { ref: string };
  mergeable?: boolean | null;
  mergeable_state?: string;
  requested_reviewers?: RawUser[];
};

export class GitHubApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    readonly payload?: unknown,
  ) {
    super(message);
  }
}

async function mapLimit<T, U>(
  values: T[],
  limit: number,
  mapper: (value: T) => Promise<U>,
): Promise<U[]> {
  const output = new Array<U>(values.length);
  let cursor = 0;
  const workers = Array.from({ length: Math.min(limit, values.length) }, async () => {
    while (cursor < values.length) {
      const index = cursor++;
      output[index] = await mapper(values[index]);
    }
  });
  await Promise.all(workers);
  return output;
}

function effectiveReviews(reviews: RawReview[]): ReviewSummary[] {
  const latest = new Map<string, ReviewSummary>();
  for (const review of reviews) {
    const state = review.state.toUpperCase();
    if (
      !review.user?.login ||
      !review.submitted_at ||
      !["APPROVED", "CHANGES_REQUESTED", "DISMISSED"].includes(state)
    ) {
      continue;
    }
    const current = latest.get(review.user.login);
    if (!current || current.submittedAt <= review.submitted_at) {
      latest.set(review.user.login, {
        login: review.user.login,
        state: state as ReviewSummary["state"],
        submittedAt: review.submitted_at,
      });
    }
  }
  return [...latest.values()].sort((a, b) => a.login.localeCompare(b.login));
}

export class GitHubClient {
  private actorVerification?: Promise<void>;
  private readonly deploymentCache = new Map<RepoId, {
    expiresAt: number;
    value: DeploymentStatus;
  }>();

  constructor(
    private readonly token: string,
    private readonly fetchImpl: Fetch = fetch,
  ) {}

  private async request<T>(path: string, init: RequestInit = {}): Promise<T> {
    const response = await this.fetchImpl(`https://api.github.com${path}`, {
      ...init,
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${this.token}`,
        "Content-Type": "application/json",
        "X-GitHub-Api-Version": API_VERSION,
        "User-Agent": "hermes-actions-service",
        ...init.headers,
      },
    });
    const text = await response.text();
    let payload: unknown;
    try {
      payload = text ? JSON.parse(text) : undefined;
    } catch {
      payload = text;
    }
    if (!response.ok) {
      const message =
        payload && typeof payload === "object" && "message" in payload
          ? String((payload as { message: unknown }).message)
          : `GitHub returned ${response.status}`;
      throw new GitHubApiError(response.status, message, payload);
    }
    return payload as T;
  }

  private ensureActor(): Promise<void> {
    this.actorVerification ??= this.request<{ login: string }>("/user").then(
      (user) => {
        if (user.login.toLowerCase() !== EXPECTED_ACTOR) {
          throw new GitHubApiError(
            403,
            `GitHub token belongs to ${user.login}, expected ${EXPECTED_ACTOR}`,
          );
        }
      },
    );
    return this.actorVerification;
  }

  private api(repo: RepoId, suffix = ""): string {
    return `/repos/${OWNER}/${repo}${suffix}`;
  }

  async getDeployment(repo: RepoId): Promise<DeploymentStatus> {
    const cached = this.deploymentCache.get(repo);
    if (cached && cached.expiresAt > Date.now()) return cached.value;
    const workflow = repo === "ultracontract" ? "deploy.yml" : "push-ecr-prod.yml";
    const response = await this.request<{ workflow_runs: RawWorkflowRun[] }>(
      this.api(
        repo,
        `/actions/workflows/${workflow}/runs?branch=prod&event=push&per_page=20`,
      ),
    );
    const latest = response.workflow_runs[0];
    const latestSuccessful = response.workflow_runs.find(
      (run) => run.status === "completed" && run.conclusion === "success",
    );
    let state: DeploymentState = "unknown";
    if (latest?.status === "queued") state = "queued";
    else if (latest?.status === "in_progress") state = "running";
    else if (latest?.status === "completed" && latest.conclusion === "success") state = "ready";
    else if (latest?.status === "completed" && latest.conclusion === "cancelled") state = "cancelled";
    else if (latest?.status === "completed") state = "failed";
    const value: DeploymentStatus = {
      repo,
      state,
      runNumber: latest?.run_number ?? 0,
      candidateTag: latest?.head_sha.slice(0, 8) ?? "",
      latestTag: latestSuccessful?.head_sha.slice(0, 8) ?? "",
      latestRunNumber: latestSuccessful?.run_number ?? 0,
      startedAtEpoch: latest
        ? Math.floor(Date.parse(latest.run_started_at ?? latest.created_at) / 1000)
        : 0,
      completedAtEpoch: latestSuccessful
        ? Math.floor(Date.parse(latestSuccessful.updated_at) / 1000)
        : 0,
    };
    this.deploymentCache.set(repo, { expiresAt: Date.now() + 10_000, value });
    return value;
  }

  async listRecentBranches(
    repo: RepoId,
    hours: number,
    now = new Date(),
  ): Promise<BranchSummary[]> {
    const branches = await this.request<RawBranch[]>(
      this.api(repo, "/branches?per_page=100"),
    );
    const candidates = branches.filter(
      (branch) => branch.name !== "main" && branch.name !== "prod",
    );
    const withDates = await mapLimit(candidates, 8, async (branch) => {
      const commit = await this.request<RawCommit>(
        `/repos/${OWNER}/${repo}/commits/${encodeURIComponent(branch.commit.sha)}`,
      );
      const committedAt =
        commit.commit.committer?.date ?? commit.commit.author?.date ?? null;
      return committedAt
        ? { name: branch.name, sha: branch.commit.sha, committedAt }
        : null;
    });
    const cutoff = now.getTime() - hours * 60 * 60 * 1000;
    return withDates
      .filter((branch): branch is BranchSummary =>
        Boolean(branch && Date.parse(branch.committedAt) >= cutoff),
      )
      .sort((a, b) => Date.parse(b.committedAt) - Date.parse(a.committedAt));
  }

  async findOpenPull(repo: RepoId, head: string, base: string): Promise<RawPull | null> {
    const query = new URLSearchParams({
      state: "open",
      head: `${OWNER}:${head}`,
      base,
      per_page: "10",
    });
    const pulls = await this.request<RawPull[]>(
      this.api(repo, `/pulls?${query.toString()}`),
    );
    return pulls[0] ?? null;
  }

  async createPull(
    repo: RepoId,
    workflow: WorkflowId,
    head: string,
    base: string,
  ): Promise<{ pull: RawPull; created: boolean }> {
    await this.ensureActor();
    const existing = await this.findOpenPull(repo, head, base);
    if (existing) {
      if (!existing.body?.includes(`${MARKER}${workflow} -->`)) {
        throw new GitHubApiError(
          409,
          `An open ${head} to ${base} pull request already exists`,
        );
      }
      return { pull: existing, created: false };
    }
    const title = `Merge ${head} into ${base}`;
    const body = `${MARKER}${workflow} -->`;
    const pull = await this.request<RawPull>(this.api(repo, "/pulls"), {
      method: "POST",
      body: JSON.stringify({ title, body, head, base, draft: false }),
    });
    return { pull, created: true };
  }

  async requestReviewers(repo: RepoId, number: number): Promise<void> {
    await this.ensureActor();
    await this.request(this.api(repo, `/pulls/${number}/requested_reviewers`), {
      method: "POST",
      body: JSON.stringify({ reviewers: REVIEWERS }),
    });
  }

  private async summarize(repo: RepoId, raw: RawPull): Promise<PullSummary> {
    const [pull, reviews] = await Promise.all([
      this.request<RawPull>(this.api(repo, `/pulls/${raw.number}`)),
      this.request<RawReview[]>(
        this.api(repo, `/pulls/${raw.number}/reviews?per_page=100`),
      ),
    ]);
    const effective = effectiveReviews(reviews).filter((review) =>
      REVIEWERS.some(
        (reviewer) => reviewer.toLowerCase() === review.login.toLowerCase(),
      ),
    );
    const approvedBy = effective
      .filter((review) => review.state === "APPROVED")
      .map((review) => review.login);
    const ready = approvedBy.length >= 1 && pull.mergeable !== false;
    return {
      number: pull.number,
      title: pull.title,
      url: pull.html_url,
      head: pull.head.ref,
      headSha: pull.head.sha,
      base: pull.base.ref,
      mergeable: pull.mergeable ?? null,
      mergeableState: pull.mergeable_state ?? "unknown",
      requestedReviewers: (pull.requested_reviewers ?? [])
        .map((user) => user.login)
        .filter((login) =>
          REVIEWERS.some(
            (reviewer) => reviewer.toLowerCase() === login.toLowerCase(),
          ),
        ),
      reviews: effective,
      approvedBy,
      ready,
    };
  }

  async summarizePull(repo: RepoId, pull: RawPull): Promise<PullSummary> {
    return this.summarize(repo, pull);
  }

  async listHermesPulls(repo: RepoId): Promise<PullSummary[]> {
    const pulls = await this.request<RawPull[]>(
      this.api(repo, "/pulls?state=open&sort=created&direction=desc&per_page=50"),
    );
    const managed = pulls
      .filter((pull) => pull.body?.includes(MARKER))
      .slice(0, 8);
    return mapLimit(managed, 5, (pull) => this.summarize(repo, pull));
  }

  async getHermesPull(repo: RepoId, number: number): Promise<PullSummary> {
    const pull = await this.request<RawPull>(this.api(repo, `/pulls/${number}`));
    if (!pull.body?.includes(MARKER)) {
      throw new GitHubApiError(403, "Pull request is not a console workflow");
    }
    return this.summarize(repo, pull);
  }

  async mergePull(
    repo: RepoId,
    number: number,
    expectedSha: string,
  ): Promise<MergeResult> {
    await this.ensureActor();
    const pull = await this.getHermesPull(repo, number);
    if (pull.headSha !== expectedSha) {
      throw new GitHubApiError(409, "Pull request changed; refresh before merging");
    }
    if (!pull.ready) {
      throw new GitHubApiError(409, "Pull request is not approved and mergeable");
    }
    return this.request<MergeResult>(this.api(repo, `/pulls/${number}/merge`), {
      method: "PUT",
      body: JSON.stringify({
        sha: expectedSha,
        merge_method: "merge",
        commit_title: `Merge ${pull.head} into ${pull.base}`,
      }),
    });
  }
}

export const githubReviewers = [...REVIEWERS];
