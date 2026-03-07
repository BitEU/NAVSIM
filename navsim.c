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
#define MAX_SHIPS        24
#define MAX_WEAPONS       8
#define MAX_NAME         32
#define MAX_TICK       3600   /* 60 minutes at 1-second ticks */
#define GRID_SIZE       300   /* nautical miles */
#define MAX_PROJECTILES 512   /* In-flight projectiles */
#define MAX_COMPARTMENTS 12   /* Ship damage compartments */
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
    CLASS_CORVETTE,
    CLASS_SUBMARINE,
    CLASS_MISSILE_BOAT,
    CLASS_AMPHIBIOUS
} ShipClass;

typedef enum {
    WPN_SSM,       /* Surface-to-surface missile */
    WPN_SAM,       /* Surface-to-air missile (used as CIWS proxy) */
    WPN_TORPEDO,
    WPN_GUN_5IN,
    WPN_GUN_130MM,
    WPN_GUN_76MM,
    WPN_GUN_ADVANCED,
    WPN_RAILGUN,
    WPN_CIWS,
    WPN_CRUISE_MISSILE,
    WPN_ASROC
} WeaponType;

typedef enum {
    RADAR_S_BAND,  /* 2-4 GHz, long range search */
    RADAR_X_BAND,  /* 8-12 GHz, fire control */
    RADAR_L_BAND,  /* 1-2 GHz, early warning */
    RADAR_C_BAND   /* 4-8 GHz, tracking */
} RadarBand;

typedef enum {
    COMP_BOW,
    COMP_BRIDGE,
    COMP_FORWARD_WEAPONS,
    COMP_MIDSHIP_PORT,
    COMP_MIDSHIP_STBD,
    COMP_ENGINE,
    COMP_AFT_WEAPONS,
    COMP_STERN,
    COMP_RADAR,
    COMP_COMMS
} Compartment;

/* ── Data Structures ─────────────────────────────────────── */

/* Ballistic projectile in flight */
typedef struct {
    int        active;
    char       weapon_name[MAX_NAME];
    char       attacker[MAX_NAME];
    char       target[MAX_NAME];
    double     x, y, z;        /* Position (NM, NM, feet) */
    double     vx, vy, vz;     /* Velocity (NM/s, NM/s, ft/s) */
    double     launch_time;    /* Tick launched */
    double     mass_kg;        /* Projectile mass */
    double     drag_coeff;     /* Aerodynamic drag */
    double     damage;         /* Damage on impact */
    WeaponType type;
} Projectile;

/* Radar system */
typedef struct {
    RadarBand  band;
    double     power_kw;       /* Transmit power */
    double     gain_db;        /* Antenna gain */
    double     frequency_ghz;  /* Operating frequency */
    double     range_nm;       /* Detection range */
    double     azimuth_res;    /* Azimuth resolution (deg) */
    int        jammed;         /* Jamming status */
    double     jam_strength;   /* Jamming effectiveness */
} RadarSystem;

/* Damage compartment */
typedef struct {
    Compartment type;
    double      integrity;     /* 0.0-1.0 */
    int         flooding;      /* Flooding status */
    int         on_fire;       /* Fire status */
    double      flood_rate;    /* m³/minute */
} DamageCompartment;

typedef struct {
    WeaponType type;
    char       name[MAX_NAME];
    double     range_nm;       /* effective range in NM */
    double     p_hit;          /* base probability of hit */
    double     damage;         /* damage points on hit */
    int        salvo_size;     /* rounds per salvo */
    int        reload_ticks;   /* ticks between salvos */
    int        ammo;           /* remaining rounds */
    int        cooldown;       /* current cooldown counter */
    /* Ballistics parameters */
    double     muzzle_velocity_mps; /* m/s for guns, mach for missiles */
    double     projectile_mass_kg;
    double     elevation_angle;     /* Launch angle in degrees */
    double     guidance_quality;    /* 0-1, for missiles */
} Weapon;

typedef struct {
    char       name[MAX_NAME];
    char       hull_class[MAX_NAME];
    Side       side;
    ShipClass  ship_class;

    /* Position & movement */
    double     x, y;           /* NM from origin */
    double     heading;        /* degrees, 0=north */
    double     speed_kts;      /* current speed */
    double     max_speed_kts;

    /* Combat */
    double     hp;
    double     max_hp;
    double     armor;          /* damage reduction factor 0-1 */
    double     rcs;            /* radar cross section (detectability) */
    int        detected;
    
    /* Radar systems */
    RadarSystem search_radar;
    RadarSystem fire_control_radar;
    
    /* Electronic warfare */
    double     ecm_power_kw;   /* ECM jammer power */
    double     esm_sensitivity;/* ESM receiver sensitivity */
    int        chaff_charges;  /* Chaff remaining */
    int        flare_charges;  /* Flares remaining */
    
    /* Damage control */
    DamageCompartment compartments[MAX_COMPARTMENTS];
    int        crew_casualties;
    double     flooding_total; /* Total water ingress (tons) */
    double     list_angle;     /* Ship list in degrees */
    int        speed_reduced;  /* Engine damage flag */

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

/* ── Forward Declarations ────────────────────────────────── */
static double dist_nm(const Ship *a, const Ship *b);
static double bearing_deg(const Ship *from, const Ship *to);

/* ── Globals ─────────────────────────────────────────────── */
static Ship ships[MAX_SHIPS];
static int  num_ships = 0;
static Projectile projectiles[MAX_PROJECTILES];
static int  num_projectiles = 0;
static EngagementRecord records[8192];
static int  num_records = 0;
/* Simple aggregated statistics tracked during the run */
static int stat_launches[2] = {0, 0};         /* launches by side (SIDE_NATO=0,SIDE_PACT=1) */
static int stat_hits[2] = {0, 0};             /* confirmed impacts by attacker side */
static int stat_misses[2] = {0, 0};           /* projectiles expended without impact */
static int stat_detect_opps[2] = {0, 0};      /* detection opportunities considered per detector side */
static int stat_detect_success[2] = {0, 0};   /* successful detections per detector side */
static int stat_detect_jammed[2] = {0, 0};    /* detection attempts while jammed per detector side */
static int stat_kills_by_side[2] = {0, 0};    /* kills awarded per side */

/* Environmental constants */
#define AIR_DENSITY_SL  1.225    /* kg/m³ at sea level */
#define GRAVITY         9.81     /* m/s² */
#define WIND_SPEED      5.0      /* knots, random component */

/* ── Ballistics Physics ──────────────────────────────────── */

static void init_projectile(Projectile *p, Ship *attacker, Ship *target,
                            Weapon *wpn, int tick) {
    p->active = 1;
    strncpy(p->weapon_name, wpn->name, MAX_NAME - 1);
    strncpy(p->attacker, attacker->name, MAX_NAME - 1);
    strncpy(p->target, target->name, MAX_NAME - 1);
    p->x = attacker->x;
    p->y = attacker->y;
    p->z = 50.0; /* Launch height ~50 feet */
    p->launch_time = tick;
    p->mass_kg = wpn->projectile_mass_kg;
    p->damage = wpn->damage;
    p->type = wpn->type;
    p->drag_coeff = 0.3; /* Aerodynamic drag coefficient */
    
    /* Calculate initial velocity vector */
    double range_m = wpn->range_nm * 1852.0; /* NM to meters */
    double v0 = wpn->muzzle_velocity_mps;
    
    if (wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE) {
        /* Missiles: direct trajectory with guidance */
        double dx = target->x - attacker->x;
        double dy = target->y - attacker->y;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist < 0.001) dist = 0.001;
        
        /* Convert mach number to m/s (mach 0.9 = 306 m/s) */
        v0 = wpn->muzzle_velocity_mps * 340.0;
        
        p->vx = (dx / dist) * v0 / 1852.0; /* Convert to NM/s */
        p->vy = (dy / dist) * v0 / 1852.0;
        p->vz = 0; /* Cruise at constant altitude */
    } else {
        /* Guns: ballistic arc */
        double angle_rad = wpn->elevation_angle * M_PI / 180.0;
        double bearing = bearing_deg(attacker, target) * M_PI / 180.0;
        
        double v_horiz = v0 * cos(angle_rad);
        p->vx = sin(bearing) * v_horiz / 1852.0; /* to NM/s */
        p->vy = cos(bearing) * v_horiz / 1852.0;
        p->vz = v0 * sin(angle_rad) * 3.28084; /* to ft/s */
    }
}

