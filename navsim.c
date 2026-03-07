/*
 * NAVSIM - Cold War Naval Tactical Engagement Simulator
 * ======================================================
 * A Monte Carlo-driven naval warfare simulator modeled after
 * 1960s-era mainframe wargaming programs. Simulates task force
 * engagements between NATO and Warsaw Pact naval groups with
 * realistic detection, tracking, and weapons employment.
 *
 * Outputs engagement logs and a CSV battle record for analysis
 * by the companion Python post-processor (navsim_analysis.py).
 *
 * (C) 2026 - PUBLIC DOMAIN / GPL-3.0 Compatible
 * In the spirit of the UNIVAC naval fire control systems
 */

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Windows Console Setup ───────────────────────────────── */
static void setup_console(void) {
#ifdef _WIN32
    /* Set console output to UTF-8 */
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    /* Enable ANSI/VT100 escape sequences on stdout */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }

    /* Also enable on stderr just in case */
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hErr, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hErr, mode);
        }
    }
#endif
    /* On Linux/macOS, UTF-8 and ANSI just work */
}

/* ── Configuration ───────────────────────────────────────── */
#define MAX_SHIPS        16
#define MAX_WEAPONS       6
#define MAX_NAME         32
#define MAX_TICK       1200   /* 20 minutes at 1-second ticks */
#define GRID_SIZE       200   /* nautical miles */
#define LOG_FILE    "battle_log.csv"

/* ── RNG (xoshiro128** for reproducibility) ──────────────── */
static uint32_t rng_state[4];

static inline uint32_t rotl(const uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

static uint32_t xoshiro128ss(void) {
    const uint32_t result = rotl(rng_state[1] * 5, 7) * 9;
    const uint32_t t = rng_state[1] << 9;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 11);
    return result;
}

static void rng_seed(uint32_t seed) {
    /* SplitMix32 to initialize xoshiro state */
    for (int i = 0; i < 4; i++) {
        seed += 0x9e3779b9;
        uint32_t z = seed;
        z = (z ^ (z >> 16)) * 0x85ebca6b;
        z = (z ^ (z >> 13)) * 0xc2b2ae35;
        z = z ^ (z >> 16);
        rng_state[i] = z;
    }
}

/* Uniform float [0, 1) */
static double rng_uniform(void) {
    return (xoshiro128ss() >> 8) * (1.0 / 16777216.0);
}

