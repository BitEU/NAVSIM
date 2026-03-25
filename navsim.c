/*
 * NAVSIM - Cold War Naval Tactical Engagement Simulator v4.0
 * ===========================================================
 * Atlantic Pole of Inaccessibility scenario: 24.1851°N 43.3704°W
 * Deep open-ocean, no land within 2033 km. No land-based aviation.
 *
 * New in v4.0:
 *   - Air-to-air combat: CAP sorties intercept enemy STRIKE sorties
 *   - Helicopter sorties: SH-60 Seahawk ASW from surface combatants
 *   - C4ISR / Link-16 / CEC: NATO shared detection picture
 *   - HARM anti-radiation missiles: home on active radar emitters
 *   - Frequency hopping: jammed radars can switch bands
 *   - Communications jamming: PACT ECM degrades Link-16
 *   - Full ncurses TUI with tactical map, event log, ship panels
 *
 * Retained from v3.0:
 *   - Aircraft carrier air wings with CAP/STRIKE/ASW/AEW sorties
 *   - Layered air defense: long-range SAM -> point SAM -> CIWS
 *   - CIWS intercept with barrel wear
 *   - Chaff/flares deployed against incoming missiles
 *   - Proper submarine warfare: sonar detection only, no radar
 *   - Sonar systems: passive/active/towed array per ship class
 *   - Torpedo decoys (Nixie)
 *   - Compartment damage degrades ship systems
 *   - Fire spreading between compartments
 *   - Progressive flooding -> capsizing
 *   - Weather system (Beaufort 0-6) affecting sensors and ASW
 *   - All ship/weapon stats from data/platforms.csv and data/weapons.csv
 *
 * (C) 2026 - PUBLIC DOMAIN / GPL-3.0 Compatible
 */

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#endif

/* ncurses */
#define NCURSES_WIDECHAR 1
#include <ncurses.h>
/* panel.h removed — using plain wnoutrefresh/doupdate instead */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Configuration ───────────────────────────────────────── */
#define MAX_SHIPS         32
#define MAX_WEAPONS        8
#define MAX_NAME          32
#define MAX_TICK        3600   /* 60 minutes */
#define GRID_SIZE        300
#define MAX_PROJECTILES 1024
#define MAX_COMPARTMENTS  12
#define MAX_SORTIES       16   /* per carrier */
#define MAX_WPN_TEMPLATES 64
#define MAX_EVENTS       512   /* rolling event log for TUI */
#define LOG_FILE    "battle_log.csv"

/* ── RNG (xoshiro128**) ──────────────────────────────────── */
static uint32_t rng_state[4];
static inline uint32_t rotl(const uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static uint32_t xoshiro128ss(void) {
    const uint32_t result = rotl(rng_state[1] * 5, 7) * 9;
    const uint32_t t = rng_state[1] << 9;
    rng_state[2] ^= rng_state[0]; rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2]; rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t; rng_state[3] = rotl(rng_state[3], 11);
    return result;
}
static void rng_seed(uint32_t seed) {
    for (int i = 0; i < 4; i++) {
        seed += 0x9e3779b9;
        uint32_t z = seed;
        z = (z ^ (z >> 16)) * 0x85ebca6b;
        z = (z ^ (z >> 13)) * 0xc2b2ae35;
        z = z ^ (z >> 16);
        rng_state[i] = z;
    }
}
static double rng_uniform(void) { return (xoshiro128ss() >> 8) * (1.0 / 16777216.0); }
static double rng_gauss(double mu, double sigma) {
    double u1 = rng_uniform(), u2 = rng_uniform();
    if (u1 < 1e-15) u1 = 1e-15;
    return mu + sigma * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ── Enums ───────────────────────────────────────────────── */
typedef enum { SIDE_NATO = 0, SIDE_PACT = 1 } Side;
typedef enum {
    CLASS_CARRIER, CLASS_CRUISER, CLASS_DESTROYER, CLASS_FRIGATE,
    CLASS_CORVETTE, CLASS_SUBMARINE, CLASS_MISSILE_BOAT, CLASS_AMPHIBIOUS
} ShipClass;
typedef enum {
    WPN_SSM, WPN_SAM, WPN_TORPEDO, WPN_GUN_5IN,
    WPN_GUN_130MM, WPN_GUN_76MM, WPN_GUN_ADVANCED,
    WPN_RAILGUN, WPN_CIWS, WPN_CRUISE_MISSILE, WPN_ASROC, WPN_HARM
} WeaponType;
typedef enum { RADAR_S_BAND, RADAR_X_BAND, RADAR_L_BAND, RADAR_C_BAND } RadarBand;
typedef enum {
    COMP_BOW, COMP_BRIDGE, COMP_FORWARD_WEAPONS, COMP_MIDSHIP_PORT,
    COMP_MIDSHIP_STBD, COMP_ENGINE, COMP_AFT_WEAPONS, COMP_STERN,
    COMP_RADAR, COMP_COMMS, COMP_MAGAZINE, COMP_KEEL
} Compartment;
typedef enum { SORTIE_CAP, SORTIE_STRIKE, SORTIE_ASW, SORTIE_AEW, SORTIE_HELO_ASW } SortieType;
typedef enum {
    EVT_HIT, EVT_MISS, EVT_CIWS_INTERCEPT, EVT_SAM_INTERCEPT,
    EVT_CHAFF_SEDUCE, EVT_AIR_STRIKE, EVT_ASW_ATTACK,
    EVT_TORPEDO_DECOY, EVT_CAPSIZE, EVT_AIR_TO_AIR, EVT_HARM_STRIKE,
    EVT_LINK16_SHARE, EVT_COMMS_JAM
} EventType;

/* ── Data Structures ─────────────────────────────────────── */
typedef struct {
    int        active;
    SortieType type;
    int        launch_tick;
    int        return_tick;
    int        target_ship_idx;
    int        effect_applied;
    double     x, y;          /* sortie position for map display */
    int        destroyed;     /* shot down by CAP */
} AircraftSortie;

typedef struct {
    double passive_range_nm;
    double active_range_nm;
    int    active_on;
    int    has_towed_array;
    double towed_range_nm;
} SonarSystem;

typedef struct {
    RadarBand  band;
    double     power_kw;
    double     gain_db;
    double     frequency_ghz;
    double     range_nm;
    double     base_range_nm;    /* original range for freq hop restore */
    double     azimuth_res;
    int        jammed;
    double     jam_strength;
    int        freq_hop_capable; /* can switch bands when jammed */
    int        freq_hop_cooldown;
    int        emitting;         /* actively radiating (HARM target) */
} RadarSystem;

typedef struct {
    Compartment type;
    double      integrity;
    int         flooding;
    int         on_fire;
    double      flood_rate;
} DamageCompartment;

typedef struct {
    WeaponType type;
    char       name[MAX_NAME];
    double     range_nm;
    double     p_hit;
    double     damage;
    int        salvo_size;
    int        reload_ticks;
    int        ammo;
    int        cooldown;
    double     muzzle_velocity_mps;
    double     projectile_mass_kg;
    double     elevation_angle;
    double     guidance_quality;
    int        is_ciws;
    int        is_sam;
    int        is_anti_sub;
    int        is_anti_air;
    int        is_sea_skimmer_capable;
    int        is_harm;          /* anti-radiation missile */
    int        rounds_fired_total;
    int        overheated;
    int        overheat_cooldown;
    int        magazine_limit;
    int        mount_position;
} Weapon;

typedef struct {
    char       name[MAX_NAME];
    char       hull_class[MAX_NAME];
    Side       side;
    ShipClass  ship_class;
    double     x, y;
    double     heading;
    double     speed_kts;
    double     max_speed_kts;
    double     base_max_speed_kts;
    double     hp, max_hp;
    double     armor;
    double     rcs;
    int        detected;
    int        detected_by_link16; /* detected via data link, not own sensors */
    int        is_submerged;

    RadarSystem  search_radar;
    RadarSystem  fire_control_radar;
    SonarSystem  sonar;

    double     ecm_power_kw;
    double     esm_sensitivity;
    int        chaff_charges;
    int        flare_charges;
    int        nixie_charges;

    /* C4ISR */
    int        has_link16;        /* NATO data link */
    int        link16_degraded;   /* comms jamming effect */
    double     link16_quality;    /* 0-1, 1=perfect sharing */

    DamageCompartment compartments[MAX_COMPARTMENTS];
    int        crew_casualties;
    double     flooding_total;
    double     list_angle;
    int        speed_reduced;

    double     radar_effectiveness;
    double     targeting_effectiveness;
    int        fwd_weapons_disabled;
    int        aft_weapons_disabled;

    Weapon     weapons[MAX_WEAPONS];
    int        num_weapons;

    /* Aviation (carriers only) */
    int        max_sorties;
    int        sorties_available;
    int        sorties_in_flight;
    int        stobar;
    AircraftSortie sorties[MAX_SORTIES];

    /* Helicopter capability (non-carrier surface ships) */
    int        helo_capacity;     /* number of embarked helicopters */
    int        helos_available;
    int        helos_in_flight;
    AircraftSortie helo_sorties[4]; /* max 4 helo slots */

    int        alive;
    int        kills;
    int        damage_dealt_total;
} Ship;

typedef struct {
    int        active;
    char       weapon_name[MAX_NAME];
    char       attacker[MAX_NAME];
    char       target[MAX_NAME];
    int        attacker_idx;
    double     x, y, z;
    double     vx, vy, vz;
    double     launch_time;
    double     mass_kg;
    double     drag_coeff;
    double     damage;
    WeaponType type;
    int        is_missile;
    int        is_torpedo;
    int        is_sea_skimmer;
    int        is_harm;         /* homes on radar emitters */
    int        harm_target_idx; /* ship index of emitter */
    int        intercepted;
} Projectile;

typedef struct {
    int       tick;
    char      attacker[MAX_NAME];
    char      defender[MAX_NAME];
    char      weapon[MAX_NAME];
    int       hit;
    double    damage;
    double    defender_hp_after;
    int       kill;
    EventType event_type;
    int       sea_state;
} EngagementRecord;

typedef struct {
    char       name[MAX_NAME];
    WeaponType type;
    double     range_nm;
    double     p_hit;
    double     damage;
    int        salvo_size;
    int        reload_ticks;
    int        ammo;
    double     muzzle_velocity_mps;
    double     projectile_mass_kg;
    double     elevation_angle;
    double     guidance_quality;
    int        is_anti_air;
    int        is_ciws;
    int        is_sam;
    int        is_anti_sub;
    int        is_sea_skimmer;
    int        is_harm;
    int        magazine_limit;
    int        mount_position;
} WeaponTemplate;

/* ── Forward Declarations ────────────────────────────────── */
static double dist_nm(const Ship *a, const Ship *b);
static double bearing_deg(const Ship *from, const Ship *to);
static void   log_event(int tick, const char *atk, const char *def,
                        const char *wpn, int hit, double dmg,
                        double hp_after, int kill, EventType et);

/* ── Globals ─────────────────────────────────────────────── */
static Ship          ships[MAX_SHIPS];
static int           num_ships = 0;
static Projectile    projectiles[MAX_PROJECTILES];
static int           num_projectiles = 0;
static EngagementRecord records[16384];
static int           num_records = 0;
static WeaponTemplate wpn_templates[MAX_WPN_TEMPLATES];
static int           num_wpn_templates = 0;
static uint32_t      g_seed = 0;

/* Rolling event log for TUI */
typedef struct {
    int  tick;
    char text[160];
    int  color_pair; /* ncurses color pair */
} EventLogEntry;
static EventLogEntry event_log[MAX_EVENTS];
static int           event_log_head = 0;
static int           event_log_count = 0;

/* Statistics */
static int stat_launches[2];
static int stat_hits[2];
static int stat_misses[2];
static int stat_detect_opps[2];
static int stat_detect_success[2];
static int stat_detect_jammed[2];
static int stat_kills_by_side[2];
static int stat_ciws_intercepts[2];
static int stat_sam_intercepts[2];
static int stat_chaff_seductions[2];
static int stat_torpedo_decoys[2];
static int stat_air_strikes[2];
static int stat_asw_attacks[2];
static int stat_capsized[2];
static int stat_a2a_kills[2];
static int stat_harm_hits[2];
static int stat_link16_shares[2];
static int stat_comms_jammed[2];
static int stat_freq_hops[2];
static int stat_helo_asw[2];

/* Weather */
static int    sea_state = 1;
static int    sea_state_timer = 600;
static double wx_radar   = 1.0;
static double wx_small   = 1.0;
static double wx_asw_helo = 1.0;
static double wx_ir      = 1.0;

static const char *beaufort_names[] = {
    "Calm","Light Air","Light Breeze","Gentle Breeze",
    "Moderate Breeze","Fresh Breeze","Strong Breeze"
};

/* Physics constants */
#define AIR_DENSITY_SL  1.225
#define GRAVITY         9.81
#define SEA_WATER_DENSITY 1025.0

/* ── TUI globals ─────────────────────────────────────────── */
static WINDOW *win_map = NULL;
static WINDOW *win_log = NULL;
static WINDOW *win_nato = NULL;
static WINDOW *win_pact = NULL;
static WINDOW *win_stats = NULL;
static WINDOW *win_header = NULL;
/* no panels — using wnoutrefresh/doupdate */

/* Color pairs */
#define CP_NORMAL    0
#define CP_NATO      1
#define CP_PACT      2
#define CP_HEADER    3
#define CP_ALERT     4
#define CP_DIM       5
#define CP_CYAN      6
#define CP_MAGENTA   7
#define CP_YELLOW    8
#define CP_WHITE     9
#define CP_MAP_BG   10
#define CP_MAP_NATO 11
#define CP_MAP_PACT 12
#define CP_MAP_DEAD 13
#define CP_MAP_PROJ 14
#define CP_MAP_AIR  15
#define CP_BORDER   16
#define CP_SPEED    17
#define CP_HELP_KEY 18
#define CP_HELP_DESC 19
#define CP_HP_HIGH  20
#define CP_HP_MED   21
#define CP_HP_LOW   22
#define CP_TITLE_BAR 23
#define CP_MODE_PLAY  24
#define CP_MODE_PAUSE 25

/* ── Playback / Speed Control ────────────────────────────── */
typedef enum { MODE_PLAY, MODE_PAUSED } PlayMode;

static PlayMode play_mode = MODE_PLAY;
static int      sim_speed = 2;       /* index into speed tables */
static int      view_snap_idx = -1;  /* -1 = live (latest) */
static int      show_help = 0;

static const int  speed_delays[] = { 200, 100, 50, 25, 10, 0 };
static const char *speed_labels[] = {"0.25x","0.5x","1x","2x","4x","MAX"};
#define NUM_SPEEDS 6

/* ── Snapshot System ─────────────────────────────────────── */
#define MAX_SNAPSHOTS 3600

typedef struct {
    int            tick;
    /* Ships */
    Ship           ships[MAX_SHIPS];
    int            num_ships;
    /* Projectiles */
    Projectile     projectiles[MAX_PROJECTILES];
    int            num_projectiles;
    /* Event log */
    EventLogEntry  event_log[MAX_EVENTS];
    int            event_log_head;
    int            event_log_count;
    /* Weather */
    int            sea_state;
    int            sea_state_timer;
    double         wx_radar, wx_small, wx_asw_helo, wx_ir;
    /* RNG */
    uint32_t       rng_state[4];
    /* Engagement records count (for trimming on rewind) */
    int            num_records;
    /* Statistics (flat copy of all stat_ arrays) */
    int            stat_launches[2];
    int            stat_hits[2];
    int            stat_misses[2];
    int            stat_detect_opps[2];
    int            stat_detect_success[2];
    int            stat_detect_jammed[2];
    int            stat_kills_by_side[2];
    int            stat_ciws_intercepts[2];
    int            stat_sam_intercepts[2];
    int            stat_chaff_seductions[2];
    int            stat_torpedo_decoys[2];
    int            stat_air_strikes[2];
    int            stat_asw_attacks[2];
    int            stat_capsized[2];
    int            stat_a2a_kills[2];
    int            stat_harm_hits[2];
    int            stat_link16_shares[2];
    int            stat_comms_jammed[2];
    int            stat_freq_hops[2];
    int            stat_helo_asw[2];
} Snapshot;

static Snapshot *snapshots = NULL;
static int       snap_count = 0;

static void snap_save(int tick) {
    if (!snapshots) return;
    if (snap_count >= MAX_SNAPSHOTS) return; /* buffer full */
    Snapshot *s = &snapshots[snap_count];
    s->tick = tick;
    memcpy(s->ships, ships, sizeof(ships));
    s->num_ships = num_ships;
    memcpy(s->projectiles, projectiles, sizeof(projectiles));
    s->num_projectiles = num_projectiles;
    memcpy(s->event_log, event_log, sizeof(event_log));
    s->event_log_head = event_log_head;
    s->event_log_count = event_log_count;
    s->sea_state = sea_state;
    s->sea_state_timer = sea_state_timer;
    s->wx_radar = wx_radar;
    s->wx_small = wx_small;
    s->wx_asw_helo = wx_asw_helo;
    s->wx_ir = wx_ir;
    memcpy(s->rng_state, rng_state, sizeof(rng_state));
    s->num_records = num_records;
    /* Stats */
    memcpy(s->stat_launches, stat_launches, sizeof(stat_launches));
    memcpy(s->stat_hits, stat_hits, sizeof(stat_hits));
    memcpy(s->stat_misses, stat_misses, sizeof(stat_misses));
    memcpy(s->stat_detect_opps, stat_detect_opps, sizeof(stat_detect_opps));
    memcpy(s->stat_detect_success, stat_detect_success, sizeof(stat_detect_success));
    memcpy(s->stat_detect_jammed, stat_detect_jammed, sizeof(stat_detect_jammed));
    memcpy(s->stat_kills_by_side, stat_kills_by_side, sizeof(stat_kills_by_side));
    memcpy(s->stat_ciws_intercepts, stat_ciws_intercepts, sizeof(stat_ciws_intercepts));
    memcpy(s->stat_sam_intercepts, stat_sam_intercepts, sizeof(stat_sam_intercepts));
    memcpy(s->stat_chaff_seductions, stat_chaff_seductions, sizeof(stat_chaff_seductions));
    memcpy(s->stat_torpedo_decoys, stat_torpedo_decoys, sizeof(stat_torpedo_decoys));
    memcpy(s->stat_air_strikes, stat_air_strikes, sizeof(stat_air_strikes));
    memcpy(s->stat_asw_attacks, stat_asw_attacks, sizeof(stat_asw_attacks));
    memcpy(s->stat_capsized, stat_capsized, sizeof(stat_capsized));
    memcpy(s->stat_a2a_kills, stat_a2a_kills, sizeof(stat_a2a_kills));
    memcpy(s->stat_harm_hits, stat_harm_hits, sizeof(stat_harm_hits));
    memcpy(s->stat_link16_shares, stat_link16_shares, sizeof(stat_link16_shares));
    memcpy(s->stat_comms_jammed, stat_comms_jammed, sizeof(stat_comms_jammed));
    memcpy(s->stat_freq_hops, stat_freq_hops, sizeof(stat_freq_hops));
    memcpy(s->stat_helo_asw, stat_helo_asw, sizeof(stat_helo_asw));
    snap_count++;
}

static void snap_restore(int idx) {
    if (!snapshots || idx < 0 || idx >= snap_count) return;
    const Snapshot *s = &snapshots[idx];
    memcpy(ships, s->ships, sizeof(ships));
    num_ships = s->num_ships;
    memcpy(projectiles, s->projectiles, sizeof(projectiles));
    num_projectiles = s->num_projectiles;
    memcpy(event_log, s->event_log, sizeof(event_log));
    event_log_head = s->event_log_head;
    event_log_count = s->event_log_count;
    sea_state = s->sea_state;
    sea_state_timer = s->sea_state_timer;
    wx_radar = s->wx_radar;
    wx_small = s->wx_small;
    wx_asw_helo = s->wx_asw_helo;
    wx_ir = s->wx_ir;
    memcpy(rng_state, s->rng_state, sizeof(rng_state));
    num_records = s->num_records;
    /* Stats */
    memcpy(stat_launches, s->stat_launches, sizeof(stat_launches));
    memcpy(stat_hits, s->stat_hits, sizeof(stat_hits));
    memcpy(stat_misses, s->stat_misses, sizeof(stat_misses));
    memcpy(stat_detect_opps, s->stat_detect_opps, sizeof(stat_detect_opps));
    memcpy(stat_detect_success, s->stat_detect_success, sizeof(stat_detect_success));
    memcpy(stat_detect_jammed, s->stat_detect_jammed, sizeof(stat_detect_jammed));
    memcpy(stat_kills_by_side, s->stat_kills_by_side, sizeof(stat_kills_by_side));
    memcpy(stat_ciws_intercepts, s->stat_ciws_intercepts, sizeof(stat_ciws_intercepts));
    memcpy(stat_sam_intercepts, s->stat_sam_intercepts, sizeof(stat_sam_intercepts));
    memcpy(stat_chaff_seductions, s->stat_chaff_seductions, sizeof(stat_chaff_seductions));
    memcpy(stat_torpedo_decoys, s->stat_torpedo_decoys, sizeof(stat_torpedo_decoys));
    memcpy(stat_air_strikes, s->stat_air_strikes, sizeof(stat_air_strikes));
    memcpy(stat_asw_attacks, s->stat_asw_attacks, sizeof(stat_asw_attacks));
    memcpy(stat_capsized, s->stat_capsized, sizeof(stat_capsized));
    memcpy(stat_a2a_kills, s->stat_a2a_kills, sizeof(stat_a2a_kills));
    memcpy(stat_harm_hits, s->stat_harm_hits, sizeof(stat_harm_hits));
    memcpy(stat_link16_shares, s->stat_link16_shares, sizeof(stat_link16_shares));
    memcpy(stat_comms_jammed, s->stat_comms_jammed, sizeof(stat_comms_jammed));
    memcpy(stat_freq_hops, s->stat_freq_hops, sizeof(stat_freq_hops));
    memcpy(stat_helo_asw, s->stat_helo_asw, sizeof(stat_helo_asw));
}

/* ── Event Log Helper ────────────────────────────────────── */
static void evt_log(int tick, int cpair, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int idx = (event_log_head + event_log_count) % MAX_EVENTS;
    if (event_log_count >= MAX_EVENTS) {
        event_log_head = (event_log_head + 1) % MAX_EVENTS;
    } else {
        event_log_count++;
    }
    event_log[idx].tick = tick;
    event_log[idx].color_pair = cpair;
    vsnprintf(event_log[idx].text, sizeof(event_log[idx].text), fmt, ap);
    va_end(ap);
}

/* ── CSV Helpers ─────────────────────────────────────────── */
static char *csv_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
    return s;
}

