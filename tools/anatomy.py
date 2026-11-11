"""Split recorded strokes into catch, drive, finish, run and approach.

Mirrors src/stroke_anatomy.cpp so the phase boundaries can be tuned from a session
before they are copied into the firmware. The thresholds below are the same ones.

Usage:
  python tools/anatomy.py --check
  python tools/anatomy.py recordings/S0021

--check builds the simulator's stroke, runs the same segmentation and reports the
phases. A session writes <session>_analysis/anatomy.png (needs matplotlib).
"""
import math
import os
import sys

import numpy as np

# Keep in step with src/stroke_anatomy.cpp.
PREROLL_S = 0.25
PREROLL_N = 25
DRIVE_BINS = 32
REC_BINS = 64
BINS = DRIVE_BINS + REC_BINS
LOWPASS_HZ = 8.0
FS = 100.0
G = 9.80665
DT = 0.01

MIN_CATCH_DEPTH_G = 0.05
HALF_DEPTH = 0.50
DIP_START_FRAC = 0.25
NOTCH_FRAC = 0.40
NOTCH_GAP_S = 0.030
FINISH_WINDOW_S = 0.35
SETTLE_G = 0.03
APPROACH_MARGIN_G = 0.025


class Biquad:
    """RBJ low-pass, transposed direct form II, matching dsp.h."""

    def __init__(self, fc, fs, q=0.70710678):
        w0 = 2 * math.pi * fc / fs
        c = math.cos(w0)
        alpha = math.sin(w0) / (2 * q)
        a0 = 1 + alpha
        self.b0 = (1 - c) * 0.5 / a0
        self.b1 = (1 - c) / a0
        self.b2 = self.b0
        self.a1 = -2 * c / a0
        self.a2 = (1 - alpha) / a0
        self.z1 = self.z2 = 0.0

    def process(self, x):
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


def lowpass(x):
    b = Biquad(LOWPASS_HZ, FS)
    return np.array([b.process(float(v)) for v in x])


def preroll_time(i):
    return -PREROLL_S + (i + 0.5) * (PREROLL_S / PREROLL_N)


def bin_time(i, drive_s, rec_s):
    if i < DRIVE_BINS:
        return (i + 0.5) / DRIVE_BINS * drive_s
    return drive_s + (i - DRIVE_BINS + 0.5) / REC_BINS * rec_s


def sample_at(kt, ka, time):
    if time <= kt[0]:
        return ka[0]
    if time >= kt[-1]:
        return ka[-1]
    i = int(np.searchsorted(kt, time))
    i = min(max(i, 1), len(kt) - 1)
    span = kt[i] - kt[i - 1]
    u = (time - kt[i - 1]) / span if span > 1e-6 else 0
    return ka[i - 1] + u * (ka[i] - ka[i - 1])


def cut_cycle(t, surge, prev_catch, finish, catch):
    """Bin one stroke the way Recorder::cut does."""
    drive = finish - prev_catch
    rec = catch - finish
    if drive < 0.15 or rec < 0.15:
        return None
    preroll = np.zeros(PREROLL_N)
    accel = np.zeros(BINS)
    n_p = np.zeros(PREROLL_N, int)
    n_a = np.zeros(BINS, int)
    sel = (t >= prev_catch - PREROLL_S) & (t <= catch)
    for ts, a in zip(t[sel], surge[sel]):
        if ts < prev_catch:
            b = int((ts - (prev_catch - PREROLL_S)) / 0.01)
            if 0 <= b < PREROLL_N:
                preroll[b] += a
                n_p[b] += 1
        elif ts < finish:
            b = int((ts - prev_catch) / drive * DRIVE_BINS)
            b = min(max(b, 0), DRIVE_BINS - 1)
            accel[b] += a
            n_a[b] += 1
        else:
            b = DRIVE_BINS + int((ts - finish) / rec * REC_BINS)
            b = min(max(b, DRIVE_BINS), BINS - 1)
            accel[b] += a
            n_a[b] += 1

    def fill(dst, counts):
        known = counts > 0
        if not np.any(known):
            return False
        dst[known] /= counts[known]
        idx = np.where(known)[0]
        dst[: idx[0]] = dst[idx[0]]
        dst[idx[-1] + 1 :] = dst[idx[-1]]
        gaps = np.where(~known)[0]
        gaps = gaps[(gaps > idx[0]) & (gaps < idx[-1])]
        for g in gaps:
            if counts[g] > 0:
                continue
            j = g
            while j <= idx[-1] and counts[j] == 0:
                j += 1
            span = j - (g - 1)
            for k in range(g, j):
                dst[k] = dst[g - 1] + (dst[j] - dst[g - 1]) * (k - (g - 1)) / span
                counts[k] = 1
        return True

    if not fill(preroll, n_p) or not fill(accel, n_a):
        return None
    return preroll, accel, drive, rec


