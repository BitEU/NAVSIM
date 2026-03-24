/*
 * NAVSIM - Cold War Naval Tactical Engagement Simulator v3.0
 * ===========================================================
 * Atlantic Pole of Inaccessibility scenario: 24.1851°N 43.3704°W
 * Deep open-ocean, no land within 2033 km. No land-based aviation.
 *
 * New in v3.0:
 *   - Aircraft carrier air wings with CAP/STRIKE/ASW/AEW sorties
 *   - Layered air defense: long-range SAM → point SAM → CIWS
 *   - CIWS actually intercepts incoming missiles (with barrel wear)
 *   - Chaff/flares deployed against incoming missiles
 *   - Proper submarine warfare: sonar detection only, no radar
 *   - Sonar systems: passive/active/towed array per ship class
 *   - Torpedo decoys (Nixie)
 *   - Compartment damage actually degrades ship systems
 *   - Fire spreading between compartments
 *   - Progressive flooding → capsizing
 *   - Weather system (Beaufort 0-6) affecting sensors and ASW
 *   - All ship/weapon stats loaded from data/platforms.csv and data/weapons.csv
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

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Windows Console Setup ───────────────────────────────── */
static void setup_console(void) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
#endif
}

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
    WPN_RAILGUN, WPN_CIWS, WPN_CRUISE_MISSILE, WPN_ASROC
} WeaponType;
typedef enum { RADAR_S_BAND, RADAR_X_BAND, RADAR_L_BAND, RADAR_C_BAND } RadarBand;
typedef enum {
    COMP_BOW, COMP_BRIDGE, COMP_FORWARD_WEAPONS, COMP_MIDSHIP_PORT,
    COMP_MIDSHIP_STBD, COMP_ENGINE, COMP_AFT_WEAPONS, COMP_STERN,
    COMP_RADAR, COMP_COMMS, COMP_MAGAZINE, COMP_KEEL
} Compartment;
typedef enum { SORTIE_CAP, SORTIE_STRIKE, SORTIE_ASW, SORTIE_AEW } SortieType;
typedef enum {
    EVT_HIT, EVT_MISS, EVT_CIWS_INTERCEPT, EVT_SAM_INTERCEPT,
    EVT_CHAFF_SEDUCE, EVT_AIR_STRIKE, EVT_ASW_ATTACK,
    EVT_TORPEDO_DECOY, EVT_CAPSIZE
} EventType;

/* ── Data Structures ─────────────────────────────────────── */
typedef struct {
    int        active;
    SortieType type;
    int        launch_tick;
    int        return_tick;
    int        target_ship_idx; /* -1 = no specific target */
    int        effect_applied;  /* 1 = mid-mission effect has fired */
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
    double     azimuth_res;
    int        jammed;
    double     jam_strength;
} RadarSystem;

