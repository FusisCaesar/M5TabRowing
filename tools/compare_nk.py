"""Compare a Tab5 session with an NK SpeedCoach recording of the same outing.

Export the NK session from NK LiNK Logbook as CSV (per-stroke data), copy
S####_strokes.csv from the Tab5 SD card, then:

  python tools/compare_nk.py S0007_strokes.csv nk_export.csv
  python tools/compare_nk.py S0007_strokes.csv nk_export.csv --lag 12.5   # skip auto-alignment

The two devices are started by hand, so their clocks differ by an unknown
offset. It is found by sliding the Tab5 stroke-rate curve over the NK one and
keeping the offset with the smallest mean rate difference.
"""
import argparse
import bisect
import csv
import statistics


def parse_time(text):
    """'1:02:03.4', '02:03.4' or '123.4' -> seconds."""
    text = text.strip()
    if not text or text.startswith("-"):
        return None
    try:
        parts = [float(p) for p in text.split(":")]
    except ValueError:
        return None
    s = 0.0
    for p in parts:
        s = s * 60 + p
    return s


def parse_float(text):
    try:
        return float(text.strip())
    except (ValueError, AttributeError):
        return None


def find_col(header, *names):
    low = [h.strip().lower() for h in header]
    for name in names:
        for i, h in enumerate(low):
            if h == name.lower():
                return i
    for name in names:
        for i, h in enumerate(low):
            if h.startswith(name.lower()):
                return i
    return None


def load_nk(path):
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as f:
        rows = list(csv.reader(f))
    for hi, header in enumerate(rows):
        if find_col(header, "Elapsed Time") is not None and find_col(header, "Stroke Rate") is not None:
            break
    else:
        raise SystemExit(f"{path}: no per-stroke header row with 'Elapsed Time' and 'Stroke Rate'")
    c_t = find_col(header, "Elapsed Time")
    c_rate = find_col(header, "Stroke Rate")
    c_split = find_col(header, "Split (GPS)", "Split")
    c_dist = find_col(header, "Distance (GPS)", "Distance")
    c_dps = find_col(header, "Distance/Stroke (GPS)", "Distance/Stroke")
    out = []
    for r in rows[hi + 1:]:
        if len(r) <= max(c for c in (c_t, c_rate) if c is not None):
            continue
        t = parse_time(r[c_t])
        rate = parse_float(r[c_rate])
        if t is None or rate is None:
            continue  # units row, blank line or summary section
        get = lambda c, fn: fn(r[c]) if c is not None and c < len(r) else None
        out.append({"t": t, "rate": rate, "split": get(c_split, parse_time),
                    "dist": get(c_dist, parse_float), "dps": get(c_dps, parse_float)})
    if not out:
        raise SystemExit(f"{path}: header found but no stroke rows")
    return out


def load_tab5(path):
    out = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rate = float(r["rate_spm"])
            if rate <= 0:
                continue
            split = float(r["split_s"])
            out.append({"t": float(r["elapsed_s"]), "rate": rate, "split": split if split > 0 else None,
                        "dist": float(r["distance_m"]), "dps": float(r["dps_m"]) or None})
    if not out:
        raise SystemExit(f"{path}: no strokes with a rate")
    return out


def nearest(series, times, t, max_gap):
    i = bisect.bisect_left(times, t)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(series) and abs(times[j] - t) <= max_gap:
            if best is None or abs(times[j] - t) < abs(times[best] - t):
                best = j
    return None if best is None else series[best]


def match(tab5, nk, lag, max_gap=1.5):
    """Pairs each Tab5 stroke (shifted by lag) with the nearest NK stroke."""
    nk_t = [s["t"] for s in nk]
    pairs = []
    for s in tab5:
        n = nearest(nk, nk_t, s["t"] + lag, max_gap)
        if n is not None:
            pairs.append((s, n))
    return pairs


UNMATCHED_COST_SPM = 3.0


def lag_cost(tab5, nk, lag):
    """Mean rate error where every unmatched Tab5 stroke counts as a large error.

    Without the penalty, steady-rate rowing fits equally well at many offsets with little overlap.
    """
    pairs = match(tab5, nk, lag)
    err = sum(min(abs(a["rate"] - b["rate"]), UNMATCHED_COST_SPM) for a, b in pairs)
    return (err + UNMATCHED_COST_SPM * (len(tab5) - len(pairs))) / len(tab5)


