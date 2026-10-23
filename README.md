# Click Before Frames (Geode)

## Keys
F6 cycle stage | F7 menu | F8 copy the console log | F9 open/close the in-game console | F10 path view | F11 forget this level's memory

Practice use only; do not use for verified or leaderboard runs.

## v2.4 options (mod settings)
- Wave auto-release (practice only, off by default): lets go for you when holding would crash within the window and letting go survives.
- Orbs: also try the other choice: if a prediction dies after touching an orb, it is retried with the opposite click/skip choice.
- Don't buffer near moving objects (experimental, off by default): inputs are left untouched when the ghost's path hits recently moved spikes/blocks.
- Drift check (debug): logs in the in-game console how far you strayed from the prediction after each input.
- Wave (v2.7): cube-style. A crash you could still turn away from counts as safe; the smallest delay that clears the obstacle wins; inputs during a delay keep their rhythm. "Wave: old v4.6 logic" brings back the v4.6 rules. Platformer levels are never buffered.
- Orb alignment (v2.6): a press made up to 4 frames before an orb is held back until you're inside the orb, so it hits.
- v2.8: the ghost copies the full player state (slopes, reverse, sideways, forces, dash), size portals are simulated and mirror portals no longer stop the prediction.
- v2.10: wave tries 3 turn-away points, stops runs as soon as they are good enough (faster), and centers saves with a 1-tick safety margin. Console is see-through.
- v2.12 robot: presses count as safe if some hold length survives; robot auto-release (practice only).
- v2.13 level memory: learns per spot from your runs (delays that worked are tried first, spots where saves failed are left alone, risky spots marked in the path view).
- v2.15 spider: flips are judged by landing on the new surface, and crashes you could flip away from count as safe.
- v2.16 orbs: orbs usable on any tick inside them in predictions, orb alignment picks the best tick inside the orb, dash orbs held through in predictions, dash releases buffered, dash auto-release.
- v2.23 unlocked FPS: all windows (look-ahead, delays, landing, spam gap, auto-release window, memory) are converted from 240/s ticks to the real physics step rate, and the ghost uses the average step length.
- v2.24 FPS: prediction ghosts no longer spawn visual effects or leave nodes/actions behind (the FPS used to sink a little with every save).
- v2.25 ship (opt-in): ship presses/releases buffered with recovery-aware scoring, own small max delay, feathering passes through.
- v2.26 orb timing: presses already inside an orb are re-timed within the orb if that survives, learned orb timings are tried first, predictions try later clicks inside orbs.
- v2.27: late cube/ball crashes skip the search; experimental moving-object prediction (straight-line motion).
- v2.28 robust saves: each save is checked with the player slightly ahead/behind; fragile edge saves are swapped for a nearby delay that survives all three.
- v2.31: hazards overlapping you at the click (player-following spikes, noclip) are ignored by predictions.
- v2.34: with noclip, being inside a spike no longer makes the mod re-press the held button every tick.
- v2.35: Click Between Frames sync is back; low latency mode (no chained delays, max 1 frame, 3 ms searches).
- v2.36 CBF: rest of a split tick is collision-checked like the game does; logs show where in the tick a CBF click landed (@0.43) and separate 'safe' from 'safe only if you react by tX'.
- v2.37: position read from the per-step node position (CBF sync actually active now, drift check / path view / memory slices correct; memory restarts per level).
- v2.39: CBF Extrapolate is detected and shown in the console/export (it's visual only; predictions are unaffected).
- v2.40 UFO: crashes you could have flapped away from count as safe, landing counts as safe, smallest delay wins, spam guard for fast flapping.
- v2.41 FPS: moving-object tracking drops far objects and keeps a short moving-now list (was checking up to 4096 objects every step and every prediction).
- v2.43: more time at death spots - where you keep dying, react-only-safe inputs get the delay that pushes the crash furthest away.
- v2.44: in-game menu (pause menu 'Buffer' button or F7): stage, main switches, status, all settings / copy log / forget level. Ship: own spam gap (feathering is buffered), max delay 4, on by default.
- v2.45 ball: short flips count as landings, landings must last 8 ticks, spam guard for fast flipping; menu switch.
- v2.46 watermark (always on, not configurable): 'Basu Input Buffer | stage' in the level while the buffer is on, and 'Basu Input Buffer was on' on the level complete screen if it was on during the attempt.
- v2.49: ghost no longer touches collision blocks (could break collision triggers); spam guard sends waiting inputs instead of dropping them (lost clicks broke click/event triggers).
- v2.50: cuter menu: pink theme, pulsing hearts, floating hearts and sparkles, your own icon in the corner; switches turn pink when on.
- v2.51: cube jumps that bump a ceiling count as landings (buffer works under blocks); smart swing (flip-back scoring + spam guard); per-level settings; practice stats (per attempt + Stats button); training mode (hints only, no delays); death heatmap on the progress bar.
- v2.52: no popup after a death; attempt stats stay in the console, the Stats button and the level complete screen.
- v2.53: ghosts respect 'disable controls' from Options triggers (a hidden Player 2 parked under spikes no longer jumps into them in predictions; fixes 'no save: press dies in 0t' on every jump in Relief).
- v2.54: when the level disables Player 1's controls and you play Player 2 (Relief's wave parts), Player 2 is predicted; death log shows both players' modes.
- v2.55: in two-player levels, P2-side inputs made outside dual mode move Player 1, and are now buffered as P1 inputs (Relief's wave parts were predicted on the idle Player 2).
- v2.56: ding when a click is saved (setting + menu switch, on by default); menu opens smaller (78%).
- v2.57: watermark hidden in practice mode (always shown in normal mode while the buffer is on).
- v2.58: fix v2.51 regression (every cube landing counted as a ceiling bump, so short hops were called safe too early); settings changed in the settings popup during a level are kept for that level; ding sound preloaded.
- v2.59: low latency on by default and on every level (not per level); the F6 stage is one setting for every level and is kept between levels and restarts.
- v2.60: low latency off by default and switched off once on every install (still one setting for all levels).
- v2.61: prediction back to v2.48 behaviour: collision objects collide again in predictions, spam drops waiting inputs again, no ceiling-bump landing rule.
- v2.62: per-level settings off by default (switched off once): every level uses the same switches, like v2.43.1.
