# NAVSIM — Advanced Naval Tactical Engagement Simulator

```
    ███╗   ██╗ █████╗ ██╗   ██╗███████╗██╗███╗   ███╗
    ████╗  ██║██╔══██╗██║   ██║██╔════╝██║████╗ ████║
    ██╔██╗ ██║███████║██║   ██║███████╗██║██╔████╔██║
    ██║╚██╗██║██╔══██║╚██╗ ██╔╝╚════██║██║██║╚██╔╝██║
    ██║ ╚████║██║  ██║ ╚████╔╝ ███████║██║██║ ╚═╝ ██║
    ╚═╝  ╚═══╝╚═╝  ╚═╝  ╚═══╝  ╚══════╝╚═╝╚═╝     ╚═╝
```

An **advanced Monte Carlo-driven naval warfare simulator** featuring realistic ballistics, radar physics, and electronic warfare. Written in **C** (simulation engine) with a **Python** companion for post-battle analytics.

Simulates modern naval engagements between NATO and Warsaw Pact battle groups with **physics-based projectile trajectories, multi-band radar detection, compartmentalized damage modeling, and ECM/ESM warfare**.

## The Scenario

**North Atlantic, circa 1985–1990.** A NATO Carrier Strike Group encounters a Soviet Battle Group.

| Side | Vessels |
|------|---------|
| **NATO CSG** | USS Nimitz (CVN-68), USS Ticonderoga (CG-47), USS Arleigh Burke (DDG-51), USS Spruance (DD-963), USS Oliver H. Perry (FFG-7), USS Knox (FF-1052), USS Independence (LCS-2), HDMS Absalon (L16), USS Los Angeles (SSN-688), USS Virginia (SSN-774) |
| **PACT BG** | Kuznetsov (Pr.1143.5), Slava (Pr.1164), Kirov (Pr.1144), Sovremenny (Pr.956), Udaloy (Pr.1155), Krivak II (Pr.1135M), Grisha V (Pr.1124M), Nanuchka III (Pr.1234.1), Tarantul III (Pr.1241.1MP), Victor III (Pr.671RTM), Akula (Pr.971) |

## New Features (v2.0)

### 🎯 Realistic Ballistics Simulation
- **Physics-based projectile trajectories** — shells and missiles travel through 3D space with gravity, drag, and wind effects
- **Accurate muzzle velocities** — 808 m/s for 5"/54 guns, Mach 0.95 for Harpoon, Mach 3.0 for Moskit
- **Projectile mass & drag modeling** — 70kg naval shells, 520kg Harpoon missiles, 4500kg Moskit missiles
- **Elevation angles** — naval guns fire at 42-52° for optimal range
- **Guided vs. unguided** — missiles have guidance quality factorization affecting terminal accuracy
- **Time-of-flight calculations** — no instant hits, watch projectiles travel to targets

### 📡 Advanced Radar Systems
- **Multi-band radar** — S-band search (long range), X-band fire control (precision), L-band early warning, C-band tracking
- **Radar equation modeling** — detection based on transmit power, antenna gain, frequency, and target RCS
- **RCS-dependent detection** — submarines (0.08-0.12), corvettes (0.28-0.38), cruisers (0.65-0.85), carriers (1.2-1.3)
- **Multipath propagation** — sea clutter and surface reflection effects at close range
- **Weather interference** — Gaussian noise from sea state
- **Frequency-dependent characteristics** — X-band has better resolution, L-band has longer range but lower precision

### 🛡️ Electronic Warfare
- **ECM jamming** — ships emit jamming power to degrade enemy radar effectiveness
- **Jam strength modeling** — ratio of jammer power to radar power determines degradation
- **ESM receivers** — passive detection of enemy radar emissions
- **Chaff & flare dispensers** — 20 charges per ship for decoys
- **Power ratings** — Nimitz: 150 kW, Ticonderoga: 80 kW, Knox: 18 kW

### 🔥 Compartmentalized Damage
- **12 damage compartments** per ship — bow, bridge, forward weapons, engines, etc.
- **Integrity tracking** — each compartment has 0.0–1.0 structural integrity
- **Flooding simulation** — damaged compartments can flood at variable rates (m³/min)
- **Fire dynamics** — 25% chance of fire per hit, affecting ship operations
- **Crew casualties** — tracked separately from ship HP
- **List angle** — ship stability affected by flooding distribution

### ⚔️ Expanded Arsenal
**New weapon types:**
- **Cruise missiles** — Tomahawk BGM-109 (280 NM), 3M-54 Kalibr (260 NM)
- **ASROC** — RUM-139 VL-ASROC, RPK-2 Viyuga anti-submarine rockets
- **Advanced guns** — 76mm Mk 110, AK-176, Millennium Gun CIWS
- **Railgun** — Future directed energy weapons (experimental)

