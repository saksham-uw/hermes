# Enable sign-in for a HERMES deployment

This setup is for the **owner of the HERMES integrations**, once per provider. People who install HERMES click Connect, approve access in their browser, and choose their workspace or repositories.

The linked Vercel project hosts the token exchange. Use a signed-in Vercel CLI. If needed, set `HERMES_VERCEL_CLI` to a current CLI command such as `npx --yes vercel@latest`.

- **New GitHub App.** Run `python3 scripts/register-oauth.py github`. Approve the prefilled registration in GitHub. The helper exchanges the single-use registration code and adds the client ID, secret, and app slug to Vercel. Install the app on the organization's selected repositories. The HERMES connector provides the installation link.
- **New Slack app.** Run `python3 scripts/register-oauth.py slack` with an owner app configuration token from Slack. The helper creates the app from `slack.json` and transfers its client credentials to Vercel. For additional workspaces, enable distribution in the Slack app settings. A workspace may require admin approval.
- **Existing provider apps.** Apply the callback and permissions in the corresponding JSON manifest. Set the Vercel production variables `HERMES_SLACK_CLIENT_ID`, `HERMES_SLACK_CLIENT_SECRET`, `HERMES_GITHUB_CLIENT_ID`, `HERMES_GITHUB_CLIENT_SECRET`, and `HERMES_GITHUB_APP_SLUG` for the providers you enable. Redeploy the website after changing variables.

The callbacks are `https://hermes-mac-saksham.vercel.app/auth/slack/callback` and `https://hermes-mac-saksham.vercel.app/auth/github/callback`. Keep client secrets in Vercel. The Mac receives a short-lived ticket bound to its locally generated verifier, exchanges it over HTTPS, and stores the resulting user credentials in Keychain. Existing manual connections continue working. Reconnect once to switch to browser authorization.

Run `node --test tests/OAuthBroker.test.mjs` to exercise the authorization handshake with fixture provider responses.

## Workspace permissions in v0.2.0

For an existing GitHub App, add read access to Actions, Checks and Commit statuses, then approve the updated installation on GitHub. The existing Contents and Pull requests permissions cover branch discovery, PR creation and review requests. Workspace reads branch tip commit dates to determine activity in the previous 48 hours.

For Slack's member picker and opening one-to-one DMs, enable the user scopes `users:read` and `im:write` and reconnect Slack once. Existing user grants with `chat:write` can send a PR to an existing direct-message link or D-prefixed conversation ID. Set reviewers and the Slack recipient together under Settings → Workflow defaults.

Workspace reads registry pushes from successful GitHub Actions jobs on the prod branch. To see the images actually running on a Docker host, enter its configured Docker CLI context in Workflow defaults. HERMES reads running containers whose image repository equals the connected repo name or begins with its name followed by a hyphen.
