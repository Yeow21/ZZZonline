// Headless smoke test for the Z Online wasm module.
//
// Boots the sim under Node, exercises the input paths, lets the AI play
// against a crude scripted opponent, and asserts the game progresses and
// terminates. Run with:  node tests/smoke.mjs
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

const wasmPath = fileURLToPath(new URL("../web/game.wasm", import.meta.url));
const bytes = await readFile(wasmPath);

async function boot() {
  const { instance } = await WebAssembly.instantiate(bytes, {});
  return instance.exports;
}

// --- basic boot state ---------------------------------------------------
let w = await boot();
w.init(12345);
if (w.unit_count(0) !== 4 || w.unit_count(1) !== 4)
  throw new Error("unexpected starting unit counts");
if (w.territory_pct(0) !== 20 || w.territory_pct(1) !== 20)
  throw new Error("unexpected starting territory");
if (w.game_status() !== 0) throw new Error("game should start in progress");

// --- input paths don't trap ----------------------------------------------
w.pointer_down(100, 100, 0);
w.pointer_move(400, 400);
w.pointer_up(400, 400, 0);
w.pointer_down(600, 300, 2);
w.key_press(83);
w.set_difficulty(1);

// --- full game: AI vs. a scripted flag-chasing player ---------------------
const spots = [];
for (let r = 0; r < 3; r++)
  for (let c = 0; c < 5; c++) spots.push([c * 192 + 96 + 42, r * 192 + 96]);

const step = 33;
let k = 0;
let result = 0;
for (let t = 0; t < 15 * 60 * 1000; t += step) {
  w.tick(step);
  if (t % 4000 < step) {
    // select everything, order a squad to the next flag on the rotation
    w.pointer_down(0, 0, 0);
    w.pointer_move(960, 576);
    w.pointer_up(960, 576, 0);
    const [x, y] = spots[k++ % spots.length];
    w.pointer_down(x, y, 2);
  }
  const n = w.render();
  if (n <= 0 || n > 4096) throw new Error("bad draw command count: " + n);
  result = w.game_status();
  if (result !== 0) {
    console.log(`game concluded at t=${(t / 1000).toFixed(0)}s, status=${result}`);
    break;
  }
}
if (result === 0)
  throw new Error("game did not conclude within 15 simulated minutes");

// --- restart works --------------------------------------------------------
w.init(999);
if (w.game_status() !== 0 || w.unit_count(0) !== 4)
  throw new Error("re-init did not reset the game");

console.log("SMOKE TEST PASSED");