typedef struct {
    Compartment type;
    double      integrity;   /* 0.0-1.0 */
    int         flooding;
    int         on_fire;
    double      flood_rate;  /* m³/min */
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
    /* Classification flags */
    int        is_ciws;
    int        is_sam;
    int        is_anti_sub;
    int        is_anti_air;
    int        is_sea_skimmer_capable; /* can engage sea-skimmers */
    /* CIWS hardware state */
    int        rounds_fired_total;
    int        overheated;
    int        overheat_cooldown;
    int        magazine_limit;       /* physical max before wear degrades */
    /* Mount position: 0=bow 1=amidships 2=stern */
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
    double     base_max_speed_kts; /* original, for damage reference */
    double     hp, max_hp;
    double     armor;
    double     rcs;
    int        detected;
    int        is_submerged;       /* submarines always 1 */

    RadarSystem  search_radar;
    RadarSystem  fire_control_radar;
    SonarSystem  sonar;

    double     ecm_power_kw;
    double     esm_sensitivity;
    int        chaff_charges;
    int        flare_charges;
    int        nixie_charges;

    DamageCompartment compartments[MAX_COMPARTMENTS];
    int        crew_casualties;
    double     flooding_total;     /* cumulative tons ingress */
    double     list_angle;
    int        speed_reduced;

    /* Derived damage-effect state */
    double     radar_effectiveness;     /* 0-1, applied to radar ranges */
    double     targeting_effectiveness; /* 0-1, applied to p_hit */
    int        fwd_weapons_disabled;
    int        aft_weapons_disabled;

    Weapon     weapons[MAX_WEAPONS];
    int        num_weapons;

    /* Aviation (carriers only) */
    int        max_sorties;
    int        sorties_available;
    int        sorties_in_flight;
    int        stobar;             /* 1 = ski-jump; no catapult */
    AircraftSortie sorties[MAX_SORTIES];

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
    int        is_missile;     /* SSM / cruise / air-launched */
    int        is_torpedo;
    int        is_sea_skimmer; /* flies very low; harder for CIWS */
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

/* Weapon template loaded from CSV */
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

/* Weather */
static int    sea_state = 1;          /* Beaufort 0-6 */
static int    sea_state_timer = 600;
static double wx_radar   = 1.0;       /* radar effectiveness */
static double wx_small   = 1.0;       /* small ship sensor penalty */
static double wx_asw_helo = 1.0;      /* ASW helo effectiveness */
static double wx_ir      = 1.0;       /* IR/optical */

static const char *beaufort_names[] = {
    "Calm","Light Air","Light Breeze","Gentle Breeze",
    "Moderate Breeze","Fresh Breeze","Strong Breeze"
};

/* Physics constants */
#define AIR_DENSITY_SL  1.225
#define GRAVITY         9.81
#define SEA_WATER_DENSITY 1025.0  /* kg/m³ */

/* ── ANSI ────────────────────────────────────────────────── */
#define ANSI_RESET   "\033[0m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_RED     "\033[31m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"

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
    if (!f) { fprintf(stderr, "[WARN] Cannot open %s; weapons must be in platforms.csv inline\n", path); return; }

    char line[512];
    int  header_done = 0;
    /* Column indices — matched by header name */
    int cName=-1,cType=-1,cRange=-1,cPHit=-1,cDmg=-1,cSalvo=-1,cReload=-1,cAmmo=-1;
    int cMV=-1,cMass=-1,cElev=-1,cGuid=-1;
    int cAA=-1,cCIWS=-1,cSAM=-1,cAS=-1,cSkim=-1,cMag=-1,cMount=-1;

    while (fgets(line, sizeof(line), f)) {
        char *p = csv_trim(line);
        if (!*p || *p == '#') continue;

        /* Build token array */
        char *toks[32]; int ntoks = 0;
        char tmp[512]; strncpy(tmp, p, 511); tmp[511]='\0';
        char *tok = strtok(tmp, ",");
        while (tok && ntoks < 32) { toks[ntoks++] = csv_trim(tok); tok = strtok(NULL, ","); }

        if (!header_done) {
            /* Map header names to column indices */
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
        w->magazine_limit   = (cMag>=0&&cMag<ntoks)    ? atoi(toks[cMag])              : w->ammo;
        w->mount_position   = (cMount>=0&&cMount<ntoks)? atoi(toks[cMount])            : 1;
    }
    fclose(f);
    printf("  [DATA] Loaded %d weapon templates from %s\n", num_wpn_templates, path);
}

/* Find weapon template by name */
static WeaponTemplate *find_weapon(const char *name) {
    for (int i = 0; i < num_wpn_templates; i++)
        if (!strcmp(wpn_templates[i].name, name)) return &wpn_templates[i];
    return NULL;
}

/* ── Ship Builder Helpers ────────────────────────────────── */
static void init_radar(RadarSystem *r, RadarBand band, double power,
                       double gain, double freq, double range) {
    r->band = band; r->power_kw = power; r->gain_db = gain;
    r->frequency_ghz = freq; r->range_nm = range;
    r->azimuth_res = 1.0; r->jammed = 0; r->jam_strength = 0;
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
    w->is_sea_skimmer_capable = t->is_ciws || t->is_sam; /* CIWS/SAM can engage skimmers */
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
    /* init compartments */
    for (int i = 0; i < MAX_COMPARTMENTS; i++) {
        s->compartments[i].type = i;
        s->compartments[i].integrity = 1.0;
    }
    return s;
}

/* ── Platforms CSV Loader ────────────────────────────────── */
static void load_platforms_csv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr,"[WARN] Cannot open %s; falling back to hardcoded scenario\n", path); return; }

    char line[1024];
    int  hdr = 0;
    int cTeam=-1,cCls=-1,cName=-1,cHull=-1,cHP=-1,cArmor=-1,cSpd=-1,cRCS=-1;
    int cSX=-1,cSY=-1,cSHdg=-1;
    int cRBand=-1,cRPow=-1,cRGain=-1,cRFreq=-1,cRRng=-1;
    int cFBand=-1,cFPow=-1,cFGain=-1,cFFreq=-1,cFRng=-1;
    int cECM=-1,cESM=-1,cChaff=-1,cFlare=-1,cNixie=-1;
    int cSPas=-1,cSAct=-1,cTArr=-1,cTRng=-1;
    int cMaxS=-1,cSTOBAR=-1;
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

        /* Position — add small random jitter */
        double bx = (cSX>=0&&cSX<ntoks) ? atof(toks[cSX]) : 150;
        double by = (cSY>=0&&cSY<ntoks) ? atof(toks[cSY]) : 150;
        s->x = bx + rng_gauss(0, 8);
        s->y = by + rng_gauss(0, 8);
        /* Clamp to grid */
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

        /* Weapons */
        for (int wi = 0; wi < 8; wi++) {
            if (cW[wi]>=0&&cW[wi]<ntoks&&*toks[cW[wi]])
                add_weapon_from_template(s, toks[cW[wi]]);
        }
    }
    fclose(f);
    printf("  [DATA] Loaded %d platforms from %s\n", num_ships, path);
}

/* ── Weather ─────────────────────────────────────────────── */
static void update_weather_factors(void) {
    wx_radar    = 1.0 - sea_state * 0.035;   /* up to -21% at SS6 */
    wx_small    = 1.0 - (sea_state > 3 ? (sea_state-3)*0.10 : 0.0);
    wx_asw_helo = 1.0 - sea_state * 0.12;    /* up to -72% at SS6 */
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
            printf("  " ANSI_CYAN "[WEATHER]" ANSI_RESET " Sea state %d → %d (%s)\n",
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
    const char *n[]={"CAP","STRIKE","ASW","AEW"}; return n[t];
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
    p->is_missile = (wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE);
    p->is_torpedo = (wpn->type == WPN_TORPEDO || wpn->type == WPN_ASROC);
    /* Sea-skimming missiles: P-700/P-500/P-270/P-120 */
    p->is_sea_skimmer = (wpn->type == WPN_SSM &&
                         (strstr(wpn->name,"Moskit") || strstr(wpn->name,"Bazalt") ||
                          strstr(wpn->name,"Granit") || strstr(wpn->name,"Malakhit") ||
                          strstr(wpn->name,"Ametist")));

    double v0 = wpn->muzzle_velocity_mps;

    if (wpn->type == WPN_SSM || wpn->type == WPN_CRUISE_MISSILE) {
        double dx = target->x - attacker->x;
        double dy = target->y - attacker->y;
        double d  = sqrt(dx*dx + dy*dy);
        if (d < 0.001) d = 0.001;
        v0 *= 340.0; /* mach to m/s */
        p->vx = (dx/d) * v0 / 1852.0;
        p->vy = (dy/d) * v0 / 1852.0;
        p->vz = 0;
        p->z  = p->is_sea_skimmer ? 15.0 : 50.0; /* sea-skimmers fly low */
    } else if (wpn->type == WPN_TORPEDO || wpn->type == WPN_ASROC) {
        double dx = target->x - attacker->x;
        double dy = target->y - attacker->y;
        double d  = sqrt(dx*dx + dy*dy);
        if (d < 0.001) d = 0.001;
        /* torpedo speed in NM/s (45 kts ≈ 0.0125 NM/s) */
        double tspd = v0 / 3600.0; /* knots to NM/s */
        p->vx = (dx/d) * tspd;
        p->vy = (dy/d) * tspd;
        p->vz = 0;
        p->z  = -200.0; /* start underwater */
    } else {
        /* Gun: ballistic arc */
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

    /* Torpedoes: hydrodynamic drag, no gravity, stay underwater */
    if (p->is_torpedo) {
        double vxy = sqrt(p->vx*p->vx + p->vy*p->vy);
        double drag = 0.3 * SEA_WATER_DENSITY * 0.04 * vxy * vxy * (1852.0*1852.0) / (p->mass_kg * 1852.0);
        if (vxy > 1e-6) {
            p->vx -= (p->vx/vxy)*drag*dt;
            p->vy -= (p->vy/vxy)*drag*dt;
        }
        p->x += p->vx*dt; p->y += p->vy*dt;
        return; /* no z physics for torpedoes */
    }

    /* Missiles and shells */
    if (p->type != WPN_SSM && p->type != WPN_CRUISE_MISSILE)
        p->vz -= GRAVITY * 3.28084 * dt;

    double vtot = sqrt((p->vx*1852.0)*(p->vx*1852.0) +
                       (p->vy*1852.0)*(p->vy*1852.0) +
                       (p->vz*0.3048)*(p->vz*0.3048));
    double ref_area, drag_mult;
    if (p->type == WPN_CRUISE_MISSILE) { ref_area=0.2; drag_mult=0.02; }
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

    if (p->z <= 0 && !p->is_missile) p->active = 0; /* shell hit water */
    if (p->x<-50||p->x>GRID_SIZE+50||p->y<-50||p->y>GRID_SIZE+50) p->active=0;
}

/* ── Detection Phase (radar — surface targets only) ─────── */
static void phase_detect(int tick) {
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        for (int j = 0; j < num_ships; j++) {
            if (i==j || !ships[j].alive) continue;
            if (ships[i].side == ships[j].side) continue;
            /* Submerged submarines are invisible to radar */
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
                    stat_detect_jammed[ships[i].side]++;
                }
            }

            /* Small ship sensor penalty in heavy weather */
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
                    if (tick % 60 == 0) {
                        printf("  " ANSI_CYAN "[RADAR]" ANSI_RESET
                               " %s %s → %s %s at %.1f NM brg %03.0f%s\n",
                               side_str(ships[i].side), ships[i].name,
                               side_str(ships[j].side), ships[j].name,
                               d, bearing_deg(&ships[i], &ships[j]),
                               radar->jam_strength>0.3?" [JAMMED]":"");
                    }
                }
            }
        }
    }
}