def find_lag(tab5, nk, span):
    candidates = [-span + 0.5 * i for i in range(int(4 * span) + 1)]
    best = min(candidates, key=lambda lag: lag_cost(tab5, nk, lag))
    best = min((best + 0.1 * i for i in range(-5, 6)), key=lambda lag: lag_cost(tab5, nk, lag))
    if lag_cost(tab5, nk, best) >= 0.9 * UNMATCHED_COST_SPM:
        raise SystemExit("could not align the sessions; pass --lag")
    # Rate varies slowly, so finish by lining up the individual catch times.
    for _ in range(3):
        pairs = match(tab5, nk, best, max_gap=0.5 * statistics.median(
            b["t"] - a["t"] for a, b in zip(nk, nk[1:])))
        if pairs:
            best += statistics.median(b["t"] - (a["t"] + best) for a, b in pairs)
    return best


def describe(name, diffs, unit):
    if not diffs:
        print(f"  {name:<18} no overlapping values")
        return
    mae = statistics.mean(abs(d) for d in diffs)
    sd = statistics.pstdev(diffs) if len(diffs) > 1 else 0.0
    within = sorted(abs(d) for d in diffs)[int(0.95 * (len(diffs) - 1))]
    print(f"  {name:<18} bias {statistics.mean(diffs):+7.2f} {unit:<4} mean abs {mae:6.2f}  "
          f"sd {sd:6.2f}  95% within {within:6.2f}  (n={len(diffs)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tab5", help="S####_strokes.csv from the Tab5")
    ap.add_argument("nk", help="NK LiNK Logbook CSV export")
    ap.add_argument("--lag", type=float, help="seconds to add to Tab5 time to match NK time")
    ap.add_argument("--span", type=float, default=300, help="auto-alignment search range in seconds")
    ap.add_argument("--from", dest="t_from", type=float, default=0, help="ignore NK time before this")
    ap.add_argument("--to", dest="t_to", type=float, default=1e9, help="ignore NK time after this")
    args = ap.parse_args()

    tab5 = load_tab5(args.tab5)
    nk = [s for s in load_nk(args.nk) if args.t_from <= s["t"] <= args.t_to]
    print(f"Tab5 {len(tab5)} strokes over {tab5[-1]['t'] - tab5[0]['t']:.0f} s, "
          f"NK {len(nk)} strokes over {nk[-1]['t'] - nk[0]['t']:.0f} s")

    lag = args.lag if args.lag is not None else find_lag(tab5, nk, args.span)
    pairs = match(tab5, nk, lag)
    print(f"offset: Tab5 time + {lag:.1f} s = NK time, {len(pairs)} strokes matched\n")
    print("Tab5 minus NK:")
    describe("stroke rate", [a["rate"] - b["rate"] for a, b in pairs], "spm")
    describe("split /500m", [a["split"] - b["split"] for a, b in pairs if a["split"] and b["split"]], "s")
    describe("distance/stroke", [a["dps"] - b["dps"] for a, b in pairs if a["dps"] and b["dps"]], "m")

    if pairs and pairs[0][1]["dist"] is not None:
        a0, b0 = pairs[0]
        a1, b1 = pairs[-1]
        d_tab5, d_nk = a1["dist"] - a0["dist"], b1["dist"] - b0["dist"]
        if d_nk > 0:
            print(f"\n  distance over matched span: Tab5 {d_tab5:.0f} m, NK {d_nk:.0f} m "
                  f"({100 * (d_tab5 - d_nk) / d_nk:+.1f}%)")

    big = [(a, b) for a, b in pairs if abs(a["rate"] - b["rate"]) >= 2]
    if big:
        print(f"\n{len(big)} strokes differ by 2 spm or more (NK time, Tab5 rate, NK rate):")
        for a, b in big[:15]:
            print(f"  {b['t']:8.1f} s  {a['rate']:5.1f}  {b['rate']:5.1f}")
        if len(big) > 15:
            print(f"  ... {len(big) - 15} more")


if __name__ == "__main__":
    main()