static WeaponType parse_wpn_type(const char *s) {
    if (!strcmp(s,"SSM"))     return WPN_SSM;
    if (!strcmp(s,"SAM"))     return WPN_SAM;
    if (!strcmp(s,"TORPEDO")) return WPN_TORPEDO;
    if (!strcmp(s,"GUN5IN"))  return WPN_GUN_5IN;
    if (!strcmp(s,"GUN130MM"))return WPN_GUN_130MM;
    if (!strcmp(s,"GUN76MM")) return WPN_GUN_76MM;
    if (!strcmp(s,"CIWS"))    return WPN_CIWS;
    if (!strcmp(s,"CRUISE"))  return WPN_CRUISE_MISSILE;
    if (!strcmp(s,"ASROC"))   return WPN_ASROC;
    if (!strcmp(s,"HARM"))    return WPN_HARM;
    return WPN_SSM;
}

static RadarBand parse_radar_band(const char *s) {
    if (!strcmp(s,"S")) return RADAR_S_BAND;
    if (!strcmp(s,"X")) return RADAR_X_BAND;
    if (!strcmp(s,"L")) return RADAR_L_BAND;
    if (!strcmp(s,"C")) return RADAR_C_BAND;
    return RADAR_S_BAND;
}

static ShipClass parse_ship_class(const char *s) {
    if (!strcmp(s,"CV"))  return CLASS_CARRIER;
    if (!strcmp(s,"CG"))  return CLASS_CRUISER;
    if (!strcmp(s,"DD"))  return CLASS_DESTROYER;
    if (!strcmp(s,"FF"))  return CLASS_FRIGATE;
    if (!strcmp(s,"FFL")) return CLASS_CORVETTE;
    if (!strcmp(s,"SS"))  return CLASS_SUBMARINE;
    if (!strcmp(s,"PGG")) return CLASS_MISSILE_BOAT;
    if (!strcmp(s,"LPD")) return CLASS_AMPHIBIOUS;
    return CLASS_DESTROYER;
}

/* ── Weapons CSV Loader ──────────────────────────────────── */
static void load_weapons_csv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[WARN] Cannot open %s\n", path); return; }

    char line[512];
    int  header_done = 0;
    int cName=-1,cType=-1,cRange=-1,cPHit=-1,cDmg=-1,cSalvo=-1,cReload=-1,cAmmo=-1;
    int cMV=-1,cMass=-1,cElev=-1,cGuid=-1;
    int cAA=-1,cCIWS=-1,cSAM=-1,cAS=-1,cSkim=-1,cMag=-1,cMount=-1,cHARM=-1;

    while (fgets(line, sizeof(line), f)) {
        char *p = csv_trim(line);
        if (!*p || *p == '#') continue;

        char *toks[32]; int ntoks = 0;
        char tmp[512]; strncpy(tmp, p, 511); tmp[511]='\0';
        char *tok = strtok(tmp, ",");
        while (tok && ntoks < 32) { toks[ntoks++] = csv_trim(tok); tok = strtok(NULL, ","); }

        if (!header_done) {
            for (int i = 0; i < ntoks; i++) {
                if (!strcmp(toks[i],"Name"))            cName=i;
                else if (!strcmp(toks[i],"Type"))       cType=i;
                else if (!strcmp(toks[i],"Range"))      cRange=i;
                else if (!strcmp(toks[i],"PHit"))       cPHit=i;
                else if (!strcmp(toks[i],"Damage"))     cDmg=i;
                else if (!strcmp(toks[i],"SalvoSize"))  cSalvo=i;
                else if (!strcmp(toks[i],"ReloadTicks"))cReload=i;
                else if (!strcmp(toks[i],"Ammo"))       cAmmo=i;
                else if (!strcmp(toks[i],"MuzzleVel"))  cMV=i;
                else if (!strcmp(toks[i],"Mass"))       cMass=i;
                else if (!strcmp(toks[i],"ElevAngle"))  cElev=i;
                else if (!strcmp(toks[i],"GuidanceQuality")) cGuid=i;
                else if (!strcmp(toks[i],"IsAntiAir"))  cAA=i;
                else if (!strcmp(toks[i],"IsCIWS"))     cCIWS=i;
                else if (!strcmp(toks[i],"IsSAM"))      cSAM=i;
                else if (!strcmp(toks[i],"IsAntiSub"))  cAS=i;
                else if (!strcmp(toks[i],"IsSeaSkimmer")) cSkim=i;
                else if (!strcmp(toks[i],"IsHARM"))     cHARM=i;
                else if (!strcmp(toks[i],"MagazineLimit")) cMag=i;
                else if (!strcmp(toks[i],"MountPosition")) cMount=i;
            }
            header_done = 1;
            continue;
        }
        if (cName < 0 || cName >= ntoks || !*toks[cName]) continue;
        if (num_wpn_templates >= MAX_WPN_TEMPLATES) break;

        WeaponTemplate *w = &wpn_templates[num_wpn_templates++];
        memset(w, 0, sizeof(*w));
        strncpy(w->name, toks[cName], MAX_NAME-1);
        w->type             = (cType>=0&&cType<ntoks)  ? parse_wpn_type(toks[cType])     : WPN_SSM;
        w->range_nm         = (cRange>=0&&cRange<ntoks)? atof(toks[cRange])              : 0;
        w->p_hit            = (cPHit>=0&&cPHit<ntoks)  ? atof(toks[cPHit])              : 0.5;
        w->damage           = (cDmg>=0&&cDmg<ntoks)    ? atof(toks[cDmg])               : 0;
        w->salvo_size       = (cSalvo>=0&&cSalvo<ntoks)? atoi(toks[cSalvo])             : 1;
        w->reload_ticks     = (cReload>=0&&cReload<ntoks)?atoi(toks[cReload])            : 60;
        w->ammo             = (cAmmo>=0&&cAmmo<ntoks)  ? atoi(toks[cAmmo])              : 0;
        w->muzzle_velocity_mps=(cMV>=0&&cMV<ntoks)     ? atof(toks[cMV])               : 1.0;
        w->projectile_mass_kg=(cMass>=0&&cMass<ntoks)  ? atof(toks[cMass])             : 100;
        w->elevation_angle  = (cElev>=0&&cElev<ntoks)  ? atof(toks[cElev])             : 0;
        w->guidance_quality = (cGuid>=0&&cGuid<ntoks)  ? atof(toks[cGuid])             : 0;
        w->is_anti_air      = (cAA>=0&&cAA<ntoks)      ? atoi(toks[cAA])               : 0;
        w->is_ciws          = (cCIWS>=0&&cCIWS<ntoks)  ? atoi(toks[cCIWS])             : 0;
        w->is_sam           = (cSAM>=0&&cSAM<ntoks)    ? atoi(toks[cSAM])              : 0;
        w->is_anti_sub      = (cAS>=0&&cAS<ntoks)       ? atoi(toks[cAS])              : 0;
        w->is_sea_skimmer   = (cSkim>=0&&cSkim<ntoks)  ? atoi(toks[cSkim])             : 0;
        w->is_harm          = (cHARM>=0&&cHARM<ntoks)  ? atoi(toks[cHARM])             : 0;
        w->magazine_limit   = (cMag>=0&&cMag<ntoks)    ? atoi(toks[cMag])              : w->ammo;
        w->mount_position   = (cMount>=0&&cMount<ntoks)? atoi(toks[cMount])            : 1;
    }
    fclose(f);
}

static WeaponTemplate *find_weapon(const char *name) {
    for (int i = 0; i < num_wpn_templates; i++)
        if (!strcmp(wpn_templates[i].name, name)) return &wpn_templates[i];
    return NULL;
}

/* ── Ship Builder Helpers ────────────────────────────────── */
static void init_radar(RadarSystem *r, RadarBand band, double power,
                       double gain, double freq, double range) {
    r->band = band; r->power_kw = power; r->gain_db = gain;
    r->frequency_ghz = freq; r->range_nm = range; r->base_range_nm = range;
    r->azimuth_res = 1.0; r->jammed = 0; r->jam_strength = 0;
    r->freq_hop_capable = 0; r->freq_hop_cooldown = 0;
    r->emitting = 1; /* radars emit by default */
}

static void add_weapon_from_template(Ship *s, const char *wname) {
    if (!wname || !*wname) return;
    if (s->num_weapons >= MAX_WEAPONS) return;
    WeaponTemplate *t = find_weapon(wname);
    if (!t) { fprintf(stderr,"[WARN] Weapon not found: '%s'\n", wname); return; }
    Weapon *w = &s->weapons[s->num_weapons++];
    memset(w, 0, sizeof(*w));
    w->type                 = t->type;
    strncpy(w->name, t->name, MAX_NAME-1);
    w->range_nm             = t->range_nm;
    w->p_hit                = t->p_hit;
    w->damage               = t->damage;
    w->salvo_size           = t->salvo_size;
    w->reload_ticks         = t->reload_ticks;
    w->ammo                 = t->ammo;
    w->muzzle_velocity_mps  = t->muzzle_velocity_mps;
    w->projectile_mass_kg   = t->projectile_mass_kg;
    w->elevation_angle      = t->elevation_angle;
    w->guidance_quality     = t->guidance_quality;
    w->is_ciws              = t->is_ciws;
    w->is_sam               = t->is_sam;
    w->is_anti_sub          = t->is_anti_sub;
    w->is_anti_air          = t->is_anti_air;
    w->is_sea_skimmer_capable = t->is_ciws || t->is_sam;
    w->is_harm              = t->is_harm;
    w->magazine_limit       = t->magazine_limit > 0 ? t->magazine_limit : t->ammo;
    w->mount_position       = t->mount_position;
    w->cooldown             = 0;
    w->overheated           = 0;
    w->overheat_cooldown    = 0;
    w->rounds_fired_total   = 0;
}

static Ship *add_ship_base(Side side, const char *name, const char *hull,
                           ShipClass cls, double hp, double armor,
                           double max_spd, double rcs) {
    if (num_ships >= MAX_SHIPS) return NULL;
    Ship *s = &ships[num_ships++];
    memset(s, 0, sizeof(Ship));
    strncpy(s->name, name, MAX_NAME-1);
    strncpy(s->hull_class, hull, MAX_NAME-1);
    s->side = side; s->ship_class = cls;
    s->hp = hp; s->max_hp = hp;
    s->armor = armor;
    s->max_speed_kts = max_spd;
    s->base_max_speed_kts = max_spd;
    s->speed_kts = max_spd * 0.7;
    s->rcs = rcs;
    s->alive = 1;
    s->is_submerged = (cls == CLASS_SUBMARINE);
    s->radar_effectiveness = 1.0;
    s->targeting_effectiveness = 1.0;
    s->esm_sensitivity = 0.5;
    s->link16_quality = 1.0;
    for (int i = 0; i < MAX_COMPARTMENTS; i++) {
        s->compartments[i].type = i;
        s->compartments[i].integrity = 1.0;
    }
    return s;
}