/* Gaussian via Box-Muller */
static double rng_gauss(double mu, double sigma) {
    double u1 = rng_uniform();
    double u2 = rng_uniform();
    if (u1 < 1e-15) u1 = 1e-15;
    return mu + sigma * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ── Enums ───────────────────────────────────────────────── */
typedef enum { SIDE_NATO, SIDE_PACT } Side;
typedef enum {
    CLASS_CARRIER,
    CLASS_CRUISER,
    CLASS_DESTROYER,
    CLASS_FRIGATE,
    CLASS_SUBMARINE,
    CLASS_MISSILE_BOAT
} ShipClass;

typedef enum {
    WPN_SSM,       /* Surface-to-surface missile */
    WPN_SAM,       /* Surface-to-air missile (used as CIWS proxy) */
    WPN_TORPEDO,
    WPN_GUN_5IN,
    WPN_GUN_130MM,
    WPN_CIWS
} WeaponType;

/* ── Data Structures ─────────────────────────────────────── */
typedef struct {
    WeaponType type;
    char       name[MAX_NAME];
    double     range_nm;      /* effective range in NM */
    double     p_hit;         /* base probability of hit */
    double     damage;        /* damage points on hit */
    int        salvo_size;    /* rounds per salvo */
    int        reload_ticks;  /* ticks between salvos */
    int        ammo;          /* remaining rounds */
    int        cooldown;      /* current cooldown counter */
} Weapon;

typedef struct {
    char       name[MAX_NAME];
    char       hull_class[MAX_NAME];
    Side       side;
    ShipClass  ship_class;

    /* Position & movement */
    double     x, y;          /* NM from origin */
    double     heading;       /* degrees, 0=north */
    double     speed_kts;     /* current speed */
    double     max_speed_kts;

    /* Combat */
    double     hp;
    double     max_hp;
    double     armor;         /* damage reduction factor 0-1 */
    double     radar_range_nm;
    double     rcs;           /* radar cross section (detectability) */
    int        detected;      /* has been detected by enemy */

    /* Weapons */
    Weapon     weapons[MAX_WEAPONS];
    int        num_weapons;

    /* Status */
    int        alive;
    int        kills;
    int        damage_dealt_total;
} Ship;

typedef struct {
    int    tick;
    char   attacker[MAX_NAME];
    char   defender[MAX_NAME];
    char   weapon[MAX_NAME];
    int    hit;
    double damage;
    double defender_hp_after;
    int    kill;
} EngagementRecord;

/* ── Globals ─────────────────────────────────────────────── */
static Ship ships[MAX_SHIPS];
static int  num_ships = 0;
static EngagementRecord records[8192];
static int  num_records = 0;

/* ── Ship Templates ──────────────────────────────────────── */

static void add_weapon(Ship *s, WeaponType t, const char *name,
                       double range, double phit, double dmg,
                       int salvo, int reload, int ammo) {
    if (s->num_weapons >= MAX_WEAPONS) return;
    Weapon *w = &s->weapons[s->num_weapons++];
    w->type = t;
    strncpy(w->name, name, MAX_NAME - 1);
    w->range_nm = range;
    w->p_hit = phit;
    w->damage = dmg;
    w->salvo_size = salvo;
    w->reload_ticks = reload;
    w->ammo = ammo;
    w->cooldown = 0;
}

static Ship *add_ship(Side side, const char *name, const char *hull,
                      ShipClass cls, double hp, double armor,
                      double max_spd, double radar, double rcs) {
    if (num_ships >= MAX_SHIPS) return NULL;
    Ship *s = &ships[num_ships++];
    memset(s, 0, sizeof(Ship));
    strncpy(s->name, name, MAX_NAME - 1);
    strncpy(s->hull_class, hull, MAX_NAME - 1);
    s->side = side;
    s->ship_class = cls;
    s->hp = hp;
    s->max_hp = hp;
    s->armor = armor;
    s->max_speed_kts = max_spd;
    s->speed_kts = max_spd * 0.7;
    s->radar_range_nm = radar;
    s->rcs = rcs;
    s->alive = 1;
    s->detected = 0;
    return s;
}

/* ── Scenario Builder ────────────────────────────────────── */
static void build_scenario(void) {
    Ship *s;

    /* ── NATO Task Force (spawns west side) ── */

    s = add_ship(SIDE_NATO, "USS Ticonderoga", "CG-47 Ticonderoga",
                 CLASS_CRUISER, 280, 0.15, 30, 150, 0.6);
    s->x = 50 + rng_uniform() * 20; s->y = 90 + rng_uniform() * 20;
    s->heading = 90;
    add_weapon(s, WPN_SSM,   "Harpoon RGM-84",  65, 0.72, 85, 2, 45, 8);
    add_weapon(s, WPN_SAM,   "SM-2 Standard",   90, 0.65, 40, 2, 30, 68);
    add_weapon(s, WPN_GUN_5IN,"Mk 45 5\"/54",   13, 0.45, 25, 3, 10, 600);
    add_weapon(s, WPN_CIWS,  "Phalanx CIWS",     1, 0.55, 15, 1,  5, 1550);

    s = add_ship(SIDE_NATO, "USS Spruance", "DD-963 Spruance",
                 CLASS_DESTROYER, 200, 0.10, 33, 120, 0.55);
    s->x = 55 + rng_uniform() * 15; s->y = 105 + rng_uniform() * 15;
    s->heading = 85;
    add_weapon(s, WPN_SSM,    "Harpoon RGM-84", 65, 0.72, 85, 2, 45, 8);
    add_weapon(s, WPN_TORPEDO,"Mk 46 Torpedo",  5,  0.60, 120,1, 60, 6);
    add_weapon(s, WPN_GUN_5IN,"Mk 45 5\"/54",  13, 0.45,  25, 3, 10, 500);
    add_weapon(s, WPN_CIWS,   "Phalanx CIWS",   1, 0.55,  15, 1,  5, 1550);

    s = add_ship(SIDE_NATO, "USS Knox", "FF-1052 Knox",
                 CLASS_FRIGATE, 140, 0.08, 27, 80, 0.45);
    s->x = 45 + rng_uniform() * 15; s->y = 80 + rng_uniform() * 15;
    s->heading = 95;
    add_weapon(s, WPN_TORPEDO,"Mk 46 Torpedo",  5, 0.60, 120, 1, 60, 6);
    add_weapon(s, WPN_GUN_5IN,"Mk 42 5\"/38",  10, 0.40,  22, 2, 12, 400);

    s = add_ship(SIDE_NATO, "USS Los Angeles", "SSN-688",
                 CLASS_SUBMARINE, 160, 0.05, 32, 40, 0.10);
    s->x = 60 + rng_uniform() * 10; s->y = 75 + rng_uniform() * 30;
    s->heading = 80;
    add_weapon(s, WPN_SSM,    "Harpoon UGM-84",65, 0.70, 85,  2, 50, 4);
    add_weapon(s, WPN_TORPEDO,"Mk 48 ADCAP",   20, 0.75, 200, 1, 90, 22);

    /* ── Warsaw Pact Task Force (spawns east side) ── */

    s = add_ship(SIDE_PACT, "Slava", "Pr.1164 Atlant",
                 CLASS_CRUISER, 300, 0.18, 32, 140, 0.7);
    s->x = 140 + rng_uniform() * 20; s->y = 85 + rng_uniform() * 25;
    s->heading = 270;
    add_weapon(s, WPN_SSM,     "P-500 Bazalt",  300, 0.55, 150, 2, 60, 16);
    add_weapon(s, WPN_SAM,     "S-300F Fort",   75,  0.60,  45, 2, 25, 64);
    add_weapon(s, WPN_GUN_130MM,"AK-130",       12,  0.40,  28, 4,  8, 500);
    add_weapon(s, WPN_CIWS,    "AK-630",         1,  0.50,  12, 1,  4, 3000);

    s = add_ship(SIDE_PACT, "Sovremenny", "Pr.956 Sarych",
                 CLASS_DESTROYER, 210, 0.12, 33, 100, 0.55);
    s->x = 145 + rng_uniform() * 15; s->y = 110 + rng_uniform() * 15;
    s->heading = 265;
    add_weapon(s, WPN_SSM,      "P-270 Moskit", 120, 0.68, 130, 2, 50,  8);
    add_weapon(s, WPN_SAM,      "Shtil",        25,  0.50,  30, 1, 20, 48);
    add_weapon(s, WPN_GUN_130MM,"AK-130",       12,  0.40,  28, 4,  8, 500);
    add_weapon(s, WPN_CIWS,     "AK-630",        1,  0.50,  12, 1,  4, 3000);

    s = add_ship(SIDE_PACT, "Nanuchka", "Pr.1234 Ovod",
                 CLASS_MISSILE_BOAT, 80, 0.05, 34, 50, 0.30);
    s->x = 135 + rng_uniform() * 10; s->y = 95 + rng_uniform() * 15;
    s->heading = 270;
    add_weapon(s, WPN_SSM,  "P-120 Malakhit",  60, 0.65, 100, 2, 45, 6);
    add_weapon(s, WPN_CIWS, "AK-630",           1, 0.50,  12, 1,  4, 2000);

    s = add_ship(SIDE_PACT, "Victor III", "Pr.671RTM Shchuka",
                 CLASS_SUBMARINE, 150, 0.05, 30, 35, 0.08);
    s->x = 150 + rng_uniform() * 10; s->y = 70 + rng_uniform() * 35;
    s->heading = 260;
    add_weapon(s, WPN_SSM,     "P-70 Ametist",  40, 0.55,  90, 1, 55, 8);
    add_weapon(s, WPN_TORPEDO, "53-65 Torpedo", 12, 0.65, 180, 1, 80, 18);
}

/* ── Utility ─────────────────────────────────────────────── */
static double dist_nm(const Ship *a, const Ship *b) {
    double dx = a->x - b->x;
    double dy = a->y - b->y;
    return sqrt(dx * dx + dy * dy);
}

static double bearing_deg(const Ship *from, const Ship *to) {
    double dx = to->x - from->x;
    double dy = to->y - from->y;
    double angle = atan2(dx, dy) * (180.0 / M_PI);
    if (angle < 0) angle += 360.0;
    return angle;
}

static const char *side_str(Side s) {
    return s == SIDE_NATO ? "NATO" : "PACT";
}

static const char *class_str(ShipClass c) {
    const char *names[] = {"CV","CG","DD","FF","SS","PGG"};
    return names[c];
}

/* ── ANSI Helpers ────────────────────────────────────────── */
#define ANSI_RESET   "\033[0m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_RED     "\033[31m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"

/* ── Detection Phase ─────────────────────────────────────── */
static void phase_detect(int tick) {
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        for (int j = 0; j < num_ships; j++) {
            if (i == j || !ships[j].alive) continue;
            if (ships[i].side == ships[j].side) continue;
            if (ships[j].detected) continue;

            double d = dist_nm(&ships[i], &ships[j]);
            /* Detection probability based on radar range, RCS, and distance */
            double eff_range = ships[i].radar_range_nm * ships[j].rcs;
            if (d < eff_range) {
                double p_detect = 1.0 - pow(d / eff_range, 2.0);
                /* Submarines are harder to detect */
                if (ships[j].ship_class == CLASS_SUBMARINE)
                    p_detect *= 0.25;
                if (rng_uniform() < p_detect) {
                    ships[j].detected = 1;
                    if (tick % 30 == 0 || tick < 60) {
                        printf("  " ANSI_CYAN "[DETECT]" ANSI_RESET
                               " %s %s contacts %s %s at %.1f NM, brg %03.0f\n",
                               side_str(ships[i].side), ships[i].name,
                               side_str(ships[j].side), ships[j].name,
                               d, bearing_deg(&ships[i], &ships[j]));
                    }
                }
            }
        }
    }
}

