# Hermes device product plan

## Product goal

The device has two root experiences:

- a glanceable home carousel for time, focus, weather, and market information;
- a deliberate operations menu for Git workflows, status, settings, Wi-Fi, and
  shutdown.

The Git application operates on the private `UltraWording/ultracontract` and
`UltraWording/ultrachatbot` repositories. It creates two known PR workflows,
reports their review state, announces new PRs in Slack, and allows an approved
PR to be merged from the device.

This replaces none of the standalone shell. The former Codex and Cursor
monitoring stack remains dormant.

## Interaction contract — do not regress

Hardware logs from 2026-09-01 showed a real regression: tap coordinates were used
to pick a menu row independently of the BOOT highlight. That is wrong for this
panel. AXS15231B samples are not a reliable hit-test. Do not restore
coordinate-based row selection, clustering, majority-sample geometry, or
avatar-area exclusion as a way to choose which item activates.

The locked contract for every list (main menu, git, repo, workflows, settings,
Wi-Fi, and later branch/merge lists):

1. `BOOT` moves the highlight down one row and wraps. It never activates.
2. A screen tap activates **only** the currently highlighted row. Touch `x,y`
   may be logged; they must never select a different entry.
3. Ignore one-frame ghost touches. Do not invent extra filtering that then
   maps a coordinate onto a row.
4. Short `PWR` goes back one level inside nested menu pages. At the main menu it
   opens the Clock home page; on any home page it returns to the main menu.
   Held `PWR` powers off from every screen.
5. Opening Wi-Fi must switch directly to the complete Wi-Fi page. The avatar may
   be used on the menu and during a deliberate full-page loading state, but it
   must not remain over the right side of the Wi-Fi rows while the page is
   appearing.

The home carousel is the only place where a touch coordinate has meaning, and
only the relative horizontal displacement between press and release is used to
recognize a left/right swipe. Absolute `x,y` coordinates must never select a
menu or list row.

## Home carousel

### Navigation state machine

Boot still ends at the main menu. The home carousel does not replace the menu.

```text
main menu --short PWR--> Clock home page
home page --short PWR--> main menu
nested menu page --short PWR--> parent menu page
any screen --held PWR--> shutdown
```

- Entering home from the main menu always starts on Clock.
- Returning to the menu preserves the previously highlighted menu row.
- Home pages appear in this order and wrap in both directions:
  `Clock -> Pomodoro -> Weather -> Crypto -> Stocks -> Clock`.
- Swipe left advances one page; swipe right goes back one page.
- A valid swipe is horizontal movement of at least 48 px whose horizontal
  distance is at least twice its vertical distance. It is resolved once, on
  release. Vertical movement, a short tap, and an ambiguous gesture do nothing.
- Swipe recognition exists only while a home page is active. Menu/list screens
  continue to use BOOT-highlight plus TAP-select and never use swipe navigation.
- `BOOT` has an action only on Pomodoro. It is a no-op on Clock, Weather,
  Crypto, and Stocks.
- The Pomodoro timer continues while another home page or the main menu is open.

Implementation must model root mode (`menu` or `home`) separately from the
selected home page. Do not encode the five home pages as extra main-menu rows,
and do not route a home swipe through the list-selection code.

### Shared home layout

The 640x172 landscape layout keeps the current visual split:

- left content region: x=12..390;
- divider: x=403;
- avatar center: x=516, y=83, with the same size and clipping bounds as the
  working main menu;
- compact page indicator and input hint along the bottom edge.

Only the left region changes between home pages. The avatar remains alive in
the right region on all five pages. A swipe or button action counts as activity.
Normal home viewing uses idle/rest routines; a running focus interval uses busy;
weather or market refresh uses thinking; success/failure feedback transitions
smoothly back to idle.

No home page may cover, resize, or recreate the avatar. It must call the same
avatar component and use the same center and viewport as the main menu.

### Clock page

- Display local time as a 12-hour `H:MM AM/PM` clock. The leading hour zero is
  suppressed.
- `design_doc.txt` is the source of truth for the digit and AM/PM artwork. Its
  glyph order is `1,2,3,4,5,6,7,8,9,0,AM,PM`.
- Do not approximate the supplied art with the built-in 5x7 font. A build-time
  generator converts each Unicode block cell into a 2x2 monochrome mask, then
  emits compact bitmap assets which are scaled with nearest-neighbor edges.
- The colon is drawn in the same block weight and may pulse once per second.
  Digits update only when the displayed minute changes.
- A small conventional-font line may show weekday and date below the clock.
- Time is synchronized with SNTP after Wi-Fi connects and written to the onboard
  PCF85063 RTC. On later offline boots, read the RTC first and correct it after
  the next successful network sync.