/* ── Platforms CSV Loader ────────────────────────────────── */
static void load_platforms_csv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr,"[WARN] Cannot open %s\n", path); return; }

    char line[1024];
    int  hdr = 0;
    int cTeam=-1,cCls=-1,cName=-1,cHull=-1,cHP=-1,cArmor=-1,cSpd=-1,cRCS=-1;
    int cSX=-1,cSY=-1,cSHdg=-1;
    int cRBand=-1,cRPow=-1,cRGain=-1,cRFreq=-1,cRRng=-1;
    int cFBand=-1,cFPow=-1,cFGain=-1,cFFreq=-1,cFRng=-1;
    int cECM=-1,cESM=-1,cChaff=-1,cFlare=-1,cNixie=-1;
    int cSPas=-1,cSAct=-1,cTArr=-1,cTRng=-1;
    int cMaxS=-1,cSTOBAR=-1;
    int cHelo=-1, cFreqHop=-1, cLink16=-1;
    int cW[8]; for(int i=0;i<8;i++) cW[i]=-1;

    while (fgets(line, sizeof(line), f)) {
        char *p = csv_trim(line);
        if (!*p || *p == '#') continue;

        char *toks[64]; int ntoks = 0;
        char tmp[1024]; strncpy(tmp, p, 1023); tmp[1023]='\0';
        char *tok = strtok(tmp, ",");
        while (tok && ntoks < 64) { toks[ntoks++] = csv_trim(tok); tok = strtok(NULL, ","); }

        if (!hdr) {
            for (int i = 0; i < ntoks; i++) {
                char *t = toks[i];
                if (!strcmp(t,"Team"))              cTeam=i;
                else if (!strcmp(t,"Class"))        cCls=i;
                else if (!strcmp(t,"Name"))         cName=i;
                else if (!strcmp(t,"Hull"))         cHull=i;
                else if (!strcmp(t,"HP"))           cHP=i;
                else if (!strcmp(t,"Armor"))        cArmor=i;
                else if (!strcmp(t,"MaxSpeed"))     cSpd=i;
                else if (!strcmp(t,"RCS"))          cRCS=i;
                else if (!strcmp(t,"StartX"))       cSX=i;
                else if (!strcmp(t,"StartY"))       cSY=i;
                else if (!strcmp(t,"StartHeading")) cSHdg=i;
                else if (!strcmp(t,"RadarBand"))    cRBand=i;
                else if (!strcmp(t,"RadarPower"))   cRPow=i;
                else if (!strcmp(t,"RadarGain"))    cRGain=i;
                else if (!strcmp(t,"RadarFreq"))    cRFreq=i;
                else if (!strcmp(t,"RadarRange"))   cRRng=i;
                else if (!strcmp(t,"FCRadarBand"))  cFBand=i;
                else if (!strcmp(t,"FCRadarPower")) cFPow=i;
                else if (!strcmp(t,"FCRadarGain"))  cFGain=i;
                else if (!strcmp(t,"FCRadarFreq"))  cFFreq=i;
                else if (!strcmp(t,"FCRadarRange")) cFRng=i;
                else if (!strcmp(t,"ECM_Power"))    cECM=i;
                else if (!strcmp(t,"ESM_Sensitivity")) cESM=i;
                else if (!strcmp(t,"Chaff"))        cChaff=i;
                else if (!strcmp(t,"Flares"))       cFlare=i;
                else if (!strcmp(t,"Nixie"))        cNixie=i;
                else if (!strcmp(t,"SonarPassiveRange")) cSPas=i;
                else if (!strcmp(t,"SonarActiveRange"))  cSAct=i;
                else if (!strcmp(t,"HasTowedArray"))     cTArr=i;
                else if (!strcmp(t,"TowedArrayRange"))   cTRng=i;
                else if (!strcmp(t,"MaxSorties"))   cMaxS=i;
                else if (!strcmp(t,"STOBAR"))       cSTOBAR=i;
                else if (!strcmp(t,"HeloCapacity")) cHelo=i;
                else if (!strcmp(t,"FreqHop"))      cFreqHop=i;
                else if (!strcmp(t,"Link16"))       cLink16=i;
                else if (!strcmp(t,"Weapon1"))      cW[0]=i;
                else if (!strcmp(t,"Weapon2"))      cW[1]=i;
                else if (!strcmp(t,"Weapon3"))      cW[2]=i;
                else if (!strcmp(t,"Weapon4"))      cW[3]=i;
                else if (!strcmp(t,"Weapon5"))      cW[4]=i;
                else if (!strcmp(t,"Weapon6"))      cW[5]=i;
                else if (!strcmp(t,"Weapon7"))      cW[6]=i;
                else if (!strcmp(t,"Weapon8"))      cW[7]=i;
            }
            hdr = 1;
            continue;
        }
        if (cName<0||cName>=ntoks||!*toks[cName]) continue;

        Side side = SIDE_NATO;
        if (cTeam>=0&&cTeam<ntoks && !strcmp(toks[cTeam],"PACT")) side = SIDE_PACT;

        ShipClass cls = CLASS_DESTROYER;
        if (cCls>=0&&cCls<ntoks) cls = parse_ship_class(toks[cCls]);

        double hp    = (cHP>=0&&cHP<ntoks)    ? atof(toks[cHP])    : 200;
        double armor = (cArmor>=0&&cArmor<ntoks)?atof(toks[cArmor]): 0.1;
        double spd   = (cSpd>=0&&cSpd<ntoks)  ? atof(toks[cSpd])   : 30;
        double rcs   = (cRCS>=0&&cRCS<ntoks)  ? atof(toks[cRCS])   : 0.5;
        const char *hull = (cHull>=0&&cHull<ntoks) ? toks[cHull] : "";

        Ship *s = add_ship_base(side, toks[cName], hull, cls, hp, armor, spd, rcs);
        if (!s) continue;

        /* Position */
        double bx = (cSX>=0&&cSX<ntoks) ? atof(toks[cSX]) : 150;
        double by = (cSY>=0&&cSY<ntoks) ? atof(toks[cSY]) : 150;
        s->x = bx + rng_gauss(0, 8);
        s->y = by + rng_gauss(0, 8);
        if (s->x < 5)  s->x = 5;  if (s->x > 295) s->x = 295;
        if (s->y < 5)  s->y = 5;  if (s->y > 295) s->y = 295;
        s->heading = (cSHdg>=0&&cSHdg<ntoks) ? atof(toks[cSHdg]) : 0;

        /* Radars */
        RadarBand rb = (cRBand>=0&&cRBand<ntoks) ? parse_radar_band(toks[cRBand]) : RADAR_S_BAND;
        double rp=(cRPow>=0&&cRPow<ntoks)?atof(toks[cRPow]):200;
        double rg=(cRGain>=0&&cRGain<ntoks)?atof(toks[cRGain]):35;
        double rf=(cRFreq>=0&&cRFreq<ntoks)?atof(toks[cRFreq]):3.0;
        double rr=(cRRng>=0&&cRRng<ntoks)?atof(toks[cRRng]):100;
        init_radar(&s->search_radar, rb, rp, rg, rf, rr);

        RadarBand fb = (cFBand>=0&&cFBand<ntoks)?parse_radar_band(toks[cFBand]):RADAR_X_BAND;
        double fp=(cFPow>=0&&cFPow<ntoks)?atof(toks[cFPow]):100;
        double fg=(cFGain>=0&&cFGain<ntoks)?atof(toks[cFGain]):30;
        double ff=(cFFreq>=0&&cFFreq<ntoks)?atof(toks[cFFreq]):9.0;
        double fr=(cFRng>=0&&cFRng<ntoks)?atof(toks[cFRng]):60;
        init_radar(&s->fire_control_radar, fb, fp, fg, ff, fr);

        /* Frequency hopping capability */
        int fhop = (cFreqHop>=0&&cFreqHop<ntoks) ? atoi(toks[cFreqHop]) : 0;
        s->search_radar.freq_hop_capable = fhop;
        s->fire_control_radar.freq_hop_capable = fhop;

        /* Link-16 */
        s->has_link16 = (cLink16>=0&&cLink16<ntoks) ? atoi(toks[cLink16]) : (side == SIDE_NATO ? 1 : 0);

        /* ECM/ESM */
        s->ecm_power_kw    = (cECM>=0&&cECM<ntoks)  ? atof(toks[cECM])  : 0;
        s->esm_sensitivity = (cESM>=0&&cESM<ntoks)  ? atof(toks[cESM])  : 0.5;
        s->chaff_charges   = (cChaff>=0&&cChaff<ntoks)?atoi(toks[cChaff]): 0;
        s->flare_charges   = (cFlare>=0&&cFlare<ntoks)?atoi(toks[cFlare]): 0;
        s->nixie_charges   = (cNixie>=0&&cNixie<ntoks)?atoi(toks[cNixie]): 0;

        /* Sonar */
        s->sonar.passive_range_nm = (cSPas>=0&&cSPas<ntoks)?atof(toks[cSPas]):0;
        s->sonar.active_range_nm  = (cSAct>=0&&cSAct<ntoks)?atof(toks[cSAct]):0;
        s->sonar.has_towed_array  = (cTArr>=0&&cTArr<ntoks)?atoi(toks[cTArr]):0;
        s->sonar.towed_range_nm   = (cTRng>=0&&cTRng<ntoks)?atof(toks[cTRng]):0;

        /* Aviation */
        s->max_sorties      = (cMaxS>=0&&cMaxS<ntoks)?atoi(toks[cMaxS]):0;
        s->sorties_available= s->max_sorties;
        s->stobar           = (cSTOBAR>=0&&cSTOBAR<ntoks)?atoi(toks[cSTOBAR]):0;

        /* Helicopters */
        s->helo_capacity = (cHelo>=0&&cHelo<ntoks) ? atoi(toks[cHelo]) : 0;
        /* Auto-assign helos for ships that historically carry them */
        if (s->helo_capacity == 0 && s->max_sorties == 0 && cls != CLASS_SUBMARINE &&
            cls != CLASS_MISSILE_BOAT && cls != CLASS_CORVETTE) {
            if (cls == CLASS_DESTROYER || cls == CLASS_CRUISER) s->helo_capacity = 2;
            else if (cls == CLASS_FRIGATE) s->helo_capacity = 1;
        }
        s->helos_available = s->helo_capacity;

        /* Weapons */
        for (int wi = 0; wi < 8; wi++) {
            if (cW[wi]>=0&&cW[wi]<ntoks&&*toks[cW[wi]])
                add_weapon_from_template(s, toks[cW[wi]]);
        }
    }
    fclose(f);
}

/* ── Weather ─────────────────────────────────────────────── */
static void update_weather_factors(void) {
    wx_radar    = 1.0 - sea_state * 0.035;
    wx_small    = 1.0 - (sea_state > 3 ? (sea_state-3)*0.10 : 0.0);
    wx_asw_helo = 1.0 - sea_state * 0.12;
    wx_ir       = 1.0 - sea_state * 0.07;
    if (wx_radar    < 0.4) wx_radar    = 0.4;
    if (wx_small    < 0.5) wx_small    = 0.5;
    if (wx_asw_helo < 0.1) wx_asw_helo = 0.1;
    if (wx_ir       < 0.3) wx_ir       = 0.3;
}

static void phase_weather(int tick) {
    (void)tick;
    if (--sea_state_timer <= 0) {
        int old = sea_state;
        sea_state += (int)(rng_uniform() * 3) - 1;
        if (sea_state < 0) sea_state = 0;
        if (sea_state > 6) sea_state = 6;
        sea_state_timer = 300 + (int)(rng_uniform() * 600);
        update_weather_factors();
        if (abs(sea_state - old) >= 2)
            evt_log(tick, CP_CYAN, "[WEATHER] Sea state %d > %d (%s)",
                    old, sea_state, beaufort_names[sea_state]);
    }
}

/* ── Utility ─────────────────────────────────────────────── */
static double dist_nm(const Ship *a, const Ship *b) {
    double dx = a->x - b->x, dy = a->y - b->y;
    return sqrt(dx*dx + dy*dy);
}
static double dist_nm_xy(double x1, double y1, double x2, double y2) {
    double dx=x1-x2, dy=y1-y2; return sqrt(dx*dx+dy*dy);
}
static double bearing_deg(const Ship *from, const Ship *to) {
    double dx=to->x-from->x, dy=to->y-from->y;
    double a = atan2(dx,dy)*(180.0/M_PI);
    return a < 0 ? a+360.0 : a;
}
static const char *side_str(Side s)    { return s==SIDE_NATO?"NATO":"PACT"; }
static const char *class_str(ShipClass c) {
    const char *n[]={"CV","CG","DD","FF","FFL","SS","PGG","LPD"}; return n[c];
}
static const char *sortie_str(SortieType t) {
    const char *n[]={"CAP","STRIKE","ASW","AEW","HELO-ASW"}; return n[t];
}

static void log_event(int tick, const char *atk, const char *def,
                      const char *wpn, int hit, double dmg,
                      double hp_after, int kill, EventType et) {
    if (num_records >= 16384) return;
    EngagementRecord *r = &records[num_records++];
    r->tick = tick;
    strncpy(r->attacker, atk, MAX_NAME-1);
    strncpy(r->defender, def, MAX_NAME-1);
    strncpy(r->weapon, wpn, MAX_NAME-1);
    r->hit = hit; r->damage = dmg;
    r->defender_hp_after = hp_after;
    r->kill = kill; r->event_type = et;
    r->sea_state = sea_state;
}

/* ── Ballistics ──────────────────────────────────────────── */
static void init_projectile(Projectile *p, Ship *attacker, Ship *target,
                            Weapon *wpn, int tick) {
    p->active = 1; p->intercepted = 0;
    strncpy(p->weapon_name, wpn->name, MAX_NAME-1);
    strncpy(p->attacker,    attacker->name, MAX_NAME-1);
    strncpy(p->target,      target->name, MAX_NAME-1);
    p->attacker_idx = (int)(attacker - ships);
    p->x = attacker->x; p->y = attacker->y; p->z = 50.0;
    p->launch_time = tick;
    p->mass_kg = wpn->projectile_mass_kg;
    p->damage  = wpn->damage;
    p->type    = wpn->type;
    p->drag_coeff = 0.3;
    p->is_missile = (wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE || wpn->type == WPN_HARM);
    p->is_torpedo = (wpn->type == WPN_TORPEDO || wpn->type == WPN_ASROC);
    p->is_harm = wpn->is_harm;
    p->harm_target_idx = -1;
    p->is_sea_skimmer = (wpn->type == WPN_SSM &&
                         (strstr(wpn->name,"Moskit") || strstr(wpn->name,"Bazalt") ||
                          strstr(wpn->name,"Granit") || strstr(wpn->name,"Malakhit") ||
                          strstr(wpn->name,"Ametist")));

    double v0 = wpn->muzzle_velocity_mps;

    if (wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE || wpn->type == WPN_HARM) {
        double dx = target->x - attacker->x;
        double dy = target->y - attacker->y;
        double d  = sqrt(dx*dx + dy*dy);
        if (d < 0.001) d = 0.001;
        v0 *= 340.0;
        p->vx = (dx/d) * v0 / 1852.0;
        p->vy = (dy/d) * v0 / 1852.0;
        p->vz = 0;
        p->z  = p->is_sea_skimmer ? 15.0 : 50.0;
        if (p->is_harm) {
            p->z = 200.0; /* HARM flies high initially */
            p->harm_target_idx = (int)(target - ships);
        }
    } else if (wpn->type == WPN_TORPEDO || wpn->type == WPN_ASROC) {
        double dx = target->x - attacker->x;
        double dy = target->y - attacker->y;
        double d  = sqrt(dx*dx + dy*dy);
        if (d < 0.001) d = 0.001;
        double tspd = v0 / 3600.0;
        p->vx = (dx/d) * tspd;
        p->vy = (dy/d) * tspd;
        p->vz = 0;
        p->z  = -200.0;
    } else {
        double angle_rad = wpn->elevation_angle * M_PI / 180.0;
        double bearing   = bearing_deg(attacker, target) * M_PI / 180.0;
        double v_horiz = v0 * cos(angle_rad);
        p->vx = sin(bearing) * v_horiz / 1852.0;
        p->vy = cos(bearing) * v_horiz / 1852.0;
        p->vz = v0 * sin(angle_rad) * 3.28084;
    }
}

