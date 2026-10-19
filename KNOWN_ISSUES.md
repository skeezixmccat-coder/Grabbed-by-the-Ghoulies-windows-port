# Preview limitations

This is a player-facing snapshot of known problems in this preview. The B-numbers are the project's internal bug IDs.

- **Performance (B01/B02):** heavy fights and death effects can slow down. At 3×, the final pre-launcher heavy-hallway repeats averaged 36.9 and 39.5fps, with one 10-second window at 34.5fps. Remaining slowdowns are accepted for this early preview; these bugs are still open. Those numbers are not an anisotropic-filtering benchmark.
- **Experimental 60fps (B06):** a timed challenge matched the 30fps control and steady movement matched near 60fps, but movement-startup distance varied with cadence. Broader timing, physics, audio synchronization and full campaign correctness are not established. Locked 30fps is the default fallback. 120/144fps are unsupported.
- **Audio (B03):** an electrical buzz was reported near the room before Chapter 1 Scene 10 (Relic Store).
- **Book menu (B04):** a page can appear black during a page-turn.
- **Textures (B05):** a sparsely textured scene was reported around a Chapter 2 ship area; it may overlap the long-session texture fix and remains unconfirmed.
- Some renderer, character-head and sound-balance fixes have not yet been individually confirmed in play. Do not assume that every recorded fix is verified throughout the game.
- Anisotropic filtering is optional and limited to eligible mipmapped textures. In testing it gave sharper distant floor detail with no noticeable performance penalty in one room; this is not a controlled benchmark or campaign-wide compatibility result.
- Only the documented USA executable revision is accepted. Full-disc/compressed images and alternate revisions are not supported.
- Windows accessibility inspection of the live game window stalled presentation in a diagnostic session (the sampled window thread was waiting inside UIAutomationCore). A bounded launch without that inspection completed normally. Screen-reader/accessibility-tool compatibility needs investigation.
- AMD, Intel graphics, Linux/Proton, Steam Deck and a clean second-PC installation have not been validated.
- No DLSS, FSR, frame generation or texture replacement is included.

For crashes, keep the explicit STOP code and report. Never upload game data with a bug report.