/* ── Sonar Detection Phase (submarines only) ────────────── */
static void phase_sonar(int tick) {
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        if (ships[i].sonar.passive_range_nm <= 0 &&
            ships[i].sonar.active_range_nm  <= 0) continue; /* no sonar */

        for (int j = 0; j < num_ships; j++) {
            if (i==j || !ships[j].alive) continue;
            if (ships[i].side == ships[j].side) continue;
            if (!ships[j].is_submerged) continue; /* sonar phase only hunts subs */

            double d = dist_nm(&ships[i], &ships[j]);

            /* Passive sonar */
            double p_rng = ships[i].sonar.passive_range_nm;
            if (ships[i].sonar.has_towed_array &&
                ships[i].sonar.towed_range_nm > p_rng)
                p_rng = ships[i].sonar.towed_range_nm;

            /* Sea noise penalty */
            p_rng *= (1.0 - sea_state * 0.05);
            if (p_rng < 5) p_rng = 5;

            if (d < p_rng) {
                double p_det = 0.3 + 0.5*(1.0 - d/p_rng);
                /* Akula is quieter than Victor */
                if (ships[j].ship_class == CLASS_SUBMARINE) {
                    if (strstr(ships[j].name,"Akula"))       p_det *= 0.55; /* quiet */
                    else if (strstr(ships[j].name,"Virginia"))p_det *= 0.45; /* very quiet */
                    else if (strstr(ships[j].name,"Los Ang")) p_det *= 0.50;
                    /* Victor III: noisy by Cold War standards */
                }
                p_det *= rng_gauss(1.0, 0.15);
                if (p_det < 0) p_det = 0;
                if (p_det > 0.95) p_det = 0.95;

                if (rng_uniform() < p_det) {
                    ships[j].detected = 1;
                    if (tick % 120 == 0)
                        printf("  " ANSI_BLUE "[SONAR]" ANSI_RESET
                               " %s %s passive contact: %s %s %.1f NM\n",
                               side_str(ships[i].side), ships[i].name,
                               side_str(ships[j].side), ships[j].name, d);
                }
            }

            /* Active sonar: louder, reveals the pinger */
            if (ships[i].sonar.active_on) {
                double a_rng = ships[i].sonar.active_range_nm;
                if (d < a_rng) {
                    ships[j].detected = 1;
                    /* Pinger is now detectable by ESM */
                    ships[i].detected = 1;
                }
            }
        }
    }
}

