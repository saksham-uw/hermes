# Hermes Avatar Lab assets

`kirby.avatar.json` is the Kirby runtime definition synchronized from
[Bible Strong Avatar Lab](https://github.com/smontlouis/bible-strong-avatar-lab).
It retains the tool's procedural body, 28 expressions, 23 semantic animations,
step timing, easing modes, color fields, and blink configuration.

Hermes uses the published `@bible-strong/avatar-core` package to validate that
definition and render every native expression and transition. The generated
firmware assets keep body and eye coverage separate so the ESP32 can apply Hermes'
active theme without rerasterizing geometry.

Firmware features can trigger any exported preset by semantic key through
`hermes_avatar_play`, for example `hermes_avatar_play(&avatar, "thinking", now)`.
The higher-level state system is configured in `hermes-states.json`:

- `idle`: idle, curious, happy, and playful routines.
- `rest`: drowsy, sleeping, and bored routines.
- `busy`: listening, working, and excited routines.
- `thinking`: thinking, searching, and confused routines.

Firmware chooses a non-repeating routine from the active pool. It enters `rest`
after 30 seconds without input, returns to `idle` on interaction, and exposes
`hermes_avatar_set_state(..., HERMES_AVATAR_STATE_BUSY, ...)` for feature work.

Avatar Lab and `@bible-strong/avatar-core` are Copyright Stéphane
Montlouis-Calixte and licensed under GNU AGPL-3.0-only. The upstream source and
license are available in the linked repository. Generated files record their
source and should be regenerated with `npm run avatar:generate`, not edited by
hand.