/* ── Movement Phase ──────────────────────────────────────── */
static void phase_move(int tick) {
    (void)tick;
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive) continue;

        /* Find nearest detected enemy and adjust heading */
        double min_d = 1e9;
        int tgt = -1;
        for (int j = 0; j < num_ships; j++) {
            if (i == j || !ships[j].alive) continue;
            if (ships[j].side == s->side) continue;
            if (!ships[j].detected) continue;
            double d = dist_nm(s, &ships[j]);
            if (d < min_d) { min_d = d; tgt = j; }
        }

        if (tgt >= 0) {
            double desired = bearing_deg(s, &ships[tgt]);
            /* Approach to optimal weapon range, then hold */
            double best_range = 0;
            for (int w = 0; w < s->num_weapons; w++) {
                if (s->weapons[w].ammo > 0 && s->weapons[w].range_nm > best_range)
                    best_range = s->weapons[w].range_nm;
            }
            double target_dist = best_range * 0.7;
            if (target_dist < 5) target_dist = 5;

            if (min_d > target_dist) {
                /* Close distance */
                s->heading = desired;
                s->speed_kts = s->max_speed_kts;
            } else if (min_d < target_dist * 0.5) {
                /* Too close, open distance */
                s->heading = fmod(desired + 180.0, 360.0);
                s->speed_kts = s->max_speed_kts * 0.9;
            } else {
                /* At range, slight evasive maneuver */
                s->heading = desired + rng_gauss(0, 15);
                s->speed_kts = s->max_speed_kts * 0.6;
            }
        }

        /* Apply movement (1 tick = 1 second) */
        double spd_nms = s->speed_kts / 3600.0; /* NM per second */
        double rad = s->heading * (M_PI / 180.0);
        s->x += sin(rad) * spd_nms;
        s->y += cos(rad) * spd_nms;

        /* Clamp to grid */
        if (s->x < 0) s->x = 0;
        if (s->x > GRID_SIZE) s->x = GRID_SIZE;
        if (s->y < 0) s->y = 0;
        if (s->y > GRID_SIZE) s->y = GRID_SIZE;
    }
}