- Apply a configured IANA/POSIX timezone with daylight-saving rules; do not bake
  a fixed UTC offset into clock arithmetic.
- Until either RTC or network time is valid, show `SYNCING TIME` rather than a
  fabricated time.

### Pomodoro page

Default cycle: 25-minute focus, 5-minute short break, and a 15-minute long break
after four completed focus intervals. Durations must be constants initially and
structured so they can become settings later.

The BOOT interaction is deliberately small and matches the requested
start/stop/reset sequence:

```text
READY  --BOOT--> RUNNING
RUNNING --BOOT--> PAUSED
PAUSED --BOOT--> READY, reset to the full current interval
```

- There is no coordinate-based button on the screen and a tap does nothing.
- At zero, show completion feedback, increment the completed-focus count when
  appropriate, prepare the next focus/break interval, and leave it in READY.
  The next interval never auto-starts.
- The page shows interval kind (`FOCUS`, `BREAK`, or `LONG BREAK`), `MM:SS`,
  state, and progress through the four-focus set.
- RUNNING uses an absolute deadline rather than decrementing a counter on every
  render. This avoids drift when avatar frames or network operations delay UI
  drawing.
- Persist timer state only at transitions (start, pause, reset, completion), not
  every second. If a running timer survives reset/power loss and trusted RTC
  time is available, recompute remaining time from its deadline. Without trusted
  time, restore it paused rather than claiming an inaccurate countdown.
- Completion feedback is visual: accent flash plus a happy avatar transition.
  Audio remains out of scope unless explicitly restored later.

### Weather page

- Show location, current temperature, condition, today's high/low,
  precipitation chance, and the age of the observation.
- Location is a configured latitude/longitude; the device does not infer or
  transmit live GPS location.
- Refresh in a background worker on entry when cached data is older than 15
  minutes, and no more than once every 15 minutes while visible.
- Keep and display the last successful result when offline, clearly marked
  `STALE <age>`. With no cache, show `WEATHER UNAVAILABLE`.
- Network activity must never block input, reopen a page after PWR navigation, or
  reset the device. Responses are bounded before parsing.

### Crypto page

- Show a configured list of up to four assets.
- Each row contains symbol, current quote currency price, and 24-hour percentage
  change with theme-aware positive/negative color.
- Show data timestamp and `LIVE`, `DELAYED`, or `STALE` explicitly.
- This is read-only market information. There are no wallet, exchange, or trade
  actions.

### Stocks page

- Show a configured list of up to four symbols.
- Each row contains symbol, latest available price, session percentage change,
  and market-open/delayed state when the provider exposes it.
- Show data timestamp and stale state using the same rules as Crypto.
- This is read-only. There are no brokerage or order actions.

### Home data service

Provider credentials must not be compiled into firmware. Extend the existing
authenticated Lambda with logically separate, read-only dashboard routes:

```text
GET /v1/home/weather
GET /v1/home/markets/crypto
GET /v1/home/markets/stocks
```

The Lambda owns provider adapters, caching, rate-limit handling, response
normalization, and secrets. The device receives bounded display-ready JSON and
caches the last good response in NVS. Market and weather routes cannot invoke
GitHub or Slack code. Their responses must not contain the device name.

Recommended refresh policy is 60 seconds for visible market data, 15 minutes for
background/non-visible market data, and 15 minutes for weather, subject to the
chosen providers' limits. Failure backs off and preserves the last good value;
it never retries in a tight loop.

### Home acceptance criteria

- Short PWR from the main menu always opens Clock; short PWR from any home page
  always returns to the menu with its highlight unchanged.
- Short PWR within Git, settings, Wi-Fi, or other nested pages still goes to its
  parent and never jumps to home.
- One qualifying horizontal swipe produces exactly one page change in the
  correct direction and wraps at both ends.
- Home swipe code cannot change a menu/list selection or activation target.
- Clock uses the exact supplied digit and AM/PM shapes, remains inside the left
  region, and reports only trusted local time.
- Pomodoro follows READY -> RUNNING -> PAUSED -> reset, does not drift, and keeps
  running while off-page.
- Weather and market failures retain last-good data, label it stale, and keep UI
  navigation responsive.
- Avatar position, dimensions, animation quality, and smooth transitions match
  the main menu on every home page.
- No provider secret appears in firmware, Git history, device logs, or crash
  output.

### Home delivery milestones

1. Add root menu/home navigation and an isolated five-page swipe carousel using
   static placeholder content; hardware-test it against the locked menu contract.
