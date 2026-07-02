# ZZZonline Roadmap

Z Online is being built in three deliberate phases. Each phase ships something
playable, and each is a load-bearing foundation for the next. The guiding
constraint from day one: **the simulation is a small, deterministic,
host-agnostic WASM core** — because that is the only architecture that scales
from "vs. the PC" to "a persistent planet at massive scale" without a rewrite.

---

## Phase 1 — Single player vs. the PC ✅ (this build)

A complete browser game against a built-in AI opponent.

**Shipped:**
- Sector/flag territory control faithful to Z: no harvesting, no base
  building; holding territory speeds up all production.
- Seven unit types across robot and vehicle factories, fixed gun turrets,
  destructible forts, win/lose conditions.
- A* pathfinding, autonomous unit combat, projectiles, effects.
- AI opponent with three difficulty levels (reaction speed, expansion
  aggression, assault thresholds).
- Freestanding C → wasm32 build via stock clang (~25 KB module, no libc, no
  Emscripten, no JS framework). Canvas host renders a primitive
  draw-command buffer.
- Headless Node smoke tests that boot the module and simulate full games.

**Phase 1 polish backlog (pre-multiplayer):**
- [ ] Sound effects and unit voice barks ("Yes, sir!").
- [ ] Larger scrolling maps + minimap (the sim already works in world
      coordinates; the host needs a camera).
- [ ] More map layouts / random map generator with connectivity guarantees.
- [ ] APCs, engineers, capturable neutral vehicles (a Z signature).
- [ ] Touch controls for mobile.

---

## Phase 2 — Web-based multiplayer 🔜

Two (then more) humans on one map, in the browser, no installs.

**Architecture: deterministic lockstep.** The Phase 1 design was chosen to
make this step cheap:

- The sim core already runs identically on any host (it has no I/O, no time
  source, no float ambiguity beyond IEEE-754 basics — inputs go in as plain
  commands, state comes out as a buffer).
- **Step 2.0 — fixed timestep.** Convert `tick(dt)` to a fixed simulation
  step (e.g. 30 Hz) with host-side interpolation, and route *all* mutation
  through a serializable command stream (`select`, `move`, `set_production`).
  The existing pointer handlers become command *producers*. Add a state hash
  export for desync detection.
- **Step 2.1 — transport.** WebRTC DataChannels peer-to-peer for 1v1, with a
  lightweight WebSocket relay/lobby server for matchmaking, NAT fallback, and
  spectators. Each client runs the same wasm sim; only commands cross the
  wire (a few bytes per action — the 56 KB/s dial-up budget of 1996 would
  still be plenty).
- **Step 2.2 — resilience.** Input delay + rollback buffer, reconnect &
  resync from a state snapshot, and the Phase 1 AI stepping in for dropped
  opponents so games always finish.
- **Step 2.3 — beyond 1v1.** The command stream and per-team data structures
  are already team-indexed rather than hardcoded to two sides; lift
  `TEAM_*` to N players, add team alliances, 2v2.

**Deliverables:** lobby site, ranked 1v1, replays (a replay *is* the command
stream — determinism makes this nearly free), spectator mode.

---

## Phase 3 — The persistent world 🌍 (the endgame)

One always-on planet. Thousands of sectors. Hundreds of simultaneous players.
A front line that moves while you sleep. **Massive scale is the point.**

The sector mechanic is what makes Z uniquely suited to an MMO-RTS, in a way
classic RTS economies are not: territory is *the* resource, it is spatially
partitionable, and ownership is a single integer per sector.

**Architecture sketch:**

- **Sharded simulation.** The world map is a grid of regions, each region a
  cluster of sectors simulated by one authoritative server process — which is
  the same wasm sim core, run server-side (wasm runtimes are not just for
  browsers). Regions hand units crossing borders to their neighbors;
  battles never span more than adjacent regions, so shards scale
  horizontally with the front line.
- **Interest management.** A client only subscribes to the regions in its
  viewport plus a low-frequency "war map" summary (sector ownership deltas —
  bytes, not states) for the rest of the planet.
- **Persistence.** Sector ownership, factory state, and standing armies live
  in a durable store checkpointed from each shard; the world survives
  restarts and patches. History is an event log — the war has a replayable
  archaeology.
- **Players as commanders, not clients.** A player logs in, is granted
  command over forces in their faction's zone, and logs out without pausing
  anything; the Phase 1 AI (grown into faction-level doctrine AI) keeps
  their front from collapsing. Death is losing ground, not losing a save.
- **Factions, alliances, seasons.** Two mega-factions (red vs. blue, as
  tradition demands) with player squads inside them; periodic world resets
  ("campaigns") with monuments to the victors.

**Milestones:**
- [ ] 3.0 — server-side authoritative sim of a single region (sim core in a
      server wasm runtime, clients as thin views).
- [ ] 3.1 — multi-region world with border handoff, 10×10 regions,
      dozens of concurrent players.
- [ ] 3.2 — persistence layer + login/identity + faction assignment.
- [ ] 3.3 — offline AI stewardship of player-held territory.
- [ ] 3.4 — the thousand-sector planet: load-tested massive-scale campaign.

---

## Non-goals

- Recreating Z's original assets, missions, or FMV — this is a from-scratch
  homage to the *mechanics*, not a port.
- Native desktop builds. The browser is the platform; WASM is the engine.