static void update_projectile_physics(Projectile *p, double dt) {
    if (!p->active) return;

    if (p->is_torpedo) {
        double vxy = sqrt(p->vx*p->vx + p->vy*p->vy);
        double drag = 0.3 * SEA_WATER_DENSITY * 0.04 * vxy * vxy * (1852.0*1852.0) / (p->mass_kg * 1852.0);
        if (vxy > 1e-6) {
            p->vx -= (p->vx/vxy)*drag*dt;
            p->vy -= (p->vy/vxy)*drag*dt;
        }
        p->x += p->vx*dt; p->y += p->vy*dt;
        return;
    }

    /* HARM: re-target toward emitter if it's still radiating */
    if (p->is_harm && p->harm_target_idx >= 0 && p->harm_target_idx < num_ships) {
        Ship *tgt = &ships[p->harm_target_idx];
        if (tgt->alive && tgt->search_radar.emitting) {
            double dx = tgt->x - p->x;
            double dy = tgt->y - p->y;
            double d = sqrt(dx*dx + dy*dy);
            if (d > 0.01) {
                double spd = sqrt(p->vx*p->vx + p->vy*p->vy);
                p->vx = (dx/d) * spd;
                p->vy = (dy/d) * spd;
            }
        }
    }

    if (p->type != WPN_SSM && p->type != WPN_CRUISE_MISSILE && p->type != WPN_HARM)
        p->vz -= GRAVITY * 3.28084 * dt;

    double vtot = sqrt((p->vx*1852.0)*(p->vx*1852.0) +
                       (p->vy*1852.0)*(p->vy*1852.0) +
                       (p->vz*0.3048)*(p->vz*0.3048));
    double ref_area, drag_mult;
    if (p->type == WPN_CRUISE_MISSILE || p->type == WPN_HARM) { ref_area=0.2; drag_mult=0.02; }
    else if (p->type == WPN_SSM)       { ref_area=0.2; drag_mult=0.10; }
    else                               { ref_area=0.01; drag_mult=1.0; }

    double drag_force = 0.5*AIR_DENSITY_SL*p->drag_coeff*ref_area*vtot*vtot*drag_mult;
    double drag_accel = (p->mass_kg > 0) ? drag_force/p->mass_kg : 0;
    if (vtot > 0.1) {
        p->vx -= (p->vx*1852.0/vtot)*drag_accel*dt/1852.0;
        p->vy -= (p->vy*1852.0/vtot)*drag_accel*dt/1852.0;
        p->vz -= (p->vz*0.3048/vtot)*drag_accel*dt*3.28084;
    }

    p->x += p->vx*dt; p->y += p->vy*dt; p->z += p->vz*dt;

    if (p->z <= 0 && !p->is_missile) p->active = 0;
    if (p->x<-50||p->x>GRID_SIZE+50||p->y<-50||p->y>GRID_SIZE+50) p->active=0;
}

/* ── C4ISR / Link-16 Phase ──────────────────────────────── */
static void phase_c4isr(int tick) {
    /* NATO Link-16 / CEC: share detections fleet-wide */
    /* First, collect all contacts detected by any NATO ship */
    int nato_detected[MAX_SHIPS];
    memset(nato_detected, 0, sizeof(nato_detected));

    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive || ships[i].side != SIDE_NATO) continue;
        if (!ships[i].has_link16) continue;

        for (int j = 0; j < num_ships; j++) {
            if (ships[j].side == SIDE_NATO) continue;
            if (ships[j].detected) nato_detected[j] = 1;
        }
    }

    /* PACT communications jamming degrades Link-16 */
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive || ships[i].side != SIDE_PACT) continue;
        if (ships[i].ecm_power_kw > 50) {
            /* Dedicated EW platforms with high ECM power can jam Link-16 */
            double jam_range = ships[i].ecm_power_kw * 0.5; /* NM */
            /* Check if any NATO ship is within jam range */
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side != SIDE_NATO) continue;
                double d = dist_nm(&ships[i], &ships[j]);
                if (d < jam_range) {
                    double jam_eff = (1.0 - d / jam_range) * 0.6;
                    ships[j].link16_degraded = 1;
                    ships[j].link16_quality = 1.0 - jam_eff;
                    if (ships[j].link16_quality < 0.3) ships[j].link16_quality = 0.3;
                    stat_comms_jammed[SIDE_PACT]++;
                }
            }
        }
    }

    /* Now share contacts via Link-16 */
    for (int j = 0; j < num_ships; j++) {
        if (!nato_detected[j]) continue;
        for (int i = 0; i < num_ships; i++) {
            if (!ships[i].alive || ships[i].side != SIDE_NATO) continue;
            if (!ships[i].has_link16) continue;

            /* Probabilistic sharing based on link quality */
            double share_prob = ships[i].link16_quality;
            if (rng_uniform() < share_prob) {
                if (!ships[j].detected_by_link16) {
                    ships[j].detected_by_link16 = 1;
                    ships[j].detected = 1;
                    stat_link16_shares[SIDE_NATO]++;
                }
            }
        }
    }

    /* Log significant comms jamming events */
    if (tick % 120 == 0) {
        for (int i = 0; i < num_ships; i++) {
            if (!ships[i].alive || ships[i].side != SIDE_NATO) continue;
            if (ships[i].link16_degraded) {
                evt_log(tick, CP_YELLOW, "[COMMS-JAM] %s Link-16 degraded (%.0f%%)",
                        ships[i].name, ships[i].link16_quality * 100);
            }
        }
    }

    /* Reset link16_degraded each tick (re-evaluated next tick) */
    for (int i = 0; i < num_ships; i++) {
        ships[i].link16_degraded = 0;
        ships[i].link16_quality = 1.0;
    }
}

/* ── Detection Phase (radar) ─────────────────────────────── */
static void phase_detect(int tick) {
    /* Reset detected flags each tick; Link-16 and sonar will re-set */
    for (int i = 0; i < num_ships; i++) {
        ships[i].detected = 0;
        ships[i].detected_by_link16 = 0;
    }

    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        /* Submarines don't use radar */
        if (ships[i].is_submerged) continue;

        /* Mark radar as emitting for HARM targeting */
        ships[i].search_radar.emitting = 1;
        ships[i].fire_control_radar.emitting = 1;

        for (int j = 0; j < num_ships; j++) {
            if (i==j || !ships[j].alive) continue;
            if (ships[i].side == ships[j].side) continue;
            if (ships[j].is_submerged) continue;

            double d = dist_nm(&ships[i], &ships[j]);
            RadarSystem *radar = &ships[i].search_radar;
            double radar_range = radar->range_nm * ships[i].radar_effectiveness * wx_radar;

            /* ECM jamming */
            if (ships[j].ecm_power_kw > 0) {
                double jam_eff = ships[j].ecm_power_kw / (radar->power_kw + 1.0);
                if (jam_eff > 0.3) {
                    radar_range *= (1.0 - jam_eff * 0.45);
                    radar->jam_strength = jam_eff;
                    radar->jammed = 1;
                    stat_detect_jammed[ships[i].side]++;
                }
            }

            /* Frequency hopping: reduce jam effectiveness */
            if (radar->jammed && radar->freq_hop_capable && radar->freq_hop_cooldown <= 0) {
                /* Attempt frequency hop */
                double hop_success = 0.65; /* 65% chance of successful hop */
                if (rng_uniform() < hop_success) {
                    radar->jam_strength *= 0.3; /* dramatically reduce jam effect */
                    radar_range = radar->base_range_nm * ships[i].radar_effectiveness * wx_radar *
                                  (1.0 - radar->jam_strength * 0.45);
                    radar->freq_hop_cooldown = 30; /* 30 second cooldown */
                    stat_freq_hops[ships[i].side]++;
                    if (tick % 120 == 0)
                        evt_log(tick, CP_CYAN, "[FREQ-HOP] %s radar frequency hop",
                                ships[i].name);
                }
            }
            if (radar->freq_hop_cooldown > 0) radar->freq_hop_cooldown--;

            if (ships[i].ship_class >= CLASS_FRIGATE)
                radar_range *= wx_small;

            double eff_range = radar_range * pow(ships[j].rcs, 0.25);
            if (d < 15) eff_range *= rng_gauss(1.0, 0.08);

            if (d < eff_range) {
                stat_detect_opps[ships[i].side]++;
                double p_det = 1.0 - pow(d/eff_range, 2.5);
                if (radar->band == RADAR_L_BAND) p_det *= 0.85;
                if (radar->band == RADAR_X_BAND) p_det *= 1.15;
                p_det *= rng_gauss(1.0, 0.10);
                if (p_det < 0.01) p_det = 0.01;
                if (p_det > 0.98) p_det = 0.98;

                if (rng_uniform() < p_det) {
                    stat_detect_success[ships[i].side]++;
                    ships[j].detected = 1;
                    if (tick % 120 == 0)
                        evt_log(tick, CP_CYAN, "[RADAR] %s %s > %s %s %.0fNM brg%03.0f%s",
                                side_str(ships[i].side), ships[i].name,
                                side_str(ships[j].side), ships[j].name,
                                d, bearing_deg(&ships[i], &ships[j]),
                                radar->jam_strength>0.3?" [JAM]":"");
                }
            }
        }
    }
}

/* ── Sonar Detection Phase ──────────────────────────────── */
static void phase_sonar(int tick) {
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        if (ships[i].sonar.passive_range_nm <= 0 &&
            ships[i].sonar.active_range_nm  <= 0) continue;

        for (int j = 0; j < num_ships; j++) {
            if (i==j || !ships[j].alive) continue;
            if (ships[i].side == ships[j].side) continue;
            if (!ships[j].is_submerged) continue;

            double d = dist_nm(&ships[i], &ships[j]);

            double p_rng = ships[i].sonar.passive_range_nm;
            if (ships[i].sonar.has_towed_array &&
                ships[i].sonar.towed_range_nm > p_rng)
                p_rng = ships[i].sonar.towed_range_nm;
            p_rng *= (1.0 - sea_state * 0.05);
            if (p_rng < 5) p_rng = 5;

            if (d < p_rng) {
                double p_det = 0.3 + 0.5*(1.0 - d/p_rng);
                if (ships[j].ship_class == CLASS_SUBMARINE) {
                    if (strstr(ships[j].name,"Akula"))       p_det *= 0.55;
                    else if (strstr(ships[j].name,"Virginia"))p_det *= 0.45;
                    else if (strstr(ships[j].name,"Los Ang")) p_det *= 0.50;
                }
                p_det *= rng_gauss(1.0, 0.15);
                if (p_det < 0) p_det = 0;
                if (p_det > 0.95) p_det = 0.95;

                if (rng_uniform() < p_det) {
                    ships[j].detected = 1;
                    if (tick % 120 == 0)
                        evt_log(tick, CP_CYAN, "[SONAR] %s %s passive: %s %s %.0fNM",
                                side_str(ships[i].side), ships[i].name,
                                side_str(ships[j].side), ships[j].name, d);
                }
            }

            if (ships[i].sonar.active_on) {
                double a_rng = ships[i].sonar.active_range_nm;
                if (d < a_rng) {
                    ships[j].detected = 1;
                    ships[i].detected = 1;
                }
            }
        }
    }
}

/* ── CIWS / SAM / Chaff Intercept Logic ─────────────────── */
static int check_intercepts(Projectile *proj, int tick) {
    if (!proj->active || !proj->is_missile) return 0;

    Side threat_side = SIDE_NATO;
    if (proj->attacker_idx >= 0 && proj->attacker_idx < num_ships)
        threat_side = ships[proj->attacker_idx].side;
    Side def_side = (threat_side == SIDE_NATO) ? SIDE_PACT : SIDE_NATO;

    /* Layer 1: Long-range SAM */
    for (int i = 0; i < num_ships; i++) {
        Ship *def = &ships[i];
        if (!def->alive || def->side != def_side) continue;
        double d = dist_nm_xy(proj->x, proj->y, def->x, def->y);

        for (int w = 0; w < def->num_weapons; w++) {
            Weapon *wpn = &def->weapons[w];
            if (!wpn->is_sam || wpn->is_ciws) continue;
            if (wpn->ammo <= 0 || wpn->cooldown > 0) continue;
            if (d > wpn->range_nm) continue;

            double p_int = wpn->p_hit * wx_radar * def->radar_effectiveness;
            if (proj->is_sea_skimmer) p_int *= 0.60;
            /* HARM missiles are harder to intercept (high speed, small RCS) */
            if (proj->is_harm) p_int *= 0.70;

            if (rng_uniform() < p_int) {
                wpn->ammo -= (wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo);
                wpn->cooldown = wpn->reload_ticks;
                proj->active = 0; proj->intercepted = 1;
                stat_sam_intercepts[def_side]++;
                evt_log(tick, CP_NATO, "[SAM-KILL] %s %s | %s %.1fNM",
                        def->name, wpn->name, proj->weapon_name, d);
                log_event(tick, proj->attacker, def->name, wpn->name,
                          1, 0, def->hp, 0, EVT_SAM_INTERCEPT);
                return 1;
            } else {
                wpn->ammo -= (wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo);
                wpn->cooldown = wpn->reload_ticks;
                break;
            }
        }
        if (proj->intercepted) return 1;
    }

    /* Layer 2: Chaff */
    for (int i = 0; i < num_ships; i++) {
        Ship *def = &ships[i];
        if (!def->alive || def->side != def_side) continue;
        double d = dist_nm_xy(proj->x, proj->y, def->x, def->y);
        if (d > 0.8) continue;
        if (strcmp(proj->target, def->name) != 0) continue;

        if (def->chaff_charges > 0) {
            double p_chaff = 0.45;
            if (proj->is_sea_skimmer) p_chaff = 0.20;
            if (proj->is_harm) p_chaff = 0.10; /* HARM homes on emissions, not radar return */
            if (def->ecm_power_kw > 60) p_chaff += 0.10;

            def->chaff_charges--;
            if (rng_uniform() < p_chaff) {
                proj->active = 0; proj->intercepted = 1;
                stat_chaff_seductions[def_side]++;
                evt_log(tick, CP_YELLOW, "[CHAFF] %s seduced %s",
                        def->name, proj->weapon_name);
                log_event(tick, proj->attacker, def->name, "Chaff",
                          0, 0, def->hp, 0, EVT_CHAFF_SEDUCE);
                return 1;
            }
        }
    }

    /* Layer 3: CIWS */
    for (int i = 0; i < num_ships; i++) {
        Ship *def = &ships[i];
        if (!def->alive || def->side != def_side) continue;
        double d = dist_nm_xy(proj->x, proj->y, def->x, def->y);

        for (int w = 0; w < def->num_weapons; w++) {
            Weapon *wpn = &def->weapons[w];
            if (!wpn->is_ciws) continue;
            if (wpn->overheated) continue;
            if (wpn->ammo <= 0) continue;
            if (d > wpn->range_nm) continue;

            int burst = wpn->is_sam ? 1 : 20;
            if (burst > wpn->ammo) burst = wpn->ammo;
            wpn->ammo -= burst;
            wpn->rounds_fired_total += burst;

            if (!wpn->is_sam && wpn->rounds_fired_total > wpn->magazine_limit * 0.75) {
                wpn->overheated = 1;
                wpn->overheat_cooldown = 90;
                evt_log(tick, CP_YELLOW, "[CIWS-HOT] %s %s overheating!",
                        def->name, wpn->name);
            }

            double p_int = wpn->p_hit * wx_radar;
            if (proj->is_sea_skimmer) p_int *= 0.72;
            if (proj->is_harm) p_int *= 0.65;
            if (def->radar_effectiveness < 0.7) p_int *= def->radar_effectiveness;

            if (rng_uniform() < p_int) {
                proj->active = 0; proj->intercepted = 1;
                stat_ciws_intercepts[def_side]++;
                evt_log(tick, CP_NATO, "[CIWS-KILL] %s %s > %s %.2fNM",
                        def->name, wpn->name, proj->weapon_name, d);
                log_event(tick, proj->attacker, def->name, wpn->name,
                          1, 0, def->hp, 0, EVT_CIWS_INTERCEPT);
                return 1;
            }
            break;
        }
        if (proj->intercepted) return 1;
    }
    return 0;
}