2. Generate clock assets from `design_doc.txt`, add SNTP/RTC/timezone handling,
   and hardware-test minute/DST/offline behavior.
3. Add the Pomodoro deadline state machine, transition persistence, and visual
   completion feedback.
4. Add read-only weather/market service routes, provider adapters, caching, and
   mocked contract tests.
5. Add firmware clients and cached renderers, then flash-test online, stale,
   oversized-response, rate-limit, and navigation-during-refresh cases.

### Home implementation status — 2026-09-02

- Delivered root menu/home navigation and the isolated five-page wrapping swipe
  carousel. Menu/list taps still activate only the BOOT-highlighted row; home
  uses only press-to-release horizontal displacement.
- Delivered generated clock assets sourced directly from `design_doc.txt`, a
  Toronto DST-aware clock, PCF85063 restore/write support, and SNTP correction.
- Delivered the 25/5/15 Pomodoro state machine with absolute deadlines, NVS
  transition persistence, four-focus long breaks, and no interval auto-start.
- Delivered and deployed the authenticated read-only Weather, Crypto, and Stocks
  Lambda routes. Provider responses are normalized and cached; automated service
  tests cover the configured symbols, caching, and oversized upstream rejection.
- Delivered bounded firmware clients, NVS last-good caches, stale/error labels,
  background refresh workers, and avatar thinking/busy integration.
- Built and flashed the ESP32-S3 over `/dev/cu.usbmodem101`. Flash SHA checks
  passed. The flashed image booted through panel/touch/UI initialization, joined
  Wi-Fi, synchronized network time, wrote the RTC successfully, entered the
  30-second avatar rest state, and remained stable under serial monitoring with
  no panic, watchdog, brownout, or reset loop.
- Fixed the home-data startup race found during hardware use: an entry made
  before Wi-Fi association no longer inherits the normal 15-minute/60-second
  success cooldown. The visible page refreshes immediately when Wi-Fi becomes
  available.

### Confirmed product decisions — 2026-09-02

- Timezone: `America/Toronto`, including daylight-saving transitions.
- Clock: 12-hour time using the supplied AM/PM art, with the weekday/date line
  visible and no leading zero on the hour.
- Weather: Toronto, Ontario in Celsius.
- Crypto: BTC, ETH, and SOL, quoted in USD.
- Stocks: AAPL, NVDA, TSLA, and SPY, quoted in USD.
- Carousel: Crypto and Stocks remain separate pages, producing five pages total.
- Pomodoro: 25-minute focus, 5-minute short break, 15-minute long break after
  four focus intervals. BOOT follows READY -> RUNNING -> PAUSED -> reset, and
  completed intervals prepare but do not auto-start the next interval.

## Navigation

The main menu becomes:

1. `git`
2. `status`
3. `settings`
4. `wifi`
5. `switch off`

Selecting `git` opens:

1. `ultracontract`
2. `ultrachatbot`

Selecting a repository opens:

1. `workflows`
2. `merge`
3. `tag`

Selecting `workflows` opens:

1. `git [main <- branch]`
2. `git [prod <- main]`

### `tag` and production-build status

The Git repository picker shows a right-aligned production badge for each
repository: the eight-character tag from the latest successful run, `QUEUED`,
`RUNNING`, `FAILED`, `CANCELLED`, `PENDING`, or `--`. This is display-only and
does not change the locked BOOT-highlight/tap-select contract.

The `tag` detail page tracks only production push runs from the exact workflows
`.github/workflows/deploy.yml` in `ultracontract` and
`.github/workflows/push-ecr-prod.yml` in `ultrachatbot`. Immutable tags are
derived from the workflow run's `head_sha[0..8]`; job logs are never downloaded
or parsed.

- `ultracontract` shows web `:<sha>`, migrator `:<sha>-migrator`, and API
  `:<sha>`.
- `ultrachatbot` shows API `:<sha>` plus its moving `:latest` alias.
- While a newer run is starting, queued, or running, the primary status replaces
  the candidate tag while the last successful tag remains visible.
- Failed or cancelled runs never replace the last successful tag.
- The visible Tag page polls every 10 seconds during active work and every 60
  seconds otherwise. Network work remains outside the UI task.
- A successful merge whose base is `prod` offers `TAP track build`, carries the
  returned merge SHA into the Tag screen, and shows `STARTING` until the matching
  workflow run appears.
- Until the fine-grained token has `Actions: read` approval for both target
  repositories, the UI shows `ACTIONS ACCESS PENDING` and retries without a
  firmware update.

### `git [prod <- main]`

1. Check for an existing open `main -> prod` PR created by this console. Reuse it
   if one exists; do not create duplicates.
