"""Turn a recorded session into plots for tuning the stroke detector.

Usage:
  python tools/analyse.py recordings/S0007            # or any S0007_*.csv file
  python tools/analyse.py recordings/S0007 --replay   # also run tools/replay.py's detector
  python tools/analyse.py recordings/S0007 --replay --k 2.0 --floor 0.10 --minima

Writes into recordings/S0007_analysis/:
  summary.txt         duration, sample rate, strokes against a periodicity estimate,
                      split jitter, and the suspicious strokes
  overview.png        stroke rate, interval and GPS speed over the whole session
  strokes.png         every stroke overlaid at the catch, plus the average shape per rate band
  timeline_NNN_*.png  the detector's view of the whole session, one page per minute
  suspect_NN_*.png    a close-up around every stroke whose interval doesn't fit its neighbours
                      (likely a missed catch or a double count)

Needs numpy and matplotlib: pip install numpy matplotlib
"""
import argparse
import csv
import glob
import os
import statistics
import sys

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    sys.exit("analyse.py needs numpy and matplotlib: pip install numpy matplotlib")

STROKE_TIMEOUT_S = 6.0      # longer gaps are pauses, not missed strokes
SHORT_FACTOR = 0.7          # interval below this x the local median: probably a double count
LONG_FACTOR = 1.6           # interval above this x the local median: probably a missed catch
NEIGHBOURS = 4              # strokes either side for the local median
MAX_SUSPECT_PAGES = 40
TIMELINE_PAGE_S = 60
RATE_BANDS = [(0, 22), (22, 26), (26, 30), (30, 60)]


