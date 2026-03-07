# NAVSIM — Cold War Naval Tactical Engagement Simulator

```
    ███╗   ██╗ █████╗ ██╗   ██╗███████╗██╗███╗   ███╗
    ████╗  ██║██╔══██╗██║   ██║██╔════╝██║████╗ ████║
    ██╔██╗ ██║███████║██║   ██║███████╗██║██╔████╔██║
    ██║╚██╗██║██╔══██║╚██╗ ██╔╝╚════██║██║██║╚██╔╝██║
    ██║ ╚████║██║  ██║ ╚████╔╝ ███████║██║██║ ╚═╝ ██║
    ╚═╝  ╚═══╝╚═╝  ╚═╝  ╚═══╝  ╚══════╝╚═╝╚═╝     ╚═╝
```

A Monte Carlo–driven naval warfare simulator in the spirit of 1960s mainframe wargaming programs. Written in **C** (simulation engine) with a **Python** companion for post-battle analytics.

Simulates Cold War–era surface engagements between NATO and Warsaw Pact task forces with probabilistic detection, realistic weapons envelopes, and damage modeling.

## The Scenario

**North Atlantic, circa 1985.** A NATO surface action group intercepts a Soviet task force.

| Side | Ships |
|------|-------|
| **NATO** | USS Ticonderoga (CG-47), USS Spruance (DD-963), USS Knox (FF-1052), USS Los Angeles (SSN-688) |
| **PACT** | Slava (Pr.1164), Sovremenny (Pr.956), Nanuchka (Pr.1234), Victor III (Pr.671RTM) |

## Features

- **xoshiro128** PRNG for deterministic, reproducible battles (pass a seed as argv[1])
- **Three-phase tick engine**: Detection → Movement → Weapons, 1-second resolution
- Radar detection modeled with range, RCS, and submarine stealth factors
- AI maneuvering: ships close to optimal weapon range, then hold station with evasive jinking
- Weapons with individual reload times, salvo sizes, ammo counts, and range-degraded hit probabilities
- ECM modifiers for capital ships, sea state gaussian noise
- Real-time ANSI terminal display with tactical ASCII map
- CSV output for post-processing
- **Monte Carlo mode**: run hundreds of battles and get aggregate win rates, survival distributions, and weapon effectiveness stats

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
| `ship_status.csv` | Final state of all ships: HP, position, kills, damage dealt |

## Architecture

```
navsim.c (C11, ~600 LOC)
├── xoshiro128** RNG (deterministic, fast)
├── Ship/Weapon data structures
├── Scenario builder (NATO vs PACT OOB)
├── Detection phase (radar range × RCS)
├── Movement phase (AI targeting + evasion)
├── Weapons phase (Monte Carlo hit resolution)
├── ANSI tactical display + ASCII map
└── CSV battle recorder

navsim_analysis.py (Python 3, ~250 LOC)
├── Single-battle analysis
│   ├── Ship survivability report
│   ├── Weapon effectiveness matrix
│   ├── Engagement timeline histogram
│   ├── Kill chain reconstruction
│   └── Lanchester force-ratio analysis
└── Monte Carlo aggregation
    ├── Win rate distribution
    ├── Force preservation statistics
    ├── Per-ship survival rates
    └── HP% outcome histogram
```

## Wargaming Notes

The P-500 Bazalt's 300 NM range gives the Slava a massive first-strike advantage, but its lower hit probability (0.55) and the Ticonderoga's SM-2 / Phalanx layered defense partially offset this. The Harpoon's shorter range (65 NM) but higher accuracy (0.72) means NATO needs to survive the initial missile exchange to close range.

The SSN-688 Los Angeles is the wild card — its low RCS (0.10) makes it nearly undetectable, and the Mk 48 ADCAP torpedo is devastating at close range. The Victor III is similarly stealthy but carries the older 53-65 torpedo.

Try running Monte Carlo with 500+ iterations to see how the balance shifts. Adjust weapon parameters in `build_scenario()` to wargame different loadouts.

## License

GPL-3.0 — in the spirit of open-source wargaming.
