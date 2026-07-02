/*
 * Z Online — JavaScript host.
 * Loads the freestanding wasm module, drives the sim loop, forwards input,
 * and rasterizes the primitive command buffer onto a 2D canvas.
 */

const HUD_H = 44;           // top HUD strip; world is drawn below it
const canvas = document.getElementById("game");
const ctx = canvas.getContext("2d");
const overlay = document.getElementById("overlay");
const overlayMsg = document.getElementById("overlay-msg");

// color palette referenced by index from wasm draw commands
const PALETTE = [
  "#3b7dd8", // 0 player blue
  "#d84a3b", // 1 enemy red
  "#8a95a1", // 2 neutral gray
  "#e8edf2", // 3 white
  "#f0c948", // 4 yellow
  "#23272d", // 5 dark
  "rgba(59,125,216,0.10)",  // 6 blue territory tint
  "rgba(216,74,59,0.10)",   // 7 red territory tint
  "#5ec26a", // 8 green
  "#f08048", // 9 orange
];

const TEXT_CODES = [
  "ROBOT FAB", "VEHICLE FAB", "GUN", "FORT",
  "G", "P", "T", "S", "J", "L", "H",
];
const UNIT_NAMES = ["Grunt", "Psycho", "Tough", "Sniper", "Jeep", "Lt. Tank", "Hv. Tank"];

let wasm, mem;
let terrainCanvas = null;

async function boot() {
  const res = await fetch("game.wasm");
  const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
  wasm = instance.exports;
  mem = wasm.memory;
  wasm.init((Math.random() * 0xffffffff) >>> 0);
  applyDifficulty();
  buildTerrain();
  requestAnimationFrame(frame);
}

/* Pre-render static terrain (grass checker + rocks + sector grid). */
function buildTerrain() {
  const w = wasm.map_w(), h = wasm.map_h(), t = wasm.tile_size();
  const tilesPtr = wasm.map_ptr();
  const tiles = new Uint8Array(mem.buffer, tilesPtr, w * h);
  terrainCanvas = document.createElement("canvas");
  terrainCanvas.width = w * t;
  terrainCanvas.height = h * t;
  const g = terrainCanvas.getContext("2d");
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const rock = tiles[y * w + x] === 1;
      g.fillStyle = rock ? "#3a3f47" : (x + y) % 2 ? "#232a24" : "#20261f";
      g.fillRect(x * t, y * t, t, t);
      if (rock) {
        g.fillStyle = "#4a505a";
        g.fillRect(x * t + 4, y * t + 4, t - 8, t - 8);
      }
    }
  }
  // sector borders (5x3 grid of 8x8-tile sectors)
  g.strokeStyle = "rgba(255,255,255,0.10)";
  g.setLineDash([4, 6]);
  g.lineWidth = 1;
  for (let i = 1; i < 5; i++) {
    g.beginPath(); g.moveTo(i * 8 * t, 0); g.lineTo(i * 8 * t, h * t); g.stroke();
  }
  for (let i = 1; i < 3; i++) {
    g.beginPath(); g.moveTo(0, i * 8 * t); g.lineTo(w * t, i * 8 * t); g.stroke();
  }
}

/* ---------------- input ---------------- */

