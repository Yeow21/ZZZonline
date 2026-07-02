/*
 * Z Online — a WebAssembly homage to "Z" (The Bitmap Brothers, 1996).
 *
 * Core game simulation, compiled as a freestanding wasm32 module (no libc,
 * no Emscripten). The JavaScript host drives the loop (tick/render), feeds
 * pointer input, and draws the primitive command buffer this module emits.
 *
 * Design pillars inherited from Z:
 *   - The map is divided into sectors, each with a flag. Touch the flag,
 *     own the sector (and everything manufactured inside it).
 *   - No resource harvesting, no base building. Factories inside sectors
 *     produce units continuously; the more territory you hold, the faster
 *     ALL your factories run.
 *   - Win by destroying the enemy fort.
 */

#define WASM_EXPORT(name) __attribute__((export_name(name), visibility("default")))

typedef unsigned char u8;
typedef unsigned int u32;

/* ------------------------------------------------------------------ */
/* No-libc support                                                     */
/* ------------------------------------------------------------------ */

void *memset(void *dst, int c, unsigned long n) {
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}
void *memcpy(void *dst, const void *src, unsigned long n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static float f_sqrt(float x) { return __builtin_sqrtf(x); }
static float f_abs(float x)  { return __builtin_fabsf(x); }
static float f_min(float a, float b) { return a < b ? a : b; }
static float f_max(float a, float b) { return a > b ? a : b; }
static int   i_abs(int x) { return x < 0 ? -x : x; }

static u32 rng_state = 0x2f6e2b1u;
static u32 rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rng_range(int n) { return (int)(rng_next() % (u32)n); }
static float rng_f(void) { return (float)(rng_next() & 0xffff) / 65535.0f; }

/* ------------------------------------------------------------------ */
/* Map constants                                                       */
/* ------------------------------------------------------------------ */

#define TILE        24
#define MAP_W       40
#define MAP_H       24
#define WORLD_W     (MAP_W * TILE)   /* 960 */
#define WORLD_H     (MAP_H * TILE)   /* 576 */

#define SEC_COLS    5
#define SEC_ROWS    3
#define SEC_COUNT   (SEC_COLS * SEC_ROWS)
#define SEC_TW      (MAP_W / SEC_COLS)  /* 8 tiles */
#define SEC_TH      (MAP_H / SEC_ROWS)  /* 8 tiles */
#define SEC_PW      (SEC_TW * TILE)
#define SEC_PH      (SEC_TH * TILE)

#define TEAM_NONE   (-1)
#define TEAM_PLAYER 0
#define TEAM_AI     1

/* terrain */
#define TER_GRASS 0
#define TER_ROCK  1
#define TER_BLDG  2   /* blocked footprint under a building (drawn as grass) */

static u8 tiles[MAP_W * MAP_H];

/* ------------------------------------------------------------------ */
/* Unit definitions                                                    */
/* ------------------------------------------------------------------ */

enum {
    UT_GRUNT, UT_PSYCHO, UT_TOUGH, UT_SNIPER,   /* robots  */
    UT_JEEP, UT_LTANK, UT_HTANK,                /* vehicles */
    UT_COUNT
};

typedef struct {
    float hp, speed, range, dmg, reload, build_time, radius;
    int is_vehicle;
} UnitDef;

static const UnitDef UDEF[UT_COUNT] = {
    /* GRUNT  */ {  60, 34,  95,  6, 0.75f,  30, 7, 0 },
    /* PSYCHO */ {  80, 37,  85,  9, 0.50f,  42, 7, 0 },
    /* TOUGH  */ { 130, 28, 105, 15, 1.00f,  54, 8, 0 },
    /* SNIPER */ {  60, 31, 160, 22, 1.50f,  66, 7, 0 },
    /* JEEP   */ { 110, 62, 105,  8, 0.55f,  42, 9, 1 },
    /* LTANK  */ { 190, 44, 135, 19, 1.20f,  66, 10, 1 },
    /* HTANK  */ { 320, 32, 155, 32, 1.65f,  96, 11, 1 },
};

#define MAX_UNITS      192
#define TEAM_UNIT_CAP  80
#define MAX_PATH       96

typedef struct {
    int   active, team, type;
    float x, y, hp;
    int   path[MAX_PATH];
    int   path_len, path_i;
    int   has_order;          /* moving toward an ordered destination */
    float ox, oy;             /* final ordered destination */
    float reload;
    int   target;             /* unit index, or -1 */
    int   btarget;            /* building sector index, or -1 */
    int   selected;
    float fx, fy;             /* facing dir (normalized) */
} Unit;

static Unit units[MAX_UNITS];

/* ------------------------------------------------------------------ */
/* Buildings & sectors                                                 */
/* ------------------------------------------------------------------ */

enum { BK_NONE, BK_RFACT, BK_VFACT, BK_GUN, BK_FORT };

typedef struct {
    int   owner;              /* TEAM_NONE / player / ai */
    int   bkind;
    float bx, by;             /* building center */
    float bhp, bhp_max;       /* guns & forts only */
    int   prod_type;          /* factories: unit type in production */
    float prod_t;             /* seconds accumulated */
    float gun_reload;
    float flag_x, flag_y;
    int   has_flag;
    int   fort_team;          /* which team's fort (forts only) */
    int   cap_team;           /* team currently raising the flag */
    float cap_t;              /* capture progress in seconds */
} Sector;

#define CAP_TIME   2.5f       /* seconds to raise a flag */
#define CAP_RADIUS 20.0f

static Sector sec[SEC_COUNT];

#define FORT_HP    1500.0f
#define GUN_HP     260.0f
#define GUN_RANGE  175.0f
#define GUN_DMG    20.0f
#define GUN_RELOAD 1.15f
#define FORT_RANGE 185.0f
#define FORT_DMG   34.0f
#define FORT_RELOAD 1.1f
#define BLD_RADIUS 22.0f

#define PLAYER_FORT_SEC 10
#define AI_FORT_SEC     4

/* ------------------------------------------------------------------ */
/* Projectiles & effects                                               */
/* ------------------------------------------------------------------ */

#define MAX_PROJ 256
typedef struct {
    int   active;
    float x, y;
    int   ttype;              /* 0 = unit, 1 = building */
    int   tidx;
    float lx, ly;             /* last known target pos */
    float speed, dmg;
    int   team;
} Proj;
static Proj projs[MAX_PROJ];

#define MAX_FX 128
typedef struct { int active; float x, y, t, max_t, r; int kind; } Fx;
static Fx fx[MAX_FX];

/* ------------------------------------------------------------------ */
/* Game / input state                                                  */
/* ------------------------------------------------------------------ */

static int   g_status;        /* 0 playing, 1 player won, 2 player lost */
static float g_time;
static float ai_think_t;
static int   g_difficulty = 1; /* 0 easy, 1 normal, 2 hard */

typedef struct {
    float think_interval;     /* seconds between AI decision passes */
    float leash_rate;         /* px/s the AI expansion front grows */
    int   max_new_claims;     /* new capture squads per decision pass */
    int   attack_at;          /* army size that triggers fort assaults */
} AiTuning;

static const AiTuning AI_TUNE[3] = {
    { 4.0f, 2.0f, 2, 14 },    /* easy   */
    { 2.5f, 3.0f, 3, 10 },    /* normal */
    { 1.5f, 5.0f, 5,  8 },    /* hard   */
};

static int   drag_active;
static float drag_x0, drag_y0, drag_x1, drag_y1;
static int   sel_factory_sec = -1;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int tile_at(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H) return TER_ROCK;
    return tiles[ty * MAP_W + tx];
}
static int tile_blocked(int tx, int ty) { return tile_at(tx, ty) != TER_GRASS; }

