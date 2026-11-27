# Pixel Hop Twins 🎮

**An independent, 8-bit platformer-inspired browser game** — a love letter to the classic
jump-'n'-run genre, built entirely with Vanilla JavaScript and the HTML5 Canvas API.

> **⚠️ Legal Disclaimer**
> This is an **original, independently created** work. It is **not** a clone, port, remake, or
> derivative of *The Great Giana Sisters*, *Super Mario Bros.*, or any other commercial title.
> All characters, level designs, music, and graphics are **entirely original**, authored from
> scratch and released as **Creative Commons Zero (CC0)** public-domain works.
> The source code is released under the **MIT License**.
> Any resemblance to existing games is incidental and limited to genre conventions that are
> themselves uncopyrightable (platforms, coins, jumping physics).

---

## Features

- 🕹️  **Pure Vanilla JS + HTML5 Canvas** — open `index.html` and play, no build step needed
- 🎨  **Procedural pixel art** — every sprite is drawn in code from pixel matrices; zero external image files
- 🔊  **WebAudio beep sounds** — jump, gem, stomp, power-up, death, level-complete — all synthesised live
- 🌍  **2 hand-crafted levels** with platforms, gaps, gems, enemies, and a goal flag
- 🔮  **Prisma Power-Up** — collect the magenta crystal to unlock a **double jump** and alternate colour palette for 15 seconds
- 👾  **2 enemy types**: patrolling ground enemies and bouncing jump enemies
- ⏱️  Coyote time, variable jump height (jump-cut), jump buffer
- 📺  Parallax mountain background, crisp pixel rendering

---

## Controls

| Action          | Keys                              |
|-----------------|-----------------------------------|
| Move left       | `←` / `A`                        |
| Move right      | `→` / `D`                        |
| Jump            | `Space` / `↑` / `W`              |
| Double jump*    | `Space` / `↑` / `W` (mid-air)    |
| Pause / Resume  | `P`                               |
| Mute / Unmute   | `M`                               |
| Restart / Enter | `Enter`                           |

\* Double jump is only available while the **Prisma Power-Up** is active.

---

## How to Play

1. **Quick start** — open `index.html` directly in any modern browser (Chrome, Firefox, Edge, Safari).
2. **Via local server** (recommended if fetch() is blocked):
   ```bash
   npx serve .
   # then open http://localhost:3000
   ```
3. **Collect gems** (+100 pts each), **stomp enemies** from above (+200 pts), and **reach the flag** to finish the level.
4. You start with **3 lives**. A timer counts down — reach the goal before time runs out!

---

## Project Structure

```
/index.html          Main HTML page
/style.css           Global styles (pixel-crisp rendering)
/src/
  constants.js       Tile IDs, physics values, 16-colour palette
  input.js           Keyboard handler (held / just-pressed)
  audio.js           WebAudio square-wave sound engine
  sprites.js         Procedural pixel-art sprite renderer
  camera.js          Smooth side-scrolling camera
  level.js           Tilemap loader & renderer
  player.js          Player physics, coyote time, prisma power-up
  enemy.js           Ground & jump enemy variants
  game.js            Game-state machine, HUD, overlays
  main.js            rAF game loop (fixed 1/60 s timestep)
/levels/
  level1.json        "Crystal Caverns"
  level2.json        "Skyward Peaks"
/README.md
/LICENSE
```

---

## Adding New Levels

1. Create a new file `levels/level3.json` following the schema of the existing levels:
   - `name`        — display name
   - `timeLimit`   — seconds
   - `playerStart` — `{ "x": col, "y": row }`
   - `tiles`       — 2-D array of tile IDs (0=empty, 1=solid, 2=platform, 3=spike, 4=goal)
   - `gems`        — array of `{ "x": col, "y": row }`
   - `powerUps`    — array of `{ "x": col, "y": row }`
   - `enemies`     — array of `{ "type": "ground"|"jump", "x": col, "y": row, "speed": px/s }`
2. Add the file path to the `LEVEL_FILES` array in `src/game.js`.

---

## Licence

- **Source code:** MIT © 2026 stm1978 (see `LICENSE`)
- **All original assets (sprites, audio, levels):** Creative Commons Zero (CC0) — public domain
