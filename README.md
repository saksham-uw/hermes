# HERMES

HERMES is a macOS workspace for the repositories you select in Settings → Connectors. Repository cards show recent branch activity, open PRs and production image tags. Open a card to prepare a branch → main or main → prod PR, request review, inspect checks or send its link in a Slack direct message. Download the universal build for macOS 13 and later from [the HERMES download page](https://hermes-mac-saksham.vercel.app). Unzip it and move HERMES.app into Applications before opening it. The first release is ad-hoc signed. If Gatekeeper blocks it, use **System Settings → Privacy & Security → Open Anyway** after attempting to open the app.

## Updates and releases

The [ArtCraft Launcher](https://github.com/storytold/craft-launcher) publishes platform packages through GitHub Releases and offers to update and restart itself. HERMES uses the same release model with [Sparkle](https://sparkle-project.org/documentation/publishing/) for installation on macOS. It checks its feed automatically and offers a manual **Check for Updates…** command. Installing an available update relaunches HERMES.

Push a new, increasing stable version tag (for example `v0.1.1`) on the desired commit. The release workflow builds both architectures, verifies the app bundle, signs the ZIP and feed, then publishes them together on GitHub Releases. The Vercel download page and feed point to the latest release. Each feed references the ZIP at its version-specific URL. Keep published releases immutable.

To build locally on this Mac, run `scripts/build-release.sh 0.1.0`. The ZIP and feed are written to `build/releases/0.1.0/`. The local signing key is stored in the login Keychain under the Sparkle account `hermes-saksham-uw`. CI reads the corresponding GitHub Actions secret `SPARKLE_PRIVATE_KEY`. The app trusts the public key in `app/update-public-key.txt`. Preserve the private key when moving to a new release machine.

The static Vercel project is `hermes-mac-saksham`. To publish changes to the download page, deploy the `website/` directory with the Vercel CLI. App releases only require the Git tag.