/* ── Weapons Phase ───────────────────────────────────────── */
static void phase_weapons(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *atk = &ships[i];
        if (!atk->alive) continue;

        /* Find priority target: lowest HP detected enemy in range */
        for (int w = 0; w < atk->num_weapons; w++) {
            Weapon *wpn = &atk->weapons[w];
            if (wpn->cooldown > 0) { wpn->cooldown--; continue; }
            if (wpn->ammo <= 0) continue;

            /* Find best target for this weapon */
            int best_tgt = -1;
            double best_score = -1;
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side == atk->side) continue;
                if (!ships[j].detected) continue;

                double d = dist_nm(atk, &ships[j]);
                if (d > wpn->range_nm) continue;

                /* Torpedoes prefer subs and close targets */
                double score = (wpn->range_nm - d) / wpn->range_nm;
                if (wpn->type == WPN_TORPEDO && ships[j].ship_class == CLASS_SUBMARINE)
                    score += 0.5;
                /* SSMs prefer high-value targets */
                if (wpn->type == WPN_SSM &&
                    (ships[j].ship_class == CLASS_CRUISER ||
                     ships[j].ship_class == CLASS_CARRIER))
                    score += 0.3;

                if (score > best_score) {
                    best_score = score;
                    best_tgt = j;
                }
            }

            if (best_tgt < 0) continue;

            Ship *def = &ships[best_tgt];
            double d = dist_nm(atk, def);

            /* Fire salvo */
            int rounds = wpn->salvo_size;
            if (rounds > wpn->ammo) rounds = wpn->ammo;
            wpn->ammo -= rounds;
            wpn->cooldown = wpn->reload_ticks;

            int total_hits = 0;
            double total_dmg = 0;

            for (int r = 0; r < rounds; r++) {
                /* Compute hit probability with range degradation */
                double p = wpn->p_hit * (1.0 - 0.3 * (d / wpn->range_nm));
                /* Sea state modifier */
                p *= rng_gauss(1.0, 0.08);
                /* ECM modifier */
                if (def->ship_class == CLASS_CRUISER ||
                    def->ship_class == CLASS_CARRIER)
                    p *= 0.85; /* better ECM on capital ships */
                /* Submarines are harder to hit on surface */
                if (def->ship_class == CLASS_SUBMARINE && wpn->type == WPN_SSM)
                    p *= 0.6;

                if (p < 0.05) p = 0.05;
                if (p > 0.95) p = 0.95;

                if (rng_uniform() < p) {
                    double dmg = wpn->damage * rng_gauss(1.0, 0.15);
                    dmg *= (1.0 - def->armor);
                    if (dmg < 1) dmg = 1;
                    def->hp -= dmg;
                    total_hits++;
                    total_dmg += dmg;
                    atk->damage_dealt_total += (int)dmg;
                }
            }

            int killed = 0;
            if (def->hp <= 0) {
                def->hp = 0;
                def->alive = 0;
                atk->kills++;
                killed = 1;
            }

            /* Log the engagement */
            if (total_hits > 0 || killed) {
                const char *hit_color = total_hits > 0 ? ANSI_RED : ANSI_DIM;
                printf("  %s[FIRE]" ANSI_RESET " %s %s -> %s %s | "
                       "%s x%d @ %.0f NM | %d/%d hit, %.0f dmg",
                       hit_color,
                       side_str(atk->side), atk->name,
                       side_str(def->side), def->name,
                       wpn->name, rounds, d,
                       total_hits, rounds, total_dmg);
                if (killed)
                    printf(" " ANSI_BOLD ANSI_RED "*** SUNK ***" ANSI_RESET);
                printf(" [HP: %.0f/%.0f]\n", def->hp, def->max_hp);
            }

            /* Record for CSV */
            if (num_records < 8192) {
                EngagementRecord *rec = &records[num_records++];
                rec->tick = tick;
                strncpy(rec->attacker, atk->name, MAX_NAME - 1);
                strncpy(rec->defender, def->name, MAX_NAME - 1);
                strncpy(rec->weapon, wpn->name, MAX_NAME - 1);
                rec->hit = total_hits;
                rec->damage = total_dmg;
                rec->defender_hp_after = def->hp;
                rec->kill = killed;
            }
        }
    }
}

