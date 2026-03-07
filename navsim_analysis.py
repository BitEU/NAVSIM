#!/usr/bin/env python3
"""
NAVSIM Post-Battle Analytics
═══════════════════════════════
Reads battle_log.csv and ship_status.csv produced by the C engine
and generates an engagement analysis report with ASCII charts.

Usage:
    python3 navsim_analysis.py [--monte-carlo N]

The --monte-carlo flag runs the C simulator N times with different
seeds and aggregates the results for statistical analysis.
"""

import csv
import os
import sys
import subprocess
import statistics
from collections import defaultdict


# ── ANSI colors ──
RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RED    = "\033[31m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
CYAN   = "\033[36m"
MAGENTA= "\033[35m"


def load_battle_log(path="battle_log.csv"):
    """Load engagement records from CSV."""
    records = []
    try:
        with open(path, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                row['tick'] = int(row['tick'])
                row['hits'] = int(row['hits'])
                row['damage'] = float(row['damage'])
                row['defender_hp_after'] = float(row['defender_hp_after'])
                row['kill'] = int(row['kill'])
                records.append(row)
    except FileNotFoundError:
        print(f"{RED}Error: {path} not found. Run navsim first.{RESET}")
        sys.exit(1)
    return records


def load_ship_status(path="ship_status.csv"):
    """Load final ship status from CSV."""
    ships = []
    try:
        with open(path, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                row['hp'] = float(row['hp'])
                row['max_hp'] = float(row['max_hp'])
                row['alive'] = int(row['alive'])
                row['kills'] = int(row['kills'])
                row['damage_dealt'] = int(row['damage_dealt'])
                row['final_x'] = float(row['final_x'])
                row['final_y'] = float(row['final_y'])
                ships.append(row)
    except FileNotFoundError:
        print(f"{RED}Error: {path} not found.{RESET}")
        sys.exit(1)
    return ships


def ascii_bar(value, max_val, width=40, fill="█", empty="░"):
    """Create an ASCII progress bar."""
    if max_val <= 0:
        return empty * width
    filled = int(round(value / max_val * width))
    filled = min(filled, width)
    return fill * filled + empty * (width - filled)


def print_header(title):
    """Print a section header."""
    print(f"\n{BOLD}{CYAN}{'═' * 70}")
    print(f"  {title}")
    print(f"{'═' * 70}{RESET}")


def analyze_single_battle(records, ships):
    """Analyze a single battle's results."""

    print_header("NAVSIM POST-BATTLE ANALYSIS")

    # ── Ship Survivability ──
    print(f"\n{BOLD}  SHIP SURVIVABILITY{RESET}")
    print(f"  {'Ship':<22} {'Side':<6} {'HP':<12} {'Status':<10} {'Kills':<6} {'Dmg Dealt'}")
    print(f"  {DIM}{'─' * 65}{RESET}")

    for s in ships:
        hp_pct = (s['hp'] / s['max_hp'] * 100) if s['max_hp'] > 0 else 0
        color = GREEN if s['alive'] else RED
        status = "ACTIVE" if s['alive'] else "SUNK"
        bar = ascii_bar(s['hp'], s['max_hp'], width=10)

        side_color = GREEN if s['side'] == 'NATO' else RED
        print(f"  {color}{s['name']:<22}{RESET} "
              f"{side_color}{s['side']:<6}{RESET} "
              f"{bar} {hp_pct:>4.0f}%  {color}{status:<10}{RESET} "
              f"{s['kills']:<6} {s['damage_dealt']}")

    # ── Weapon Effectiveness ──
    print_header("WEAPON EFFECTIVENESS ANALYSIS")

    weapon_stats = defaultdict(lambda: {'fired': 0, 'hits': 0, 'damage': 0, 'kills': 0})
    for r in records:
        w = weapon_stats[r['weapon']]
        w['fired'] += 1
        w['hits'] += r['hits']
        w['damage'] += r['damage']
        w['kills'] += r['kill']

    print(f"\n  {'Weapon':<22} {'Salvos':<8} {'Hits':<8} {'Hit%':<8} "
          f"{'Total Dmg':<10} {'Kills'}")
    print(f"  {DIM}{'─' * 65}{RESET}")

    for wname, ws in sorted(weapon_stats.items(), key=lambda x: -x[1]['damage']):
        hit_rate = (ws['hits'] / ws['fired'] * 100) if ws['fired'] > 0 else 0
        bar = ascii_bar(hit_rate, 100, width=8, fill="▓", empty="░")
        print(f"  {YELLOW}{wname:<22}{RESET} {ws['fired']:<8} {ws['hits']:<8} "
              f"{bar} {ws['damage']:>9.0f} {ws['kills']}")

    # ── Engagement Timeline ──
    print_header("ENGAGEMENT TIMELINE")

    if records:
        max_tick = max(r['tick'] for r in records)
        min_tick = min(r['tick'] for r in records)
        bucket_size = max(1, (max_tick - min_tick + 1) // 20)
        buckets = defaultdict(float)
        for r in records:
            bucket = ((r['tick'] - min_tick) // bucket_size) * bucket_size + min_tick
            buckets[bucket] += r['damage']

        max_dmg = max(buckets.values()) if buckets else 1
        print(f"\n  Damage over time (each row ≈ {bucket_size} seconds)")
        print(f"  {DIM}{'─' * 55}{RESET}")

        for t in sorted(buckets.keys()):
            minutes = t // 60
            seconds = t % 60
            bar_len = int(buckets[t] / max_dmg * 40)
            bar = "█" * bar_len
            color = RED if buckets[t] > max_dmg * 0.7 else YELLOW if buckets[t] > max_dmg * 0.3 else GREEN
            print(f"  T+{minutes:02d}:{seconds:02d} │{color}{bar:<40}{RESET}│ {buckets[t]:>6.0f}")

    # ── Kill Chain ──
    kills = [r for r in records if r['kill']]
    if kills:
        print_header("KILL CHAIN")
        for k in kills:
            t = k['tick']
            print(f"  {RED}✕{RESET} T+{t//60:02d}:{t%60:02d}  "
                  f"{k['attacker']} sank {RED}{k['defender']}{RESET} "
                  f"with {YELLOW}{k['weapon']}{RESET}")

    # ── Force Ratio Summary ──
    print_header("FORCE RATIO ANALYSIS")
    nato_hp = sum(s['hp'] for s in ships if s['side'] == 'NATO')
    pact_hp = sum(s['hp'] for s in ships if s['side'] == 'PACT')
    nato_max = sum(s['max_hp'] for s in ships if s['side'] == 'NATO')
    pact_max = sum(s['max_hp'] for s in ships if s['side'] == 'PACT')

    print(f"\n  {GREEN}NATO{RESET}  {ascii_bar(nato_hp, nato_max, 30)} "
          f"{nato_hp:.0f}/{nato_max:.0f} HP ({nato_hp/nato_max*100:.0f}%)")
    print(f"  {RED}PACT{RESET}  {ascii_bar(pact_hp, pact_max, 30)} "
          f"{pact_hp:.0f}/{pact_max:.0f} HP ({pact_hp/pact_max*100:.0f}%)")

    # ── Lanchester Analysis ──
    nato_alive = sum(1 for s in ships if s['side'] == 'NATO' and s['alive'])
    pact_alive = sum(1 for s in ships if s['side'] == 'PACT' and s['alive'])
    nato_total = sum(1 for s in ships if s['side'] == 'NATO')
    pact_total = sum(1 for s in ships if s['side'] == 'PACT')
    nato_attrition = (1 - nato_alive / nato_total) * 100 if nato_total else 0
    pact_attrition = (1 - pact_alive / pact_total) * 100 if pact_total else 0

    print(f"\n  Attrition rates:")
    print(f"    NATO: {nato_attrition:.0f}% ({nato_total - nato_alive}/{nato_total} lost)")
    print(f"    PACT: {pact_attrition:.0f}% ({pact_total - pact_alive}/{pact_total} lost)")

    exchange_ratio = (pact_max - pact_hp) / max(1, nato_max - nato_hp)
    print(f"\n  Exchange ratio (dmg dealt to PACT / dmg taken by NATO): {exchange_ratio:.2f}")
    if exchange_ratio > 1.5:
        print(f"  {GREEN}→ NATO achieved favorable exchange{RESET}")
    elif exchange_ratio < 0.67:
        print(f"  {RED}→ PACT achieved favorable exchange{RESET}")
    else:
        print(f"  {YELLOW}→ Roughly even exchange{RESET}")


def run_monte_carlo(n_runs, sim_path=None):
    """Run the simulator N times and aggregate results."""
    if sim_path is None:
        sim_path = "navsim.exe" if os.name == 'nt' else "./navsim"
    print_header(f"MONTE CARLO ANALYSIS ({n_runs} RUNS)")
    print(f"\n  Running {n_runs} simulations...\n")

    nato_wins = 0
    pact_wins = 0
    draws = 0
    nato_survival_rates = []
    pact_survival_rates = []
    nato_hp_pcts = []
    pact_hp_pcts = []
    ship_survival = defaultdict(int)
    ship_kill_totals = defaultdict(list)

    for i in range(n_runs):
        seed = 10000 + i * 7
        # Run simulation silently
        result = subprocess.run(
            [sim_path, str(seed)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30
        )

        ships = load_ship_status("ship_status.csv")

        nato_hp = sum(s['hp'] for s in ships if s['side'] == 'NATO')
        pact_hp = sum(s['hp'] for s in ships if s['side'] == 'PACT')
        nato_max = sum(s['max_hp'] for s in ships if s['side'] == 'NATO')
        pact_max = sum(s['max_hp'] for s in ships if s['side'] == 'PACT')
        nato_alive = sum(1 for s in ships if s['side'] == 'NATO' and s['alive'])
        pact_alive = sum(1 for s in ships if s['side'] == 'PACT' and s['alive'])
        nato_total = sum(1 for s in ships if s['side'] == 'NATO')
        pact_total = sum(1 for s in ships if s['side'] == 'PACT')

        nato_hp_pcts.append(nato_hp / nato_max * 100 if nato_max else 0)
        pact_hp_pcts.append(pact_hp / pact_max * 100 if pact_max else 0)
        nato_survival_rates.append(nato_alive / nato_total * 100 if nato_total else 0)
        pact_survival_rates.append(pact_alive / pact_total * 100 if pact_total else 0)

        for s in ships:
            if s['alive']:
                ship_survival[s['name']] += 1
            ship_kill_totals[s['name']].append(s['kills'])

        nato_score = (nato_hp / nato_max) * 100 + (pact_max - pact_hp) if nato_max else 0
        pact_score = (pact_hp / pact_max) * 100 + (nato_max - nato_hp) if pact_max else 0

        if nato_score > pact_score * 1.1:
            nato_wins += 1
        elif pact_score > nato_score * 1.1:
            pact_wins += 1
        else:
            draws += 1

        # Progress bar
        progress = (i + 1) / n_runs
        bar = ascii_bar(progress, 1.0, width=40)
        print(f"\r  {bar} {i+1}/{n_runs}", end="", flush=True)

    print("\n")

    # ── Win rates ──
    print(f"  {BOLD}WIN RATES:{RESET}")
    print(f"    {GREEN}NATO:{RESET}  {nato_wins}/{n_runs} ({nato_wins/n_runs*100:.1f}%)")
    print(f"    {RED}PACT:{RESET}  {pact_wins}/{n_runs} ({pact_wins/n_runs*100:.1f}%)")
    print(f"    {YELLOW}DRAW:{RESET}  {draws}/{n_runs} ({draws/n_runs*100:.1f}%)")

    total_w = 50
    nato_bar = int(nato_wins / n_runs * total_w)
    pact_bar = int(pact_wins / n_runs * total_w)
    draw_bar = total_w - nato_bar - pact_bar
    print(f"\n    {GREEN}{'█' * nato_bar}{YELLOW}{'█' * draw_bar}{RED}{'█' * pact_bar}{RESET}")

    # ── HP distributions ──
    print(f"\n  {BOLD}FORCE PRESERVATION (avg remaining HP%):{RESET}")
    nato_mean = statistics.mean(nato_hp_pcts)
    pact_mean = statistics.mean(pact_hp_pcts)
    nato_std = statistics.stdev(nato_hp_pcts) if len(nato_hp_pcts) > 1 else 0
    pact_std = statistics.stdev(pact_hp_pcts) if len(pact_hp_pcts) > 1 else 0
    print(f"    {GREEN}NATO:{RESET} {nato_mean:.1f}% ± {nato_std:.1f}%  "
          f"{ascii_bar(nato_mean, 100, 30)}")
    print(f"    {RED}PACT:{RESET} {pact_mean:.1f}% ± {pact_std:.1f}%  "
          f"{ascii_bar(pact_mean, 100, 30)}")

    # ── Ship survival rates ──
    print(f"\n  {BOLD}INDIVIDUAL SHIP SURVIVAL RATES:{RESET}")
    print(f"  {'Ship':<22} {'Survival':<10} {'Avg Kills'}")
    print(f"  {DIM}{'─' * 45}{RESET}")
    for name in sorted(ship_survival.keys(), key=lambda n: -ship_survival[n]):
        surv_rate = ship_survival[name] / n_runs * 100
        avg_kills = statistics.mean(ship_kill_totals[name])
        bar = ascii_bar(surv_rate, 100, width=15)
        color = GREEN if surv_rate > 60 else YELLOW if surv_rate > 30 else RED
        print(f"  {name:<22} {color}{bar} {surv_rate:>5.1f}%{RESET}  {avg_kills:.1f}")

    # ── Histogram of outcomes ──
    print(f"\n  {BOLD}NATO HP% DISTRIBUTION (histogram):{RESET}")
    buckets = [0] * 10
    for hp in nato_hp_pcts:
        idx = min(int(hp / 10), 9)
        buckets[idx] += 1

    max_count = max(buckets) if buckets else 1
    for i in range(9, -1, -1):
        bar = "█" * int(buckets[i] / max_count * 30) if max_count > 0 else ""
        label = f"{i*10:>3}-{(i+1)*10:<3}%"
        print(f"    {label} │{GREEN}{bar:<30}{RESET}│ {buckets[i]}")


def main():
    monte_carlo = 0
    if "--monte-carlo" in sys.argv:
        idx = sys.argv.index("--monte-carlo")
        if idx + 1 < len(sys.argv):
            monte_carlo = int(sys.argv[idx + 1])

    if monte_carlo > 0:
        # Compile if needed
        exe_name = "navsim.exe" if os.name == 'nt' else "navsim"
        if not os.path.exists(exe_name):
            print(f"  {YELLOW}Compiling navsim...{RESET}")
            if os.name == 'nt':
                os.system("gcc -O2 -Wall -o navsim.exe navsim.c -lm")
            else:
                os.system("gcc -O2 -o navsim navsim.c -lm")
        run_monte_carlo(monte_carlo)
    else:
        records = load_battle_log()
        ships = load_ship_status()
        analyze_single_battle(records, ships)


if __name__ == "__main__":
    main()