static void update_projectile_physics(Projectile *p, double dt) {
    if (!p->active) return;
    
    /* Apply gravity (for ballistic shells) */
    if (p->type != WPN_SSM && p->type != WPN_CRUISE_MISSILE) {
        p->vz -= GRAVITY * 3.28084 * dt; /* ft/s² */
    }
    
    /* Apply drag (simplified model - missiles use minimal drag) */
    double v_total_ms = sqrt(
        (p->vx * 1852.0) * (p->vx * 1852.0) +
        (p->vy * 1852.0) * (p->vy * 1852.0) +
        (p->vz * 0.3048) * (p->vz *0.3048)
    );
    
    /* Cruise missiles maintain speed with thrust - minimal net drag
       SSMs briefly accelerate then coast - moderate drag
       Shells are ballistic - full drag */
    double ref_area, drag_multiplier;
    if (p->type == WPN_CRUISE_MISSILE) {
        ref_area = 0.2;
        drag_multiplier = 0.02;  /* Engines mostly counteract drag */
    } else if (p->type == WPN_SSM) {
        ref_area = 0.2;
        drag_multiplier = 0.1;   /* Some thrust, but coasting */
    } else {
        ref_area = 0.01;
        drag_multiplier = 1.0;   /* Full ballistic drag */
    }
    
    double drag_force = 0.5 * AIR_DENSITY_SL * p->drag_coeff * ref_area * v_total_ms * v_total_ms * drag_multiplier;
    double drag_accel = drag_force / p->mass_kg;
    
    if (v_total_ms > 0.1) {
        p->vx -= (p->vx * 1852.0 / v_total_ms) * drag_accel * dt / 1852.0;
        p->vy -= (p->vy * 1852.0 / v_total_ms) * drag_accel * dt / 1852.0;
        p->vz -= (p->vz * 0.3048 / v_total_ms) * drag_accel * dt * 3.28084;
    }
    
    /* Update position */
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
    
    /* Check impact */
    if (p->z <= 0) {
        p->active = 0; /* Hit water */
        /* Debug: printf("  [DEBUG] Projectile %s hit water at (%.1f, %.1f)\n", p->weapon_name, p->x, p->y); */
    }
    
    /* Out of bounds check */
    if (p->x < -50 || p->x > GRID_SIZE + 50 ||
        p->y < -50 || p->y > GRID_SIZE + 50) {
        p->active = 0;
        /* Debug: printf("  [DEBUG] Projectile %s out of bounds at (%.1f, %.1f)\n", p->weapon_name, p->x, p->y); */
    }
}

/* ── Ship Templates ──────────────────────────────────────── */

static void add_weapon(Ship *s, WeaponType t, const char *name,
                       double range, double phit, double dmg,
                       int salvo, int reload, int ammo,
                       double muzzle_vel, double mass, double elev, double guidance) {
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
    w->muzzle_velocity_mps = muzzle_vel;
    w->projectile_mass_kg = mass;
    w->elevation_angle = elev;
    w->guidance_quality = guidance;
}

static void init_radar(RadarSystem *r, RadarBand band, double power,
                       double gain, double freq, double range) {
    r->band = band;
    r->power_kw = power;
    r->gain_db = gain;
    r->frequency_ghz = freq;
    r->range_nm = range;
    r->azimuth_res = 1.0;
    r->jammed = 0;
    r->jam_strength = 0;
}

static Ship *add_ship(Side side, const char *name, const char *hull,
                      ShipClass cls, double hp, double armor,
                      double max_spd, double rcs) {
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
    s->rcs = rcs;
    s->alive = 1;
    s->detected = 0;
    s->ecm_power_kw = 0;
    s->esm_sensitivity = 0.5;
    s->chaff_charges = 20;
    s->flare_charges = 20;
    s->crew_casualties = 0;
    s->flooding_total = 0;
    s->list_angle = 0;
    s->speed_reduced = 0;
    
    /* Initialize compartments */
    for (int i = 0; i < MAX_COMPARTMENTS; i++) {
        s->compartments[i].type = i;
        s->compartments[i].integrity = 1.0;
        s->compartments[i].flooding = 0;
        s->compartments[i].on_fire = 0;
        s->compartments[i].flood_rate = 0;
    }
    
    return s;
}