static int check_torpedo_decoy(Projectile *proj, Ship *target, int tick) {
    if (!proj->is_torpedo || target->nixie_charges <= 0) return 0;
    double d = dist_nm_xy(proj->x, proj->y, target->x, target->y);
    if (d > 0.5) return 0;
    target->nixie_charges--;
    double p_decoy = 0.40;
    if (rng_uniform() < p_decoy) {
        proj->active = 0; proj->intercepted = 1;
        stat_torpedo_decoys[target->side]++;
        evt_log(tick, CP_YELLOW, "[NIXIE] %s decoyed %s",
                target->name, proj->weapon_name);
        log_event(tick, proj->attacker, target->name, "Nixie",
                  0, 0, target->hp, 0, EVT_TORPEDO_DECOY);
        return 1;
    }
    return 0;
}

/* ── Movement Phase ──────────────────────────────────────── */
static void phase_move(int tick) {
    (void)tick;
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive) continue;

        double approach_factor = (s->is_submerged) ? 1.3 : 0.7;
        double min_d = 1e9; int tgt = -1;
        for (int j = 0; j < num_ships; j++) {
            if (i==j || !ships[j].alive || ships[j].side==s->side) continue;
            if (!ships[j].detected) continue;
            double d = dist_nm(s, &ships[j]);
            if (d < min_d) { min_d=d; tgt=j; }
        }

        if (tgt >= 0) {
            double desired = bearing_deg(s, &ships[tgt]);
            double best_range = 0;
            for (int w = 0; w < s->num_weapons; w++)
                if (s->weapons[w].ammo>0 && s->weapons[w].range_nm>best_range)
                    best_range = s->weapons[w].range_nm;
            double target_dist = best_range * approach_factor;
            if (target_dist < 5) target_dist = 5;

            double eff_spd = s->speed_kts;
            if (s->speed_reduced && s->compartments[COMP_ENGINE].integrity < 0.5)
                eff_spd *= s->compartments[COMP_ENGINE].integrity;

            if (min_d > target_dist) {
                s->heading = desired;
                s->speed_kts = eff_spd;
            } else if (min_d < target_dist * 0.5) {
                s->heading = fmod(desired+180.0, 360.0);
                s->speed_kts = eff_spd * 0.9;
            } else {
                s->heading = desired + rng_gauss(0, 15);
                s->speed_kts = eff_spd * 0.6;
            }
        }

        double spd_nms = s->speed_kts / 3600.0;
        double rad = s->heading * (M_PI/180.0);
        s->x += sin(rad)*spd_nms;
        s->y += cos(rad)*spd_nms;
        if (s->x<0) s->x=0; if (s->x>GRID_SIZE) s->x=GRID_SIZE;
        if (s->y<0) s->y=0; if (s->y>GRID_SIZE) s->y=GRID_SIZE;
    }
}

/* ── Weapons Phase ───────────────────────────────────────── */
static void phase_weapons(int tick) {
    /* CIWS cooldown */
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        for (int w = 0; w < ships[i].num_weapons; w++) {
            Weapon *wpn = &ships[i].weapons[w];
            if (wpn->overheated) {
                if (--wpn->overheat_cooldown <= 0) {
                    wpn->overheated = 0;
                    evt_log(tick, CP_CYAN, "[CIWS-COOL] %s %s cooled", ships[i].name, wpn->name);
                }
            }
        }
    }

    /* Offensive fire */
    for (int i = 0; i < num_ships; i++) {
        Ship *atk = &ships[i];
        if (!atk->alive) continue;

        for (int w = 0; w < atk->num_weapons; w++) {
            Weapon *wpn = &atk->weapons[w];
            if (wpn->cooldown > 0) { wpn->cooldown--; continue; }
            if (wpn->ammo <= 0) continue;
            if (wpn->overheated) continue;
            if (wpn->is_ciws || wpn->is_sam) continue;

            /* HARM: special targeting - look for ships with active radars */
            if (wpn->is_harm) {
                int best_tgt = -1;
                double best_d = 1e9;
                for (int j = 0; j < num_ships; j++) {
                    if (!ships[j].alive || ships[j].side == atk->side) continue;
                    if (!ships[j].search_radar.emitting) continue;
                    double d = dist_nm(atk, &ships[j]);
                    if (d > wpn->range_nm) continue;
                    /* Prioritize ships with strongest radar emissions */
                    double score = ships[j].search_radar.power_kw / (d + 1.0);
                    if (score > 0 && d < best_d) { best_d = d; best_tgt = j; }
                }
                if (best_tgt < 0) continue;

                Ship *def = &ships[best_tgt];
                int rounds = wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo;
                wpn->ammo -= rounds;
                wpn->cooldown = wpn->reload_ticks;

                for (int r = 0; r < rounds && num_projectiles < MAX_PROJECTILES; r++) {
                    init_projectile(&projectiles[num_projectiles], atk, def, wpn, tick);
                    num_projectiles++;
                }
                stat_launches[atk->side] += rounds;
                evt_log(tick, CP_MAGENTA, "[HARM] %s %s > %s | %s x%d @ %.0fNM",
                        side_str(atk->side), atk->name, def->name,
                        wpn->name, rounds, best_d);
                continue;
            }

            /* Normal weapon targeting */
            int best_tgt = -1;
            double best_score = -1;
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side==atk->side) continue;
                if (!ships[j].detected) continue;
                if (wpn->is_anti_sub && ships[j].ship_class != CLASS_SUBMARINE) continue;
                if (!wpn->is_anti_sub && ships[j].is_submerged) continue;
                if (wpn->mount_position == 0 && atk->fwd_weapons_disabled) continue;
                if (wpn->mount_position == 2 && atk->aft_weapons_disabled) continue;

                double d = dist_nm(atk, &ships[j]);
                if (d > wpn->range_nm) continue;

                double score = (wpn->range_nm - d) / wpn->range_nm;
                if ((wpn->type==WPN_SSM||wpn->type==WPN_CRUISE_MISSILE) &&
                     ships[j].ship_class==CLASS_CARRIER)   score += 0.5;
                if ((wpn->type==WPN_SSM||wpn->type==WPN_CRUISE_MISSILE) &&
                     ships[j].ship_class==CLASS_CRUISER)   score += 0.35;
                if (wpn->is_anti_sub &&
                     ships[j].ship_class==CLASS_SUBMARINE) score += 0.6;

                if (score > best_score) { best_score=score; best_tgt=j; }
            }
            if (best_tgt < 0) continue;

            Ship *def = &ships[best_tgt];
            double d = dist_nm(atk, def);
            int rounds = wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo;
            wpn->ammo -= rounds;
            wpn->cooldown = wpn->reload_ticks;

            int launched = 0;
            for (int r = 0; r < rounds && num_projectiles < MAX_PROJECTILES; r++) {
                init_projectile(&projectiles[num_projectiles], atk, def, wpn, tick);
                num_projectiles++;
                launched++;
            }
            stat_launches[atk->side] += launched;
            evt_log(tick, CP_YELLOW, "[LAUNCH] %s %s > %s | %s x%d @ %.0fNM",
                    side_str(atk->side), atk->name, def->name,
                    wpn->name, rounds, d);
        }
    }

    /* Update projectiles, intercepts, impacts */
    for (int p = 0; p < num_projectiles; p++) {
        if (!projectiles[p].active) continue;

        if (projectiles[p].is_missile) {
            if (check_intercepts(&projectiles[p], tick)) {
                stat_misses[projectiles[p].attacker_idx >= 0 ?
                    ships[projectiles[p].attacker_idx].side : SIDE_NATO]++;
                continue;
            }
        }

        update_projectile_physics(&projectiles[p], 1.0);

        if (!projectiles[p].active) {
            Side as = (projectiles[p].attacker_idx>=0) ?
                ships[projectiles[p].attacker_idx].side : SIDE_NATO;
            stat_misses[as]++;
            log_event(tick, projectiles[p].attacker, projectiles[p].target,
                      projectiles[p].weapon_name, 0, 0, -1, 0, EVT_MISS);
            continue;
        }

        /* Check impacts */
        for (int i = 0; i < num_ships; i++) {
            if (!ships[i].alive) continue;
            if (strcmp(ships[i].name, projectiles[p].attacker) == 0) continue;
            Side as = (projectiles[p].attacker_idx>=0) ?
                ships[projectiles[p].attacker_idx].side : SIDE_NATO;
            if (ships[i].side == as) continue;

            double dx = projectiles[p].x - ships[i].x;
            double dy = projectiles[p].y - ships[i].y;
            double ddist = sqrt(dx*dx + dy*dy);

            double hit_r = 0.05;
            if (ships[i].ship_class == CLASS_CARRIER)  hit_r = 0.15;
            else if (ships[i].ship_class == CLASS_CRUISER) hit_r = 0.10;

            if (projectiles[p].is_torpedo) {
                if (ddist > hit_r) continue;
                if (check_torpedo_decoy(&projectiles[p], &ships[i], tick)) break;
            } else {
                if (ddist > hit_r) continue;
            }

            /* HARM: extra damage to radar compartment */
            double dmg = projectiles[p].damage * (1.0 - ships[i].armor);
            WeaponTemplate *wt = find_weapon(projectiles[p].weapon_name);
            double gq = wt ? wt->guidance_quality : 0.5;
            dmg *= rng_gauss(1.0, gq > 0.7 ? 0.10 : 0.25);
            if (dmg < 1) dmg = 1;

            int comp;
            if (projectiles[p].is_harm) {
                /* HARM specifically targets radar installations */
                comp = COMP_RADAR;
                ships[i].compartments[comp].integrity -= dmg / ships[i].max_hp * 2.0;
                stat_harm_hits[as]++;
                evt_log(tick, CP_MAGENTA, "[HARM-HIT] %s > %s radar %.0f dmg",
                        projectiles[p].weapon_name, ships[i].name, dmg);
            } else {
                comp = (int)(rng_uniform() * MAX_COMPARTMENTS);
            }

            if (comp < MAX_COMPARTMENTS) {
                ships[i].compartments[comp].integrity -= dmg / ships[i].max_hp;
                if (ships[i].compartments[comp].integrity < 0.3) {
                    ships[i].compartments[comp].flooding = 1;
                    ships[i].compartments[comp].flood_rate = rng_uniform() * 20 + 2;
                }
                if (rng_uniform() < 0.25)
                    ships[i].compartments[comp].on_fire = 1;
            }

            ships[i].hp -= dmg;
            if (projectiles[p].attacker_idx >= 0)
                ships[projectiles[p].attacker_idx].damage_dealt_total += (int)dmg;
            stat_hits[as]++;

            int killed = 0;
            if (ships[i].hp <= 0) {
                ships[i].hp = 0; ships[i].alive = 0; killed = 1;
                if (projectiles[p].attacker_idx >= 0)
                    ships[projectiles[p].attacker_idx].kills++;
                stat_kills_by_side[as]++;
            }

            int cpair = CP_ALERT;
            if (!projectiles[p].is_harm) {
                evt_log(tick, cpair, "[IMPACT] %s > %s %.0f dmg%s [HP:%.0f/%.0f]",
                        projectiles[p].weapon_name, ships[i].name, dmg,
                        killed ? " ***SUNK***" : "",
                        ships[i].hp, ships[i].max_hp);
            }

            log_event(tick, projectiles[p].attacker, ships[i].name,
                      projectiles[p].weapon_name, 1, dmg, ships[i].hp,
                      killed, projectiles[p].is_harm ? EVT_HARM_STRIKE : EVT_HIT);

            projectiles[p].active = 0;
            break;
        }
    }

    /* Compact */
    if (tick % 20 == 0) {
        int ac = 0;
        for (int p = 0; p < num_projectiles; p++) {
            if (projectiles[p].active) {
                if (p != ac) projectiles[ac] = projectiles[p];
                ac++;
            }
        }
        num_projectiles = ac;
    }
}

/* ── Air-to-Air Combat ──────────────────────────────────── */
/* CAP sorties intercept enemy STRIKE sorties */
static void phase_air_to_air(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *carrier = &ships[i];
        if (!carrier->alive || carrier->max_sorties == 0) continue;

        for (int s = 0; s < MAX_SORTIES; s++) {
            AircraftSortie *cap = &carrier->sorties[s];
            if (!cap->active || cap->type != SORTIE_CAP) continue;
            if (cap->destroyed) continue;

            /* CAP patrols area around carrier */
            cap->x = carrier->x + rng_gauss(0, 30);
            cap->y = carrier->y + rng_gauss(0, 30);

            /* Look for enemy strike sorties within intercept range */
            for (int j = 0; j < num_ships; j++) {
                Ship *enemy_cv = &ships[j];
                if (!enemy_cv->alive || enemy_cv->side == carrier->side) continue;
                if (enemy_cv->max_sorties == 0) continue;

                for (int es = 0; es < MAX_SORTIES; es++) {
                    AircraftSortie *strike = &enemy_cv->sorties[es];
                    if (!strike->active || strike->destroyed) continue;
                    if (strike->type != SORTIE_STRIKE) continue;

                    /* Compute strike position (interpolate toward target) */
                    double progress = (double)(tick - strike->launch_tick) /
                                      (double)(strike->return_tick - strike->launch_tick + 1);
                    if (progress > 1.0) progress = 1.0;
                    double strike_x = enemy_cv->x;
                    double strike_y = enemy_cv->y;
                    if (strike->target_ship_idx >= 0 && strike->target_ship_idx < num_ships) {
                        strike_x += (ships[strike->target_ship_idx].x - enemy_cv->x) * progress;
                        strike_y += (ships[strike->target_ship_idx].y - enemy_cv->y) * progress;
                    }
                    strike->x = strike_x;
                    strike->y = strike_y;

                    /* Check intercept range */
                    double d = dist_nm_xy(cap->x, cap->y, strike_x, strike_y);
                    if (d > 80) continue; /* beyond CAP patrol range */

                    /* Air combat resolution */
                    /* NATO F-14/F-18 vs PACT Su-33 */
                    double p_kill;
                    if (carrier->side == SIDE_NATO) {
                        /* F-14/F-18: better BVR capability, AIM-120 AMRAAM */
                        p_kill = 0.35;
                        if (carrier->stobar) p_kill = 0.25; /* shouldn't happen for NATO */
                    } else {
                        /* Su-33: good dogfighter but inferior BVR */
                        p_kill = 0.20;
                    }

                    /* AEW bonus: if friendly AEW is up, better intercept */
                    for (int ck = 0; ck < MAX_SORTIES; ck++) {
                        if (carrier->sorties[ck].active &&
                            carrier->sorties[ck].type == SORTIE_AEW)
                            p_kill += 0.10;
                    }

                    /* Only attempt intercept once per tick per pairing */
                    if (rng_uniform() < p_kill * 0.02) { /* per-tick probability */
                        strike->destroyed = 1;
                        strike->effect_applied = 1; /* prevent damage delivery */
                        stat_a2a_kills[carrier->side]++;
                        evt_log(tick, CP_MAGENTA,
                                "[A2A-KILL] %s CAP shot down %s strike sortie",
                                carrier->name, enemy_cv->name);
                        log_event(tick, carrier->name, enemy_cv->name,
                                  "Air-to-Air", 1, 0, 0, 0, EVT_AIR_TO_AIR);
                    }
                }
            }
        }
    }
}