2. Otherwise create a non-draft PR from `main` into `prod`.
3. Request reviews from the GitHub logins `Ultrassure` (display name matti) and
   `CAI-yw` (display name stellar).
4. Send the raw PR URL to the configured Slack destination **as saksham's Slack
   user**, not as a bot or app. No extra text or formatting.
5. Show the PR number and success/failure result on the device.

### `git [main <- branch]`

1. Fetch repository branches other than `main` and `prod`.
2. Keep only branches whose head commit activity is within the last 48 hours.
   GitHub's normal branch API does not expose ref-push time, so v1 uses the head
   commit's committer timestamp as the stable proxy for recent push activity.
3. Sort candidates newest first and let BOOT move through them. A tap then
   activates the highlighted branch; coordinates still do not pick the row.
4. After selection, check for and reuse an existing open `<branch> -> main` PR.
5. Otherwise create the PR, request the same two reviewers, and announce the
   raw PR URL in Slack from saksham's user account.

If exact ref-push timestamps become important later, the action service can
record GitHub `push` webhooks and replace the commit-time proxy without changing
the device API.

## Merge screen

The merge screen lists open PRs created by these console workflows, newest first.
Each entry shows:

- PR number and `base <- head` direction;
- GitHub mergeability/status-check state;
- requested reviewers;
- each reviewer's latest effective review state;
- the logins that have approved.

Selecting a PR opens its detail page. Merge is never a one-tap action: the user
must highlight `merge` with BOOT, tap to activate it, then highlight and tap
`confirm merge` on a second screen. `cancel` is always present. The service
sends the current head SHA with the merge request so GitHub rejects the
operation if new commits arrived after confirmation.

The device does not bypass repository protection. Merge is enabled when at least
one requested reviewer has an effective approval and GitHub has not reported
the PR unmergeable. GitHub remains the final authority and may reject a merge if
checks, rulesets, or approvals changed.

## Authorship, Slack identity, and naming

GitHub and Slack are public to collaborators. **The word Hermes must not appear
in any GitHub or Slack artifact**: PR titles, PR bodies, commit titles, merge
messages, review comments, check/status text created by this system, Slack text,
Slack sender overrides, or link-unfurl metadata controlled by this system. This
rule is case-insensitive. The device name stays on the device and in this source
repository; it is not an author, bot, or brand on either target repository.

All GitHub API writes use a fine-grained user token belonging to `saksham-uw` so
GitHub records PR creation and merge actions under that account. Titles and merge
commits use `Merge <head> into <base>`. The PR body contains only an invisible
HTML comment used to recognize console-created PRs, of the form
`<!-- git-workflow:<workflow-id> -->`. There is no visible "created from …" audit
line.

Slack announcements use a **user token** (`xoxp-…`) for saksham's Slack account,
never a bot token (`xoxb-…`). Before enabling announcements, the service calls
Slack `auth.test` and requires the returned user ID to equal the configured
saksham account ID; startup/deployment fails closed on a bot identity or a
different user. `chat.postMessage` is sent without `username`, `icon_url`, or
other identity overrides. The message is the raw PR URL with no extra text, so
Slack's GitHub unfurl can format it.

The service records an operation ID for every create, reviewer request, Slack
announcement, refresh, and merge. Device retries use idempotency keys so a lost
response cannot create a duplicate PR or Slack message.

## Architecture and secret handling

GitHub and Slack credentials must never be compiled into the ESP32 image. Flash
contents are extractable. The device instead calls a narrow HTTPS action service
with a revocable device token:

```text
ESP32 -> action service -> GitHub REST API
                       `-> Slack Web API (user token)
