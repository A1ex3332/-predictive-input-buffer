# Predictive Input Buffer (Geode)

## Get the .geode file without installing anything
1. Create a free GitHub repo and upload everything in this folder (keep the .github folder).
2. Open the Actions tab, run "Build Mod" (or just push).
3. When it finishes, download "Build Output" and take the .geode file.
4. Put it in your Geode mods folder (or drag onto the GD window).

## Build locally instead
Install the Geode CLI and SDK, then run:  geode build

## Keys
F6 cycle stage | F7 toggle wave mode

Edit "yourname" in mod.json first. Practice use only; do not use for verified or leaderboard runs.

## v2.4 options (mod settings)
- Wave auto-release (practice only, off by default): lets go for you when holding would crash within the window and letting go survives.
- Orbs: also try the other choice: if a prediction dies after touching an orb, it is retried with the opposite click/skip choice.
- Don't buffer near moving objects: inputs are left untouched when the ghost's path hits recently moved spikes/blocks.
- Drift check (debug): logs in the K panel how far you strayed from the prediction after each input.
- Wave buffering uses the v4.6 rules exactly (65/50 lookahead, gain bar, opposite-state ghost, 1-tick queued opposite). Platformer levels are never buffered.