### 🚢 Massive Fleet Expansion
- **24 ship capacity** (up from 8) — true battle group vs. battle group
- **11 NATO vessels** — including carrier, Aegis cruiser, destroyers, frigates, corvettes, SSNs
- **11 PACT vessels** — including carrier, Kirov battlecruiser, destroyers, corvettes, fast attack craft, SSNs
- **300 NM battlespace** (expanded from 200 NM)
- **60-minute engagements** (up from 20 minutes)

## Features

- **xoshiro128** PRNG for deterministic, reproducible battles (pass a seed as argv[1])
- **Four-phase tick engine**: Detection → Movement → Weapons → Projectile Physics, 1-second resolution
- **Physics-based ballistics** with drag, gravity, and guidance simulation
- **Multi-band radar detection** with ECM/ESM modeling
- **Compartmentalized damage** with flooding and fire propagation
- **Advanced AI maneuvering** — ships position for optimal weapon range with evasive jinking
- **Realistic weapons** with individual reload times, salvo sizes, ammo counts, and ballistic parameters
- **Electronic warfare** with jamming, chaff, and radar frequency effects
- **Real-time ANSI terminal display** with tactical ASCII map showing projectiles in flight
- **CSV output** for post-processing and Monte Carlo analysis
- **Monte Carlo mode** — run hundreds of battles for aggregate win rates, survival distributions, and weapon effectiveness stats

## Build & Run

```bash
make          # compile
make run      # compile and run one battle
make analyze  # run battle + Python post-analysis
make monte-carlo  # 100 Monte Carlo runs with statistical analysis
```

Or manually:
```bash
gcc -O2 -Wall -o navsim navsim.c -lm

./navsim              # random seed
./navsim 42           # deterministic seed
python3 navsim_analysis.py                   # analyze last battle
python3 navsim_analysis.py --monte-carlo 200 # aggregate analysis
```

Or
```bash
& "C:\msys64\ucrt64\bin\gcc.exe" -o navsim.exe navsim.c
```

## Output Files

| File | Contents |
|------|----------|
| `battle_log.csv` | Every weapons engagement: tick, attacker, defender, weapon, hits, damage, kills |
| `ship_status.csv` | Final state of all ships: HP, position, kills, damage dealt, compartment status |

## Architecture

```
navsim.c (C11, ~1300 LOC)
├── xoshiro128** RNG (deterministic, fast)
├── Ship/Weapon/Radar/Projectile data structures
├── Scenario builder (NATO CSG vs PACT BG, 22 ships)
├── Ballistics physics engine
│   ├── Projectile initialization with velocity vectors
│   ├── Gravity and drag calculations
│   ├── 3D trajectory integration
│   └── Impact detection and damage application
├── Detection phase
│   ├── Multi-band radar modeling (S/X/L/C-band)
│   ├── Radar equation (power × gain × RCS)
│   ├── ECM jamming effects
│   ├── Multipath propagation over water
│   └── Weather/sea state interference
├── Movement phase (AI targeting + evasion)
├── Weapons phase
│   ├── Target prioritization
│   ├── Projectile launch with ballistic parameters
│   ├── Compartmental damage modeling
│   └── Flooding and fire simulation
├── ANSI tactical display + ASCII map
└── CSV battle recorder

navsim_analysis.py (Python 3, ~350 LOC)
├── Single-battle analysis
│   ├── Ship survivability with compartment damage
│   ├── Weapon effectiveness matrix
│   ├── Engagement timeline (launch vs. impact)
│   ├── Kill chain reconstruction
│   ├── Ballistic trajectory analysis
│   └── Lanchester force-ratio modeling
└── Monte Carlo aggregation
    ├── Win rate distribution
    ├── Force preservation statistics
    ├── Per-ship survival rates
    ├── Radar detection effectiveness
    └── ECM/ESM impact analysis
```

## Technical Details

### Ballistics Model
The simulator uses a simplified but realistic ballistic model:

**For guns (shells):**
- Initial velocity: 762-1000 m/s depending on caliber
- Elevation angle: 42-52° for maximum range
- Projectile mass: 22-70 kg
- Drag coefficient: 0.3 (typical for naval shells)
- Gravity: 9.81 m/s² applied to vertical component
- Impact detection: 100-yard radius depending on ship size

**For missiles:**
- Cruise speed: Mach 0.72-3.0 depending on type
- Guidance quality: 0.65-0.92 (affects terminal accuracy)
- Constant altitude flight for SSMs
- Lead-pursuit guidance toward moving targets
- Mass: 407-7000 kg

