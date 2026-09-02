export const workflows = ["main-from-branch", "prod-from-main"] as const;
export type WorkflowId = (typeof workflows)[number];

export const gitRepos = ["ultracontract", "ultrachatbot"] as const;
export type RepoId = (typeof gitRepos)[number];

export function isRepoId(value: string): value is RepoId {
  return (gitRepos as readonly string[]).includes(value);
}

export type BranchSummary = {
  name: string;
  sha: string;
  committedAt: string;
};

export type ReviewSummary = {
  login: string;
  state: "APPROVED" | "CHANGES_REQUESTED" | "DISMISSED";
  submittedAt: string;
};

export type PullSummary = {
  number: number;
  title: string;
  url: string;
  head: string;
  headSha: string;
  base: string;
  mergeable: boolean | null;
  mergeableState: string;
  requestedReviewers: string[];
  reviews: ReviewSummary[];
  approvedBy: string[];
  ready: boolean;
};

export type PullCreation = {
  pull: PullSummary;
  created: boolean;
  warnings: string[];
};

export type MergeResult = {
  merged: boolean;
  sha?: string;
  message: string;
};

export type DeploymentState =
  | "unknown"
  | "queued"
  | "running"
  | "ready"
  | "failed"
  | "cancelled";

export type DeploymentStatus = {
  repo: RepoId;
  state: DeploymentState;
  runNumber: number;
  candidateTag: string;
  latestTag: string;
  latestRunNumber: number;
  startedAtEpoch: number;
  completedAtEpoch: number;
};

export type ServiceErrorBody = {
  error: {
    code: string;
    message: string;
  };
};