def analyse(preroll, accel, drive_s, rec_s):
    interval = drive_s + rec_s
    if drive_s < 0.15 or rec_s < 0.15:
        return None
    n = int(round((PREROLL_S + interval) / DT))
    kt = np.array([preroll_time(i) for i in range(PREROLL_N)] + [bin_time(i, drive_s, rec_s) for i in range(BINS)])
    ka = np.concatenate([preroll, accel])
    gt = -PREROLL_S + np.arange(n) * DT
    ga = np.array([sample_at(kt, ka, float(ts)) for ts in gt])
    gv = np.zeros(n)
    gv[1:] = np.cumsum(0.5 * (ga[1:] + ga[:-1]) * G * DT)

    def vel(time):
        return float(np.interp(time, gt, gv))

    i_lo = int(np.searchsorted(gt, -PREROLL_S))
    i_hi = min(n - 1, int(np.searchsorted(gt, 0.20)))
    imin = i_lo + int(np.argmin(ga[i_lo : i_hi + 1]))
    depth = -ga[imin]
    if depth < MIN_CATCH_DEPTH_G:
        return None

    def cross_left(frm, level):
        j = frm
        while j > 0 and ga[j] < level:
            j -= 1
        if j + 1 >= n or ga[j] < level or ga[j + 1] >= level:
            return gt[j]
        span = ga[j + 1] - ga[j]
        u = (level - ga[j]) / span if span else 0
        return gt[j] + u * (gt[j + 1] - gt[j])

    def cross_right(frm, level):
        j = frm
        while j < n - 1 and ga[j] < level:
            j += 1
        if j == 0 or ga[j] < level or ga[j - 1] >= level:
            return gt[j]
        span = ga[j] - ga[j - 1]
        u = (level - ga[j - 1]) / span if span else 0
        return gt[j - 1] + u * (gt[j] - gt[j - 1])

    def cross_down(frm, level):
        for i in range(frm, n - 1):
            if ga[i] >= level and ga[i + 1] < level:
                span = ga[i + 1] - ga[i]
                u = (level - ga[i]) / span if span else 0
                return gt[i] + u * (gt[i + 1] - gt[i])
        return -1

    half = -HALF_DEPTH * depth
    width_l = cross_left(imin, half)
    width_r = cross_right(imin, half)
    dip_start = cross_left(imin, -DIP_START_FRAC * depth)
    dip_end = cross_right(imin, 0)
    if dip_end > 0.30:
        dip_end = width_r

    notches = 0
    last = -1e9
    i_start = int(np.searchsorted(gt, dip_start))
    i_end = int(np.searchsorted(gt, dip_end))
    for i in range(i_start + 1, min(i_end, n - 1)):
        if ga[i] <= ga[i - 1] and ga[i] < ga[i + 1] and ga[i] < -NOTCH_FRAC * depth and gt[i] - last >= NOTCH_GAP_S:
            notches += 1
            last = gt[i]

    i_drive = int(np.searchsorted(gt, max(dip_end, 0)))
    i_finish = int(np.searchsorted(gt, drive_s))
    i_peak = i_drive + int(np.argmax(ga[i_drive : i_finish + 1]))
    peak = ga[i_peak]
    peak_pct = min(1, max(0, gt[i_peak] / drive_s))
    gain = max(0, vel(drive_s) - vel(dip_end))

    mid = 0.5 * (max(dip_end, 0) + drive_s)
    i_mid = int(np.searchsorted(gt, mid))
    i_p1 = i_drive + int(np.argmax(ga[i_drive:i_mid])) if i_mid > i_drive else i_drive
    i_p2 = i_mid + int(np.argmax(ga[i_mid : i_finish + 1])) if i_finish >= i_mid else i_mid
    sag = 0
    if ga[i_p1] > 0.45 * peak and ga[i_p2] > 0.45 * peak and i_p2 > i_p1:
        valley = np.min(ga[i_p1 : i_p2 + 1])
        if min(ga[i_p1], ga[i_p2]) - valley > 0.03:
            sag = min(ga[i_p1], ga[i_p2]) - valley

    t80 = cross_down(i_peak, 0.8 * peak) if peak > 0 else -1
    t20 = cross_down(int(np.searchsorted(gt, t80)), 0.2 * peak) if t80 > 0 else -1
    fall = t20 - t80 if t20 > t80 > 0 else 0

    run_from, run_to = drive_s + 0.20, interval - 0.40
    if run_to - run_from < 0.15:
        run_from, run_to = drive_s + 0.10, interval - 0.10
    run_base = 0
    if run_to > run_from:
        a, c = int(np.searchsorted(gt, run_from)), int(np.searchsorted(gt, run_to))
        if c >= a:
            run_base = float(np.median(ga[a : c + 1]))

    i_rel0 = int(np.searchsorted(gt, drive_s))
    i_rel1 = int(np.searchsorted(gt, min(interval, drive_s + FINISH_WINDOW_S)))
    i_rel = i_rel0 + int(np.argmin(ga[i_rel0 : i_rel1 + 1]))
    release = max(0, run_base - ga[i_rel])
    finish_end = gt[min(i_rel1, n - 1)]
    for i in range(i_rel, min(i_rel1, n - 1) + 1):
        if abs(ga[i] - run_base) < SETTLE_G:
            finish_end = gt[i]
            break

    thr = run_base - APPROACH_MARGIN_G
    i_back = int(np.searchsorted(gt, min(interval, drive_s + 0.05)))
    i_glide = i_back
    for i in range(n - 1, i_back - 1, -1):
        if ga[i] > thr:
            i_glide = i
            break
    approach = gt[i_glide]
    lead = max(0, interval - approach)
    approach_loss = max(0, vel(approach) - vel(interval))

    i_run0 = int(np.searchsorted(gt, finish_end))
    i_run1 = int(np.searchsorted(gt, approach))
    run_mean = float(np.mean(ga[i_run0 : i_run1 + 1])) if i_run1 >= i_run0 else run_base
    run_mg = max(0, -run_mean * 1000)
    i_fast = int(np.searchsorted(gt, 0))
    i_fast = i_fast + int(np.argmax(gv[i_fast:]))
    speed_peak = max(0, gt[i_fast])

    if dip_end < 0:
        dip_end = 0
    if dip_end > drive_s:
        dip_end = drive_s
    if finish_end < drive_s:
        finish_end = drive_s
    if approach < finish_end:
        approach = finish_end

    return {
        "catch0": dip_start,
        "catch1": dip_end,
        "finish": drive_s,
        "finish_end": finish_end,
        "approach": approach,
        "interval": interval,
        "depth": depth,
        "width": max(0, width_r - width_l),
        "loss": max(0, vel(dip_start) - vel(dip_end)),
        "notches": notches if notches else 1,
        "peak": peak,
        "gain": gain,
        "peak_pct": peak_pct,
        "sag": sag,
        "release": release,
        "fall": fall,
        "run_mg": run_mg,
        "speed_peak": speed_peak,
        "lead": lead,
        "approach_loss": approach_loss,
        "curve_t": gt,
        "curve_a": ga,
    }