```

The previous Codex/Cursor bridge is not reused. The action service is a separate,
small package with its own process and environment.

Required service environment:

- `HERMES_ACTIONS_DEVICE_TOKEN`: random token accepted only from the device;
- `HERMES_GITHUB_TOKEN`: fine-grained `saksham-uw` token scoped to
  `UltraWording/ultracontract` and `UltraWording/ultrachatbot` with Contents
  read/write and Pull requests read/write;
- `HERMES_SLACK_USER_TOKEN`: Slack user token for saksham's account with
  `chat:write`. Bot tokens are rejected;
- `HERMES_SLACK_DESTINATION`: a Slack user ID for a DM, or a channel ID that
  saksham's user is a member of;
- `HERMES_SLACK_EXPECTED_USER_ID`: saksham's Slack member ID, used to verify the
  token identity with `auth.test` before posting;
- `HERMES_ACTIONS_PORT`: optional, default `8787`.

The firmware stores only the action-service URL and device token. Production must
use HTTPS with a valid public certificate. The first local/hardware milestone can
run in read-only demo mode until the service URL and credentials are configured.

## Device API

All responses are bounded for ESP32 memory and use short strings.

- `GET /v1/health`
- `GET /v1/git/branches?repo=ultracontract&recent_hours=48`
- `POST /v1/git/workflows/main-from-branch/pulls`
- `POST /v1/git/workflows/prod-from-main/pulls`
- `GET /v1/git/pulls?repo=ultracontract`
- `GET /v1/git/pulls/:number?repo=ultracontract`
- `POST /v1/git/pulls/:number/merge`

`repo` is required on every Git route and must be `ultracontract` or
`ultrachatbot`. Create and merge bodies also include `"repo"`.

Every request uses `Authorization: Bearer <device token>`. Mutating requests also
carry `Idempotency-Key`. Error responses have a stable machine code and a concise
display message.

## UI states

- Loading or network work uses the `thinking` avatar state.
- PR creation and merge use `busy`.
- A completed operation returns to `idle`.
- Network work always runs outside the UI task; navigation remains responsive.
- Short PWR may leave a loading page. A stale worker may update cached data but
  must never reopen the page or overwrite the user's current screen.

## Failure behavior

- Offline: show `service offline`; never restart the ESP32.
- Unauthorized: show `device auth failed`; do not retry in a loop.
- Existing PR: display and reuse it.
- No recent branches: show `no branches active in 48h`.
- Reviewer request or Slack failure after PR creation: retain the PR result and
  show the partial failure; retry only the failed step with the same operation ID.
- Merge conflict/check/rules failure: keep the PR open and display GitHub's short
  reason.
- Any response larger than the firmware limit is rejected safely.

## Delivery milestones

1. Fix the Wi-Fi/avatar transition and add the complete Git navigation shell.
2. Add the typed action service, GitHub adapter, Slack adapter, idempotency, and
   contract tests using mocked upstream APIs.
3. Add the firmware HTTPS client and populate branches/PRs from the service.
4. Configure the user-provided GitHub token, Slack **user** token, and deploy the
   service.
5. Flash and hardware-test create, review refresh, Back during loading, merge
   confirmation, and failure recovery.

## Acceptance criteria

- Wi-Fi rows are never obscured by the avatar after navigation completes.
- Every BOOT press advances exactly once through every new menu.
- A tap activates only the highlighted item. Touch coordinates never change which
  row is selected, including after future Git/merge list work.
- Tapping `git` can reach workflows and merge without a reset or blocked UI.
- Branch choices contain only recent eligible branches and are newest first.
- Repeating Create cannot produce duplicate open PRs.
- Both correct GitHub logins are requested as reviewers.
- Slack receives exactly one announcement per created PR: the raw PR URL only,
  posted as saksham's user, with no extra text and no mention of Hermes.
- Slack posting fails closed when `auth.test` does not identify saksham's member
  ID or identifies a bot.
- GitHub PR titles, bodies, comments, statuses created by this system, and merge
  commits never contain the word Hermes, in any capitalization.
- The device displays approvals by login and never claims approval based on an
  old, dismissed, or superseded review.
- Merge requires explicit confirmation and current head SHA.
- All GitHub writes are attributed to `saksham-uw`.
- GitHub/Slack secrets are absent from Git, logs, firmware, and crash output.

## Implementation status — 2026-09-02

Completed locally:

- Wi-Fi is rendered as a full-width page immediately; the avatar overlay was
  removed from scan and connection transitions.
- The five-item main menu and Git/workflow navigation are implemented in
  firmware. Branch list, PR create, merge list/detail, and two-step merge
  confirmation call the action service over HTTPS from a background task.
- Main-menu activation follows the interaction contract above: BOOT chooses the
  row and a tap anywhere activates that highlighted row. Touch coordinates never
  select a different menu entry; one-frame ghost touches are ignored. Do not
  reintroduce coordinate hit-testing or sample-clustering to pick a row.
- The action service is deployed as AWS Lambda Function URL. GitHub/Slack
  secrets stay in Lambda environment variables. Firmware stores only the URL
  and device token in a generated, gitignored header.
- Ten service and HTTP contract tests pass.
- The image was flashed to the connected ESP32-S3. Wi-Fi associated after
  reset.
- The home carousel is fully specified above but is not implemented or flashed
  yet. Its product decisions are confirmed and implementation can begin with the
  isolated navigation milestone.
- Slack currently rejects bot tokens and posts only the raw PR URL. Verification
  against saksham's expected Slack member ID is specified but still needs to be
  implemented and tested before the next action-service deployment.

Live operations now depend on the already-configured GitHub token, Slack user
token, and Lambda remaining reachable.