/* ── Helicopter Phase ────────────────────────────────────── */
static void phase_helicopters(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive || s->helo_capacity == 0) continue;
        if (s->is_submerged) continue; /* subs don't launch helos */

        /* Process returning helos */
        for (int h = 0; h < 4; h++) {
            AircraftSortie *helo = &s->helo_sorties[h];
            if (!helo->active) continue;

            /* Update helo position */
            helo->x = s->x + rng_gauss(0, 15);
            helo->y = s->y + rng_gauss(0, 15);

            /* Mid-mission ASW attack */
            if (!helo->effect_applied && tick >= (helo->launch_tick + helo->return_tick)/2) {
                helo->effect_applied = 1;

                /* Hunt nearest enemy sub */
                for (int j = 0; j < num_ships; j++) {
                    if (!ships[j].alive || ships[j].side == s->side) continue;
                    if (ships[j].ship_class != CLASS_SUBMARINE) continue;
                    if (!ships[j].detected) continue;
                    double d = dist_nm(s, &ships[j]);
                    if (d > 40) continue; /* helo ASW range from parent ship */

                    double p_kill = 0.30 * wx_asw_helo;
                    /* SH-60 with dipping sonar */
                    if (s->side == SIDE_NATO) p_kill += 0.10;

                    if (rng_uniform() < p_kill) {
                        double dmg = rng_gauss(140, 25) * (1.0 - ships[j].armor);
                        if (dmg < 20) dmg = 20;
                        ships[j].hp -= dmg;
                        s->damage_dealt_total += (int)dmg;
                        stat_helo_asw[s->side]++;

                        int killed = (ships[j].hp <= 0);
                        if (killed) {
                            ships[j].hp = 0; ships[j].alive = 0;
                            s->kills++;
                            stat_kills_by_side[s->side]++;
                        }
                        evt_log(tick, CP_MAGENTA,
                                "[HELO-ASW] %s SH-60 > %s %.0f dmg%s",
                                s->name, ships[j].name, dmg,
                                killed ? " ***SUNK***" : "");
                        log_event(tick, s->name, ships[j].name, "Helo ASW Torpedo",
                                  1, dmg, ships[j].hp, killed, EVT_ASW_ATTACK);
                    }
                    break;
                }

                /* Helo also extends sonar detection */
                for (int j = 0; j < num_ships; j++) {
                    if (!ships[j].alive || ships[j].side == s->side) continue;
                    if (!ships[j].is_submerged) continue;
                    double d = dist_nm(s, &ships[j]);
                    if (d < 30) {
                        double p_det = 0.40 * wx_asw_helo;
                        if (rng_uniform() < p_det) {
                            ships[j].detected = 1;
                            evt_log(tick, CP_CYAN,
                                    "[HELO-SONAR] %s helo detected %s %.0fNM",
                                    s->name, ships[j].name, d);
                        }
                    }
                }
            }

            /* Return */
            if (tick >= helo->return_tick) {
                helo->active = 0;
                s->helos_in_flight--;
                s->helos_available++;
            }
        }

        /* Launch new helo sorties every 120 ticks */
        if (tick % 120 != 0) continue;
        if (s->helos_available <= 0) continue;
        if (s->helos_in_flight >= s->helo_capacity) continue;

        /* Only launch if there's a sub threat or ASW mission */
        int sub_threat = 0;
        for (int j = 0; j < num_ships; j++) {
            if (!ships[j].alive || ships[j].side == s->side) continue;
            if (ships[j].ship_class == CLASS_SUBMARINE) {
                if (ships[j].detected || dist_nm(s, &ships[j]) < 50) {
                    sub_threat = 1;
                    break;
                }
            }
        }
        if (!sub_threat && rng_uniform() > 0.3) continue; /* sometimes launch patrol anyway */

        for (int h = 0; h < 4; h++) {
            if (s->helo_sorties[h].active) continue;
            AircraftSortie *helo = &s->helo_sorties[h];
            helo->active = 1;
            helo->type = SORTIE_HELO_ASW;
            helo->launch_tick = tick;
            helo->return_tick = tick + 360; /* 6 minute mission */
            helo->target_ship_idx = -1;
            helo->effect_applied = 0;
            helo->destroyed = 0;
            helo->x = s->x;
            helo->y = s->y;
            s->helos_available--;
            s->helos_in_flight++;
            evt_log(tick, CP_MAGENTA, "[HELO] %s launches SH-60 ASW sortie (%d/%d)",
                    s->name, s->helos_in_flight, s->helo_capacity);
            break;
        }
    }
}

/* ── Aviation Phase ──────────────────────────────────────── */
static void phase_aviation(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *carrier = &ships[i];
        if (!carrier->alive || carrier->max_sorties == 0) continue;

        /* Process returning sorties */
        for (int s = 0; s < MAX_SORTIES; s++) {
            AircraftSortie *sr = &carrier->sorties[s];
            if (!sr->active) continue;

            /* Update sortie position for map display */
            if (sr->target_ship_idx >= 0 && sr->target_ship_idx < num_ships) {
                double progress = (double)(tick - sr->launch_tick) /
                                  (double)(sr->return_tick - sr->launch_tick + 1);
                if (progress > 1.0) progress = 1.0;
                if (progress > 0.5) progress = 1.0 - progress; /* return leg */
                sr->x = carrier->x + (ships[sr->target_ship_idx].x - carrier->x) * progress * 2.0;
                sr->y = carrier->y + (ships[sr->target_ship_idx].y - carrier->y) * progress * 2.0;
            } else {
                sr->x = carrier->x + rng_gauss(0, 20);
                sr->y = carrier->y + rng_gauss(0, 20);
            }

            /* Mid-mission effect */
            if (!sr->effect_applied && tick >= (sr->launch_tick + sr->return_tick)/2) {
                sr->effect_applied = 1;

                if (sr->destroyed) {
                    /* Shot down by CAP — no effect */
                } else if (sr->type == SORTIE_STRIKE && sr->target_ship_idx >= 0) {
                    Ship *tgt = &ships[sr->target_ship_idx];
                    if (tgt->alive) {
                        double base_dmg = carrier->stobar ? 120.0 : 170.0;
                        double dmg = rng_gauss(base_dmg, 30.0) * (1.0 - tgt->armor);
                        dmg *= (0.75 + wx_ir * 0.25);
                        if (dmg < 20) dmg = 20;
                        for (int h = 0; h < 2; h++) {
                            int c = (int)(rng_uniform() * MAX_COMPARTMENTS);
                            tgt->compartments[c].integrity -= dmg / (2.0 * tgt->max_hp);
                            if (tgt->compartments[c].integrity < 0.3) {
                                tgt->compartments[c].flooding = 1;
                                tgt->compartments[c].flood_rate = rng_uniform()*15+2;
                            }
                            if (rng_uniform() < 0.35) tgt->compartments[c].on_fire = 1;
                        }
                        tgt->hp -= dmg;
                        carrier->damage_dealt_total += (int)dmg;
                        stat_air_strikes[carrier->side]++;

                        int killed = 0;
                        if (tgt->hp <= 0) {
                            tgt->hp=0; tgt->alive=0; killed=1;
                            carrier->kills++;
                            stat_kills_by_side[carrier->side]++;
                        }
                        evt_log(tick, CP_MAGENTA,
                                "[AIR STRIKE] %s > %s %.0f dmg%s [HP:%.0f/%.0f]",
                                carrier->name, tgt->name, dmg,
                                killed?" ***SUNK***":"",
                                tgt->hp, tgt->max_hp);
                        log_event(tick, carrier->name, tgt->name, "Air Strike",
                                  1, dmg, tgt->hp, killed, EVT_AIR_STRIKE);
                    }
                } else if (sr->type == SORTIE_ASW) {
                    for (int j = 0; j < num_ships; j++) {
                        if (!ships[j].alive || ships[j].side==carrier->side) continue;
                        if (ships[j].ship_class != CLASS_SUBMARINE) continue;
                        if (!ships[j].detected) continue;
                        double d = dist_nm(carrier, &ships[j]);
                        if (d > 80) continue;

                        double p_kill = 0.35 * wx_asw_helo;
                        if (rng_uniform() < p_kill) {
                            double dmg = rng_gauss(150, 30) * (1.0-ships[j].armor);
                            if (dmg < 20) dmg = 20;
                            ships[j].hp -= dmg;
                            carrier->damage_dealt_total += (int)dmg;
                            stat_asw_attacks[carrier->side]++;
                            int killed = (ships[j].hp <= 0);
                            if (killed) {
                                ships[j].hp=0; ships[j].alive=0;
                                carrier->kills++;
                                stat_kills_by_side[carrier->side]++;
                            }
                            evt_log(tick, CP_MAGENTA,
                                    "[ASW HIT] %s helo > %s %.0f dmg%s",
                                    carrier->name, ships[j].name, dmg,
                                    killed?" ***SUNK***":"");
                            log_event(tick, carrier->name, ships[j].name, "ASW Torpedo",
                                      1, dmg, ships[j].hp, killed, EVT_ASW_ATTACK);
                        }
                        break;
                    }
                } else if (sr->type == SORTIE_AEW) {
                    carrier->search_radar.range_nm =
                        fmin(carrier->search_radar.base_range_nm * 1.6, 300.0);
                }
            }

            /* Return */
            if (tick >= sr->return_tick) {
                if (sr->type == SORTIE_AEW)
                    carrier->search_radar.range_nm = carrier->search_radar.base_range_nm;
                sr->active = 0;
                carrier->sorties_in_flight--;
                carrier->sorties_available++;
            }
        }

        /* Launch new sorties */
        if (tick % 60 != 0) continue;
        if (carrier->sorties_available <= 0) continue;
        if (carrier->sorties_in_flight >= carrier->max_sorties / 3) continue;

        SortieType stype = SORTIE_CAP;
        int tgt_idx = -1;

        int aew_up = 0, cap_up = 0;
        for (int s2 = 0; s2 < MAX_SORTIES; s2++) {
            if (!carrier->sorties[s2].active) continue;
            if (carrier->sorties[s2].type == SORTIE_AEW) aew_up = 1;
            if (carrier->sorties[s2].type == SORTIE_CAP) cap_up++;
        }

        /* Priority: AEW first, then CAP if none, then ASW, then strike */
        if (!aew_up && !carrier->stobar) {
            stype = SORTIE_AEW;
        } else if (cap_up < 2) {
            stype = SORTIE_CAP; /* maintain at least 2 CAP sorties */
        } else {
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side==carrier->side) continue;
                if (ships[j].ship_class!=CLASS_SUBMARINE || !ships[j].detected) continue;
                if (dist_nm(carrier, &ships[j]) < 80) { stype=SORTIE_ASW; break; }
            }
            if (stype == SORTIE_CAP) {
                double best = -1;
                for (int j = 0; j < num_ships; j++) {
                    if (!ships[j].alive || ships[j].side==carrier->side) continue;
                    if (!ships[j].detected) continue;
                    if (ships[j].is_submerged) continue;
                    double sc = 0;
                    if (ships[j].ship_class==CLASS_CARRIER) sc=1.0;
                    else if (ships[j].ship_class==CLASS_CRUISER) sc=0.8;
                    else sc = 0.5;
                    sc /= (1 + dist_nm(carrier, &ships[j])/100.0);
                    if (sc > best) { best=sc; tgt_idx=j; }
                }
                if (tgt_idx >= 0) stype = SORTIE_STRIKE;
            }
        }

        for (int s2 = 0; s2 < MAX_SORTIES; s2++) {
            if (carrier->sorties[s2].active) continue;
            AircraftSortie *sr = &carrier->sorties[s2];
            sr->active = 1;
            sr->type = stype;
            sr->launch_tick = tick;
            sr->destroyed = 0;
            int dur = 0;
            if (stype==SORTIE_AEW)    dur = carrier->stobar ? 0 : 1200;
            else if (stype==SORTIE_STRIKE) dur = carrier->stobar ? 900 : 720;
            else if (stype==SORTIE_ASW)    dur = 480;
            else                           dur = 600;
            sr->return_tick = tick + dur;
            sr->target_ship_idx = tgt_idx;
            sr->effect_applied = 0;
            sr->x = carrier->x;
            sr->y = carrier->y;
            carrier->sorties_available--;
            carrier->sorties_in_flight++;
            evt_log(tick, CP_MAGENTA, "[SORTIE] %s launches %s (%d/%d)%s",
                    carrier->name, sortie_str(stype),
                    carrier->sorties_in_flight, carrier->max_sorties,
                    carrier->stobar?" [STOBAR]":"");
            break;
        }
    }
}

/* ── Damage Consequences Phase ───────────────────────────── */
static void phase_damage_consequences(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive) continue;

        for (int c = 0; c < MAX_COMPARTMENTS; c++) {
            DamageCompartment *comp = &s->compartments[c];

            if (comp->on_fire) {
                comp->integrity -= 0.0005;
                if (rng_uniform() < 0.003) {
                    int adj = (c + 1) % MAX_COMPARTMENTS;
                    s->compartments[adj].on_fire = 1;
                }
                if (c == COMP_MAGAZINE && comp->on_fire && comp->integrity < 0.3) {
                    if (rng_uniform() < 0.0005) {
                        evt_log(tick, CP_ALERT,
                                "[MAG EXPLOSION] %s magazine detonation!", s->name);
                        s->hp = 0; s->alive = 0;
                        stat_kills_by_side[1 - s->side]++;
                        break;
                    }
                }
            }

            if (comp->flooding) {
                s->flooding_total += comp->flood_rate / 60.0;
                comp->integrity -= 0.0001;
            }
        }

        if (!s->alive) continue;

        double r_int = s->compartments[COMP_RADAR].integrity;
        s->radar_effectiveness = r_int > 0.5 ? 1.0 : 0.5 + r_int;
        if (s->radar_effectiveness < 0.3) s->radar_effectiveness = 0.3;

        /* HARM damage: if radar compartment badly hit, shut down radar */
        if (r_int < 0.3) {
            s->search_radar.emitting = 0;
            s->fire_control_radar.emitting = 0;
        }

        double b_int = s->compartments[COMP_BRIDGE].integrity;
        s->targeting_effectiveness = b_int > 0.5 ? 1.0 : 0.4 + b_int;
        if (s->targeting_effectiveness < 0.3) s->targeting_effectiveness = 0.3;

        double e_int = s->compartments[COMP_ENGINE].integrity;
        if (e_int < 0.5) {
            s->speed_reduced = 1;
            s->max_speed_kts = s->base_max_speed_kts * (0.4 + e_int * 0.6);
            if (s->speed_kts > s->max_speed_kts) s->speed_kts = s->max_speed_kts;
        }

        /* Comms damage degrades Link-16 */
        double c_int = s->compartments[COMP_COMMS].integrity;
        if (c_int < 0.5) {
            s->has_link16 = 0;
        }

        s->fwd_weapons_disabled = (s->compartments[COMP_FORWARD_WEAPONS].integrity < 0.2);
        s->aft_weapons_disabled = (s->compartments[COMP_AFT_WEAPONS].integrity < 0.2);

        double capsize_threshold = s->max_hp * 2.5;
        if (s->flooding_total > 200) {
            double flood_pct = s->flooding_total / capsize_threshold;
            double spd_pen = 1.0 - fmin(flood_pct * 0.7, 0.80);
            if (s->max_speed_kts > s->base_max_speed_kts * spd_pen)
                s->max_speed_kts = s->base_max_speed_kts * spd_pen;
            s->list_angle = flood_pct * 20.0;
        }

        if (s->flooding_total > capsize_threshold) {
            evt_log(tick, CP_ALERT, "[CAPSIZE] %s capsizes! (%.0f tons)",
                    s->name, s->flooding_total);
            s->alive = 0;
            stat_capsized[s->side]++;
            log_event(tick, "flooding", s->name, "flooding",
                      1, s->flooding_total, 0, 1, EVT_CAPSIZE);
        }
    }
}

/* ── TUI Setup and Drawing ──────────────────────────────── */
static int tui_cols = 0, tui_rows = 0;
static int map_w = 0, map_h = 0;
static int log_w = 0, log_h = 0;

static void tui_init(void) {
    setlocale(LC_ALL, "");
#ifdef _WIN32
    /* Enable VT processing for Windows Terminal */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();

        /* Try to define a custom dark navy background color (index 16) */
        int ocean_bg = COLOR_BLUE;
        if (can_change_color() && COLORS >= 17) {
            init_color(16, 0, 0, 350);  /* dark navy: R=0 G=0 B=35% */
            ocean_bg = 16;
        }

        init_pair(CP_NATO,     COLOR_GREEN,  -1);
        init_pair(CP_PACT,     COLOR_RED,    -1);
        init_pair(CP_HEADER,   COLOR_CYAN,   ocean_bg);
        init_pair(CP_ALERT,    COLOR_RED,    -1);
        init_pair(CP_DIM,      COLOR_WHITE,  -1);
        init_pair(CP_CYAN,     COLOR_CYAN,   -1);
        init_pair(CP_MAGENTA,  COLOR_MAGENTA,-1);
        init_pair(CP_YELLOW,   COLOR_YELLOW, -1);
        init_pair(CP_WHITE,    COLOR_WHITE,  -1);
        init_pair(CP_MAP_BG,   COLOR_CYAN,   ocean_bg);  /* cyan grid on navy */
        init_pair(CP_MAP_NATO, COLOR_GREEN,  ocean_bg);
        init_pair(CP_MAP_PACT, COLOR_RED,    ocean_bg);
        init_pair(CP_MAP_DEAD, COLOR_WHITE,  COLOR_RED);
        init_pair(CP_MAP_PROJ, COLOR_YELLOW, ocean_bg);
        init_pair(CP_MAP_AIR,  COLOR_CYAN,   ocean_bg);
        init_pair(CP_BORDER,   COLOR_CYAN,   -1);
        /* New color pairs */
        init_pair(CP_SPEED,      COLOR_YELLOW,  -1);
        init_pair(CP_HELP_KEY,   COLOR_YELLOW,  ocean_bg);
        init_pair(CP_HELP_DESC,  COLOR_WHITE,   ocean_bg);
        init_pair(CP_HP_HIGH,    COLOR_GREEN,   -1);
        init_pair(CP_HP_MED,     COLOR_YELLOW,  -1);
        init_pair(CP_HP_LOW,     COLOR_RED,     -1);
        init_pair(CP_TITLE_BAR,  COLOR_WHITE,   ocean_bg);
        init_pair(CP_MODE_PLAY,  COLOR_GREEN,   ocean_bg);
        init_pair(CP_MODE_PAUSE, COLOR_YELLOW,  ocean_bg);
    }

    /* Must refresh stdscr before creating panels (ncurses 6.5 requirement) */
    refresh();

    getmaxyx(stdscr, tui_rows, tui_cols);

    /* Layout:
     * Row 0-2: Header (full width)
     * Row 3 to (rows-12): Left 60% = Map, Right 40% = Event Log
     * Bottom 11 rows: Left = NATO status, Middle = PACT status, Right = Stats
     */
    int header_h = 3;
    int bottom_h = 12;
    int mid_h = tui_rows - header_h - bottom_h;
    if (mid_h < 10) mid_h = 10;

    map_w = tui_cols * 60 / 100;
    if (map_w > tui_cols - 40) map_w = tui_cols - 40;
    map_h = mid_h;

    log_w = tui_cols - map_w;
    log_h = mid_h;

    int bottom_w1 = tui_cols / 3;
    int bottom_w2 = tui_cols / 3;
    int bottom_w3 = tui_cols - bottom_w1 - bottom_w2;

    win_header = newwin(header_h, tui_cols, 0, 0);
    win_map    = newwin(map_h, map_w, header_h, 0);
    win_log    = newwin(log_h, log_w, header_h, map_w);
    win_nato   = newwin(bottom_h, bottom_w1, header_h + mid_h, 0);
    win_pact   = newwin(bottom_h, bottom_w2, header_h + mid_h, bottom_w1);
    win_stats  = newwin(bottom_h, bottom_w3, header_h + mid_h, bottom_w1 + bottom_w2);

    /* Set background colors */
    wbkgd(win_header, COLOR_PAIR(CP_TITLE_BAR));
    wbkgd(win_map, COLOR_PAIR(CP_MAP_BG));
    keypad(win_map, TRUE); /* for arrow keys in subwindows */
}