### Radar Physics
Detection probability is modeled using a simplified radar equation:

```
Effective Range = Base Range × RCS^0.25
P(detect) = 1 - (distance / effective_range)^2.5
```

With modifications for:
- **Band effects**: L-band (×0.85), X-band (×1.15)
- **ECM jamming**: Range reduction = 1 - (Jammer Power / Radar Power) × 0.5
- **Sea clutter**: Gaussian noise σ=0.12 at close range
- **Submarine stealth**: Detection probability ×0.15

### Damage & Flooding
Each ship has 12 compartments that can be individually damaged:
- Bow, Bridge, Forward Weapons, Midship (Port/Stbd)
- Engine Room, Aft Weapons, Stern, Radar, Communications

When a compartment's integrity falls below 0.3:
- **Flooding begins** at 0-20 m³/min
- **Fire risk**: 25% chance per hit
- **Speed reduction** if engine room damaged
- **Weapon degradation** if weapons bay damaged

## Wargaming Notes

### Range Advantage
The **P-700 Granit** (340 NM) and **Tomahawk** (280 NM) cruise missiles give both sides massive first-strike capability, but their lower hit probability (0.62-0.78) means the initial exchange is crucial. The **P-270 Moskit's** Mach 3 speed (125 NM range, 0.72 hit rate) makes it deadly in medium-range engagements.

NATO's **Harpoon** (70 NM, 0.75 hit rate) and Soviet **P-500 Bazalt** (310 NM, 0.60 hit rate) represent different doctrines: NATO focuses on accuracy at medium range, while the Soviets rely on saturation attacks from beyond visual range.

### Submarine Warfare
The **SSN-688 Los Angeles** and **SSN-774 Virginia** are the NATO wildcards — ultra-low RCS (0.10-0.12) makes them nearly undetectable, and the **Mk 48 ADCAP torpedo** (22 NM, 240 damage, 0.78-0.80 hit rate) is devastating. They can also launch **Tomahawks** from underwater.

The Soviet **Victor III** and **Akula** are similarly stealthy (RCS 0.08-0.09) and carry the powerful **USET-80 torpedo** (18 NM, 195 damage, 0.68-0.70 hit rate), plus the **3M-54 Kalibr** cruise missile on the Akula.

### Electronic Warfare
Heavy ECM from carriers and cruisers (80-150 kW) can reduce radar effective range by 30-50% against smaller radars. The **Kirov battlecruiser** with 110 kW ECM and the **Nimitz carrier** with 150 kW ECM are electronic warfare powerhouses that degrade enemy targeting.

### Ballistic Realism
Watch shells arc through the sky — a 5"/54 gun firing at 45° sends a 70kg shell on a 20-second parabolic trajectory. Missiles cruise at Mach 0.85-3.0, taking 30-90 seconds to reach distant targets. This time-of-flight creates tactical opportunities for maneuvering and countermeasures.

### Compartmental Damage
A single **P-700 Granit** hit (165 damage) can breach 2-3 compartments on a destroyer, causing flooding and fires. Ships can survive hits but lose capability progressively — radar damage blinds them, engine hits slow them, weapons bay damage disarms them.

## Data Sources

Ship and weapon specifications are based on real-world naval reference data:
- **SeaForces.org** — https://www.seaforces.org/ — Ship specifications, armament, sensors
- **NavWeaps.com** — http://www.navweaps.com/ — Detailed weapon ballistics, ranges, and performance

## Performance

On a modern CPU, NAVSIM v2.0 runs at approximately:
- **Single battle**: ~2-5 seconds (60-minute engagement)
- **Monte Carlo (100 runs)**: ~3-6 minutes
- **Projectile tracking**: up to 512 simultaneous projectiles in flight

## Future Enhancements

Potential additions for v3.0:
- Air defense network coordination (cooperative engagement capability)
- Weather systems (storms, fog, wind effects on missiles)
- Minefield simulation
- Helicopter ASW operations
- Salvo doctrine modeling (coordinated multi-ship strikes)
- More ship classes (amphibious, auxiliary, patrol boats)
- Geographic terrain (islands, coastlines, shallow water)

## License

Public Domain / GPL-3.0 Compatible

In the spirit of the UNIVAC naval fire control systems and classic naval wargaming.

---

**v2.0** — Advanced physics-based naval combat simulation  
For questions, issues, or contributions, see the repository.

Try running Monte Carlo with 500+ iterations to see how the balance shifts. Adjust weapon parameters in `build_scenario()` to wargame different loadouts.

## License

GPL-3.0 — in the spirit of open-source wargaming.
