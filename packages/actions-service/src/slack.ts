import type { PullSummary } from "./types.js";

type Fetch = typeof fetch;

type SlackResponse = {
  ok: boolean;
  error?: string;
};

export class SlackApiError extends Error {}

export class SlackClient {
  constructor(
    private readonly token: string,
    private readonly destination: string,
    private readonly fetchImpl: Fetch = fetch,
  ) {}

  async announcePull(pull: PullSummary): Promise<void> {
    // User token required: chat.postMessage then posts as that Slack user,
    // not as a bot or app.
    const response = await this.fetchImpl("https://slack.com/api/chat.postMessage", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${this.token}`,
        "Content-Type": "application/json; charset=utf-8",
      },
      body: JSON.stringify({
        channel: this.destination,
        text: pull.url,
        unfurl_links: true,
        unfurl_media: true,
      }),
    });
    let payload: SlackResponse;
    try {
      payload = (await response.json()) as SlackResponse;
    } catch {
      throw new SlackApiError(`Slack returned invalid JSON (${response.status})`);
    }
    if (!response.ok || !payload.ok) {
      throw new SlackApiError(payload.error ?? `Slack returned ${response.status}`);
    }
  }
}