static void tui_cleanup(void) {
    delwin(win_header);
    delwin(win_map);
    delwin(win_log);
    delwin(win_nato);
    delwin(win_pact);
    delwin(win_stats);
    endwin();
}

static void draw_header(int tick) {
    werase(win_header);
    wattron(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(win_header, 0, 1,
              " NAVSIM v4.0 | Atlantic PoI 24.19N 43.37W | T+%02d:%02d | SS%d %s | Seed:%u",
              tick/60, tick%60, sea_state, beaufort_names[sea_state], g_seed);

    /* Mode / speed indicator */
    if (play_mode == MODE_PAUSED) {
        wattron(win_header, COLOR_PAIR(CP_MODE_PAUSE) | A_BOLD);
        wprintw(win_header, " [PAUSED %d/%d]", view_snap_idx + 1, snap_count);
        wattroff(win_header, COLOR_PAIR(CP_MODE_PAUSE) | A_BOLD);
    } else {
        wattron(win_header, COLOR_PAIR(CP_MODE_PLAY) | A_BOLD);
        wprintw(win_header, " [%s]", speed_labels[sim_speed]);
        wattroff(win_header, COLOR_PAIR(CP_MODE_PLAY) | A_BOLD);
    }
    wattroff(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);

    /* Controls bar (replaces old feature bar) */
    int col = 1;
    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    mvwprintw(win_header, 1, col, " SPC");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Pause ");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "<");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, "/");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));
    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, ">");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Rew/Fwd ");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "[");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, "/");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));
    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "]");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Step ");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "+/-");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Speed ");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "?");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Help ");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    wattron(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wprintw(win_header, "Q");
    wattroff(win_header, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    wattron(win_header, COLOR_PAIR(CP_HELP_DESC));
    wprintw(win_header, ":Quit");
    wattroff(win_header, COLOR_PAIR(CP_HELP_DESC));

    /* Horizontal separator */
    wattron(win_header, COLOR_PAIR(CP_BORDER));
    mvwhline(win_header, 2, 0, ACS_HLINE, tui_cols);
    wattroff(win_header, COLOR_PAIR(CP_BORDER));
}

static void draw_map(void) {
    werase(win_map);
    int mw = map_w - 2; /* inner width */
    int mh = map_h - 2; /* inner height */

    /* Border */
    wattron(win_map, COLOR_PAIR(CP_BORDER));
    box(win_map, 0, 0);
    mvwprintw(win_map, 0, 2, " TACTICAL PLOT %dx%d NM ", GRID_SIZE, GRID_SIZE);
    wattroff(win_map, COLOR_PAIR(CP_BORDER));

    /* Draw grid dots — background already set by wbkgd */
    for (int r = 1; r <= mh; r++) {
        for (int c = 1; c <= mw; c++) {
            if (r % 4 == 0 && c % 8 == 0) {
                wattron(win_map, COLOR_PAIR(CP_MAP_BG) | A_DIM);
                mvwaddch(win_map, r, c, '+');
                wattroff(win_map, COLOR_PAIR(CP_MAP_BG) | A_DIM);
            }
        }
    }

    /* Draw projectiles */
    wattron(win_map, COLOR_PAIR(CP_MAP_PROJ) | A_BOLD);
    for (int p = 0; p < num_projectiles; p++) {
        if (!projectiles[p].active) continue;
        int c = 1 + (int)(projectiles[p].x * mw / GRID_SIZE);
        int r = mh - (int)(projectiles[p].y * mh / GRID_SIZE);
        if (c < 1) c = 1; if (c > mw) c = mw;
        if (r < 1) r = 1; if (r > mh) r = mh;
        mvwaddch(win_map, r, c, projectiles[p].is_torpedo ? '~' : '*');
    }
    wattroff(win_map, COLOR_PAIR(CP_MAP_PROJ) | A_BOLD);

    /* Draw aircraft sorties */
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive) continue;
        int cpair = (s->side == SIDE_NATO) ? CP_MAP_NATO : CP_MAP_PACT;

        /* Carrier sorties */
        for (int j = 0; j < MAX_SORTIES; j++) {
            AircraftSortie *sr = &s->sorties[j];
            if (!sr->active || sr->destroyed) continue;
            int c = 1 + (int)(sr->x * mw / GRID_SIZE);
            int r = mh - (int)(sr->y * mh / GRID_SIZE);
            if (c < 1) c = 1; if (c > mw) c = mw;
            if (r < 1) r = 1; if (r > mh) r = mh;
            wattron(win_map, COLOR_PAIR(CP_MAP_AIR) | A_BOLD);
            char ac = '^';
            if (sr->type == SORTIE_STRIKE) ac = '!';
            else if (sr->type == SORTIE_ASW) ac = 'w';
            else if (sr->type == SORTIE_AEW) ac = 'E';
            mvwaddch(win_map, r, c, ac);
            wattroff(win_map, COLOR_PAIR(CP_MAP_AIR) | A_BOLD);
        }

        /* Helo sorties */
        for (int j = 0; j < 4; j++) {
            AircraftSortie *h = &s->helo_sorties[j];
            if (!h->active) continue;
            int c = 1 + (int)(h->x * mw / GRID_SIZE);
            int r = mh - (int)(h->y * mh / GRID_SIZE);
            if (c < 1) c = 1; if (c > mw) c = mw;
            if (r < 1) r = 1; if (r > mh) r = mh;
            wattron(win_map, COLOR_PAIR(cpair) | A_BOLD);
            mvwaddch(win_map, r, c, 'h');
            wattroff(win_map, COLOR_PAIR(cpair) | A_BOLD);
        }
    }

    /* Draw ships */
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        int c = 1 + (int)(s->x * mw / GRID_SIZE);
        int r = mh - (int)(s->y * mh / GRID_SIZE);
        if (c < 1) c = 1; if (c > mw) c = mw;
        if (r < 1) r = 1; if (r > mh) r = mh;

        if (!s->alive) {
            wattron(win_map, COLOR_PAIR(CP_MAP_DEAD) | A_BOLD);
            mvwaddch(win_map, r, c, 'X');
            wattroff(win_map, COLOR_PAIR(CP_MAP_DEAD) | A_BOLD);
        } else {
            int cpair = (s->side == SIDE_NATO) ? CP_MAP_NATO : CP_MAP_PACT;
            int attr = A_BOLD;
            double hp_pct = s->hp / s->max_hp;
            if (hp_pct < 0.3) attr |= A_BLINK;

            char sym;
            switch (s->ship_class) {
                case CLASS_CARRIER:    sym = (s->side==SIDE_NATO)?'A':'a'; break;
                case CLASS_CRUISER:    sym = (s->side==SIDE_NATO)?'C':'c'; break;
                case CLASS_DESTROYER:  sym = (s->side==SIDE_NATO)?'D':'d'; break;
                case CLASS_FRIGATE:    sym = (s->side==SIDE_NATO)?'F':'f'; break;
                case CLASS_CORVETTE:   sym = (s->side==SIDE_NATO)?'V':'v'; break;
                case CLASS_SUBMARINE:  sym = (s->side==SIDE_NATO)?'S':'s'; break;
                case CLASS_MISSILE_BOAT: sym = (s->side==SIDE_NATO)?'M':'m'; break;
                default:               sym = (s->side==SIDE_NATO)?'?':'?'; break;
            }

            wattron(win_map, COLOR_PAIR(cpair) | attr);
            mvwaddch(win_map, r, c, sym);
            wattroff(win_map, COLOR_PAIR(cpair) | attr);

            /* Ship name label (abbreviated) */
            if (s->ship_class <= CLASS_CRUISER || s->ship_class == CLASS_SUBMARINE) {
                char label[12];
                snprintf(label, sizeof(label), "%.8s", s->name);
                wattron(win_map, COLOR_PAIR(cpair) | A_DIM);
                if (c + 2 + (int)strlen(label) <= mw)
                    mvwprintw(win_map, r, c + 1, " %s", label);
                wattroff(win_map, COLOR_PAIR(cpair) | A_DIM);
            }
        }
    }

    /* Legend */
    int ly = mh;
    wattron(win_map, COLOR_PAIR(CP_DIM));
    mvwprintw(win_map, ly, 1,
              "A/a=CV C/c=CG D/d=DD F/f=FF S/s=SS M/m=PGG ^=CAP !=STK h=Helo *=Missile ~=Torp");
    wattroff(win_map, COLOR_PAIR(CP_DIM));
}

static void draw_event_log(void) {
    werase(win_log);
    wattron(win_log, COLOR_PAIR(CP_BORDER));
    box(win_log, 0, 0);
    mvwprintw(win_log, 0, 2, " EVENT LOG ");
    wattroff(win_log, COLOR_PAIR(CP_BORDER));

    int inner_h = log_h - 2;
    int inner_w = log_w - 2;
    int start = 0;
    if (event_log_count > inner_h)
        start = event_log_count - inner_h;

    for (int i = start; i < event_log_count; i++) {
        int idx = (event_log_head + i) % MAX_EVENTS;
        int row = 1 + (i - start);
        if (row >= log_h - 1) break;

        wattron(win_log, COLOR_PAIR(CP_DIM));
        mvwprintw(win_log, row, 1, "%02d:%02d ",
                  event_log[idx].tick / 60, event_log[idx].tick % 60);
        wattroff(win_log, COLOR_PAIR(CP_DIM));

        wattron(win_log, COLOR_PAIR(event_log[idx].color_pair));
        /* Truncate to fit */
        char buf[256];
        snprintf(buf, inner_w - 6, "%s", event_log[idx].text);
        wprintw(win_log, "%s", buf);
        wattroff(win_log, COLOR_PAIR(event_log[idx].color_pair));
    }
}

static void draw_ship_panel(WINDOW *win, int side, int w, int h) {
    werase(win);
    wattron(win, COLOR_PAIR(CP_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(CP_BORDER));

    int cpair = (side == SIDE_NATO) ? CP_NATO : CP_PACT;
    const char *title = (side == SIDE_NATO) ? " NATO FORCES " : " PACT FORCES ";
    wattron(win, COLOR_PAIR(cpair) | A_BOLD);
    mvwprintw(win, 0, 2, "%s", title);
    wattroff(win, COLOR_PAIR(cpair) | A_BOLD);

    int inner_w = w - 2;
    (void)inner_w;

    /* Column headers */
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 1, "%-16s %3s %4s %5s %3s %4s %3s", "Ship", "Cls", "HP%", "Flood", "Kts", "Ammo", "K");
    wattroff(win, A_BOLD);

    int row = 2;
    for (int i = 0; i < num_ships && row < h - 1; i++) {
        Ship *s = &ships[i];
        if ((int)s->side != side) continue;

        int total_ammo = 0;
        for (int ww = 0; ww < s->num_weapons; ww++) total_ammo += s->weapons[ww].ammo;
        double hp_pct = s->max_hp > 0 ? (s->hp / s->max_hp) * 100 : 0;

        int sc = CP_WHITE;
        if (!s->alive) sc = CP_DIM;
        else if (hp_pct < 30) sc = CP_ALERT;
        else if (hp_pct < 60) sc = CP_YELLOW;
        else sc = cpair;

        wattron(win, COLOR_PAIR(sc));
        char name_buf[17];
        snprintf(name_buf, sizeof(name_buf), "%.16s", s->name);

        char extra[16] = "";
        if (s->max_sorties > 0)
            snprintf(extra, sizeof(extra), "[%d/%dA]", s->sorties_in_flight, s->max_sorties);
        else if (s->helo_capacity > 0)
            snprintf(extra, sizeof(extra), "[%dH]", s->helos_in_flight);

        mvwprintw(win, row, 1, "%-16s %3s %3.0f%% %5.0f %3.0f %4d %2d %s",
                  name_buf, class_str(s->ship_class),
                  hp_pct, s->flooding_total, s->speed_kts,
                  total_ammo, s->kills, extra);
        wattroff(win, COLOR_PAIR(sc));
        row++;
    }
}

static void draw_stats(int tick) {
    werase(win_stats);
    int sh;
    int sw;
    getmaxyx(win_stats, sh, sw);
    (void)sw;
    wattron(win_stats, COLOR_PAIR(CP_BORDER));
    box(win_stats, 0, 0);
    mvwprintw(win_stats, 0, 2, " BATTLE STATS ");
    wattroff(win_stats, COLOR_PAIR(CP_BORDER));

    int row = 1;
    wattron(win_stats, COLOR_PAIR(CP_NATO) | A_BOLD);
    mvwprintw(win_stats, row++, 1, "NATO");
    wattroff(win_stats, COLOR_PAIR(CP_NATO) | A_BOLD);

    wattron(win_stats, COLOR_PAIR(CP_WHITE));
    mvwprintw(win_stats, row++, 1, "Launches:%d Hit:%d", stat_launches[0], stat_hits[0]);
    mvwprintw(win_stats, row++, 1, "SAM:%d CIWS:%d Chaff:%d",
              stat_sam_intercepts[0], stat_ciws_intercepts[0], stat_chaff_seductions[0]);
    mvwprintw(win_stats, row++, 1, "Air:%d A2A:%d HARM:%d Helo:%d",
              stat_air_strikes[0], stat_a2a_kills[0], stat_harm_hits[0], stat_helo_asw[0]);
    wattroff(win_stats, COLOR_PAIR(CP_WHITE));

    row++;
    wattron(win_stats, COLOR_PAIR(CP_PACT) | A_BOLD);
    mvwprintw(win_stats, row++, 1, "PACT");
    wattroff(win_stats, COLOR_PAIR(CP_PACT) | A_BOLD);

    wattron(win_stats, COLOR_PAIR(CP_WHITE));
    if (row < sh - 1) mvwprintw(win_stats, row++, 1, "Launches:%d Hit:%d", stat_launches[1], stat_hits[1]);
    if (row < sh - 1) mvwprintw(win_stats, row++, 1, "SAM:%d CIWS:%d Chaff:%d",
              stat_sam_intercepts[1], stat_ciws_intercepts[1], stat_chaff_seductions[1]);
    if (row < sh - 1) mvwprintw(win_stats, row++, 1, "Air:%d A2A:%d Jammed:%d",
              stat_air_strikes[1], stat_a2a_kills[1], stat_comms_jammed[1]);
    wattroff(win_stats, COLOR_PAIR(CP_WHITE));

    if (row < sh - 1) {
        wattron(win_stats, COLOR_PAIR(CP_DIM));
        mvwprintw(win_stats, row++, 1, "T+%02d:%02d Proj:%d", tick/60, tick%60, num_projectiles);
        wattroff(win_stats, COLOR_PAIR(CP_DIM));
    }
}