/* ── CIWS / SAM / Chaff Intercept Logic ─────────────────── */
/* Returns 1 if projectile was intercepted */
static int check_intercepts(Projectile *proj, int tick) {
    if (!proj->active || !proj->is_missile) return 0;

    /* Find which side the projectile is targeting */
    Side threat_side = SIDE_NATO; /* side launching the missile */
    if (proj->attacker_idx >= 0 && proj->attacker_idx < num_ships)
        threat_side = ships[proj->attacker_idx].side;
    Side def_side = (threat_side == SIDE_NATO) ? SIDE_PACT : SIDE_NATO;

    /* Layer 1: Long-range area SAM (SM-2, S-300F) */
    for (int i = 0; i < num_ships; i++) {
        Ship *def = &ships[i];
        if (!def->alive || def->side != def_side) continue;
        double d = dist_nm_xy(proj->x, proj->y, def->x, def->y);

        for (int w = 0; w < def->num_weapons; w++) {
            Weapon *wpn = &def->weapons[w];
            if (!wpn->is_sam || wpn->is_ciws) continue; /* pure SAMs only */
            if (wpn->ammo <= 0 || wpn->cooldown > 0) continue;
            if (d > wpn->range_nm) continue;

            double p_int = wpn->p_hit * wx_radar * def->radar_effectiveness;
            /* Sea-skimmers are harder for SAMs */
            if (proj->is_sea_skimmer) p_int *= 0.60;

            if (rng_uniform() < p_int) {
                wpn->ammo -= (wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo);
                wpn->cooldown = wpn->reload_ticks;
                proj->active = 0; proj->intercepted = 1;
                stat_sam_intercepts[def_side]++;
                printf("  " ANSI_GREEN "[SAM-KILL]" ANSI_RESET
                       " %s %s | %s at %.1f NM\n",
                       def->name, wpn->name, proj->weapon_name, d);
                log_event(tick, proj->attacker, def->name, wpn->name,
                          1, 0, def->hp, 0, EVT_SAM_INTERCEPT);
                return 1;
            } else {
                /* Fired but missed */
                wpn->ammo -= (wpn->salvo_size < wpn->ammo ? wpn->salvo_size : wpn->ammo);
                wpn->cooldown = wpn->reload_ticks;
                break; /* one attempt per ship per missile per tick */
            }
        }
        if (proj->intercepted) return 1;
    }

    /* Layer 2: Chaff and ECM seduction */
    for (int i = 0; i < num_ships; i++) {
        Ship *def = &ships[i];
        if (!def->alive || def->side != def_side) continue;
        double d = dist_nm_xy(proj->x, proj->y, def->x, def->y);
        if (d > 0.8) continue; /* chaff window: missile within 0.8 NM */

        /* Only deploy if this ship is the apparent target */
        if (strcmp(proj->target, def->name) != 0) continue;

        if (def->chaff_charges > 0) {
            double p_chaff = 0.45;
            if (proj->is_sea_skimmer) p_chaff = 0.20; /* harder to seduce */
            if (def->ecm_power_kw > 60) p_chaff += 0.10;

            def->chaff_charges--;
            if (rng_uniform() < p_chaff) {
                proj->active = 0; proj->intercepted = 1;
                stat_chaff_seductions[def_side]++;
                printf("  " ANSI_YELLOW "[CHAFF]" ANSI_RESET
                       " %s seduced %s\n", def->name, proj->weapon_name);
                log_event(tick, proj->attacker, def->name, "Chaff",
                          0, 0, def->hp, 0, EVT_CHAFF_SEDUCE);
                return 1;
            }
        }
    }

    /* Layer 3: CIWS last-ditch */
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

            /* Burst: consume rounds */
            int burst = wpn->is_sam ? 1 : 20; /* SeaRAM fires 1 missile; Phalanx bursts */
            if (burst > wpn->ammo) burst = wpn->ammo;
            wpn->ammo -= burst;
            wpn->rounds_fired_total += burst;

            /* Barrel wear / overheat check */
            if (!wpn->is_sam && wpn->rounds_fired_total > wpn->magazine_limit * 0.75) {
                wpn->overheated = 1;
                wpn->overheat_cooldown = 90;
                printf("  " ANSI_YELLOW "[CIWS-HOT]" ANSI_RESET " %s %s overheating!\n",
                       def->name, wpn->name);
            }

            double p_int = wpn->p_hit * wx_radar;
            if (proj->is_sea_skimmer) p_int *= 0.72; /* skimmers are hard */
            if (def->radar_effectiveness < 0.7) p_int *= def->radar_effectiveness;

            if (rng_uniform() < p_int) {
                proj->active = 0; proj->intercepted = 1;
                stat_ciws_intercepts[def_side]++;
                printf("  " ANSI_GREEN "[CIWS-KILL]" ANSI_RESET
                       " %s %s → %s at %.2f NM\n",
                       def->name, wpn->name, proj->weapon_name, d);
                log_event(tick, proj->attacker, def->name, wpn->name,
                          1, 0, def->hp, 0, EVT_CIWS_INTERCEPT);
                return 1;
            }
            /* Only one CIWS attempt per defensive ship per missile */
            break;
        }
        if (proj->intercepted) return 1;
    }
    return 0;
}

