# Action service

This small Node service is the credential boundary between the ESP32 and GitHub
or Slack. It is intentionally separate from the dormant Codex/Cursor bridge.

GitHub pull requests and Slack messages must look like they came from saksham's
accounts. They must not mention Hermes, the device, or this service.

## Required configuration

Copy `.env.example` into your secret manager or deployment environment; the
service does not load dotenv files itself.

- `HERMES_ACTIONS_DEVICE_TOKEN`: a random, revocable token used only by the
  device.
- `HERMES_GITHUB_TOKEN`: a fine-grained user token owned by `saksham-uw`, limited
  to `UltraWording/ultracontract` and `UltraWording/ultrachatbot`, with Contents
  read and Pull requests read/write. The service calls `/user` before a write and
  rejects any other account. Created PRs are titled `Merge <head> into <base>`
  and carry only an invisible `<!-- git-workflow:... -->` marker.
- `HERMES_SLACK_USER_TOKEN`: a Slack **user** token (`xoxp-`) for saksham's
  account with `chat:write`. Bot tokens (`xoxb-`) are rejected so announcements
  cannot post as an app.
- `HERMES_SLACK_DESTINATION`: a channel ID that saksham's user can post to, or
  a Slack user ID for a DM from that account.
- `HERMES_ACTIONS_PORT`: optional; defaults to `8787`.
- `HERMES_ACTIONS_IDEMPOTENCY_FILE`: optional persistent state path.

Keep all four credentials in environment or secret-manager storage. They must
not be committed, logged, or copied into the ESP32 image.

## Deploy (AWS Lambda)

The ESP32 talks to this service over HTTPS. `npm run actions:deploy` from the
repo root packages the handler, creates or updates `hermes-actions` in
`us-east-1`, and writes `firmware/main/hermes_actions_config.h` (gitignored)
with the Function URL and device token. GitHub and Slack secrets stay in Lambda
environment variables, not in firmware.

```bash
npm run actions:test
npm run actions:deploy
```

```bash
npm install
npm run actions:test
npm run actions:build
HERMES_ACTIONS_DEVICE_TOKEN=... \
HERMES_GITHUB_TOKEN=... \
HERMES_SLACK_USER_TOKEN=... \
HERMES_SLACK_DESTINATION=... \
npm run actions:start
```

Put the service behind HTTPS with a valid public certificate. All endpoints,
including `/v1/health`, require `Authorization: Bearer <device token>`.
Mutations additionally require an `Idempotency-Key` header.

The API does not bypass GitHub branch protection. A merge is attempted only
when at least one requested reviewer has approved, GitHub does not report the
PR unmergeable, and the device-confirmed head SHA still matches. GitHub may
still reject the merge.