/* ── Status Display ──────────────────────────────────────── */
static void print_status_board(int tick) {
    printf("\n" ANSI_BOLD "═══════════════════════════════════════"
           "═══════════════════════════════════════════\n");
    printf("  NAVSIM TACTICAL DISPLAY  │  T+%02d:%02d  │  "
           "GRID %dx%d NM\n", tick / 60, tick % 60, GRID_SIZE, GRID_SIZE);
    printf("═══════════════════════════════════════"
           "═══════════════════════════════════════════\n" ANSI_RESET);

    for (int side = 0; side <= 1; side++) {
        const char *color = (side == 0) ? ANSI_GREEN : ANSI_RED;
        printf(" %s%s %-6s FORCES%s\n", ANSI_BOLD, color,
               side == 0 ? "NATO" : "PACT", ANSI_RESET);
        printf(" %-20s %-8s %6s %8s %8s %6s %6s\n",
               "Ship", "Class", "HP", "Pos", "Hdg/Spd", "Kills", "Ammo");
        printf(" " ANSI_DIM "────────────────────────────────────"
               "───────────────────────────────────────\n" ANSI_RESET);

        for (int i = 0; i < num_ships; i++) {
            Ship *s = &ships[i];
            if ((int)s->side != side) continue;

            int total_ammo = 0;
            for (int w = 0; w < s->num_weapons; w++)
                total_ammo += s->weapons[w].ammo;

            char pos_buf[16], hdg_buf[16], hp_buf[16];
            snprintf(pos_buf, sizeof(pos_buf), "%03.0f,%03.0f", s->x, s->y);
            snprintf(hdg_buf, sizeof(hdg_buf), "%03.0f/%02.0f",
                     fmod(s->heading + 360, 360), s->speed_kts);
            double hp_pct = s->max_hp > 0 ? (s->hp / s->max_hp) * 100 : 0;

            const char *status_color = ANSI_RESET;
            if (!s->alive) status_color = ANSI_DIM;
            else if (hp_pct < 30) status_color = ANSI_RED;
            else if (hp_pct < 60) status_color = ANSI_YELLOW;

            snprintf(hp_buf, sizeof(hp_buf), "%3.0f%%", hp_pct);

            printf(" %s%-20s %-8s %6s %8s %8s %5d %5d%s\n",
                   status_color,
                   s->name, class_str(s->ship_class),
                   s->alive ? hp_buf : "SUNK",
                   pos_buf, hdg_buf,
                   s->kills, total_ammo,
                   ANSI_RESET);
        }
        printf("\n");
    }
}

