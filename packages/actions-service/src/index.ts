import { loadConfig } from "./config.js";
import { GitHubClient } from "./github.js";
import { IdempotencyStore } from "./idempotency.js";
import { createActionsServer } from "./server.js";
import { GitWorkflowService } from "./service.js";
import { SlackClient } from "./slack.js";
import { HomeDataService } from "./home.js";

const config = loadConfig();
const service = new GitWorkflowService(
  new GitHubClient(config.githubToken),
  new SlackClient(config.slackToken, config.slackDestination),
  new IdempotencyStore(config.idempotencyFile),
);
const server = createActionsServer(service, config.deviceToken, new HomeDataService());

server.listen(config.port, "0.0.0.0", () => {
  console.log(`Hermes action service listening on port ${config.port}`);
});