def session_files(arg):
    base = arg
    for suffix in ("_imu.csv", "_strokes.csv", "_seconds.csv", "_laps.csv", "_info.csv", ".csv"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    # A renamed session is S0007_<label>_<stream>.csv, so the stream is whatever follows the last '_'.
    found = glob.glob(base + "_*.csv")
    files = {os.path.basename(p)[:-4].rsplit("_", 1)[1]: p for p in found}
    if "imu" not in files:
        sys.exit(f"no {base}_*imu.csv found (raw IMU logging must be ON in Settings while recording)")
    return files["imu"][: -len("_imu.csv")], files


def load_columns(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        names = reader.fieldnames or []
    cols = {}
    for name in names:
        try:
            cols[name] = np.array([float(r[name]) if r[name] not in ("", None) else np.nan for r in rows])
        except ValueError:
            cols[name] = [r[name] for r in rows]
    return cols


def load_info(path):
    if not path:
        return {}
    with open(path, newline="") as f:
        return {r["key"]: r["value"] for r in csv.DictReader(f)}


def expected_strokes(t, surge):
    """How many strokes the surge's own periodicity implies, with no threshold involved.

    Each 12 s window that repeats every 1.4-4 s (15-43 spm) contributes one hop's worth of
    strokes at that period. Windows without a clear rhythm, or that are nearly still, add nothing.
    """
    if len(t) < 200:
        return 0.0
    dt = float(np.median(np.diff(t)))
    if dt <= 0:
        return 0.0
    fs = 1.0 / dt
    window = int(round(12 * fs))
    hop = window // 2
    lo, hi = int(1.4 * fs), int(4.0 * fs)
    if window < 50 or hop < 1 or hi >= window or hi <= lo + 1:
        return 0.0
    total = 0.0
    for a in range(0, len(surge) - window, hop):
        seg = surge[a:a + window]
        if float(np.std(seg)) < 0.03:
            continue
        seg = seg - np.mean(seg)
        ac = np.correlate(seg, seg, "full")[window - 1:]
        ac = ac / ac[0]
        k = lo + int(np.argmax(ac[lo:hi]))
        if ac[k] > 0.25:
            total += hop / k
    return total


def split_jitter(split, interval):
    """Median and 90th percentile of the stroke-to-stroke change, in s/500m.

    Pauses and strokes with no split are left out. Returns None when there are too few.
    """
    if split is None or len(split) < 5:
        return None
    change = np.abs(np.diff(split))
    good = (split[1:] > 0) & (split[:-1] > 0)
    if interval is not None and len(interval) == len(split):
        kept = (interval > 0) & (interval <= STROKE_TIMEOUT_S)
        good &= kept[1:] & kept[:-1]
    if int(good.sum()) < 4:
        return None
    return float(np.median(change[good])), float(np.percentile(change[good], 90)), int(good.sum())


def find_suspects(catch_t):
    """Strokes whose interval is far from the median of their neighbours."""
    iv = np.diff(catch_t)
    out = []
    for i, x in enumerate(iv):
        if x > STROKE_TIMEOUT_S:
            continue
        lo, hi = max(0, i - NEIGHBOURS), min(len(iv), i + NEIGHBOURS + 1)
        near = [iv[j] for j in range(lo, hi) if j != i and iv[j] <= STROKE_TIMEOUT_S]
        if len(near) < 3:
            continue
        ref = statistics.median(near)
        if x < SHORT_FACTOR * ref:
            out.append((catch_t[i + 1], f"short interval {x:.2f} s vs usual {ref:.2f} s: double count?"))
        elif x > LONG_FACTOR * ref:
            out.append((catch_t[i + 1], f"long interval {x:.2f} s vs usual {ref:.2f} s: missed catch?"))
    return out


def plot_overview(path, t, gps, catch_t, suspects, replay_t):
    fig, axes = plt.subplots(3, 1, figsize=(16, 9), sharex=True)
    iv = np.diff(catch_t)
    ok = iv <= STROKE_TIMEOUT_S
    axes[0].plot(catch_t[1:][ok], 60 / iv[ok], ".", ms=4, label="device catches")
    if replay_t is not None and len(replay_t) > 1:
        riv = np.diff(replay_t)
        rok = riv <= STROKE_TIMEOUT_S
        axes[0].plot(replay_t[1:][rok], 60 / riv[rok], "x", ms=4, color="orange", alpha=0.7, label="replay")
    axes[0].set_ylabel("rate (spm)")
    axes[0].set_ylim(10, 45)
    axes[0].legend(loc="upper right")
    axes[1].plot(catch_t[1:][ok], iv[ok], ".", ms=4)
    for st, _ in suspects:
        axes[1].axvline(st, color="red", alpha=0.3)
    axes[1].set_ylabel("interval (s)\nred = suspect")
    g = np.where(gps >= 0, gps, np.nan)
    axes[2].plot(t, g, lw=0.8)
    axes[2].set_ylabel("GPS speed (m/s)")
    axes[2].set_xlabel("session time (s)")
    for ax in axes:
        ax.grid(alpha=0.3)
    fig.suptitle("Session overview")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def plot_strokes(path, t, surge, surge_lin, catch_t):
    dt = np.median(np.diff(t))
    pre, post = int(0.5 / dt), int(2.5 / dt)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(16, 6))
    idx = np.searchsorted(t, catch_t)
    cmap = plt.get_cmap("viridis")
    iv = np.diff(catch_t)
    shown = 0
    for k in range(1, len(idx)):
        i = idx[k]
        if i - pre < 0 or i + post >= len(t) or iv[k - 1] > STROKE_TIMEOUT_S:
            continue
        rate = 60 / iv[k - 1]
        a1.plot((np.arange(-pre, post) * dt), surge[i - pre:i + post], color=cmap((rate - 16) / 20), lw=0.4,
                alpha=0.35)
        shown += 1
    a1.axvline(0, color="g")
    a1.set_title(f"Band-passed surge around each catch ({shown} strokes, colour = rate 16..36 spm)")
    a1.set_xlabel("time from catch (s)")
    a1.set_ylabel("surge (g)")
    a1.grid(alpha=0.3)

    phase = np.linspace(0, 1, 200)
    sig = surge_lin if surge_lin is not None else surge
    for lo, hi in RATE_BANDS:
        curves = []
        for k in range(len(idx) - 1):
            if iv[k] > STROKE_TIMEOUT_S or not lo <= 60 / iv[k] < hi:
                continue
            seg_t = t[idx[k]:idx[k + 1]]
            if len(seg_t) < 20:
                continue
            curves.append(np.interp(phase, (seg_t - seg_t[0]) / (seg_t[-1] - seg_t[0]), sig[idx[k]:idx[k + 1]]))
        if len(curves) >= 3:
            a2.plot(phase, np.mean(curves, axis=0), lw=2, label=f"{lo}-{hi} spm ({len(curves)})")
    a2.set_title("Average stroke shape per rate band (" + ("unfiltered" if surge_lin is not None else "band-passed")
                 + " surge, catch to catch)")
    a2.set_xlabel("fraction of the stroke cycle")
    a2.set_ylabel("surge (g)")
    a2.legend()
    a2.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def plot_window(path, title, center, half, d, catch_t, finish_t, replay_t, highlight=True):
    t = d["t"]
    sel = (t >= center - half) & (t <= center + half)
    if sel.sum() < 10:
        return False
    fig, ax = plt.subplots(figsize=(16, 6))
    ax.plot(t[sel], d["surge"][sel], lw=1.8, label="surge (detector input, g)")
    ax.plot(t[sel], d["threshold"][sel], "r--", lw=1, label="catch threshold")
    if d["surge_lin"] is not None:
        ax.plot(t[sel], d["surge_lin"][sel], lw=0.7, alpha=0.6, label="surge unfiltered")
    if d["vert"] is not None:
        ax.plot(t[sel], d["vert"][sel] - 0.45, lw=0.7, alpha=0.6, label="vertical (offset -0.45)")
    for c in catch_t[(catch_t >= center - half) & (catch_t <= center + half)]:
        ax.axvline(c, color="g", lw=1.2)
    for f in finish_t[(finish_t >= center - half) & (finish_t <= center + half)]:
        ax.axvline(f, color="c", lw=1, ymin=0.4, ymax=0.6)
    if highlight:
        ax.axvline(center, color="red", lw=3, alpha=0.3)
    if replay_t is not None:
        rt = replay_t[(replay_t >= center - half) & (replay_t <= center + half)]
        ax.plot(rt, np.full(len(rt), -0.55), "^", color="orange", ms=9, label="replay catch")
    ax.set_xlim(center - half, center + half)
    ax.set_ylim(-0.65, 0.65)
    ax.set_xlabel("session time (s)   green = device catch, cyan = finish"
                  + (", red band = the suspect stroke" if highlight else ""))
    ax.set_title(title)
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", help="e.g. recordings/S0007 or recordings/S0007_imu.csv")
    ap.add_argument("--out", help="output folder (default: <session>_analysis)")
    ap.add_argument("--window", type=float, default=16, help="seconds shown in each close-up (default 16)")
    ap.add_argument("--replay", action="store_true", help="also run the offline detector from replay.py")
    ap.add_argument("--k", type=float, help="replay threshold factor (default: the session's setting)")
    ap.add_argument("--floor", type=float, help="replay threshold floor in g (default: the session's)")
    ap.add_argument("--minima", action="store_true", help="replay with the minima threshold")
    args = ap.parse_args()

    base, files = session_files(args.session)
    out = args.out or base + "_analysis"
    os.makedirs(out, exist_ok=True)
    info = load_info(files.get("info"))
    imu = load_columns(files["imu"])
    t_abs = imu["t_s"]
    t0 = float(info.get("start_t_s", t_abs[0]))
    t = t_abs - t0
    d = {
        "t": t,
        "surge": imu["surge_g"],
        "threshold": imu["threshold_g"],
        "surge_lin": imu.get("surge_lin_g"),
        "vert": imu.get("vert_g"),
    }
    # The IMU file flags the sample where a catch was confirmed; the strokes file holds the catch time itself.
    catch_t = t[imu["catch"] > 0]
    stroke_cols = None
    if "strokes" in files:
        stroke_cols = load_columns(files["strokes"])
        if "t_s" in stroke_cols and len(stroke_cols["t_s"]):
            catch_t = stroke_cols["t_s"] - t0
    finish_t = t[imu["finish"] > 0] if "finish" in imu else np.array([])
    gps = imu.get("gps_mps", np.full(len(t), -1.0))

    replay_t = None
    if args.replay:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import replay

        k = args.k if args.k is not None else float(info.get("threshold_k", 1.8))
        floor = args.floor if args.floor is not None else float(info.get("threshold_floor_g", 0.12))
        minima = args.minima or info.get("minima_threshold") == "1"
        rows = list(zip(t_abs, imu["ax_g"], imu["ay_g"], imu["az_g"], imu["catch"].astype(int), gps))
        events, _, _ = replay.run(rows, k, floor, minima)
        replay_t = np.array([e["t"] for e in events]) - t0
        replay_label = f"K {k:.2f} floor {floor:.2f} minima {int(minima)}"

    dts = np.diff(t)
    gaps = int((dts > 0.025).sum())
    suspects = find_suspects(catch_t)
    iv = np.diff(catch_t)
    rates = 60 / iv[iv <= STROKE_TIMEOUT_S] if len(iv) else np.array([])

    lines = [f"Session {os.path.basename(base)}"]
    if info:
        lines.append("Settings: " + ", ".join(f"{k}={v}" for k, v in info.items()
                                              if k in ("build", "threshold_k", "threshold_floor_g",
                                                       "minima_threshold", "start_utc")))
    lines.append(f"Duration {t[-1] - t[0]:.0f} s, {len(t)} samples, {len(t) / max(t[-1] - t[0], 1):.1f} Hz, "
                 f"{gaps} gaps over 25 ms")
    if len(rates):
        lines.append(f"Device strokes {len(catch_t)}, rate median {np.median(rates):.1f} spm "
                     f"(range {rates.min():.1f}-{rates.max():.1f})")
    else:
        lines.append(f"Device strokes {len(catch_t)}")
    expected = expected_strokes(t, d["surge"])
    if expected >= 1:
        lines.append(f"Expected strokes from surge periodicity: {expected:.0f} "
                     f"(device found {len(catch_t)}, {100.0 * len(catch_t) / expected:.0f}%)")
    if stroke_cols is not None:
        interval = stroke_cols.get("interval_s")
        for column, label in (("split_s", "one stroke"), ("split3_s", "3-stroke")):
            jitter = split_jitter(stroke_cols.get(column), interval)
            if jitter:
                med, p90, n = jitter
                lines.append(f"Split jitter, {label}: median {med:.1f} s/500m, 90th {p90:.1f} ({n} strokes)")
    if replay_t is not None:
        matched = sum(1 for c in catch_t if np.any(np.abs(replay_t - c) < 0.3))
        lines.append(f"Replay ({replay_label}): {len(replay_t)} strokes, {matched} of the device's "
                     f"{len(catch_t)} matched within 0.3 s")
    lines.append(f"Suspicious strokes: {len(suspects)}")
    for st, why in suspects:
        lines.append(f"  {st:8.1f} s  {why}")
    summary = "\n".join(lines)
    print(summary)
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write(summary + "\n")

    plot_overview(os.path.join(out, "overview.png"), t, gps, catch_t, suspects, replay_t)
    if len(catch_t) > 3:
        plot_strokes(os.path.join(out, "strokes.png"), t, d["surge"], d["surge_lin"], catch_t)
    page = 0
    for start in np.arange(max(t[0], 0), t[-1], TIMELINE_PAGE_S):
        page += 1
        plot_window(os.path.join(out, f"timeline_{page:03d}_{start:.0f}s.png"),
                    f"Timeline {start:.0f}-{start + TIMELINE_PAGE_S:.0f} s", start + TIMELINE_PAGE_S / 2,
                    TIMELINE_PAGE_S / 2, d, catch_t, finish_t, replay_t, highlight=False)
    half = args.window / 2
    for i, (st, why) in enumerate(suspects[:MAX_SUSPECT_PAGES], 1):
        plot_window(os.path.join(out, f"suspect_{i:02d}_{st:.0f}s.png"), f"Suspect {i} at {st:.1f} s: {why}",
                    st, half, d, catch_t, finish_t, replay_t)
    print(f"plots written to {out}")


if __name__ == "__main__":
    main()