/* ── Tactical Map (ASCII) ────────────────────────────────── */
static void print_tactical_map(void) {
    /* 60x30 character map */
    const int MAP_W = 60, MAP_H = 30;
    char map[30][61];

    /* Init with water */
    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++)
            map[r][c] = '.';
        map[r][MAP_W] = '\0';
    }

    printf(ANSI_BOLD " TACTICAL PLOT\n" ANSI_RESET);
    printf(" " ANSI_DIM "N↑  Scale: 1 char ≈ %.1f NM\n" ANSI_RESET,
           (double)GRID_SIZE / MAP_W);

    /* Place ships */
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        int c = (int)(s->x * MAP_W / GRID_SIZE);
        int r = MAP_H - 1 - (int)(s->y * MAP_H / GRID_SIZE);
        if (c < 0) c = 0;
        if (c >= MAP_W) c = MAP_W - 1;
        if (r < 0) r = 0;
        if (r >= MAP_H) r = MAP_H - 1;

        if (!s->alive) {
            map[r][c] = 'X';
        } else if (s->side == SIDE_NATO) {
            map[r][c] = "AGDFSU"[s->ship_class]; /* carrier/cruiser/dd/ff/sub/missile */
        } else {
            map[r][c] = "agdfsu"[s->ship_class]; /* lowercase = pact */
        }
    }

    printf(" ┌");
    for (int c = 0; c < MAP_W; c++) printf("─");
    printf("┐\n");

    for (int r = 0; r < MAP_H; r++) {
        printf(" │");
        for (int c = 0; c < MAP_W; c++) {
            char ch = map[r][c];
            if (ch == '.')
                printf(ANSI_DIM "·" ANSI_RESET);
            else if (ch == 'X')
                printf(ANSI_RED "✕" ANSI_RESET);
            else if (ch >= 'A' && ch <= 'Z')
                printf(ANSI_GREEN "%c" ANSI_RESET, ch);
            else if (ch >= 'a' && ch <= 'z')
                printf(ANSI_RED "%c" ANSI_RESET, ch);
            else
                printf("%c", ch);
        }
        printf("│\n");
    }

    printf(" └");
    for (int c = 0; c < MAP_W; c++) printf("─");
    printf("┘\n");
    printf("  " ANSI_DIM "Legend: " ANSI_GREEN "UPPERCASE" ANSI_RESET ANSI_DIM
           "=NATO  " ANSI_RED "lowercase" ANSI_RESET ANSI_DIM
           "=PACT  " ANSI_RED "✕" ANSI_RESET ANSI_DIM "=sunk  "
           "G=CG D=DD F=FF S=SS U=PGG\n" ANSI_RESET);
}