def _rate(t):
    return 26 - 6 * math.cos(2 * math.pi * t / 120)


def _phase(t):
    w = 2 * math.pi / 120
    cycles = (26 * t - 6 * math.sin(w * t) / w) / 60
    return cycles % 1


def _gauss(x, mu, sigma):
    d = (x - mu) / sigma
    return math.exp(-0.5 * d * d)


def simulator_surge(t):
    """The hull surge from src/sim.cpp, without the mounting tilt."""
    period = 60 / _rate(t)
    tau = _phase(t) * period
    tw = tau - period if tau > period * 0.5 else tau
    catch_sigma = 0.055 - 0.018 * math.sin(t / 23)
    drive_gain = 1 + 0.22 * math.sin(t / 17)
    approach_lead = 0.24 - 0.10 * math.sin(t / 29)
    release_amp = 0.015 + 0.030 * (1 - math.sin(t / 31))
    notch = max(0, math.sin(t / 13))
    surge = -0.55 * _gauss(tw, 0, catch_sigma)
    surge += -0.30 * notch * _gauss(tw, 0.055, 0.022)
    surge += 0.22 * drive_gain * _gauss(tau, 0.26, 0.09)
    surge += 0.16 * drive_gain * _gauss(tau, 0.50, 0.11)
    surge += -release_amp * _gauss(tau, 0.72, 0.035)
    surge += -0.028
    if tau > period - approach_lead:
        surge += -0.08 * (tau - (period - approach_lead)) / approach_lead
    return surge