/* Torpedo decoy check */
static int check_torpedo_decoy(Projectile *proj, Ship *target, int tick) {
    if (!proj->is_torpedo || target->nixie_charges <= 0) return 0;
    double d = dist_nm_xy(proj->x, proj->y, target->x, target->y);
    if (d > 0.5) return 0; /* only works close in */
    target->nixie_charges--;
    double p_decoy = 0.40;
    if (rng_uniform() < p_decoy) {
        proj->active = 0; proj->intercepted = 1;
        stat_torpedo_decoys[target->side]++;
        printf("  " ANSI_YELLOW "[NIXIE]" ANSI_RESET " %s decoyed %s torpedo\n",
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

        /* Submarines: prefer to stay deep and at stand-off range */
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
    /* CIWS overheat cooldown */
    for (int i = 0; i < num_ships; i++) {
        if (!ships[i].alive) continue;
        for (int w = 0; w < ships[i].num_weapons; w++) {
            Weapon *wpn = &ships[i].weapons[w];
            if (wpn->overheated) {
                if (--wpn->overheat_cooldown <= 0) {
                    wpn->overheated = 0;
                    printf("  " ANSI_CYAN "[CIWS-COOL]" ANSI_RESET " %s %s cooled\n",
                           ships[i].name, wpn->name);
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
            /* CIWS/SAMs don't fire offensively in this phase */
            if (wpn->is_ciws || wpn->is_sam) continue;

            /* Find best target */
            int best_tgt = -1;
            double best_score = -1;
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side==atk->side) continue;
                if (!ships[j].detected) continue;

                /* Anti-sub weapons only target subs */
                if (wpn->is_anti_sub && ships[j].ship_class != CLASS_SUBMARINE) continue;
                /* Non-anti-sub weapons shouldn't waste shots on submerged subs */
                if (!wpn->is_anti_sub && ships[j].is_submerged) continue;

                /* Check mount disabled */
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
            printf("  " ANSI_YELLOW "[LAUNCH]" ANSI_RESET
                   " %s %s → %s | %s x%d @ %.1f NM\n",
                   side_str(atk->side), atk->name, def->name,
                   wpn->name, rounds, d);
        }
    }

    /* Update projectiles, run intercepts, check impacts */
    for (int p = 0; p < num_projectiles; p++) {
        if (!projectiles[p].active) continue;

        /* Run CIWS/SAM intercepts before physics (in-envelope check) */
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
            if (ships[i].side == as) continue; /* no friendly fire */

            double dx = projectiles[p].x - ships[i].x;
            double dy = projectiles[p].y - ships[i].y;
            double dist = sqrt(dx*dx + dy*dy);

            /* Hit radius */
            double hit_r = 0.05;
            if (ships[i].ship_class == CLASS_CARRIER)  hit_r = 0.15;
            else if (ships[i].ship_class == CLASS_CRUISER) hit_r = 0.10;

            /* Torpedo: check underwater proximity; also check decoy */
            if (projectiles[p].is_torpedo) {
                if (dist > hit_r) continue;
                if (check_torpedo_decoy(&projectiles[p], &ships[i], tick)) break;
            } else {
                if (dist > hit_r) continue;
            }

            /* HIT */
            double dmg = projectiles[p].damage * (1.0 - ships[i].armor);

            /* Guidance variance */
            WeaponTemplate *wt = find_weapon(projectiles[p].weapon_name);
            double gq = wt ? wt->guidance_quality : 0.5;
            dmg *= rng_gauss(1.0, gq > 0.7 ? 0.10 : 0.25);
            if (dmg < 1) dmg = 1;

            /* Apply to compartments */
            int comp = (int)(rng_uniform() * MAX_COMPARTMENTS);
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

            printf("  " ANSI_RED "[IMPACT]" ANSI_RESET
                   " %s → %s %.0f dmg%s [HP: %.0f/%.0f]\n",
                   projectiles[p].weapon_name, ships[i].name, dmg,
                   killed ? " " ANSI_BOLD ANSI_RED "*** SUNK ***" ANSI_RESET : "",
                   ships[i].hp, ships[i].max_hp);

            log_event(tick, projectiles[p].attacker, ships[i].name,
                      projectiles[p].weapon_name, 1, dmg, ships[i].hp,
                      killed, EVT_HIT);

            projectiles[p].active = 0;
            break;
        }
    }

    /* Compact inactive projectiles */
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

/* ── Aviation Phase ──────────────────────────────────────── */
static void phase_aviation(int tick) {
    for (int i = 0; i < num_ships; i++) {
        Ship *carrier = &ships[i];
        if (!carrier->alive || carrier->max_sorties == 0) continue;

        /* Process returning sorties */
        for (int s = 0; s < MAX_SORTIES; s++) {
            AircraftSortie *sr = &carrier->sorties[s];
            if (!sr->active) continue;

            /* Mid-mission effect */
            if (!sr->effect_applied && tick >= (sr->launch_tick + sr->return_tick)/2) {
                sr->effect_applied = 1;
                if (sr->type == SORTIE_STRIKE && sr->target_ship_idx >= 0) {
                    Ship *tgt = &ships[sr->target_ship_idx];
                    if (tgt->alive) {
                        double base_dmg = carrier->stobar ? 120.0 : 170.0;
                        double dmg = rng_gauss(base_dmg, 30.0) * (1.0 - tgt->armor);
                        /* Sea state affects delivery */
                        dmg *= (0.75 + wx_ir * 0.25);
                        if (dmg < 20) dmg = 20;
                        /* Apply to compartments */
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
                        printf("  " ANSI_MAGENTA "[AIR STRIKE]" ANSI_RESET
                               " %s aircraft → %s %.0f dmg%s [HP: %.0f/%.0f]\n",
                               carrier->name, tgt->name, dmg,
                               killed?" " ANSI_BOLD ANSI_RED "*** SUNK ***" ANSI_RESET:"",
                               tgt->hp, tgt->max_hp);
                        log_event(tick, carrier->name, tgt->name, "Air Strike",
                                  1, dmg, tgt->hp, killed, EVT_AIR_STRIKE);
                    }
                } else if (sr->type == SORTIE_ASW) {
                    /* Hunt nearest enemy sub */
                    for (int j = 0; j < num_ships; j++) {
                        if (!ships[j].alive || ships[j].side==carrier->side) continue;
                        if (ships[j].ship_class != CLASS_SUBMARINE) continue;
                        if (!ships[j].detected) continue;
                        double d = dist_nm(carrier, &ships[j]);
                        if (d > 80) continue; /* helo range */

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
                            printf("  " ANSI_MAGENTA "[ASW HIT]" ANSI_RESET
                                   " %s helo → %s %.0f dmg%s\n",
                                   carrier->name, ships[j].name, dmg,
                                   killed?" ***SUNK***":"");
                            log_event(tick, carrier->name, ships[j].name, "ASW Torpedo",
                                      1, dmg, ships[j].hp, killed, EVT_ASW_ATTACK);
                        }
                        break;
                    }
                } else if (sr->type == SORTIE_AEW) {
                    /* E-2C extends carrier radar range while airborne */
                    carrier->search_radar.range_nm =
                        fmin(carrier->search_radar.range_nm * 1.6, 300.0);
                }
            }

            /* Return */
            if (tick >= sr->return_tick) {
                /* Reset AEW radar extension */
                if (sr->type == SORTIE_AEW)
                    carrier->search_radar.range_nm /= 1.6;
                sr->active = 0;
                carrier->sorties_in_flight--;
                carrier->sorties_available++;
            }
        }

        /* Launch new sorties every 60 ticks */
        if (tick % 60 != 0) continue;
        if (carrier->sorties_available <= 0) continue;
        if (carrier->sorties_in_flight >= carrier->max_sorties / 3) continue;

        /* Decide sortie type */
        SortieType stype = SORTIE_CAP;
        int tgt_idx = -1;

        /* Check if AEW is up */
        int aew_up = 0;
        for (int s2 = 0; s2 < MAX_SORTIES; s2++)
            if (carrier->sorties[s2].active && carrier->sorties[s2].type == SORTIE_AEW)
                aew_up = 1;

        /* Priority: AEW first, then ASW if sub contact, then strike */
        if (!aew_up && !carrier->stobar) {
            stype = SORTIE_AEW;
        } else {
            /* Check for detected enemy subs nearby */
            for (int j = 0; j < num_ships; j++) {
                if (!ships[j].alive || ships[j].side==carrier->side) continue;
                if (ships[j].ship_class!=CLASS_SUBMARINE || !ships[j].detected) continue;
                if (dist_nm(carrier, &ships[j]) < 80) { stype=SORTIE_ASW; break; }
            }
            if (stype == SORTIE_CAP) {
                /* Find best strike target */
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

        /* Find free sortie slot */
        for (int s2 = 0; s2 < MAX_SORTIES; s2++) {
            if (carrier->sorties[s2].active) continue;
            AircraftSortie *sr = &carrier->sorties[s2];
            sr->active = 1;
            sr->type = stype;
            sr->launch_tick = tick;
            /* Mission duration: STOBAR slower recovery; AEW stays longer */
            int dur = 0;
            if (stype==SORTIE_AEW)    dur = carrier->stobar ? 0 : 1200; /* E-2C 20 min */
            else if (stype==SORTIE_STRIKE) dur = carrier->stobar ? 900 : 720;
            else if (stype==SORTIE_ASW)    dur = 480;
            else                           dur = 600; /* CAP */
            sr->return_tick = tick + dur;
            sr->target_ship_idx = tgt_idx;
            sr->effect_applied = 0;
            carrier->sorties_available--;
            carrier->sorties_in_flight++;
            printf("  " ANSI_MAGENTA "[SORTIE]" ANSI_RESET
                   " %s launches %s sortie (%d/%d in flight)%s\n",
                   carrier->name, sortie_str(stype),
                   carrier->sorties_in_flight, carrier->max_sorties,
                   carrier->stobar?" [STOBAR]":"");
            break;
        }
    }
}

/* ── Damage Consequences Phase ───────────────────────────── */
static void phase_damage_consequences(int tick) {
    (void)tick;
    for (int i = 0; i < num_ships; i++) {
        Ship *s = &ships[i];
        if (!s->alive) continue;

        /* Process each compartment */
        for (int c = 0; c < MAX_COMPARTMENTS; c++) {
            DamageCompartment *comp = &s->compartments[c];

            /* Fire spreading */
            if (comp->on_fire) {
                comp->integrity -= 0.0005; /* slow fire damage */
                if (rng_uniform() < 0.003) { /* ~0.3% per tick = ~11 min to spread */
                    int adj = (c + 1) % MAX_COMPARTMENTS;
                    s->compartments[adj].on_fire = 1;
                }
                /* Magazine fire: high risk of catastrophic explosion */
                if (c == COMP_MAGAZINE && comp->on_fire && comp->integrity < 0.3) {
                    if (rng_uniform() < 0.0005) { /* ~0.05% per tick */
                        printf("  " ANSI_BOLD ANSI_RED "[MAGAZINE EXPLOSION]" ANSI_RESET
                               " %s magazine detonation!\n", s->name);
                        s->hp = 0; s->alive = 0;
                        stat_kills_by_side[1 - s->side]++; /* credited to enemy */
                        break;
                    }
                }
            }

            /* Flooding */
            if (comp->flooding) {
                s->flooding_total += comp->flood_rate / 60.0; /* per-second rate */
                comp->integrity -= 0.0001;
            }
        }

        if (!s->alive) continue;

        /* Derived damage effects */

        /* RADAR compartment → radar effectiveness */
        double r_int = s->compartments[COMP_RADAR].integrity;
        s->radar_effectiveness = r_int > 0.5 ? 1.0 : 0.5 + r_int;
        if (s->radar_effectiveness < 0.3) s->radar_effectiveness = 0.3;
        s->search_radar.range_nm =
            s->base_max_speed_kts > 0 ? /* reuse to avoid adding another field */
            s->search_radar.range_nm : s->search_radar.range_nm; /* placeholder */

        /* BRIDGE compartment → targeting effectiveness */
        double b_int = s->compartments[COMP_BRIDGE].integrity;
        s->targeting_effectiveness = b_int > 0.5 ? 1.0 : 0.4 + b_int;
        if (s->targeting_effectiveness < 0.3) s->targeting_effectiveness = 0.3;

        /* ENGINE compartment → speed */
        double e_int = s->compartments[COMP_ENGINE].integrity;
        if (e_int < 0.5) {
            s->speed_reduced = 1;
            s->max_speed_kts = s->base_max_speed_kts * (0.4 + e_int * 0.6);
            if (s->speed_kts > s->max_speed_kts) s->speed_kts = s->max_speed_kts;
        }

        /* FORWARD/AFT weapons mounts */
        s->fwd_weapons_disabled = (s->compartments[COMP_FORWARD_WEAPONS].integrity < 0.2);
        s->aft_weapons_disabled = (s->compartments[COMP_AFT_WEAPONS].integrity < 0.2);

        /* Flooding → progressive effects */
        double capsize_threshold = s->max_hp * 2.5;
        if (s->flooding_total > 200) {
            double flood_pct = s->flooding_total / capsize_threshold;
            double spd_pen = 1.0 - fmin(flood_pct * 0.7, 0.80);
            if (s->max_speed_kts > s->base_max_speed_kts * spd_pen)
                s->max_speed_kts = s->base_max_speed_kts * spd_pen;
            s->list_angle = flood_pct * 20.0;
        }

        /* Capsize check */
        if (s->flooding_total > capsize_threshold) {
            printf("  " ANSI_BOLD ANSI_RED "[CAPSIZE]" ANSI_RESET
                   " %s capsizes! (%.0f tons ingress)\n",
                   s->name, s->flooding_total);
            s->alive = 0;
            stat_capsized[s->side]++;
            log_event(tick, "flooding", s->name, "flooding",
                      1, s->flooding_total, 0, 1, EVT_CAPSIZE);
        }
    }
}

/* ── Status Display ──────────────────────────────────────── */
static void print_status_board(int tick) {
    printf("\n" ANSI_BOLD "════════════════════════════════════════════════════════\n");
    printf("  NAVSIM TACTICAL  T+%02d:%02d  Sea State %d (%s)\n",
           tick/60, tick%60, sea_state, beaufort_names[sea_state]);
    printf("════════════════════════════════════════════════════════\n" ANSI_RESET);

    for (int side = 0; side <= 1; side++) {
        const char *col = (side==0) ? ANSI_GREEN : ANSI_RED;
        printf(" %s%s%s FORCES\n", ANSI_BOLD, col, side==0?"NATO":"PACT");
        printf(ANSI_RESET);
        printf(" %-20s %-5s %5s %8s %6s %6s %5s %5s\n",
               "Ship","Class","HP%","Pos","Kts","Flood","Kills","Ammo");
        printf(" " ANSI_DIM "─────────────────────────────────────────────────────\n" ANSI_RESET);

        for (int i = 0; i < num_ships; i++) {
            Ship *s = &ships[i];
            if ((int)s->side != side) continue;
            int total_ammo = 0;
            for (int w = 0; w < s->num_weapons; w++) total_ammo += s->weapons[w].ammo;
            double hp_pct = s->max_hp>0 ? (s->hp/s->max_hp)*100 : 0;
            const char *sc = ANSI_RESET;
            if (!s->alive) sc=ANSI_DIM;
            else if (hp_pct < 30) sc=ANSI_RED;
            else if (hp_pct < 60) sc=ANSI_YELLOW;

            char sorties_buf[16]="";
            if (s->max_sorties>0)
                snprintf(sorties_buf,sizeof(sorties_buf)," [%d/%dS]",
                         s->sorties_in_flight, s->max_sorties);

            printf(" %s%-20s %-5s %4.0f%% %3.0f,%3.0f %5.0f %6.0f %5d %5d%s%s\n",
                   sc, s->name, class_str(s->ship_class),
                   hp_pct, s->x, s->y, s->speed_kts, s->flooding_total,
                   s->kills, total_ammo, sorties_buf, ANSI_RESET);
        }
        printf("\n");
    }
}

static void print_tactical_map(void) {
    const int W=60, H=30;
    char map[30][61];
    for (int r=0;r<H;r++){for(int c=0;c<W;c++)map[r][c]='.';map[r][W]='\0';}
    for (int i=0;i<num_ships;i++){
        Ship *s=&ships[i];
        int c=(int)(s->x*W/GRID_SIZE), r=H-1-(int)(s->y*H/GRID_SIZE);
        if(c<0)c=0;if(c>=W)c=W-1;if(r<0)r=0;if(r>=H)r=H-1;
        if(!s->alive) map[r][c]='X';
        else if(s->side==SIDE_NATO) map[r][c]="AGDFSU"[s->ship_class];
        else                        map[r][c]="agdfsu"[s->ship_class];
    }
    printf(ANSI_BOLD " TACTICAL PLOT\n" ANSI_RESET);
    printf(" ┌"); for(int c=0;c<W;c++)printf("─"); printf("┐\n");
    for(int r=0;r<H;r++){
        printf(" │");
        for(int c=0;c<W;c++){
            char ch=map[r][c];
            if(ch=='.') printf(ANSI_DIM "·" ANSI_RESET);
            else if(ch=='X') printf(ANSI_RED "✕" ANSI_RESET);
            else if(ch>='A'&&ch<='Z') printf(ANSI_GREEN "%c" ANSI_RESET,ch);
            else printf(ANSI_RED "%c" ANSI_RESET,ch);
        }
        printf("│\n");
    }
    printf(" └"); for(int c=0;c<W;c++)printf("─"); printf("┘\n");
}

/* ── Write CSV ───────────────────────────────────────────── */
static void write_csv(void) {
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) { perror("fopen"); return; }
    const char *evt_names[]={"hit","miss","ciws_intercept","sam_intercept",
                              "chaff_seduce","air_strike","asw_attack",
                              "torpedo_decoy","capsize"};
    fprintf(f,"tick,time,attacker,defender,weapon,hits,damage,"
              "defender_hp_after,kill,event_type,sea_state\n");
    for (int i=0;i<num_records;i++){
        EngagementRecord *r=&records[i];
        fprintf(f,"%d,%02d:%02d,%s,%s,%s,%d,%.1f,%.1f,%d,%s,%d\n",
                r->tick, r->tick/60, r->tick%60,
                r->attacker, r->defender, r->weapon,
                r->hit, r->damage, r->defender_hp_after, r->kill,
                evt_names[r->event_type], r->sea_state);
    }
    fclose(f);

    FILE *f2 = fopen("ship_status.csv","w");
    if(f2){
        fprintf(f2,"name,side,class,hp,max_hp,alive,kills,damage_dealt,"
                   "flooding_total,capsized,sorties_flown,final_x,final_y\n");
        for(int i=0;i<num_ships;i++){
            Ship *s=&ships[i];
            int sorties_flown=0;
            for(int j=0;j<MAX_SORTIES;j++)
                if(s->sorties[j].launch_tick>0) sorties_flown++;
            fprintf(f2,"%s,%s,%s,%.0f,%.0f,%d,%d,%d,%.1f,%d,%d,%.1f,%.1f\n",
                    s->name, side_str(s->side), class_str(s->ship_class),
                    s->hp, s->max_hp, s->alive, s->kills,
                    s->damage_dealt_total, s->flooding_total,
                    stat_capsized[s->side]>0&&!s->alive?1:0,
                    sorties_flown, s->x, s->y);
        }
        fclose(f2);
    }
}

/* ── After-Action Report ─────────────────────────────────── */
static void assess_victory(void) {
    int na=0,pa=0; double nhp=0,php=0,nmax=0,pmax=0;
    for(int i=0;i<num_ships;i++){
        Ship *s=&ships[i];
        if(s->side==SIDE_NATO){ nmax+=s->max_hp; if(s->alive){na++;nhp+=s->hp;} }
        else                  { pmax+=s->max_hp; if(s->alive){pa++;php+=s->hp;} }
    }
    printf("\n" ANSI_BOLD
           "══════════════════════════════════════════════════════════════════════════════\n"
           "  AFTER-ACTION REPORT\n"
           "══════════════════════════════════════════════════════════════════════════════\n"
           ANSI_RESET);
    printf("  " ANSI_GREEN "NATO:" ANSI_RESET " %d ships, %.0f/%.0f HP (%.0f%%)\n",
           na,nhp,nmax,nmax>0?nhp/nmax*100:0);
    printf("  " ANSI_RED   "PACT:" ANSI_RESET " %d ships, %.0f/%.0f HP (%.0f%%)\n",
           pa,php,pmax,pmax>0?php/pmax*100:0);

    double ns = (nhp/nmax)*100 + (pmax-php);
    double ps = (php/pmax)*100 + (nmax-nhp);
    printf("\n  Combat Score: NATO %.0f | PACT %.0f\n", ns, ps);

    /* Damage breakdown */
    int dn=0,dp=0,kn=0,kp=0;
    for(int i=0;i<num_ships;i++){
        if(ships[i].side==SIDE_NATO){dn+=ships[i].damage_dealt_total;kn+=ships[i].kills;}
        else                        {dp+=ships[i].damage_dealt_total;kp+=ships[i].kills;}
    }
    int ln=stat_launches[0],lp=stat_launches[1];
    int hn=stat_hits[0],hp2=stat_hits[1];
    int mn=stat_misses[0],mp=stat_misses[1];
    int ifn=ln-hn-mn, ifp=lp-hp2-mp;

    printf("\n  Fires:\n");
    printf("    NATO: launches=%d hits=%d (%.1f%%) misses=%d in-flight=%d\n",
           ln,hn,ln>0?hn*100.0/ln:0,mn,ifn<0?0:ifn);
    printf("          damage=%d  kills=%d  air_strikes=%d\n",
           dn,kn,stat_air_strikes[0]);
    printf("          SAM_intercepts=%d  CIWS_kills=%d  chaff=%d\n",
           stat_sam_intercepts[0],stat_ciws_intercepts[0],stat_chaff_seductions[0]);
    printf("          torpedo_decoys=%d  ASW_attacks=%d  capsized=%d\n",
           stat_torpedo_decoys[0],stat_asw_attacks[0],stat_capsized[0]);

    printf("\n    PACT: launches=%d hits=%d (%.1f%%) misses=%d in-flight=%d\n",
           lp,hp2,lp>0?hp2*100.0/lp:0,mp,ifp<0?0:ifp);
    printf("          damage=%d  kills=%d  air_strikes=%d\n",
           dp,kp,stat_air_strikes[1]);
    printf("          SAM_intercepts=%d  CIWS_kills=%d  chaff=%d\n",
           stat_sam_intercepts[1],stat_ciws_intercepts[1],stat_chaff_seductions[1]);
    printf("          torpedo_decoys=%d  ASW_attacks=%d  capsized=%d\n",
           stat_torpedo_decoys[1],stat_asw_attacks[1],stat_capsized[1]);

    printf("\n  Weather: final sea state %d (%s)\n",sea_state,beaufort_names[sea_state]);

    const char *result;
    if      (na==0&&pa==0)       result=ANSI_YELLOW "MUTUAL DESTRUCTION";
    else if (ns > ps*1.5)        result=ANSI_GREEN   "DECISIVE NATO VICTORY";
    else if (ps > ns*1.5)        result=ANSI_RED     "DECISIVE PACT VICTORY";
    else if (ns > ps)            result=ANSI_GREEN   "MARGINAL NATO VICTORY";
    else if (ps > ns)            result=ANSI_RED     "MARGINAL PACT VICTORY";
    else                         result=ANSI_YELLOW  "DRAW";
    printf("\n  " ANSI_BOLD "%s\n" ANSI_RESET, result);
    printf("\n  Engagement data: %s\n  Ship status:     ship_status.csv\n", LOG_FILE);
}

/* ── Main ────────────────────────────────────────────────── */
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
    ANSI_RESET ANSI_DIM
    "    v3.0 | Atlantic Pole of Inaccessibility 24.1851°N 43.3704°W\n"
    "    Deep-ocean engagement. No land within 2034 km. RNG Seed: %u\n"
    ANSI_RESET "\n", seed);

    /* Load data */
    load_weapons_csv("data/weapons.csv");
    if (num_wpn_templates == 0) {
        fprintf(stderr, "[FATAL] No weapon templates loaded. Ensure data/weapons.csv exists.\n");
        return 1;
    }
    load_platforms_csv("data/platforms.csv");
    if (num_ships == 0) {
        fprintf(stderr, "[FATAL] No ships loaded. Ensure data/platforms.csv exists.\n");
        return 1;
    }

    /* Init weather */
    sea_state = (int)(rng_uniform() * 4);
    sea_state_timer = 600 + (int)(rng_uniform() * 600);
    update_weather_factors();

    printf(ANSI_BOLD "\n  SCENARIO: North Atlantic Battle Group Engagement\n" ANSI_RESET);
    printf("  NATO: %d platforms loaded\n", num_ships);
    for(int i=0;i<num_ships;i++)
        if(ships[i].side==SIDE_NATO)
            printf("    " ANSI_GREEN "%s" ANSI_RESET " (%s) %d wpns%s\n",
                   ships[i].name, ships[i].hull_class, ships[i].num_weapons,
                   ships[i].max_sorties>0?" [CARRIER]":"");
    int pact_start=0;
    for(int i=0;i<num_ships;i++) if(ships[i].side==SIDE_PACT){pact_start=i;break;}
    printf("  PACT: %d platforms loaded\n", num_ships-pact_start);
    for(int i=0;i<num_ships;i++)
        if(ships[i].side==SIDE_PACT)
            printf("    " ANSI_RED "%s" ANSI_RESET " (%s) %d wpns%s\n",
                   ships[i].name, ships[i].hull_class, ships[i].num_weapons,
                   ships[i].max_sorties>0?" [CARRIER]":"");

    printf("\n  Features: Layered air defense • Carrier aviation • Sonar/ASW\n");
    printf("             CIWS intercept • Chaff/ECM • Compartment damage\n");
    printf("             Weather SS%d (%s) • Torpedo decoys\n\n",
           sea_state, beaufort_names[sea_state]);

    print_status_board(0);
    print_tactical_map();
    printf(ANSI_BOLD "\n  ─── ENGAGEMENT BEGINS ───\n\n" ANSI_RESET);

    int battle_over = 0;
    for (int tick = 1; tick <= MAX_TICK && !battle_over; tick++) {
        phase_weather(tick);
        phase_detect(tick);
        phase_sonar(tick);
        phase_move(tick);
        phase_weapons(tick);
        phase_aviation(tick);
        phase_damage_consequences(tick);

        if (tick % 120 == 0) {
            print_status_board(tick);
            print_tactical_map();
        }

        int nu=0, pu=0;
        for (int i=0;i<num_ships;i++){
            if(!ships[i].alive) continue;
            if(ships[i].side==SIDE_NATO) nu++;
            else pu++;
        }
        if (nu==0 || pu==0) battle_over=1;
    }

    print_status_board(MAX_TICK);
    print_tactical_map();
    write_csv();
    assess_victory();
    return 0;
}