/* ── Write CSV ───────────────────────────────────────────── */
static void write_csv(void) {
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) { perror("fopen"); return; }

    fprintf(f, "tick,time,attacker,defender,weapon,hits,damage,"
               "defender_hp_after,kill\n");
    for (int i = 0; i < num_records; i++) {
        EngagementRecord *r = &records[i];
        fprintf(f, "%d,%02d:%02d,%s,%s,%s,%d,%.1f,%.1f,%d\n",
                r->tick, r->tick / 60, r->tick % 60,
                r->attacker, r->defender, r->weapon,
                r->hit, r->damage, r->defender_hp_after, r->kill);
    }

    /* Also write final ship status */
    FILE *f2 = fopen("ship_status.csv", "w");
    if (f2) {
        fprintf(f2, "name,side,class,hp,max_hp,alive,kills,damage_dealt,"
                    "final_x,final_y\n");
        for (int i = 0; i < num_ships; i++) {
            Ship *s = &ships[i];
            fprintf(f2, "%s,%s,%s,%.0f,%.0f,%d,%d,%d,%.1f,%.1f\n",
                    s->name, side_str(s->side), class_str(s->ship_class),
                    s->hp, s->max_hp, s->alive, s->kills,
                    s->damage_dealt_total, s->x, s->y);
        }
        fclose(f2);
    }
    fclose(f);
}

/* ── Victory Assessment ──────────────────────────────────── */
static void assess_victory(void) {
    int nato_alive = 0, pact_alive = 0;
    double nato_hp = 0, pact_hp = 0;
    double nato_max = 0, pact_max = 0;

    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (s->side == SIDE_NATO) {
            nato_max += s->max_hp;
            if (s->alive) { nato_alive++; nato_hp += s->hp; }
        } else {
            pact_max += s->max_hp;
            if (s->alive) { pact_alive++; pact_hp += s->hp; }
        }
    }

    printf("\n" ANSI_BOLD "═══════════════════════════════════════"
           "═══════════════════════════════════════════\n");
    printf("  AFTER-ACTION REPORT\n");
    printf("═══════════════════════════════════════"
           "═══════════════════════════════════════════\n" ANSI_RESET);

    printf("  " ANSI_GREEN "NATO:" ANSI_RESET " %d ships surviving, "
           "%.0f/%.0f HP remaining (%.0f%%)\n",
           nato_alive, nato_hp, nato_max,
           nato_max > 0 ? (nato_hp / nato_max) * 100 : 0);
    printf("  " ANSI_RED "PACT:" ANSI_RESET " %d ships surviving, "
           "%.0f/%.0f HP remaining (%.0f%%)\n",
           pact_alive, pact_hp, pact_max,
           pact_max > 0 ? (pact_hp / pact_max) * 100 : 0);

    double nato_score = (nato_hp / nato_max) * 100 + (pact_max - pact_hp);
    double pact_score = (pact_hp / pact_max) * 100 + (nato_max - nato_hp);

    printf("\n  Combat Effectiveness Score:\n");
    printf("    NATO: %.0f  |  PACT: %.0f\n", nato_score, pact_score);

    if (nato_alive == 0 && pact_alive == 0)
        printf("\n  " ANSI_BOLD "RESULT: MUTUAL DESTRUCTION\n" ANSI_RESET);
    else if (nato_score > pact_score * 1.5)
        printf("\n  " ANSI_BOLD ANSI_GREEN
               "RESULT: DECISIVE NATO VICTORY\n" ANSI_RESET);
    else if (pact_score > nato_score * 1.5)
        printf("\n  " ANSI_BOLD ANSI_RED
               "RESULT: DECISIVE PACT VICTORY\n" ANSI_RESET);
    else if (nato_score > pact_score)
        printf("\n  " ANSI_BOLD ANSI_GREEN
               "RESULT: MARGINAL NATO VICTORY\n" ANSI_RESET);
    else if (pact_score > nato_score)
        printf("\n  " ANSI_BOLD ANSI_RED
               "RESULT: MARGINAL PACT VICTORY\n" ANSI_RESET);
    else
        printf("\n  " ANSI_BOLD ANSI_YELLOW "RESULT: DRAW\n" ANSI_RESET);

    printf("\n  Engagement data written to: %s\n", LOG_FILE);
    printf("  Ship status written to:     ship_status.csv\n");
}