def find_finish(t, surge, catch, nxt):
    """Where the drive falls through 35% of its peak, as the detector does."""
    m = np.where((t >= catch) & (t < min(catch + 0.9, nxt)))[0]
    if len(m) < 5:
        return None
    peak_i = m[int(np.argmax(surge[m]))]
    peak = surge[peak_i]
    if peak < 0.05:
        return None
    level = 0.35 * peak
    for i in range(peak_i, m[-1]):
        if surge[i] >= level and surge[i + 1] < level:
            span = surge[i + 1] - surge[i]
            u = (level - surge[i]) / span if span else 0
            return t[i] + u * (t[i + 1] - t[i])
    return None


def self_check():
    t = np.arange(0, 160, DT)
    raw = np.array([simulator_surge(float(ts)) for ts in t])
    surge = lowpass(raw)
    wraps = np.where(np.diff(_phase_array(t)) < -0.5)[0]
    catches = t[wraps + 1]
    sharp, wide, early, late = [], [], [], []
    for prev, catch in zip(catches[:-1], catches[1:]):
        if not (1.2 < catch - prev < 4.0) or prev < 5:
            continue
        finish = find_finish(t, surge, prev, catch)
        if finish is None:
            continue
        cyc = cut_cycle(t, surge, prev, finish, catch)
        if cyc is None:
            continue
        feat = analyse(*cyc)
        if feat is None:
            continue
        order = [feat["catch0"], 0, feat["catch1"], feat["finish"], feat["finish_end"], feat["approach"], feat["interval"]]
        if any(order[i] > order[i + 1] + 1e-6 for i in range(len(order) - 1)):
            print("phases out of order:", order)
            return 1
        bucket = sharp if math.sin(prev / 23) > 0.5 else wide if math.sin(prev / 23) < -0.5 else None
        if bucket is not None:
            bucket.append(feat)
        (late if math.sin(prev / 29) > 0.5 else early if math.sin(prev / 29) < -0.5 else []).append(feat["lead"])
    if len(sharp) < 3 or len(wide) < 3 or len(early) < 3 or len(late) < 3:
        print("not enough strokes: sharp %d wide %d early %d late %d" % (len(sharp), len(wide), len(early), len(late)))
        return 1
    sw, ww = np.mean([f["width"] for f in sharp]), np.mean([f["width"] for f in wide])
    sl, wl = np.mean(late), np.mean(early)
    print("sharp catch %.0f ms (%d)   wide catch %.0f ms (%d)" % (sw * 1000, len(sharp), ww * 1000, len(wide)))
    print("late dip %.2f s (%d)   early dip %.2f s (%d)" % (sl, len(late), wl, len(early)))
    f0 = sharp[0]
    print("sample: depth %.2f g  notches %.0f  peak %.0f%%  gain %+.2f m/s  fall %.0f ms  release %.3f g  run %.0f mg" % (
        f0["depth"], f0["notches"], f0["peak_pct"] * 100, f0["gain"], f0["fall"] * 1000, f0["release"], f0["run_mg"]))
    if not (ww > sw * 1.15 and ww - sw > 0.015):
        print("wide catches were not wider than sharp ones")
        return 1
    if not (0.04 < sw < 0.25 and 0.04 < ww < 0.30):
        print("catch width outside a plausible range")
        return 1
    if sl <= 0.05 or wl < sl + 0.05:
        print("approach lead did not grow on the early-dip strokes")
        return 1
    print("check ok")
    return 0


def _phase_array(t):
    w = 2 * math.pi / 120
    cycles = (26 * t - 6 * np.sin(w * t) / w) / 60
    return cycles % 1