function worldCoords(ev) {
  const r = canvas.getBoundingClientRect();
  const sx = canvas.width / r.width, sy = canvas.height / r.height;
  return [(ev.clientX - r.left) * sx, (ev.clientY - r.top) * sy - HUD_H];
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("mousedown", (e) => {
  if (!wasm) return;
  const [x, y] = worldCoords(e);
  if (y < 0) return;
  wasm.pointer_down(x, y, e.button);
});
canvas.addEventListener("mousemove", (e) => {
  if (!wasm) return;
  const [x, y] = worldCoords(e);
  wasm.pointer_move(x, y);
});
window.addEventListener("mouseup", (e) => {
  if (!wasm) return;
  const [x, y] = worldCoords(e);
  wasm.pointer_up(x, y, e.button);
});
window.addEventListener("keydown", (e) => {
  if (!wasm) return;
  if (e.key.length === 1) wasm.key_press(e.key.charCodeAt(0));
});
const difficultySel = document.getElementById("difficulty");
function applyDifficulty() {
  if (wasm) wasm.set_difficulty(parseInt(difficultySel.value, 10));
}
difficultySel.addEventListener("change", applyDifficulty);

document.getElementById("restart").addEventListener("click", () => {
  wasm.init((Math.random() * 0xffffffff) >>> 0);
  applyDifficulty();
  buildTerrain();
  overlay.classList.remove("show");
});

/* ---------------- render ---------------- */

function drawCommands() {
  const n = wasm.render();
  const buf = new Float32Array(mem.buffer, wasm.draw_buf(), n * 8);
  for (let i = 0; i < n; i++) {
    const o = i * 8;
    const type = buf[o];
    const a = buf[o + 1], b = buf[o + 2], c = buf[o + 3], d = buf[o + 4];
    const e = buf[o + 5], f = buf[o + 6];
    switch (type) {
      case 0: { // RECT x,y,w,h,color,alpha
        ctx.globalAlpha = f > 0 && f <= 1 ? f : 1;
        ctx.fillStyle = PALETTE[e | 0] || "#fff";
        ctx.fillRect(a, b, c, d);
        ctx.globalAlpha = 1;
        break;
      }
      case 1: { // CIRCLE x,y,r,color,alpha
        ctx.globalAlpha = e > 0 && e <= 1 ? e : 1;
        ctx.fillStyle = PALETTE[d | 0] || "#fff";
        ctx.beginPath();
        ctx.arc(a, b, Math.max(0.5, c), 0, Math.PI * 2);
        ctx.fill();
        ctx.globalAlpha = 1;
        break;
      }
      case 2: { // LINE x1,y1,x2,y2,color,width
        ctx.strokeStyle = PALETTE[e | 0] || "#fff";
        ctx.lineWidth = f || 1;
        ctx.beginPath(); ctx.moveTo(a, b); ctx.lineTo(c, d); ctx.stroke();
        break;
      }
      case 3: { // FLAG x,y,color
        ctx.strokeStyle = "#d8dee5";
        ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(a, b + 10); ctx.lineTo(a, b - 14); ctx.stroke();
        ctx.fillStyle = PALETTE[c | 0] || "#8a95a1";
        ctx.beginPath();
        ctx.moveTo(a, b - 14);
        ctx.lineTo(a + 13, b - 10);
        ctx.lineTo(a, b - 6);
        ctx.closePath();
        ctx.fill();
        break;
      }
      case 4: { // HPBAR x,y,w,frac
        ctx.fillStyle = "#111";
        ctx.fillRect(a, b, c, 4);
        const frac = Math.max(0, Math.min(1, d));
        ctx.fillStyle = frac > 0.5 ? "#5ec26a" : frac > 0.25 ? "#f0c948" : "#e05252";
        ctx.fillRect(a, b, c * frac, 4);
        break;
      }
      case 5: { // RING x,y,r,color
        ctx.strokeStyle = PALETTE[d | 0] || "#fff";
        ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.arc(a, b, Math.max(0.5, c), 0, Math.PI * 2); ctx.stroke();
        break;
      }
      case 6: { // TEXT x,y,code,color,size
        const s = TEXT_CODES[c | 0] || "?";
        ctx.fillStyle = PALETTE[d | 0] || "#fff";
        ctx.font = `bold ${e | 0}px system-ui, sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(s, a, b);
        break;
      }
    }
  }
}

function drawHud() {
  ctx.fillStyle = "#1d2127";
  ctx.fillRect(0, 0, canvas.width, HUD_H);
  ctx.strokeStyle = "#2c333c";
  ctx.beginPath(); ctx.moveTo(0, HUD_H - 0.5); ctx.lineTo(canvas.width, HUD_H - 0.5); ctx.stroke();

  ctx.textAlign = "left";
  ctx.font = "bold 13px system-ui, sans-serif";

  // player side
  ctx.fillStyle = PALETTE[0];
  ctx.fillText(`BLUE  territory ${wasm.territory_pct(0)}%  ·  units ${wasm.unit_count(0)}  ·  fort ${wasm.fort_hp_pct(0)}%`, 12, 18);
  // enemy side
  ctx.fillStyle = PALETTE[1];
  ctx.fillText(`RED   territory ${wasm.territory_pct(1)}%  ·  units ${wasm.unit_count(1)}  ·  fort ${wasm.fort_hp_pct(1)}%`, 12, 36);

  // selection info
  ctx.textAlign = "right";
  ctx.fillStyle = "#cfd6dd";
  const selFactory = wasm.sel_factory();
  if (selFactory >= 0) {
    const t = wasm.factory_prod(selFactory);
    ctx.fillText(`Factory: building ${UNIT_NAMES[t]} (${wasm.factory_pct(selFactory)}%) — click again to change`, canvas.width - 12, 18);
  } else {
    const n = wasm.sel_count();
    ctx.fillText(n > 0 ? `${n} unit${n > 1 ? "s" : ""} selected` : "no selection", canvas.width - 12, 18);
  }
  ctx.fillStyle = "#7c8794";
  ctx.font = "11px system-ui, sans-serif";
  ctx.fillText("capture flags → hold territory → out-produce the enemy", canvas.width - 12, 35);
}

/* ---------------- main loop ---------------- */

let last = performance.now();
let gameOverShown = false;

function frame(now) {
  const dt = Math.min(100, now - last);
  last = now;

  wasm.tick(dt);

  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.save();
  ctx.translate(0, HUD_H);
  ctx.drawImage(terrainCanvas, 0, 0);
  drawCommands();
  ctx.restore();
  drawHud();

  const status = wasm.game_status();
  if (status !== 0 && !gameOverShown) {
    gameOverShown = true;
    overlayMsg.textContent = status === 1 ? "VICTORY" : "FORT LOST";
    overlayMsg.className = "msg " + (status === 1 ? "win" : "lose");
    overlay.classList.add("show");
  }
  if (status === 0) gameOverShown = false;

  requestAnimationFrame(frame);
}

boot();