/* ── Main Loop ───────────────────────────────────────────── */
int main(int argc, char **argv) {
    setup_console();

    uint32_t seed = (uint32_t)time(NULL);
    if (argc > 1) seed = (uint32_t)atoi(argv[1]);
    rng_seed(seed);

    printf(ANSI_BOLD ANSI_CYAN
    "\n"
    "    ███╗   ██╗ █████╗ ██╗   ██╗███████╗██╗███╗   ███╗\n"
    "    ████╗  ██║██╔══██╗██║   ██║██╔════╝██║████╗ ████║\n"
    "    ██╔██╗ ██║███████║██║   ██║███████╗██║██╔████╔██║\n"
    "    ██║╚██╗██║██╔══██║╚██╗ ██╔╝╚════██║██║██║╚██╔╝██║\n"
    "    ██║ ╚████║██║  ██║ ╚████╔╝ ███████║██║██║ ╚═╝ ██║\n"
    "    ╚═╝  ╚═══╝╚═╝  ╚═╝  ╚═══╝  ╚══════╝╚═╝╚═╝     ╚═╝\n"
    ANSI_RESET
    ANSI_DIM
    "    Cold War Naval Tactical Engagement Simulator\n"
    "    Monte Carlo Engine v1.0  │  RNG Seed: %u\n"
    ANSI_RESET "\n", seed);

    build_scenario();

    printf(ANSI_BOLD "  SCENARIO: North Atlantic Engagement\n" ANSI_RESET);
    printf("  NATO TF: Ticonderoga CG, Spruance DD, Knox FF, Los Angeles SSN\n");
    printf("  PACT TF: Slava CG, Sovremenny DD, Nanuchka PGG, Victor III SSN\n\n");

    print_status_board(0);
    print_tactical_map();

    printf(ANSI_BOLD "\n  ─── ENGAGEMENT BEGINS ───\n\n" ANSI_RESET);

    int battle_over = 0;
    for (int tick = 1; tick <= MAX_TICK && !battle_over; tick++) {
        phase_detect(tick);
        phase_move(tick);
        phase_weapons(tick);

        /* Print tactical update every 120 ticks (2 min) */
        if (tick % 120 == 0) {
            print_status_board(tick);
            print_tactical_map();
        }

        /* Check if one side is eliminated */
        int nato_up = 0, pact_up = 0;
        for (int i = 0; i < num_ships; i++) {
            if (!ships[i].alive) continue;
            if (ships[i].side == SIDE_NATO) nato_up++;
            else pact_up++;
        }
        if (nato_up == 0 || pact_up == 0) battle_over = 1;
    }

    print_status_board(MAX_TICK);
    print_tactical_map();
    write_csv();
    assess_victory();

    return 0;
}