def session_paths(arg):
    base = arg
    for suffix in ("_imu.csv", "_strokes.csv", "_analysis", ".csv"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    return base, base + "_imu.csv", base + "_strokes.csv"


def load_strokes(imu_path, strokes_path):
    imu = np.genfromtxt(imu_path, delimiter=",", names=True)
    t = imu["t_s"]
    surge = lowpass(imu["surge_lin_g"])
    if os.path.exists(strokes_path):
        st = np.genfromtxt(strokes_path, delimiter=",", names=True)
        rows = []
        for row in st:
            if row["interval_s"] <= 0 or row["drive_s"] <= 0:
                continue
            catch = float(row["t_s"])
            prev = catch - float(row["interval_s"])
            finish = prev + float(row["drive_s"])
            rows.append((prev, finish, catch, float(row["rate_spm"])))
        return t, surge, rows
    catch_i = np.where(imu["catch"] > 0)[0]
    finish_i = np.where(imu["finish"] > 0)[0]
    rows = []
    for a, b in zip(catch_i[:-1], catch_i[1:]):
        finish = t[finish_i[(t[finish_i] > t[a]) & (t[finish_i] < t[b])]]
        if len(finish) == 0:
            continue
        rows.append((float(t[a]), float(finish[0]), float(t[b]), 60.0 / (t[b] - t[a])))
    return t, surge, rows


def plot_session(base, t, surge, rows):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    bands = {}
    for prev, finish, catch, rate in rows:
        cyc = cut_cycle(t, surge, prev, finish, catch)
        if cyc is None:
            continue
        feat = analyse(*cyc)
        if feat is None:
            continue
        key = int(round(rate / 4.0) * 4)
        bands.setdefault(key, []).append((cyc, feat))
    if not bands:
        print("no strokes could be split")
        return 1
    keys = sorted(bands)
    fig, axes = plt.subplots(len(keys), 1, figsize=(10, 3.2 * len(keys)), squeeze=False)
    for ax, key in zip(axes[:, 0], keys):
        items = bands[key]
        acc = np.mean([np.concatenate(c[0][:2]) for c in items], axis=0)
        drive = np.mean([c[0][2] for c in items])
        rec = np.mean([c[0][3] for c in items])
        feat = analyse(acc[:PREROLL_N], acc[PREROLL_N:], drive, rec)
        tt = [preroll_time(i) for i in range(PREROLL_N)] + [bin_time(i, drive, rec) for i in range(BINS)]
        ax.plot(tt, acc, color="C0", lw=1.8)
        if feat:
            spans = [feat["catch0"], feat["catch1"], feat["finish"], feat["finish_end"], feat["approach"], feat["interval"]]
            colors = ["#f4c7c3", "#c6dafc", "#d0f0f0", "#e6e6e6", "#fce8c3"]
            names = ["catch", "drive", "finish", "run", "approach"]
            for i in range(5):
                ax.axvspan(spans[i], spans[i + 1], color=colors[i], alpha=0.7, zorder=0)
                ax.text((spans[i] + spans[i + 1]) / 2, ax.get_ylim()[1], names[i], ha="center", va="bottom", fontsize=8)
            ax.set_title(
                "%d spm  n=%d  catch %.0f ms %.2f g  drive %+.2f m/s peak %.0f%%  "
                "fall %.0f ms  approach %.2f s" % (
                    key, len(items), feat["width"] * 1000, feat["depth"], feat["gain"], feat["peak_pct"] * 100,
                    feat["fall"] * 1000, feat["lead"]))
        ax.set_ylabel("surge g")
        ax.axhline(0, color="k", lw=0.4)
    axes[-1, 0].set_xlabel("seconds from catch")
    fig.tight_layout()
    out_dir = base + "_analysis"
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "anatomy.png")
    fig.savefig(path, dpi=110)
    print(path)
    return 0


def main():
    if "--check" in sys.argv:
        return self_check()
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    base, imu_path, strokes_path = session_paths(sys.argv[1])
    if not os.path.exists(imu_path):
        print("no IMU file at", imu_path)
        return 1
    t, surge, rows = load_strokes(imu_path, strokes_path)
    print("%d strokes" % len(rows))
    return plot_session(base, t, surge, rows)


if __name__ == "__main__":
    sys.exit(main())