static void draw_help_overlay(void) {
    int ow = 52, oh = 20;
    int oy = (tui_rows - oh) / 2;
    int ox = (tui_cols - ow) / 2;
    if (oy < 0) oy = 0;
    if (ox < 0) ox = 0;

    WINDOW *hw = newwin(oh, ow, oy, ox);
    wbkgd(hw, COLOR_PAIR(CP_HELP_DESC));
    werase(hw);
    box(hw, 0, 0);

    wattron(hw, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
    mvwprintw(hw, 0, 2, " NAVSIM CONTROLS ");
    wattroff(hw, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);

    int r = 2;
    struct { const char *key; const char *desc; } keys[] = {
        {"SPACE",    "Pause / Resume simulation"},
        {"[ or ,",   "Step / scrub backward (while paused)"},
        {"] or .",   "Step / scrub forward (while paused)"},
        {"< (Shift+,)", "Jump back 10 frames (while paused)"},
        {"> (Shift+.)", "Jump forward 10 frames (while paused)"},
        {"+/=",      "Increase simulation speed"},
        {"-",        "Decrease simulation speed"},
        {"R",        "Resume from current frame (rewind+play)"},
        {"?",        "Toggle this help overlay"},
        {"Q",        "Quit simulation"},
        {NULL, NULL}
    };

    for (int i = 0; keys[i].key; i++) {
        wattron(hw, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
        mvwprintw(hw, r, 2, " %14s ", keys[i].key);
        wattroff(hw, COLOR_PAIR(CP_HELP_KEY) | A_BOLD);
        wattron(hw, COLOR_PAIR(CP_HELP_DESC));
        wprintw(hw, " %s", keys[i].desc);
        wattroff(hw, COLOR_PAIR(CP_HELP_DESC));
        r++;
    }

    r += 1;
    wattron(hw, COLOR_PAIR(CP_DIM));
    mvwprintw(hw, r++, 2, "Speed levels: 0.25x 0.5x 1x 2x 4x MAX");
    mvwprintw(hw, r++, 2, "Press ? or any key to close this overlay");
    wattroff(hw, COLOR_PAIR(CP_DIM));

    wrefresh(hw);
    delwin(hw);
}

static void tui_draw(int tick) {
    draw_header(tick);
    draw_map();
    draw_event_log();
    int nw, nh;
    getmaxyx(win_nato, nh, nw);
    draw_ship_panel(win_nato, SIDE_NATO, nw, nh);
    int pw, ph;
    getmaxyx(win_pact, ph, pw);
    draw_ship_panel(win_pact, SIDE_PACT, pw, ph);
    draw_stats(tick);
    wnoutrefresh(win_header);
    wnoutrefresh(win_map);
    wnoutrefresh(win_log);
    wnoutrefresh(win_nato);
    wnoutrefresh(win_pact);
    wnoutrefresh(win_stats);
    doupdate();
    if (show_help) draw_help_overlay();
}

/* ── Write CSV ───────────────────────────────────────────── */
static void write_csv(void) {
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) { return; }
    const char *evt_names[]={"hit","miss","ciws_intercept","sam_intercept",
                              "chaff_seduce","air_strike","asw_attack",
                              "torpedo_decoy","capsize","air_to_air",
                              "harm_strike","link16_share","comms_jam"};
    fprintf(f,"tick,time,attacker,defender,weapon,hits,damage,"
              "defender_hp_after,kill,event_type,sea_state\n");
    for (int i=0;i<num_records;i++){
        EngagementRecord *r=&records[i];
        const char *ename = (r->event_type < 13) ? evt_names[r->event_type] : "unknown";
        fprintf(f,"%d,%02d:%02d,%s,%s,%s,%d,%.1f,%.1f,%d,%s,%d\n",
                r->tick, r->tick/60, r->tick%60,
                r->attacker, r->defender, r->weapon,
                r->hit, r->damage, r->defender_hp_after, r->kill,
                ename, r->sea_state);
    }
    fclose(f);

    FILE *f2 = fopen("ship_status.csv","w");
    if(f2){
        fprintf(f2,"name,side,class,hp,max_hp,alive,kills,damage_dealt,"
                   "flooding_total,capsized,sorties_flown,helo_sorties,final_x,final_y\n");
        for(int i=0;i<num_ships;i++){
            Ship *s=&ships[i];
            int sorties_flown=0;
            for(int j=0;j<MAX_SORTIES;j++)
                if(s->sorties[j].launch_tick>0) sorties_flown++;
            int helo_flown=0;
            for(int j=0;j<4;j++)
                if(s->helo_sorties[j].launch_tick>0) helo_flown++;
            fprintf(f2,"%s,%s,%s,%.0f,%.0f,%d,%d,%d,%.1f,%d,%d,%d,%.1f,%.1f\n",
                    s->name, side_str(s->side), class_str(s->ship_class),
                    s->hp, s->max_hp, s->alive, s->kills,
                    s->damage_dealt_total, s->flooding_total,
                    stat_capsized[s->side]>0&&!s->alive?1:0,
                    sorties_flown, helo_flown, s->x, s->y);
        }
        fclose(f2);
    }
}

/* ── After-Action Report (printed after ncurses cleanup) ── */
static void assess_victory(void) {
    int na=0,pa=0; double nhp=0,php=0,nmax=0,pmax=0;
    for(int i=0;i<num_ships;i++){
        Ship *s=&ships[i];
        if(s->side==SIDE_NATO){ nmax+=s->max_hp; if(s->alive){na++;nhp+=s->hp;} }
        else                  { pmax+=s->max_hp; if(s->alive){pa++;php+=s->hp;} }
    }
    printf("\n\033[1m\033[36m");
    printf("==========================================================================\n");
    printf("  NAVSIM v4.0 AFTER-ACTION REPORT\n");
    printf("==========================================================================\n");
    printf("\033[0m");
    printf("  \033[32mNATO:\033[0m %d ships, %.0f/%.0f HP (%.0f%%)\n",
           na,nhp,nmax,nmax>0?nhp/nmax*100:0);
    printf("  \033[31mPACT:\033[0m %d ships, %.0f/%.0f HP (%.0f%%)\n",
           pa,php,pmax,pmax>0?php/pmax*100:0);

    double ns = (nhp/fmax(nmax,1))*100 + (pmax-php);
    double ps = (php/fmax(pmax,1))*100 + (nmax-nhp);
    printf("\n  Combat Score: NATO %.0f | PACT %.0f\n", ns, ps);

    int dn=0,dp=0,kn=0,kp=0;
    for(int i=0;i<num_ships;i++){
        if(ships[i].side==SIDE_NATO){dn+=ships[i].damage_dealt_total;kn+=ships[i].kills;}
        else                        {dp+=ships[i].damage_dealt_total;kp+=ships[i].kills;}
    }
    int ln=stat_launches[0],lp=stat_launches[1];
    int hn=stat_hits[0],hp2=stat_hits[1];

    printf("\n  \033[32mNATO Fires:\033[0m\n");
    printf("    launches=%d hits=%d (%.1f%%) damage=%d kills=%d\n",
           ln,hn,ln>0?hn*100.0/ln:0,dn,kn);
    printf("    SAM_intercepts=%d  CIWS_kills=%d  chaff=%d\n",
           stat_sam_intercepts[0],stat_ciws_intercepts[0],stat_chaff_seductions[0]);
    printf("    air_strikes=%d  A2A_kills=%d  HARM_hits=%d  helo_ASW=%d\n",
           stat_air_strikes[0],stat_a2a_kills[0],stat_harm_hits[0],stat_helo_asw[0]);
    printf("    Link-16_shares=%d  freq_hops=%d  torpedo_decoys=%d\n",
           stat_link16_shares[0],stat_freq_hops[0],stat_torpedo_decoys[0]);

    printf("\n  \033[31mPACT Fires:\033[0m\n");
    printf("    launches=%d hits=%d (%.1f%%) damage=%d kills=%d\n",
           lp,hp2,lp>0?hp2*100.0/lp:0,dp,kp);
    printf("    SAM_intercepts=%d  CIWS_kills=%d  chaff=%d\n",
           stat_sam_intercepts[1],stat_ciws_intercepts[1],stat_chaff_seductions[1]);
    printf("    air_strikes=%d  A2A_kills=%d  comms_jammed=%d  helo_ASW=%d\n",
           stat_air_strikes[1],stat_a2a_kills[1],stat_comms_jammed[1],stat_helo_asw[1]);

    printf("\n  Weather: final sea state %d (%s)\n",sea_state,beaufort_names[sea_state]);

    const char *result;
    if      (na==0&&pa==0)       result="\033[33mMUTUAL DESTRUCTION";
    else if (ns > ps*1.5)        result="\033[32mDECISIVE NATO VICTORY";
    else if (ps > ns*1.5)        result="\033[31mDECISIVE PACT VICTORY";
    else if (ns > ps)            result="\033[32mMARGINAL NATO VICTORY";
    else if (ps > ns)            result="\033[31mMARGINAL PACT VICTORY";
    else                         result="\033[33mDRAW";
    printf("\n  \033[1m%s\033[0m\n", result);
    printf("\n  Engagement data: %s\n  Ship status:     ship_status.csv\n\n", LOG_FILE);
}

/* ── Main ────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    g_seed = (uint32_t)time(NULL);
    if (argc > 1) g_seed = (uint32_t)atoi(argv[1]);
    rng_seed(g_seed);

    /* Load data before TUI init (errors go to stderr) */
    load_weapons_csv("data/weapons.csv");
    if (num_wpn_templates == 0) {
        fprintf(stderr, "[FATAL] No weapon templates loaded.\n");
        return 1;
    }
    load_platforms_csv("data/platforms.csv");
    if (num_ships == 0) {
        fprintf(stderr, "[FATAL] No ships loaded.\n");
        return 1;
    }

    /* Init weather */
    sea_state = (int)(rng_uniform() * 4);
    sea_state_timer = 600 + (int)(rng_uniform() * 600);
    update_weather_factors();

    /* Initialize ncurses TUI */
    tui_init();

    /* Initial event log entries */
    evt_log(0, CP_HEADER, "NAVSIM v4.0 Tactical Engagement Simulator");
    evt_log(0, CP_HEADER, "Atlantic PoI 24.19N 43.37W | Seed: %u", g_seed);
    evt_log(0, CP_CYAN, "Weather: SS%d (%s)", sea_state, beaufort_names[sea_state]);

    int nato_count = 0, pact_count = 0;
    for (int i = 0; i < num_ships; i++) {
        if (ships[i].side == SIDE_NATO) nato_count++;
        else pact_count++;
    }
    evt_log(0, CP_NATO, "NATO: %d platforms loaded", nato_count);
    evt_log(0, CP_PACT, "PACT: %d platforms loaded", pact_count);
    evt_log(0, CP_WHITE, "--- ENGAGEMENT BEGINS ---");

    /* Allocate snapshot buffer */
    snapshots = (Snapshot *)malloc(sizeof(Snapshot) * MAX_SNAPSHOTS);
    if (!snapshots) {
        endwin();
        fprintf(stderr, "[FATAL] Could not allocate snapshot buffer.\n");
        return 1;
    }
    snap_count = 0;

    /* Draw initial state and save tick-0 snapshot */
    tui_draw(0);
    snap_save(0);

    int battle_over = 0;
    int sim_finished = 0;
    int current_tick = 0;
    play_mode = MODE_PLAY;
    view_snap_idx = 0;

    while (!battle_over) {
        /* --- Input handling (always) --- */
        int ch = getch();

        if (ch == 'q' || ch == 'Q') {
            if (!sim_finished)
                evt_log(current_tick, CP_ALERT, "--- SIMULATION ABORTED BY USER ---");
            battle_over = 1;
            continue;
        }

        switch (ch) {
            case ' ':
                if (play_mode == MODE_PLAY) {
                    play_mode = MODE_PAUSED;
                    view_snap_idx = snap_count - 1;
                } else {
                    /* Resume: restore state from current view and continue sim */
                    if (!sim_finished) {
                        snap_restore(view_snap_idx);
                        current_tick = snapshots[view_snap_idx].tick;
                        /* Discard snapshots after this point (branching timeline) */
                        snap_count = view_snap_idx + 1;
                        play_mode = MODE_PLAY;
                    }
                }
                break;

            case 'r': case 'R':
                /* Resume from current viewed frame */
                if (play_mode == MODE_PAUSED && !sim_finished) {
                    snap_restore(view_snap_idx);
                    current_tick = snapshots[view_snap_idx].tick;
                    snap_count = view_snap_idx + 1;
                    play_mode = MODE_PLAY;
                }
                break;

            case '[': case ',':
                /* Step/scrub backward */
                play_mode = MODE_PAUSED;
                if (view_snap_idx > 0) view_snap_idx--;
                snap_restore(view_snap_idx);
                tui_draw(snapshots[view_snap_idx].tick);
                break;

            case ']': case '.':
                /* Step/scrub forward */
                play_mode = MODE_PAUSED;
                if (view_snap_idx < snap_count - 1) view_snap_idx++;
                snap_restore(view_snap_idx);
                tui_draw(snapshots[view_snap_idx].tick);
                break;

            case '<':
                /* Jump back 10 frames */
                play_mode = MODE_PAUSED;
                view_snap_idx -= 10;
                if (view_snap_idx < 0) view_snap_idx = 0;
                snap_restore(view_snap_idx);
                tui_draw(snapshots[view_snap_idx].tick);
                break;

            case '>':
                /* Jump forward 10 frames */
                play_mode = MODE_PAUSED;
                view_snap_idx += 10;
                if (view_snap_idx >= snap_count) view_snap_idx = snap_count - 1;
                snap_restore(view_snap_idx);
                tui_draw(snapshots[view_snap_idx].tick);
                break;

            case '+': case '=':
                if (sim_speed < NUM_SPEEDS - 1) sim_speed++;
                break;

            case '-': case '_':
                if (sim_speed > 0) sim_speed--;
                break;

            case '?': case 'h':
                show_help = !show_help;
                if (play_mode == MODE_PAUSED) {
                    snap_restore(view_snap_idx);
                    tui_draw(snapshots[view_snap_idx].tick);
                }
                break;

            default:
                break;
        }

        /* --- Simulation tick (only in PLAY mode) --- */
        if (play_mode == MODE_PLAY && !sim_finished) {
            current_tick++;
            if (current_tick > MAX_TICK) {
                sim_finished = 1;
                evt_log(MAX_TICK, CP_HEADER, "--- ENGAGEMENT COMPLETE ---");
                play_mode = MODE_PAUSED;
                view_snap_idx = snap_count - 1;
                tui_draw(MAX_TICK);
                continue;
            }

            phase_weather(current_tick);
            phase_detect(current_tick);
            phase_sonar(current_tick);
            phase_c4isr(current_tick);
            phase_move(current_tick);
            phase_weapons(current_tick);
            phase_air_to_air(current_tick);
            phase_helicopters(current_tick);
            phase_aviation(current_tick);
            phase_damage_consequences(current_tick);

            /* Save snapshot every tick */
            snap_save(current_tick);
            view_snap_idx = snap_count - 1;

            /* Check victory */
            int nu = 0, pu = 0;
            for (int i = 0; i < num_ships; i++) {
                if (!ships[i].alive) continue;
                if (ships[i].side == SIDE_NATO) nu++;
                else pu++;
            }
            if (nu == 0 || pu == 0) {
                sim_finished = 1;
                evt_log(current_tick, CP_HEADER, "--- ENGAGEMENT COMPLETE ---");
                play_mode = MODE_PAUSED;
                view_snap_idx = snap_count - 1;
            }

            /* Draw at configured interval */
            if (current_tick % 2 == 0) {
                tui_draw(current_tick);
                if (speed_delays[sim_speed] > 0)
                    napms(speed_delays[sim_speed]);
            }
        } else if (play_mode == MODE_PAUSED) {
            /* In pause mode, just idle briefly to avoid CPU spin */
            napms(30);
        }
    }

    /* Final draw */
    if (snap_count > 0) {
        snap_restore(snap_count - 1);
        tui_draw(snapshots[snap_count - 1].tick);
    }

    /* Wait for keypress before exiting ncurses */
    nodelay(stdscr, FALSE);
    play_mode = MODE_PAUSED;
    view_snap_idx = snap_count - 1;
    mvwprintw(win_header, 1, 1, " Press any key for after-action report...                                    ");
    wrefresh(win_header);
    getch();

    /* Free snapshot buffer */
    free(snapshots);
    snapshots = NULL;

    /* Cleanup TUI */
    tui_cleanup();

    /* Write CSVs and print report to normal terminal */
    write_csv();
    assess_victory();

    return 0;
}