/* ── Scenario Builder ────────────────────────────────────── */
static void build_scenario(void) {
    Ship *s;

    /* ── NATO Battle Group (spawns west side) ── */
    
    /* Carrier Strike Group */
    s = add_ship(SIDE_NATO, "USS Nimitz", "CVN-68",
                 CLASS_CARRIER, 950, 0.25, 30, 1.2);
    s->x = 40 + rng_uniform() * 15; s->y = 140 + rng_uniform() * 20;
    s->heading = 90;
    init_radar(&s->search_radar, RADAR_S_BAND, 500, 45, 3.0, 180);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 200, 40, 9.5, 120);
    s->ecm_power_kw = 150;
    add_weapon(s, WPN_SAM, "Sea Sparrow", 10, 0.60, 35, 2, 25, 40, 2.5, 180, 0, 0.75);
    add_weapon(s, WPN_CIWS, "Phalanx CIWS", 1.5, 0.62, 18, 1, 3, 2000, 1100, 0.1, 85, 0.9);

    s = add_ship(SIDE_NATO, "USS Ticonderoga", "CG-47",
                 CLASS_CRUISER, 320, 0.18, 32, 0.65);
    s->x = 48 + rng_uniform() * 20; s->y = 130 + rng_uniform() * 20;
    s->heading = 90;
    init_radar(&s->search_radar, RADAR_S_BAND, 350, 42, 3.3, 170);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 180, 38, 9.0, 110);
    s->ecm_power_kw = 80;
    add_weapon(s, WPN_CRUISE_MISSILE, "Tomahawk BGM-109", 280, 0.78, 180, 1, 90, 32, 0.72, 1400, 0, 0.92);
    add_weapon(s, WPN_SSM, "Harpoon RGM-84", 70, 0.75, 95, 2, 45, 16, 0.85, 520, 0, 0.88);
    add_weapon(s, WPN_SAM, "SM-2MR Standard", 95, 0.68, 48, 2, 28, 96, 3.5, 700, 0, 0.82);
    add_weapon(s, WPN_GUN_5IN, "Mk 45 5\"/54", 14, 0.48, 28, 4, 8, 680, 808, 70, 45, 0);
    add_weapon(s, WPN_CIWS, "Phalanx CIWS", 1.5, 0.62, 18, 1, 3, 2000, 1100, 0.1, 85, 0.9);

    s = add_ship(SIDE_NATO, "USS Arleigh Burke", "DDG-51",
                 CLASS_DESTROYER, 290, 0.15, 33, 0.55);
    s->x = 55 + rng_uniform() * 18; s->y = 150 + rng_uniform() * 18;
    s->heading = 88;
    init_radar(&s->search_radar, RADAR_S_BAND, 320, 40, 3.2, 160);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 160, 36, 9.2, 105);
    s->ecm_power_kw = 60;
    add_weapon(s, WPN_CRUISE_MISSILE, "Tomahawk BGM-109", 280, 0.76, 180, 1, 95, 28, 0.72, 1400, 0, 0.90);
    add_weapon(s, WPN_SSM, "Harpoon RGM-84", 70, 0.75, 95, 2, 45, 16, 0.85, 520, 0, 0.88);
    add_weapon(s, WPN_SAM, "SM-2 Standard", 90, 0.66, 45, 2, 30, 90, 3.5, 700, 0, 0.80);
    add_weapon(s, WPN_GUN_5IN, "Mk 45 5\"/62", 15, 0.50, 30, 4, 7, 700, 870, 70, 45, 0);
    add_weapon(s, WPN_CIWS, "Phalanx CIWS", 1.5, 0.62, 18, 1, 3, 1800, 1100, 0.1, 85, 0.9);
    add_weapon(s, WPN_ASROC, "RUM-139 VL-ASROC", 15, 0.58, 110, 1, 70, 24, 2.8, 400, 0, 0.70);

    s = add_ship(SIDE_NATO, "USS Spruance", "DD-963",
                 CLASS_DESTROYER, 230, 0.12, 33, 0.58);
    s->x = 58 + rng_uniform() * 15; s->y = 110 + rng_uniform() * 15;
    s->heading = 85;
    init_radar(&s->search_radar, RADAR_S_BAND, 280, 38, 3.0, 130);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 140, 34, 9.0, 95);
    s->ecm_power_kw = 40;
    add_weapon(s, WPN_CRUISE_MISSILE, "Tomahawk BGM-109", 280, 0.74, 180, 1, 100, 12, 0.72, 1400, 0, 0.88);
    add_weapon(s, WPN_SSM, "Harpoon RGM-84", 70, 0.75, 95, 2, 45, 8, 0.85, 520, 0, 0.88);
    add_weapon(s, WPN_TORPEDO, "Mk 46 ASW", 6, 0.62, 130, 1, 65, 12, 45, 235, 0, 0.65);
    add_weapon(s, WPN_GUN_5IN, "Mk 45 5\"/54", 14, 0.48, 28, 4, 8, 600, 808, 70, 45, 0);
    add_weapon(s, WPN_CIWS, "Phalanx CIWS", 1.5, 0.62, 18, 1, 3, 1800, 1100, 0.1, 85, 0.9);

    s = add_ship(SIDE_NATO, "USS Oliver H. Perry", "FFG-7",
                 CLASS_FRIGATE, 185, 0.10, 29, 0.48);
    s->x = 43 + rng_uniform() * 15; s->y = 95 + rng_uniform() * 20;
    s->heading = 92;
    init_radar(&s->search_radar, RADAR_S_BAND, 220, 35, 3.1, 110);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 120, 32, 9.1, 85);
    s->ecm_power_kw = 25;
    add_weapon(s, WPN_SSM, "Harpoon RGM-84", 70, 0.75, 95, 2, 50, 4, 0.85, 520, 0, 0.88);
    add_weapon(s, WPN_SAM, "SM-1MR Standard", 25, 0.58, 35, 1, 22, 40, 2.5, 450, 0, 0.70);
    add_weapon(s, WPN_GUN_76MM, "Mk 75 76mm", 10, 0.42, 20, 5, 6, 500, 925, 22, 50, 0);
    add_weapon(s, WPN_CIWS, "Phalanx CIWS", 1.5, 0.62, 18, 1, 3, 1500, 1100, 0.1, 85, 0.9);
    
    s = add_ship(SIDE_NATO, "USS Knox", "FF-1052",
                 CLASS_FRIGATE, 155, 0.09, 27, 0.45);
    s->x = 45 + rng_uniform() * 15; s->y = 75 + rng_uniform() * 18;
    s->heading = 95;
    init_radar(&s->search_radar, RADAR_S_BAND, 180, 32, 3.0, 85);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 100, 30, 9.0, 70);
    s->ecm_power_kw = 18;
    add_weapon(s, WPN_TORPEDO, "Mk 46 ASW", 6, 0.62, 130, 1, 65, 16, 45, 235, 0, 0.65);
    add_weapon(s, WPN_GUN_5IN, "Mk 42 5\"/38", 11, 0.42, 24, 3, 10, 450, 762, 68, 42, 0);
    add_weapon(s, WPN_ASROC, "ASROC", 8, 0.52, 100, 1, 80, 16, 2.2, 350, 0, 0.60);

    s = add_ship(SIDE_NATO, "USS Independence", "LCS-2",
                 CLASS_CORVETTE, 110, 0.06, 45, 0.28);
    s->x = 50 + rng_uniform() * 10; s->y = 155 + rng_uniform() * 15;
    s->heading = 88;
    init_radar(&s->search_radar, RADAR_S_BAND, 160, 30, 3.2, 75);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 90, 28, 9.3, 60);
    s->ecm_power_kw = 15;
    add_weapon(s, WPN_SSM, "NSM", 100, 0.72, 85, 2, 55, 8, 0.95, 407, 0, 0.90);
    add_weapon(s, WPN_GUN_76MM, "Mk 110 57mm", 9, 0.45, 18, 6, 5, 600, 1000, 25, 52, 0);
    add_weapon(s, WPN_CIWS, "SeaRAM", 5, 0.58, 25, 1, 12, 116, 2.2, 280, 0, 0.85);

    s = add_ship(SIDE_NATO, "HDMS Absalon", "L16 Absalon",
                 CLASS_FRIGATE, 175, 0.11, 24, 0.52);
    s->x = 42 + rng_uniform() * 12; s->y = 120 + rng_uniform() * 15;
    s->heading = 93;
    init_radar(&s->search_radar, RADAR_S_BAND, 200, 34, 3.0, 100);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 110, 31, 9.1, 75);
    s->ecm_power_kw = 22;
    add_weapon(s, WPN_SSM, "Harpoon RGM-84", 70, 0.75, 95, 2, 50, 16, 0.85, 520, 0, 0.88);
    add_weapon(s, WPN_GUN_76MM, "Mk 110 76mm", 12, 0.44, 21, 5, 6, 480, 925, 22, 48, 0);
    add_weapon(s, WPN_CIWS, "Millennium Gun", 2, 0.56, 16, 1, 4, 1000, 1000, 35, 75, 0.88);

    s = add_ship(SIDE_NATO, "USS Los Angeles", "SSN-688",
                 CLASS_SUBMARINE, 180, 0.06, 32, 0.12);
    s->x = 62 + rng_uniform() * 10; s->y = 70 + rng_uniform() * 35;
    s->heading = 82;
    init_radar(&s->search_radar, RADAR_S_BAND, 80, 25, 3.5, 45);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 50, 22, 10.0, 25);
    s->ecm_power_kw = 8;
    add_weapon(s, WPN_CRUISE_MISSILE, "Tomahawk UGM-109", 280, 0.76, 180, 1, 120, 8, 0.72, 1400, 0, 0.90);
    add_weapon(s, WPN_SSM, "Harpoon UGM-84", 70, 0.73, 95, 2, 55, 4, 0.85, 520, 0, 0.86);
    add_weapon(s, WPN_TORPEDO, "Mk 48 ADCAP", 22, 0.78, 240, 1, 95, 26, 55, 1663, 0, 0.72);

    s = add_ship(SIDE_NATO, "USS Virginia", "SSN-774",
                 CLASS_SUBMARINE, 195, 0.07, 34, 0.10);
    s->x = 68 + rng_uniform() * 10; s->y = 85 + rng_uniform() * 30;
    s->heading = 78;
    init_radar(&s->search_radar, RADAR_S_BAND, 85, 26, 3.4, 48);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 55, 24, 9.8, 28);
    s->ecm_power_kw = 10;
    add_weapon(s, WPN_CRUISE_MISSILE, "Tomahawk UGM-109", 280, 0.78, 180, 1, 120, 12, 0.72, 1400, 0, 0.92);
    add_weapon(s, WPN_TORPEDO, "Mk 48 ADCAP", 22, 0.80, 240, 1, 95, 26, 55, 1663, 0, 0.74);

    /* ── Warsaw Pact Battle Group (spawns east side) ── */
    
    s = add_ship(SIDE_PACT, "Kuznetsov", "Pr.1143.5",
                 CLASS_CARRIER, 880, 0.22, 29, 1.3);
    s->x = 245 + rng_uniform() * 15; s->y = 135 + rng_uniform() * 20;
    s->heading = 270;
    init_radar(&s->search_radar, RADAR_S_BAND, 480, 43, 2.9, 175);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 190, 38, 5.5, 115);
    s->ecm_power_kw = 140;
    add_weapon(s, WPN_SSM, "P-700 Granit", 150, 0.45, 165, 2, 70, 12, 2.5, 7000, 0, 0.68);
    add_weapon(s, WPN_SAM, "Kinzhal SAM", 12, 0.56, 40, 2, 18, 192, 2.8, 330, 0, 0.76);
    add_weapon(s, WPN_CIWS, "AK-630M", 1.5, 0.54, 15, 1, 3, 3000, 900, 30, 80, 0.90);

    s = add_ship(SIDE_PACT, "Slava", "Pr.1164 Atlant",
                 CLASS_CRUISER, 340, 0.20, 32, 0.75);
    s->x = 238 + rng_uniform() * 20; s->y = 125 + rng_uniform() * 25;
    s->heading = 270;
    init_radar(&s->search_radar, RADAR_S_BAND, 360, 41, 3.0, 155);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 170, 36, 5.2, 100);
    s->ecm_power_kw = 75;
    add_weapon(s, WPN_SSM, "P-500 Bazalt", 140, 0.48, 165, 2, 65, 16, 2.5, 4800, 0, 0.70);
    add_weapon(s, WPN_SAM, "S-300F Fort", 80, 0.64, 50, 2, 26, 64, 6.0, 1800, 0, 0.84);
    add_weapon(s, WPN_GUN_130MM, "AK-130", 13, 0.44, 32, 5, 7, 560, 850, 33.4, 48, 0);
    add_weapon(s, WPN_CIWS, "AK-630", 1.2, 0.52, 14, 1, 3, 3200, 900, 30, 80, 0.88);

    s = add_ship(SIDE_PACT, "Kirov", "Pr.1144 Orlan",
                 CLASS_CRUISER, 450, 0.24, 32, 0.85);
    s->x = 242 + rng_uniform() * 18; s->y = 145 + rng_uniform() * 20;
    s->heading = 268;
    init_radar(&s->search_radar, RADAR_S_BAND, 420, 44, 2.8, 185);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 200, 39, 5.0, 125);
    s->ecm_power_kw = 110;
    add_weapon(s, WPN_CRUISE_MISSILE, "P-700 Granit", 155, 0.50, 165, 2, 75, 20, 2.5, 7000, 0, 0.72);
    add_weapon(s, WPN_SAM, "S-300F Fort", 80, 0.64, 50, 2, 26, 96, 6.0, 1800, 0, 0.84);
    add_weapon(s, WPN_GUN_130MM, "AK-130", 13, 0.44, 32, 5, 7, 640, 850, 33.4, 48, 0);
    add_weapon(s, WPN_CIWS, "Kashtan CIWS", 2.5, 0.60, 20, 1, 4, 2400, 900, 30, 82, 0.92);
    add_weapon(s, WPN_ASROC, "RPK-6 Vodopad", 25, 0.54, 115, 1, 75, 20, 2.0, 550, 0, 0.68);

    s = add_ship(SIDE_PACT, "Sovremenny", "Pr.956 Sarych",
                 CLASS_DESTROYER, 240, 0.14, 33, 0.60);
    s->x = 248 + rng_uniform() * 15; s->y = 160 + rng_uniform() * 15;
    s->heading = 265;
    init_radar(&s->search_radar, RADAR_S_BAND, 300, 38, 3.1, 115);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 145, 34, 5.4, 90);
    s->ecm_power_kw = 50;
    add_weapon(s, WPN_SSM, "P-270 Moskit", 65, 0.58, 145, 2, 52, 8, 3.0, 4500, 0, 0.74);
    add_weapon(s, WPN_SAM, "Shtil", 28, 0.54, 35, 1, 20, 48, 3.5, 715, 0, 0.72);
    add_weapon(s, WPN_GUN_130MM, "AK-130", 13, 0.44, 32, 5, 7, 560, 850, 33.4, 48, 0);
    add_weapon(s, WPN_CIWS, "AK-630", 1.2, 0.52, 14, 1, 3, 3200, 900, 30, 80, 0.88);

    s = add_ship(SIDE_PACT, "Udaloy", "Pr.1155 Fregat",
                 CLASS_DESTROYER, 225, 0.13, 32, 0.58);
    s->x = 246 + rng_uniform() * 14; s->y = 108 + rng_uniform() * 18;
    s->heading = 268;
    init_radar(&s->search_radar, RADAR_S_BAND, 285, 37, 3.0, 110);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 140, 33, 5.3, 85);
    s->ecm_power_kw = 42;
    add_weapon(s, WPN_TORPEDO, "SET-65", 15, 0.64, 150, 1, 70, 40, 45, 400, 0, 0.62);
    add_weapon(s, WPN_GUN_130MM, "AK-100", 11, 0.42, 30, 4, 8, 480, 900, 30, 45, 0);
    add_weapon(s, WPN_CIWS, "AK-630", 1.2, 0.52, 14, 1, 3, 3200, 900, 30, 80, 0.88);
    add_weapon(s, WPN_ASROC, "RPK-2 Viyuga", 35, 0.56, 120, 1, 72, 16, 2.2, 565, 0, 0.70);

    s = add_ship(SIDE_PACT, "Krivak II", "Pr.1135M",
                 CLASS_FRIGATE, 190, 0.11, 30, 0.50);
    s->x = 240 + rng_uniform() * 12; s->y = 92 + rng_uniform() * 18;
    s->heading = 272;
    init_radar(&s->search_radar, RADAR_S_BAND, 240, 35, 3.1, 92);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 125, 31, 5.5, 72);
    s->ecm_power_kw = 28;
    add_weapon(s, WPN_SSM, "P-270 Moskit", 65, 0.58, 145, 1, 60, 4, 3.0, 4500, 0, 0.74);
    add_weapon(s, WPN_SAM, "Osa-M", 12, 0.48, 28, 1, 18, 20, 2.4, 186, 0, 0.65);
    add_weapon(s, WPN_GUN_76MM, "AK-726", 9, 0.40, 22, 4, 7, 400, 900, 27, 50, 0);
    add_weapon(s, WPN_TORPEDO, "SET-53M", 8, 0.58, 125, 1, 75, 8, 40, 305, 0, 0.55);

    s = add_ship(SIDE_PACT, "Grisha V", "Pr.1124M",
                 CLASS_CORVETTE, 125, 0.08, 34, 0.38);
    s->x = 235 + rng_uniform() * 10; s->y = 75 + rng_uniform() * 15;
    s->heading = 270;
    init_radar(&s->search_radar, RADAR_S_BAND, 180, 32, 3.2, 68);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 95, 28, 5.6, 52);
    s->ecm_power_kw = 16;
    add_weapon(s, WPN_SAM, "Osa-M", 12, 0.48, 28, 1, 18, 20, 2.4, 186, 0, 0.65);
    add_weapon(s, WPN_GUN_76MM, "AK-176", 8, 0.38, 20, 5, 6, 350, 900, 25, 52, 0);
    add_weapon(s, WPN_TORPEDO, "SET-40", 5, 0.52, 110, 1, 80, 4, 35, 180, 0, 0.48);

    s = add_ship(SIDE_PACT, "Nanuchka III", "Pr.1234.1 Ovod",
                 CLASS_MISSILE_BOAT, 95, 0.06, 35, 0.32);
    s->x = 232 + rng_uniform() * 10; s->y = 98 + rng_uniform() * 15;
    s->heading = 272;
    init_radar(&s->search_radar, RADAR_S_BAND, 140, 30, 3.3, 55);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 85, 26, 5.7, 42);
    s->ecm_power_kw = 12;
    add_weapon(s, WPN_SSM, "P-120 Malakhit", 45, 0.52, 110, 2, 48, 6, 0.95, 870, 0, 0.70);
    add_weapon(s, WPN_SAM, "Osa-M", 12, 0.48, 28, 1, 18, 16, 2.4, 186, 0, 0.65);
    add_weapon(s, WPN_GUN_76MM, "AK-176", 8, 0.38, 20, 5, 6, 320, 900, 25, 52, 0);

    s = add_ship(SIDE_PACT, "Tarantul III", "Pr.1241.1MP",
                 CLASS_MISSILE_BOAT, 85, 0.05, 38, 0.28);
    s->x = 234 + rng_uniform() * 8; s->y = 118 + rng_uniform() * 12;
    s->heading = 270;
    init_radar(&s->search_radar, RADAR_S_BAND, 120, 28, 3.4, 48);
    init_radar(&s->fire_control_radar, RADAR_C_BAND, 75, 24, 5.8, 36);
    s->ecm_power_kw = 10;
    add_weapon(s, WPN_SSM, "P-270 Moskit", 65, 0.58, 145, 2, 55, 4, 3.0, 4500, 0, 0.74);
    add_weapon(s, WPN_GUN_76MM, "AK-176", 8, 0.38, 20, 5, 6, 280, 900, 25, 52, 0);
    add_weapon(s, WPN_CIWS, "AK-630", 1.2, 0.52, 14, 1, 3, 2000, 900, 30, 80, 0.88);

    s = add_ship(SIDE_PACT, "Victor III", "Pr.671RTM Shchuka",
                 CLASS_SUBMARINE, 170, 0.06, 30, 0.09);
    s->x = 252 + rng_uniform() * 10; s->y = 68 + rng_uniform() * 35;
    s->heading = 262;
    init_radar(&s->search_radar, RADAR_S_BAND, 75, 24, 3.6, 42);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 48, 20, 10.2, 22);
    s->ecm_power_kw = 6;
    add_weapon(s, WPN_SSM, "P-70 Ametist", 35, 0.48, 98, 1, 58, 8, 0.95, 560, 0, 0.65);
    add_weapon(s, WPN_TORPEDO, "USET-80", 18, 0.68, 195, 1, 85, 18, 50, 830, 0, 0.66);

    s = add_ship(SIDE_PACT, "Akula", "Pr.971 Shchuka-B",
                 CLASS_SUBMARINE, 185, 0.07, 32, 0.08);
    s->x = 256 + rng_uniform() * 10; s->y = 82 + rng_uniform() * 32;
    s->heading = 258;
    init_radar(&s->search_radar, RADAR_S_BAND, 80, 25, 3.5, 45);
    init_radar(&s->fire_control_radar, RADAR_X_BAND, 52, 22, 10.0, 24);
    s->ecm_power_kw = 7;
    add_weapon(s, WPN_CRUISE_MISSILE, "3M-54 Kalibr", 160, 0.58, 170, 1, 110, 8, 0.80, 1400, 0, 0.75);
    add_weapon(s, WPN_TORPEDO, "USET-80", 18, 0.70, 195, 1, 85, 28, 50, 830, 0, 0.68);
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
    const char *names[] = {"CV","CG","DD","FF","FFL","SS","PGG","LPD"};
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

            double d = dist_nm(&ships[i], &ships[j]);
            
            /* Advanced radar detection with ECM/ESM */
            RadarSystem *radar = &ships[i].search_radar;
            double radar_range = radar->range_nm;
            
            /* Apply ECM jamming effects */
            if (ships[j].ecm_power_kw > 0 && !radar->jammed) {
                double jam_effectiveness = ships[j].ecm_power_kw / (radar->power_kw + 1.0);
                if (jam_effectiveness > 0.3) {
                    radar_range *= (1.0 - jam_effectiveness * 0.5);
                    radar->jam_strength = jam_effectiveness;
                }
            }
            
            /* Radar equation: range based on power, gain, RCS */
            double eff_range = radar_range * pow(ships[j].rcs, 0.25);
            
            /* Multipath effects over water */
            if (d < 15) { /* Close range */
                eff_range *= rng_gauss(1.0, 0.08); /* Sea clutter noise */
            }
            
            if (d < eff_range) {
                /* Count this as a detection opportunity for the detector side */
                stat_detect_opps[ships[i].side]++;
                if (radar->jam_strength > 0.3) stat_detect_jammed[ships[i].side]++;

                double p_detect = 1.0 - pow(d / eff_range, 2.5);

                /* Frequency-dependent detection */
                if (radar->band == RADAR_L_BAND) {
                    p_detect *= 0.85; /* Lower resolution but longer range */
                } else if (radar->band == RADAR_X_BAND) {
                    p_detect *= 1.15; /* Better resolution */
                }

                /* Submarines are much harder to detect */
                if (ships[j].ship_class == CLASS_SUBMARINE)
                    p_detect *= 0.15;

                /* Weather effects */
                p_detect *= rng_gauss(1.0, 0.12);

                /* Clamp probability */
                if (p_detect < 0.01) p_detect = 0.01;
                if (p_detect > 0.98) p_detect = 0.98;

                if (rng_uniform() < p_detect) {
                    /* Successful detection */
                    stat_detect_success[ships[i].side]++;
                    ships[j].detected = 1;
                    if (tick % 30 == 0 || tick < 60) {
                        const char *jam_note = (radar->jam_strength > 0.3) ? " [JAMMED]" : "";
                        printf("  " ANSI_CYAN "[DETECT]" ANSI_RESET
                               " %s %s contacts %s %s at %.1f NM, brg %03.0f%s%s\n",
                               side_str(ships[i].side), ships[i].name,
                               side_str(ships[j].side), ships[j].name,
                               d, bearing_deg(&ships[i], &ships[j]),
                               jam_note, ANSI_RESET);
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
    /* Launch new projectiles */
    for (int i = 0; i < num_ships; i++) {
        Ship *atk = &ships[i];
        if (!atk->alive) continue;

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

                /* Target prioritization */
                double score = (wpn->range_nm - d) / wpn->range_nm;
                if (wpn->type == WPN_TORPEDO && ships[j].ship_class == CLASS_SUBMARINE)
                    score += 0.6;
                if ((wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE) &&
                    ships[j].ship_class == CLASS_CARRIER)
                    score += 0.5;
                if ((wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE) &&
                    ships[j].ship_class == CLASS_CRUISER)
                    score += 0.35;

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

            /* Launch projectiles */
            int launched = 0;
            for (int r = 0; r < rounds && num_projectiles < MAX_PROJECTILES; r++) {
                init_projectile(&projectiles[num_projectiles], atk, def, wpn, tick);
                num_projectiles++;
                launched++;
            }
            /* Track actual launches by side */
            stat_launches[atk->side] += launched;

            printf("  " ANSI_YELLOW "[LAUNCH]" ANSI_RESET " %s %s -> %s %s | "
                   "%s x%d @ %.1f NM\n",
                   side_str(atk->side), atk->name,
                   side_str(def->side), def->name,
                   wpn->name, rounds, d);
        }
    }
    
    /* Update projectiles and check for impacts */
    for (int p = 0; p < num_projectiles; p++) {
        if (!projectiles[p].active) continue;

        update_projectile_physics(&projectiles[p], 1.0);

        /* If projectile deactivated by physics (water / out-of-bounds), count as a miss */
        if (!projectiles[p].active) {
            Side attacker_side = SIDE_NATO;
            for (int si = 0; si < num_ships; si++) {
                if (strcmp(ships[si].name, projectiles[p].attacker) == 0) {
                    attacker_side = ships[si].side;
                    break;
                }
            }
            stat_misses[attacker_side]++;
            if (num_records < 8192) {
                EngagementRecord *rec = &records[num_records++];
                rec->tick = tick;
                strncpy(rec->attacker, projectiles[p].attacker, MAX_NAME - 1);
                strncpy(rec->defender, projectiles[p].target, MAX_NAME - 1);
                strncpy(rec->weapon, projectiles[p].weapon_name, MAX_NAME - 1);
                rec->hit = 0;
                rec->damage = 0.0;
                rec->defender_hp_after = -1.0;
                rec->kill = 0;
            }
            continue;
        }

        /* Check for impacts with ships */
        for (int i = 0; i < num_ships; i++) {
            if (!ships[i].alive) continue;
            if (strcmp(ships[i].name, projectiles[p].attacker) == 0) continue;

            /* Find attacker's side and skip friendly ships */
            Side attacker_side = SIDE_NATO;
            for (int si = 0; si < num_ships; si++) {
                if (strcmp(ships[si].name, projectiles[p].attacker) == 0) {
                    attacker_side = ships[si].side;
                    break;
                }
            }
            if (ships[i].side == attacker_side) continue;

            double dx = projectiles[p].x - ships[i].x;
            double dy = projectiles[p].y - ships[i].y;
            double dist = sqrt(dx * dx + dy * dy);

            /* Hit radius: 0.05 NM = ~100 yards */
            double hit_radius = 0.05;
            if (ships[i].ship_class == CLASS_CARRIER) hit_radius = 0.15;
            else if (ships[i].ship_class == CLASS_CRUISER) hit_radius = 0.10;

            if (dist < hit_radius) {
                /* Impact! */
                double dmg = projectiles[p].damage;

                /* Apply armor */
                dmg *= (1.0 - ships[i].armor);

                /* Guided weapons are more accurate */
                Weapon *orig_wpn = NULL;
                for (int si = 0; si < num_ships; si++) {
                    if (strcmp(ships[si].name, projectiles[p].attacker) != 0) continue;
                    for (int w = 0; w < ships[si].num_weapons; w++) {
                        if (strcmp(ships[si].weapons[w].name, projectiles[p].weapon_name) == 0) {
                            orig_wpn = &ships[si].weapons[w];
                            break;
                        }
                    }
                }

                /* Guidance quality affects damage */
                if (orig_wpn && orig_wpn->guidance_quality > 0.7) {
                    dmg *= rng_gauss(1.0, 0.10);
                } else {
                    dmg *= rng_gauss(1.0, 0.25);
                }

                /* Damage compartments */
                int comp_hit = (int)(rng_uniform() * MAX_COMPARTMENTS);
                if (comp_hit < MAX_COMPARTMENTS) {
                    ships[i].compartments[comp_hit].integrity -= dmg / ships[i].max_hp;
                    if (ships[i].compartments[comp_hit].integrity < 0.3) {
                        ships[i].compartments[comp_hit].flooding = 1;
                        ships[i].compartments[comp_hit].flood_rate = rng_uniform() * 20;
                    }
                    if (rng_uniform() < 0.25) {
                        ships[i].compartments[comp_hit].on_fire = 1;
                    }
                }

                if (dmg < 1) dmg = 1;
                ships[i].hp -= dmg;

                /* Find attacker for stats and add hit/damage */
                for (int si = 0; si < num_ships; si++) {
                    if (strcmp(ships[si].name, projectiles[p].attacker) == 0) {
                        ships[si].damage_dealt_total += (int)dmg;
                        break;
                    }
                }
                stat_hits[attacker_side]++;

                int killed = 0;
                if (ships[i].hp <= 0) {
                    ships[i].hp = 0;
                    ships[i].alive = 0;
                    killed = 1;
                    /* Award kill to attacker */
                    for (int si = 0; si < num_ships; si++) {
                        if (strcmp(ships[si].name, projectiles[p].attacker) == 0) {
                            ships[si].kills++;
                            break;
                        }
                    }
                    stat_kills_by_side[attacker_side]++;
                }

                printf("  " ANSI_RED "[IMPACT]" ANSI_RESET " %s -> %s | "
                       "%.0f dmg",
                       projectiles[p].weapon_name, ships[i].name, dmg);
                if (killed)
                    printf(" " ANSI_BOLD ANSI_RED "*** SUNK ***" ANSI_RESET);
                printf(" [HP: %.0f/%.0f]\n", ships[i].hp, ships[i].max_hp);

                /* Record engagement */
                if (num_records < 8192) {
                    EngagementRecord *rec = &records[num_records++];
                    rec->tick = tick;
                    strncpy(rec->attacker, projectiles[p].attacker, MAX_NAME - 1);
                    strncpy(rec->defender, ships[i].name, MAX_NAME - 1);
                    strncpy(rec->weapon, projectiles[p].weapon_name, MAX_NAME - 1);
                    rec->hit = 1;
                    rec->damage = dmg;
                    rec->defender_hp_after = ships[i].hp;
                    rec->kill = killed;
                }

                projectiles[p].active = 0;
                break;
            }
        }
    }
    
    /* Clean up inactive projectiles periodically */
    if (tick % 30 == 0) {
        int active_count = 0;
        for (int p = 0; p < num_projectiles; p++) {
            if (projectiles[p].active) {
                if (p != active_count) {
                    projectiles[active_count] = projectiles[p];
                }
                active_count++;
            }
        }
        num_projectiles = active_count;
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

        /* Aggregate additional statistics for After-Action Report */
        int launches_nato = stat_launches[SIDE_NATO];
        int launches_pact = stat_launches[SIDE_PACT];
        int hits_nato = stat_hits[SIDE_NATO];
        int hits_pact = stat_hits[SIDE_PACT];
        int misses_nato = stat_misses[SIDE_NATO];
        int misses_pact = stat_misses[SIDE_PACT];
        int in_flight_nato = launches_nato - hits_nato - misses_nato;
        int in_flight_pact = launches_pact - hits_pact - misses_pact;

        int damage_nato = 0, damage_pact = 0;
        int kills_nato = 0, kills_pact = 0;
        for (int i = 0; i < num_ships; i++) {
         if (ships[i].side == SIDE_NATO) {
             damage_nato += ships[i].damage_dealt_total;
             kills_nato += ships[i].kills;
         } else {
             damage_pact += ships[i].damage_dealt_total;
             kills_pact += ships[i].kills;
         }
        }

        /* Detection summary */
        int det_opps_n = stat_detect_opps[SIDE_NATO];
        int det_opps_p = stat_detect_opps[SIDE_PACT];
        int det_succ_n = stat_detect_success[SIDE_NATO];
        int det_succ_p = stat_detect_success[SIDE_PACT];
        int det_jam_n = stat_detect_jammed[SIDE_NATO];
        int det_jam_p = stat_detect_jammed[SIDE_PACT];

        int nato_ships_total = 0, pact_ships_total = 0;
        int nato_ships_detected = 0, pact_ships_detected = 0;
        for (int i = 0; i < num_ships; i++) {
         if (ships[i].side == SIDE_NATO) {
             nato_ships_total++;
             if (ships[i].detected) nato_ships_detected++;
         } else {
             pact_ships_total++;
             if (ships[i].detected) pact_ships_detected++;
         }
        }

        double nato_hit_rate = launches_nato > 0 ? (double)hits_nato / launches_nato * 100.0 : 0.0;
        double pact_hit_rate = launches_pact > 0 ? (double)hits_pact / launches_pact * 100.0 : 0.0;
        double nato_avg_dmg = hits_nato > 0 ? (double)damage_nato / hits_nato : 0.0;
        double pact_avg_dmg = hits_pact > 0 ? (double)damage_pact / hits_pact : 0.0;

        printf("\n  Combat Statistics:\n");
        printf("    NATO: launches=%d  hits=%d (%.1f%%)  misses=%d  in-flight=%d\n",
            launches_nato, hits_nato, nato_hit_rate, misses_nato, in_flight_nato);
        printf("          damage_dealt=%d  avg_dmg/hit=%.1f  kills=%d\n",
            damage_nato, nato_avg_dmg, kills_nato);
        printf("          detections: attempts=%d  successes=%d (%.1f%%)  jammed_attempts=%d\n",
            det_opps_n, det_succ_n, det_opps_n > 0 ? (double)det_succ_n / det_opps_n * 100.0 : 0.0, det_jam_n);
        printf("          ships detected: %d/%d (%.1f%%)\n",
            nato_ships_detected, nato_ships_total, nato_ships_total > 0 ? (double)nato_ships_detected / nato_ships_total * 100.0 : 0.0);

        printf("\n    PACT: launches=%d  hits=%d (%.1f%%)  misses=%d  in-flight=%d\n",
            launches_pact, hits_pact, pact_hit_rate, misses_pact, in_flight_pact);
        printf("          damage_dealt=%d  avg_dmg/hit=%.1f  kills=%d\n",
            damage_pact, pact_avg_dmg, kills_pact);
        printf("          detections: attempts=%d  successes=%d (%.1f%%)  jammed_attempts=%d\n",
            det_opps_p, det_succ_p, det_opps_p > 0 ? (double)det_succ_p / det_opps_p * 100.0 : 0.0, det_jam_p);
        printf("          ships detected: %d/%d (%.1f%%)\n",
            pact_ships_detected, pact_ships_total, pact_ships_total > 0 ? (double)pact_ships_detected / pact_ships_total * 100.0 : 0.0);

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
    "    Advanced Naval Combat Simulator with Realistic Ballistics & Radar\n"
    "    Physics Engine v2.0  │  RNG Seed: %u\n"
    ANSI_RESET "\n", seed);

    build_scenario();

    printf(ANSI_BOLD "  SCENARIO: North Atlantic Battle Group Engagement\n" ANSI_RESET);
    printf("  NATO CSG: Nimitz CVN, Ticonderoga CG, Arleigh Burke DDG, Spruance DD,\n");
    printf("            Oliver H. Perry FFG, Knox FF, Independence LCS, Absalon FF,\n");
    printf("            Los Angeles SSN, Virginia SSN\n");
    printf("  PACT BG:  Kuznetsov CV, Slava CG, Kirov CGN, Sovremenny DD, Udaloy DD,\n");
    printf("            Krivak II FF, Grisha V FFL, Nanuchka III PGG, Tarantul III PGG,\n");
    printf("            Victor III SSN, Akula SSN\n\n");
    printf("  " ANSI_CYAN "Features:" ANSI_RESET " Realistic ballistics • Advanced radar modeling\n");
    printf("             ECM/ESM warfare • Compartmentalized damage • Physics-based projectiles\n\n");

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