static int sector_of(float x, float y) {
    int sx = (int)(x / SEC_PW), sy = (int)(y / SEC_PH);
    if (sx < 0) sx = 0; if (sx >= SEC_COLS) sx = SEC_COLS - 1;
    if (sy < 0) sy = 0; if (sy >= SEC_ROWS) sy = SEC_ROWS - 1;
    return sy * SEC_COLS + sx;
}

static float dist2(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

static int team_territory(int team) {
    int n = 0;
    for (int i = 0; i < SEC_COUNT; i++) if (sec[i].owner == team) n++;
    return n;
}

static int team_units(int team) {
    int n = 0;
    for (int i = 0; i < MAX_UNITS; i++)
        if (units[i].active && units[i].team == team) n++;
    return n;
}

static void spawn_fx(float x, float y, int kind, float r, float dur) {
    for (int i = 0; i < MAX_FX; i++) {
        if (!fx[i].active) {
            fx[i].active = 1; fx[i].x = x; fx[i].y = y;
            fx[i].t = 0; fx[i].max_t = dur; fx[i].r = r; fx[i].kind = kind;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pathfinding (A* on the tile grid, 8-directional)                    */
/* ------------------------------------------------------------------ */

#define NODES (MAP_W * MAP_H)
static int   pf_g[NODES];
static int   pf_from[NODES];
static u8    pf_state[NODES]; /* 0 unseen, 1 open, 2 closed */
static int   pf_open[NODES];
static int   pf_open_n;

static int pf_heur(int a, int b) {
    int ax = a % MAP_W, ay = a / MAP_W, bx = b % MAP_W, by = b / MAP_W;
    int dx = i_abs(ax - bx), dy = i_abs(ay - by);
    int dmin = dx < dy ? dx : dy, dmax = dx < dy ? dy : dx;
    return dmin * 14 + (dmax - dmin) * 10;
}

/* Find nearest walkable tile to (tx,ty) via small spiral search. */
static int nearest_walkable(int tx, int ty) {
    if (!tile_blocked(tx, ty)) return ty * MAP_W + tx;
    for (int r = 1; r < 6; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (i_abs(dx) != r && i_abs(dy) != r) continue;
                if (!tile_blocked(tx + dx, ty + dy))
                    return (ty + dy) * MAP_W + (tx + dx);
            }
    return -1;
}

static int find_path(float sx, float sy, float gx, float gy, int *out, int max_out) {
    int start = nearest_walkable((int)(sx / TILE), (int)(sy / TILE));
    int goal  = nearest_walkable((int)(gx / TILE), (int)(gy / TILE));
    if (start < 0 || goal < 0) return 0;
    if (start == goal) { out[0] = goal; return 1; }

    memset(pf_state, 0, sizeof(pf_state));
    pf_open_n = 0;
    pf_g[start] = 0;
    pf_from[start] = -1;
    pf_open[pf_open_n++] = start;
    pf_state[start] = 1;

    static const int NX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int NY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    int found = -1, iter = 0;
    while (pf_open_n > 0 && iter++ < 4000) {
        /* pick lowest f from open list (linear scan; grid is small) */
        int best_i = 0, best_f = pf_g[pf_open[0]] + pf_heur(pf_open[0], goal);
        for (int i = 1; i < pf_open_n; i++) {
            int f = pf_g[pf_open[i]] + pf_heur(pf_open[i], goal);
            if (f < best_f) { best_f = f; best_i = i; }
        }
        int cur = pf_open[best_i];
        pf_open[best_i] = pf_open[--pf_open_n];
        pf_state[cur] = 2;
        if (cur == goal) { found = cur; break; }

        int cx = cur % MAP_W, cy = cur / MAP_W;
        for (int d = 0; d < 8; d++) {
            int nx = cx + NX[d], ny = cy + NY[d];
            if (tile_blocked(nx, ny)) continue;
            if (d >= 4 && (tile_blocked(cx + NX[d], cy) || tile_blocked(cx, cy + NY[d])))
                continue; /* no corner cutting */
            int n = ny * MAP_W + nx;
            if (pf_state[n] == 2) continue;
            int cost = pf_g[cur] + (d < 4 ? 10 : 14);
            if (pf_state[n] == 1 && cost >= pf_g[n]) continue;
            pf_g[n] = cost;
            pf_from[n] = cur;
            if (pf_state[n] != 1) { pf_open[pf_open_n++] = n; pf_state[n] = 1; }
        }
    }
    if (found < 0) return 0;

    /* walk back, then reverse into out[] */
    int rev[MAX_PATH * 2];
    int n = 0, c = found;
    while (c >= 0 && n < MAX_PATH * 2) { rev[n++] = c; c = pf_from[c]; }
    int len = n < max_out ? n : max_out;
    for (int i = 0; i < len; i++) out[i] = rev[n - 1 - i];
    return len;
}

/* ------------------------------------------------------------------ */
/* Orders                                                              */
/* ------------------------------------------------------------------ */

static void order_move(int ui, float gx, float gy) {
    Unit *u = &units[ui];
    if (gx < 8) gx = 8; if (gx > WORLD_W - 8) gx = WORLD_W - 8;
    if (gy < 8) gy = 8; if (gy > WORLD_H - 8) gy = WORLD_H - 8;
    u->path_len = find_path(u->x, u->y, gx, gy, u->path, MAX_PATH);
    u->path_i = 0;
    u->has_order = u->path_len > 0;
    u->ox = gx; u->oy = gy;
    u->target = -1; u->btarget = -1;
}

static int spawn_unit(int team, int type, float x, float y) {
    if (team_units(team) >= TEAM_UNIT_CAP) return -1;
    for (int i = 0; i < MAX_UNITS; i++) {
        if (!units[i].active) {
            Unit *u = &units[i];
            memset(u, 0, sizeof(*u));
            u->active = 1; u->team = team; u->type = type;
            u->x = x; u->y = y;
            u->hp = UDEF[type].hp;
            u->target = -1; u->btarget = -1;
            u->fx = team == TEAM_PLAYER ? 1.0f : -1.0f; u->fy = 0;
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* World setup                                                         */
/* ------------------------------------------------------------------ */

static void setup_sector(int s, int bkind, int fort_team) {
    int scx = s % SEC_COLS, scy = s / SEC_COLS;
    float cx = scx * SEC_PW + SEC_PW * 0.5f;
    float cy = scy * SEC_PH + SEC_PH * 0.5f;
    Sector *S = &sec[s];
    S->bkind = bkind;
    S->owner = TEAM_NONE;
    S->fort_team = fort_team;
    S->prod_type = bkind == BK_VFACT ? UT_JEEP : UT_GRUNT;
    S->prod_t = 0;
    S->gun_reload = 0;
    S->cap_team = -1;
    S->cap_t = 0;
    if (bkind == BK_NONE) {
        S->flag_x = cx; S->flag_y = cy; S->has_flag = 1;
    } else {
        S->bx = cx - 20; S->by = cy;
        S->flag_x = cx + 42; S->flag_y = cy;
        S->has_flag = (bkind != BK_FORT);
        if (bkind == BK_GUN)  { S->bhp = S->bhp_max = GUN_HP; }
        if (bkind == BK_FORT) { S->bhp = S->bhp_max = FORT_HP; }
    }
}

static void clear_tiles_near(float x, float y, int r) {
    int tx = (int)(x / TILE), ty = (int)(y / TILE);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int nx = tx + dx, ny = ty + dy;
            if (nx >= 0 && ny >= 0 && nx < MAP_W && ny < MAP_H)
                tiles[ny * MAP_W + nx] = TER_GRASS;
        }
}

WASM_EXPORT("init")
void game_init(u32 seed) {
    rng_state = seed ? seed : 0x2f6e2b1u;
    memset(units, 0, sizeof(units));
    memset(projs, 0, sizeof(projs));
    memset(fx, 0, sizeof(fx));
    memset(sec, 0, sizeof(sec));
    g_status = 0; g_time = 0; ai_think_t = 0;
    drag_active = 0; sel_factory_sec = -1;

    /* terrain: sparse rock clusters, kept away from sector centers */
    memset(tiles, TER_GRASS, sizeof(tiles));
    for (int i = 0; i < 26; i++) {
        int tx = 2 + rng_range(MAP_W - 4), ty = 2 + rng_range(MAP_H - 4);
        /* keep the middle band of each sector clear so flags stay reachable */
        int mx = tx % SEC_TW, my = ty % SEC_TH;
        if (mx >= 2 && mx <= 5 && my >= 2 && my <= 5) continue;
        tiles[ty * MAP_W + tx] = TER_ROCK;
        if (rng_range(2)) tiles[ty * MAP_W + tx + 1] = TER_ROCK;
        if (rng_range(2) && ty + 1 < MAP_H) tiles[(ty + 1) * MAP_W + tx] = TER_ROCK;
    }

    /* sector layout — symmetric under 180-degree rotation (i <-> 14-i)
       row 0:  GUN   .     VFACT RFACT FORT(ai)
       row 1:  RFACT VFACT GUN   VFACT RFACT
       row 2:  FORT(pl) RFACT VFACT .   GUN            */
    setup_sector(0,  BK_GUN,   -1);
    setup_sector(1,  BK_NONE,  -1);
    setup_sector(2,  BK_VFACT, -1);
    setup_sector(3,  BK_RFACT, -1);
    setup_sector(4,  BK_FORT,  TEAM_AI);
    setup_sector(5,  BK_RFACT, -1);
    setup_sector(6,  BK_VFACT, -1);
    setup_sector(7,  BK_GUN,   -1);
    setup_sector(8,  BK_VFACT, -1);
    setup_sector(9,  BK_RFACT, -1);
    setup_sector(10, BK_FORT,  TEAM_PLAYER);
    setup_sector(11, BK_RFACT, -1);
    setup_sector(12, BK_VFACT, -1);
    setup_sector(13, BK_NONE,  -1);
    setup_sector(14, BK_GUN,   -1);

    /* starting ownership */
    sec[10].owner = TEAM_PLAYER; sec[11].owner = TEAM_PLAYER; sec[5].owner = TEAM_PLAYER;
    sec[4].owner  = TEAM_AI;     sec[3].owner  = TEAM_AI;     sec[9].owner = TEAM_AI;

    /* keep building/flag surroundings walkable */
    for (int s = 0; s < SEC_COUNT; s++) {
        if (sec[s].bkind != BK_NONE) clear_tiles_near(sec[s].bx, sec[s].by, 2);
        clear_tiles_near(sec[s].flag_x, sec[s].flag_y, 2);
    }
    /* ...but the building footprint itself is solid */
    for (int s = 0; s < SEC_COUNT; s++) {
        if (sec[s].bkind == BK_NONE) continue;
        for (int ty = 0; ty < MAP_H; ty++)
            for (int tx = 0; tx < MAP_W; tx++) {
                float px2 = tx * TILE + TILE * 0.5f, py2 = ty * TILE + TILE * 0.5f;
                if (f_abs(px2 - sec[s].bx) < 21 && f_abs(py2 - sec[s].by) < 21)
                    tiles[ty * MAP_W + tx] = TER_BLDG;
            }
    }

    /* starting squads */
    float px = sec[10].bx, py = sec[10].by;
    float ax = sec[4].bx,  ay = sec[4].by;
    for (int i = 0; i < 3; i++) {
        spawn_unit(TEAM_PLAYER, UT_GRUNT, px + 50, py - 30 + i * 30);
        spawn_unit(TEAM_AI,     UT_GRUNT, ax - 50, ay - 30 + i * 30);
    }
    spawn_unit(TEAM_PLAYER, UT_PSYCHO, px + 80, py);
    spawn_unit(TEAM_AI,     UT_PSYCHO, ax - 80, ay);
}

/* ------------------------------------------------------------------ */
/* Combat helpers                                                      */
/* ------------------------------------------------------------------ */

static int find_enemy_unit(int team, float x, float y, float range) {
    int best = -1; float best_d = range * range;
    for (int i = 0; i < MAX_UNITS; i++) {
        Unit *e = &units[i];
        if (!e->active || e->team == team) continue;
        float d = dist2(x, y, e->x, e->y);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* attackable building = enemy gun (via sector owner) or enemy fort, hp > 0 */
static int building_team(int s) {
    if (sec[s].bkind == BK_FORT) return sec[s].fort_team;
    if (sec[s].bkind == BK_GUN)  return sec[s].owner;
    return TEAM_NONE;
}

static int find_enemy_building(int team, float x, float y, float range) {
    int best = -1; float best_d = 1e18f;
    for (int s = 0; s < SEC_COUNT; s++) {
        int bk = sec[s].bkind;
        if (bk != BK_GUN && bk != BK_FORT) continue;
        if (sec[s].bhp <= 0) continue;
        int bt = building_team(s);
        if (bt == TEAM_NONE || bt == team) continue;
        float d = dist2(x, y, sec[s].bx, sec[s].by);
        float r = range + BLD_RADIUS;
        if (d < r * r && d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void fire_proj(int team, float x, float y, int ttype, int tidx, float dmg, float speed) {
    for (int i = 0; i < MAX_PROJ; i++) {
        if (!projs[i].active) {
            Proj *p = &projs[i];
            p->active = 1; p->x = x; p->y = y;
            p->ttype = ttype; p->tidx = tidx;
            p->dmg = dmg; p->speed = speed; p->team = team;
            if (ttype == 0) { p->lx = units[tidx].x; p->ly = units[tidx].y; }
            else            { p->lx = sec[tidx].bx;  p->ly = sec[tidx].by; }
            return;
        }
    }
}

static void damage_building(int s, float dmg) {
    if (sec[s].bhp <= 0) return;
    sec[s].bhp -= dmg;
    if (sec[s].bhp <= 0) {
        sec[s].bhp = 0;
        spawn_fx(sec[s].bx, sec[s].by, 1, 34, 0.7f);
        if (sec[s].bkind == BK_FORT)
            g_status = (sec[s].fort_team == TEAM_AI) ? 1 : 2;
    }
}

/* ------------------------------------------------------------------ */
/* Simulation                                                          */
/* ------------------------------------------------------------------ */

static float prod_speed(int team) {
    float frac = (float)team_territory(team) / (float)SEC_COUNT;
    return 0.6f + 1.8f * frac;   /* territory makes everything faster */
}

static void tick_factories(float dt) {
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        if (S->bkind != BK_RFACT && S->bkind != BK_VFACT) continue;
        if (S->owner == TEAM_NONE) continue;
        if (team_units(S->owner) >= TEAM_UNIT_CAP) continue;
        S->prod_t += dt * prod_speed(S->owner);
        float need = UDEF[S->prod_type].build_time;
        if (S->prod_t >= need) {
            S->prod_t = 0;
            float dx = (S->owner == TEAM_PLAYER) ? 30.0f : -30.0f;
            int ui = spawn_unit(S->owner, S->prod_type,
                                S->bx + dx, S->by + 34 + rng_f() * 10);
            if (ui >= 0 && S->owner == TEAM_AI) {
                /* fresh AI units get picked up by the next think pass */
            }
            spawn_fx(S->bx, S->by + 30, 2, 14, 0.4f);
        }
    }
}

static void tick_guns(float dt) {
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        int bk = S->bkind;
        if (bk != BK_GUN && bk != BK_FORT) continue;
        if (S->bhp <= 0) continue;
        int team = building_team(s);
        if (team == TEAM_NONE) continue;
        S->gun_reload -= dt;
        if (S->gun_reload > 0) continue;
        float range = bk == BK_GUN ? GUN_RANGE : FORT_RANGE;
        int t = find_enemy_unit(team, S->bx, S->by, range);
        if (t >= 0) {
            fire_proj(team, S->bx, S->by - 6, 0, t,
                      bk == BK_GUN ? GUN_DMG : FORT_DMG, 300);
            S->gun_reload = bk == BK_GUN ? GUN_RELOAD : FORT_RELOAD;
        }
    }
}

static void tick_projs(float dt) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &projs[i];
        if (!p->active) continue;
        /* refresh target position while target lives */
        if (p->ttype == 0) {
            Unit *t = &units[p->tidx];
            if (t->active) { p->lx = t->x; p->ly = t->y; }
        }
        float dx = p->lx - p->x, dy = p->ly - p->y;
        float d = f_sqrt(dx * dx + dy * dy);
        float step = p->speed * dt;
        if (d <= step + 6.0f) {
            /* impact */
            if (p->ttype == 0) {
                Unit *t = &units[p->tidx];
                if (t->active && dist2(t->x, t->y, p->lx, p->ly) < 400) {
                    t->hp -= p->dmg;
                    spawn_fx(t->x, t->y, 0, 8, 0.25f);
                    if (t->hp <= 0) {
                        t->active = 0;
                        spawn_fx(t->x, t->y, 1, 16, 0.5f);
                    }
                }
            } else {
                damage_building(p->tidx, p->dmg);
                spawn_fx(p->lx, p->ly, 0, 10, 0.3f);
            }
            p->active = 0;
            continue;
        }
        p->x += dx / d * step;
        p->y += dy / d * step;
    }
}

static void tick_fx(float dt) {
    for (int i = 0; i < MAX_FX; i++) {
        if (!fx[i].active) continue;
        fx[i].t += dt;
        if (fx[i].t >= fx[i].max_t) fx[i].active = 0;
    }
}

static void tick_units(float dt) {
    for (int i = 0; i < MAX_UNITS; i++) {
        Unit *u = &units[i];
        if (!u->active) continue;
        const UnitDef *D = &UDEF[u->type];

        u->reload -= dt;

        /* --- target acquisition (units in Z fight autonomously) --- */
        if (u->target >= 0 &&
            (!units[u->target].active ||
             dist2(u->x, u->y, units[u->target].x, units[u->target].y) >
                 (D->range + 30) * (D->range + 30)))
            u->target = -1;
        if (u->target < 0)
            u->target = find_enemy_unit(u->team, u->x, u->y, D->range);
        if (u->target < 0 && u->btarget < 0 && !u->has_order)
            u->btarget = find_enemy_building(u->team, u->x, u->y, D->range);
        if (u->btarget >= 0 &&
            (sec[u->btarget].bhp <= 0 ||
             building_team(u->btarget) == u->team ||
             building_team(u->btarget) == TEAM_NONE))
            u->btarget = -1;

        /* --- shooting --- */
        if (u->reload <= 0) {
            if (u->target >= 0) {
                Unit *t = &units[u->target];
                if (dist2(u->x, u->y, t->x, t->y) <= D->range * D->range) {
                    fire_proj(u->team, u->x, u->y, 0, u->target, D->dmg, 340);
                    u->reload = D->reload;
                    float dx = t->x - u->x, dy = t->y - u->y;
                    float d = f_sqrt(dx * dx + dy * dy);
                    if (d > 1) { u->fx = dx / d; u->fy = dy / d; }
                }
            } else if (u->btarget >= 0) {
                Sector *S = &sec[u->btarget];
                float r = D->range + BLD_RADIUS;
                if (dist2(u->x, u->y, S->bx, S->by) <= r * r) {
                    fire_proj(u->team, u->x, u->y, 1, u->btarget, D->dmg, 340);
                    u->reload = D->reload;
                }
            }
        }

        /* --- idle behaviour: close on nearby enemies --- */
        if (!u->has_order && u->target < 0 && u->btarget < 0) {
            int e = find_enemy_unit(u->team, u->x, u->y, D->range * 1.8f);
            if (e >= 0) order_move(i, units[e].x, units[e].y);
        }
        /* idle units drift toward buildings they should besiege */
        if (!u->has_order && u->target < 0 && u->btarget >= 0) {
            Sector *S = &sec[u->btarget];
            float r = D->range + BLD_RADIUS - 10;
            if (dist2(u->x, u->y, S->bx, S->by) > r * r)
                order_move(i, S->bx, S->by);
        }

        /* --- movement along path --- */
        if (u->has_order && u->path_i < u->path_len) {
            int node = u->path[u->path_i];
            float wx = (node % MAP_W) * TILE + TILE * 0.5f;
            float wy = (node / MAP_W) * TILE + TILE * 0.5f;
            /* last waypoint: head to the exact ordered spot */
            if (u->path_i == u->path_len - 1) { wx = u->ox; wy = u->oy; }
            float dx = wx - u->x, dy = wy - u->y;
            float d = f_sqrt(dx * dx + dy * dy);
            float step = D->speed * dt;
            /* units keep firing on the move but slow down slightly */
            if (u->target >= 0) step *= 0.7f;
            if (d <= step + 2.0f) {
                u->x = wx; u->y = wy;
                u->path_i++;
                if (u->path_i >= u->path_len) u->has_order = 0;
            } else {
                u->x += dx / d * step;
                u->y += dy / d * step;
                u->fx = dx / d; u->fy = dy / d;
            }
        }

    }

    /* --- flag capture: hold the flag to raise it --- */
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        if (!S->has_flag) continue;
        int present[2] = { 0, 0 };
        for (int i = 0; i < MAX_UNITS; i++) {
            Unit *u = &units[i];
            if (!u->active) continue;
            if (dist2(u->x, u->y, S->flag_x, S->flag_y) < CAP_RADIUS * CAP_RADIUS)
                present[u->team] = 1;
        }
        int challenger = -1;
        if (present[TEAM_PLAYER] && S->owner != TEAM_PLAYER) challenger = TEAM_PLAYER;
        if (present[TEAM_AI] && S->owner != TEAM_AI)         challenger = TEAM_AI;
        if (present[TEAM_PLAYER] && present[TEAM_AI])        challenger = -2; /* contested */

        if (challenger >= 0) {
            if (S->cap_team != challenger) { S->cap_team = challenger; S->cap_t = 0; }
            S->cap_t += dt;
            if (S->cap_t >= CAP_TIME) {
                S->owner = challenger;
                S->cap_team = -1; S->cap_t = 0;
                spawn_fx(S->flag_x, S->flag_y, 3, 26, 0.6f);
            }
        } else if (challenger == -1) {
            S->cap_team = -1; S->cap_t = 0;   /* nobody raising: reset */
        }
        /* contested (-2): progress freezes */
    }

    /* --- soft separation between friendly units --- */
    for (int i = 0; i < MAX_UNITS; i++) {
        Unit *a = &units[i];
        if (!a->active) continue;
        for (int j = i + 1; j < MAX_UNITS; j++) {
            Unit *b = &units[j];
            if (!b->active) continue;
            float min_d = UDEF[a->type].radius + UDEF[b->type].radius;
            float d2 = dist2(a->x, a->y, b->x, b->y);
            if (d2 < min_d * min_d && d2 > 0.01f) {
                float d = f_sqrt(d2);
                float push = (min_d - d) * 0.5f;
                float nx = (a->x - b->x) / d, ny = (a->y - b->y) / d;
                a->x += nx * push; a->y += ny * push;
                b->x -= nx * push; b->y -= ny * push;
            }
        }
        a->x = f_max(6, f_min(WORLD_W - 6, a->x));
        a->y = f_max(6, f_min(WORLD_H - 6, a->y));
    }
}

/* ------------------------------------------------------------------ */
/* AI opponent                                                         */
/* ------------------------------------------------------------------ */

static void ai_think(void) {
    const AiTuning *T = &AI_TUNE[g_difficulty];
    int my_units = team_units(TEAM_AI);
    int claimed[SEC_COUNT];
    int new_claims = 0;
    memset(claimed, 0, sizeof(claimed));

    /* factories choose what to build (weighted; heavier when rich) */
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        if (S->owner != TEAM_AI) continue;
        if (S->bkind == BK_RFACT && S->prod_t == 0) {
            int r = rng_range(100);
            S->prod_type = r < 40 ? UT_GRUNT : r < 65 ? UT_PSYCHO
                         : r < 85 ? UT_TOUGH : UT_SNIPER;
        } else if (S->bkind == BK_VFACT && S->prod_t == 0) {
            int r = rng_range(100);
            S->prod_type = r < 40 ? UT_JEEP : r < 75 ? UT_LTANK : UT_HTANK;
        }
    }

    int attack_mode = my_units >= T->attack_at;
    Sector *pf = &sec[PLAYER_FORT_SEC];

    for (int i = 0; i < MAX_UNITS; i++) {
        Unit *u = &units[i];
        if (!u->active || u->team != TEAM_AI) continue;
        if (u->has_order || u->target >= 0 || u->btarget >= 0) continue;

        if (attack_mode && rng_range(100) < 55) {
            order_move(i, pf->bx + (rng_f() - 0.5f) * 120,
                          pf->by + (rng_f() - 0.5f) * 120);
            continue;
        }
        /* find nearest unowned flag not yet claimed this pass;
           the AI expands as a front — its reach grows over time so it
           doesn't sprint across the whole map in the opening seconds */
        float leash = 280.0f + g_time * T->leash_rate;
        int best = -1; float best_d = leash * leash;
        if (new_claims < T->max_new_claims) {
            for (int s = 0; s < SEC_COUNT; s++) {
                if (!sec[s].has_flag || sec[s].owner == TEAM_AI) continue;
                if (claimed[s] >= 2) continue;
                float d = dist2(u->x, u->y, sec[s].flag_x, sec[s].flag_y);
                if (d < best_d) { best_d = d; best = s; }
            }
        }
        if (best >= 0) {
            new_claims++;
            claimed[best]++;
            order_move(i, sec[best].flag_x, sec[best].flag_y);
        } else if (attack_mode) {
            order_move(i, pf->bx, pf->by);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main tick                                                           */
/* ------------------------------------------------------------------ */

WASM_EXPORT("tick")
void game_tick(float dt_ms) {
    if (g_status != 0) return;
    float dt = dt_ms / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_time += dt;

    tick_units(dt);
    tick_factories(dt);
    tick_guns(dt);
    tick_projs(dt);
    tick_fx(dt);

    ai_think_t -= dt;
    if (ai_think_t <= 0) { ai_think(); ai_think_t = AI_TUNE[g_difficulty].think_interval; }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void clear_selection(void) {
    for (int i = 0; i < MAX_UNITS; i++) units[i].selected = 0;
    sel_factory_sec = -1;
}

static int selected_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_UNITS; i++)
        if (units[i].active && units[i].selected) n++;
    return n;
}

WASM_EXPORT("pointer_down")
void pointer_down(float x, float y, int button) {
    if (g_status != 0) return;
    if (button == 0) {
        drag_active = 1;
        drag_x0 = drag_x1 = x;
        drag_y0 = drag_y1 = y;
    } else if (button == 2) {
        /* right click: move order for selection, fanned out around target */
        int n = selected_count();
        if (n == 0) return;
        int k = 0;
        for (int i = 0; i < MAX_UNITS; i++) {
            Unit *u = &units[i];
            if (!u->active || !u->selected) continue;
            float ang_x = ((k % 3) - 1) * 26.0f;
            float ang_y = ((k / 3) % 3 - 1) * 26.0f;
            order_move(i, x + (n > 1 ? ang_x : 0), y + (n > 1 ? ang_y : 0));
            k++;
        }
    }
}

WASM_EXPORT("pointer_move")
void pointer_move(float x, float y) {
    if (drag_active) { drag_x1 = x; drag_y1 = y; }
}

WASM_EXPORT("pointer_up")
void pointer_up(float x, float y, int button) {
    if (button != 0 || !drag_active) return;
    drag_active = 0;
    float x0 = f_min(drag_x0, x), x1 = f_max(drag_x0, x);
    float y0 = f_min(drag_y0, y), y1 = f_max(drag_y0, y);

    if (x1 - x0 < 5 && y1 - y0 < 5) {
        /* click: unit first, then factory */
        int hit = -1; float best = 18 * 18;
        for (int i = 0; i < MAX_UNITS; i++) {
            Unit *u = &units[i];
            if (!u->active || u->team != TEAM_PLAYER) continue;
            float d = dist2(u->x, u->y, x, y);
            if (d < best) { best = d; hit = i; }
        }
        if (hit >= 0) {
            clear_selection();
            units[hit].selected = 1;
            return;
        }
        int s = sector_of(x, y);
        Sector *S = &sec[s];
        if ((S->bkind == BK_RFACT || S->bkind == BK_VFACT) &&
            S->owner == TEAM_PLAYER &&
            f_abs(x - S->bx) < 30 && f_abs(y - S->by) < 30) {
            if (sel_factory_sec == s) {
                /* clicking a selected factory cycles its production */
                if (S->bkind == BK_RFACT)
                    S->prod_type = (S->prod_type + 1) % 4; /* grunt..sniper */
                else {
                    S->prod_type = S->prod_type + 1;
                    if (S->prod_type > UT_HTANK || S->prod_type < UT_JEEP)
                        S->prod_type = UT_JEEP;
                }
                S->prod_t = 0;
            } else {
                clear_selection();
                sel_factory_sec = s;
            }
            return;
        }
        clear_selection();
    } else {
        /* box select */
        clear_selection();
        for (int i = 0; i < MAX_UNITS; i++) {
            Unit *u = &units[i];
            if (!u->active || u->team != TEAM_PLAYER) continue;
            if (u->x >= x0 && u->x <= x1 && u->y >= y0 && u->y <= y1)
                u->selected = 1;
        }
    }
}

WASM_EXPORT("set_difficulty")
void set_difficulty(int d) {
    if (d < 0) d = 0; if (d > 2) d = 2;
    g_difficulty = d;
}

WASM_EXPORT("key_press")
void key_press(int code) {
    if (code == 'S' || code == 's') { /* stop */
        for (int i = 0; i < MAX_UNITS; i++)
            if (units[i].active && units[i].selected) {
                units[i].has_order = 0;
                units[i].path_len = 0;
            }
    }
}

/* ------------------------------------------------------------------ */
/* HUD getters                                                         */
/* ------------------------------------------------------------------ */

WASM_EXPORT("game_status")   int  q_status(void)          { return g_status; }
WASM_EXPORT("territory_pct") int  q_territory(int team)   { return team_territory(team) * 100 / SEC_COUNT; }
WASM_EXPORT("fort_hp_pct")   int  q_fort_hp(int team) {
    int s = team == TEAM_PLAYER ? PLAYER_FORT_SEC : AI_FORT_SEC;
    return (int)(sec[s].bhp * 100.0f / sec[s].bhp_max);
}
WASM_EXPORT("unit_count")    int  q_units(int team)       { return team_units(team); }
WASM_EXPORT("sel_count")     int  q_sel(void)             { return selected_count(); }
WASM_EXPORT("sel_factory")   int  q_sel_factory(void)     { return sel_factory_sec; }
WASM_EXPORT("factory_prod")  int  q_factory_prod(int s)   { return sec[s].prod_type; }
WASM_EXPORT("factory_pct")   int  q_factory_pct(int s) {
    if (s < 0 || s >= SEC_COUNT) return 0;
    return (int)(sec[s].prod_t * 100.0f / UDEF[sec[s].prod_type].build_time);
}
WASM_EXPORT("map_ptr")       u8  *q_map(void)             { return tiles; }
WASM_EXPORT("map_w")         int  q_map_w(void)           { return MAP_W; }
WASM_EXPORT("map_h")         int  q_map_h(void)           { return MAP_H; }
WASM_EXPORT("tile_size")     int  q_tile(void)            { return TILE; }

/* ------------------------------------------------------------------ */
/* Render command buffer                                               */
/*                                                                     */
/* Each command is 8 floats: [type, a, b, c, d, e, f, g]               */
/*   0 RECT    x, y, w, h, color, alpha                                */
/*   1 CIRCLE  x, y, r, color, alpha                                   */
/*   2 LINE    x1, y1, x2, y2, color, width                            */
/*   3 FLAG    x, y, color                                             */
/*   4 HPBAR   x, y, w, frac                                           */
/*   5 RING    x, y, r, color                                          */
/*   6 TEXT    x, y, code, color, size                                 */
/* ------------------------------------------------------------------ */

#define MAX_CMDS 4096
static float cmds[MAX_CMDS * 8];
static int   n_cmds;

static void cmd(float t, float a, float b, float c, float d, float e, float f, float g) {
    if (n_cmds >= MAX_CMDS) return;
    float *p = &cmds[n_cmds * 8];
    p[0] = t; p[1] = a; p[2] = b; p[3] = c; p[4] = d; p[5] = e; p[6] = f; p[7] = g;
    n_cmds++;
}

/* colors: 0 blue, 1 red, 2 neutral gray, 3 white, 4 yellow, 5 dark,
   6 blue-tint, 7 red-tint, 8 green, 9 orange */

static int team_color(int team) {
    return team == TEAM_PLAYER ? 0 : team == TEAM_AI ? 1 : 2;
}

/* text codes understood by the JS host */
enum { TXT_RFACT, TXT_VFACT, TXT_GUN, TXT_FORT,
       TXT_G, TXT_P, TXT_T, TXT_S, TXT_J, TXT_L, TXT_H };

static int prod_letter(int t) {
    switch (t) {
        case UT_GRUNT:  return TXT_G;
        case UT_PSYCHO: return TXT_P;
        case UT_TOUGH:  return TXT_T;
        case UT_SNIPER: return TXT_S;
        case UT_JEEP:   return TXT_J;
        case UT_LTANK:  return TXT_L;
        default:        return TXT_H;
    }
}

WASM_EXPORT("render")
int game_render(void) {
    n_cmds = 0;

    /* sector ownership tints + flags */
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        float sx = (s % SEC_COLS) * SEC_PW, sy = (s / SEC_COLS) * SEC_PH;
        if (S->owner != TEAM_NONE)
            cmd(0, sx, sy, SEC_PW, SEC_PH, S->owner == TEAM_PLAYER ? 6 : 7, 1, 0);
        if (S->has_flag) {
            cmd(3, S->flag_x, S->flag_y, team_color(S->owner), 0, 0, 0, 0);
            if (S->cap_team >= 0 && S->cap_t > 0)
                cmd(4, S->flag_x - 12, S->flag_y + 14, 24, S->cap_t / CAP_TIME, 0, 0, 0);
        }
    }

    /* buildings */
    for (int s = 0; s < SEC_COUNT; s++) {
        Sector *S = &sec[s];
        if (S->bkind == BK_NONE) continue;
        int col = S->bkind == BK_FORT ? team_color(S->fort_team)
                                      : team_color(S->owner);
        float bw = S->bkind == BK_FORT ? 52 : 44;
        int destroyed = (S->bkind == BK_GUN || S->bkind == BK_FORT) && S->bhp <= 0;
        cmd(0, S->bx - bw / 2, S->by - bw / 2, bw, bw, destroyed ? 5 : col, 1, 0);
        cmd(0, S->bx - bw / 2 + 5, S->by - bw / 2 + 5, bw - 10, bw - 10, 5, 1, 0);
        switch (S->bkind) {
            case BK_RFACT: cmd(6, S->bx, S->by - bw / 2 - 8, TXT_RFACT, 3, 10, 0, 0); break;
            case BK_VFACT: cmd(6, S->bx, S->by - bw / 2 - 8, TXT_VFACT, 3, 10, 0, 0); break;
            case BK_GUN:   cmd(6, S->bx, S->by - bw / 2 - 8, TXT_GUN,   3, 10, 0, 0); break;
            case BK_FORT:  cmd(6, S->bx, S->by - bw / 2 - 8, TXT_FORT,  4, 11, 0, 0); break;
        }
        if (S->bkind == BK_GUN && !destroyed) {
            /* barrel toward nearest enemy-ish direction: draw simple turret */
            cmd(1, S->bx, S->by, 9, col, 1, 0, 0);
            cmd(2, S->bx, S->by, S->bx, S->by - 16, 5, 3, 0);
        }
        if ((S->bkind == BK_GUN || S->bkind == BK_FORT) && S->bhp > 0 && S->bhp < S->bhp_max)
            cmd(4, S->bx - 24, S->by - bw / 2 - 16, 48, S->bhp / S->bhp_max, 0, 0, 0);

        /* production status */
        if ((S->bkind == BK_RFACT || S->bkind == BK_VFACT) && S->owner != TEAM_NONE) {
            float frac = S->prod_t / UDEF[S->prod_type].build_time;
            cmd(4, S->bx - 20, S->by + bw / 2 + 6, 40, frac, 0, 0, 0);
            cmd(6, S->bx + 30, S->by + bw / 2 + 9, prod_letter(S->prod_type),
                team_color(S->owner), 11, 0, 0);
        }
        if (s == sel_factory_sec)
            cmd(5, S->bx, S->by, bw / 2 + 8, 4, 0, 0, 0);
    }

    /* units */
    for (int i = 0; i < MAX_UNITS; i++) {
        Unit *u = &units[i];
        if (!u->active) continue;
        const UnitDef *D = &UDEF[u->type];
        int col = team_color(u->team);
        if (u->selected)
            cmd(5, u->x, u->y, D->radius + 4, 8, 0, 0, 0);
        if (D->is_vehicle) {
            cmd(0, u->x - D->radius, u->y - D->radius * 0.75f,
                D->radius * 2, D->radius * 1.5f, col, 1, 0);
            cmd(2, u->x, u->y, u->x + u->fx * (D->radius + 6),
                u->y + u->fy * (D->radius + 6), 5, 3, 0);
        } else {
            cmd(1, u->x, u->y, D->radius, col, 1, 0, 0);
            cmd(2, u->x, u->y, u->x + u->fx * (D->radius + 4),
                u->y + u->fy * (D->radius + 4), 3, 2, 0);
        }
        /* type marker: inner dot shade per type */
        cmd(1, u->x, u->y, 2.5f, 3, 1, 0, 0);
        if (u->hp < D->hp || u->selected)
            cmd(4, u->x - 10, u->y - D->radius - 7, 20, u->hp / D->hp, 0, 0, 0);
    }

    /* projectiles */
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &projs[i];
        if (!p->active) continue;
        cmd(1, p->x, p->y, 2.2f, p->team == TEAM_PLAYER ? 4 : 9, 1, 0, 0);
    }

    /* effects */
    for (int i = 0; i < MAX_FX; i++) {
        Fx *e = &fx[i];
        if (!e->active) continue;
        float k = e->t / e->max_t;
        if (e->kind == 3) cmd(5, e->x, e->y, e->r * k, 4, 0, 0, 0);
        else cmd(1, e->x, e->y, e->r * (0.3f + 0.7f * k),
                 e->kind == 1 ? 9 : 4, 1.0f - k, 0, 0);
    }

    /* drag box */
    if (drag_active &&
        (f_abs(drag_x1 - drag_x0) > 4 || f_abs(drag_y1 - drag_y0) > 4)) {
        float x0 = f_min(drag_x0, drag_x1), x1 = f_max(drag_x0, drag_x1);
        float y0 = f_min(drag_y0, drag_y1), y1 = f_max(drag_y0, drag_y1);
        cmd(0, x0, y0, x1 - x0, y1 - y0, 8, 0.15f, 0);
        cmd(2, x0, y0, x1, y0, 8, 1, 0);
        cmd(2, x1, y0, x1, y1, 8, 1, 0);
        cmd(2, x1, y1, x0, y1, 8, 1, 0);
        cmd(2, x0, y1, x0, y0, 8, 1, 0);
    }

    return n_cmds;
}

WASM_EXPORT("draw_buf")
float *q_draw_buf(void) { return cmds; }
