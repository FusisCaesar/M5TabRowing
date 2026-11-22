"""How late is the GPS speed in a recording?

The logged GPS speed is stamped when the sentence is read, not when the boat did that
speed, so it trails the accelerometer by an amount that depends on the receiver, its
settings and the baud rate. The firmware and the viewer assume one fixed delay
(GPS_SPEED_DELAY_S); this measures it from the recording itself.

How: in blocks of a minute the logged surge acceleration is integrated to speed (in the
frequency domain over the stroke band, so the integration adds no delay of its own) and
cross-correlated with the GPS speed. The lag of the peak is how late the GPS is.

Usage:
  python tools/gps_lag.py                       # every *_imu.csv in recordings/
  python tools/gps_lag.py recordings/S0017_imu.csv ...
  python tools/gps_lag.py S0017_imu.csv --blocks    # also one line per block
  python tools/gps_lag.py --selftest            # check the method on a known delay
"""
import argparse
import glob
import os
import sys

import numpy as np

FS = 100.0
G = 9.80665
BAND_HZ = (0.25, 1.6)      # stroke rate and its first harmonics
BLOCK_S = 60.0
STEP_S = 30.0
# Searched lags. The GPS cannot be early, and the range is kept shorter than half a stroke: a logged
# surge with the wrong sign matches the GPS just as well half a stroke away, which would read as a
# lag of -0.5 s or +1.6 s instead of the real +0.5 s.
LAG_RANGE_S = (-0.1, 0.9)
MIN_SPEED_MPS = 1.5
MAX_FIX_GAP_S = 0.6        # a block with a longer hole in the GPS is skipped
MIN_CORR = 0.4             # blocks that match worse than this say nothing about the lag
CONFIGURED_DELAY_S = 0.5   # GPS_SPEED_DELAY_S in src/config.h


def load(path):
    """t, surge acceleration (m/s2), GPS speed (m/s, -1 or 0 when there is no fix)."""
    with open(path, newline="") as f:
        header = f.readline().strip().split(",")
    cols = [header.index(k) for k in ("t_s", "surge_lin_g", "gps_mps")]
    data = np.loadtxt(path, delimiter=",", skiprows=1, usecols=cols, ndmin=2)
    return data[:, 0], data[:, 1] * G, data[:, 2]


def read_info(path):
    info = {}
    info_path = path.replace("_imu.csv", "_info.csv")
    if os.path.exists(info_path):
        with open(info_path) as f:
            next(f, None)
            for line in f:
                k, _, v = line.strip().partition(",")
                info[k] = v
    return info


def regular(t, accel, gps):
    """Everything on a 100 Hz grid. GPS is held between fixes in the log, so it is rebuilt from
    the instants it changed, joined by straight lines. Returns None when there are no fixes."""
    grid = np.arange(t[0], t[-1], 1.0 / FS)
    a = np.interp(grid, t, accel)
    ok = gps > 0
    change = ok & (np.diff(gps, prepend=-1.0) != 0)
    ft, fv = t[change], gps[change]
    if len(ft) < 20:
        return grid, a, None, None
    g = np.interp(grid, ft, fv)
    # Where the fixes stop for a while the straight line is a guess: remember where it is.
    gap = np.zeros(len(grid))
    holes = np.flatnonzero(np.diff(ft) > MAX_FIX_GAP_S)
    for i in holes:
        gap[(grid > ft[i]) & (grid < ft[i + 1])] = 1
    gap[grid < ft[0]] = 1
    gap[grid > ft[-1]] = 1
    return grid, a, g, gap


def block_lag(a, g):
    """Lag (s) and correlation of the peak, for one block of acceleration and GPS speed.
    Positive: the GPS is late."""
    n = len(a)
    w = np.hanning(n)
    A = np.fft.rfft((a - a.mean()) * w)
    Gs = np.fft.rfft((g - g.mean()) * w)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    band = (f >= BAND_HZ[0]) & (f <= BAND_HZ[1])
    if not band.any():
        return None
    V = np.zeros_like(A)
    V[band] = A[band] / (2j * np.pi * f[band])   # integrating adds no delay of its own
    Gb = np.where(band, Gs, 0)
    norm = np.sqrt(np.sum(np.abs(Gb) ** 2) * np.sum(np.abs(V) ** 2))
    if norm <= 0:
        return None
    xc = np.fft.irfft(Gb * np.conj(V), n) * n / (2 * norm)   # xc[k]: g(t + k) against v(t)
    lo, hi = int(LAG_RANGE_S[0] * FS), int(LAG_RANGE_S[1] * FS)
    lags = np.arange(lo, hi + 1)
    r = xc[lags % n]
    # The device could have a wrong idea of forward in this block: then the match is negative.
    i = int(np.argmax(np.abs(r)))
    if 0 < i < len(r) - 1:
        y0, y1, y2 = np.abs(r[i - 1]), np.abs(r[i]), np.abs(r[i + 1])
        den = y0 - 2 * y1 + y2
        off = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    else:
        off = 0.0
    return (lags[i] + off) / FS, float(r[i])


def analyse(t, accel, gps):
    grid, a, g, gap = regular(t, accel, gps)
    blocks = []
    if g is None:
        return blocks
    size, step = int(BLOCK_S * FS), int(STEP_S * FS)
    for s in range(0, len(grid) - size + 1, step):
        sl = slice(s, s + size)
        if gap[sl].any() or np.median(g[sl]) < MIN_SPEED_MPS or np.min(g[sl]) < 0.5:
            continue
        res = block_lag(a[sl], g[sl])
        if res is None:
            continue
        blocks.append((grid[s] - grid[0], res[0], res[1], float(np.median(g[sl]))))
    return blocks


def summarise(blocks):
    use = [b for b in blocks if abs(b[2]) >= MIN_CORR]
    if not use:
        return None
    lag = np.array([b[1] for b in use])
    corr = np.array([abs(b[2]) for b in use])
    third = max(1, len(use) // 3)
    return {
        "blocks": len(blocks), "used": len(use),
        "median": float(np.median(lag)),
        "q1": float(np.percentile(lag, 25)), "q3": float(np.percentile(lag, 75)),
        "corr": float(np.median(corr)),
        "first": float(np.median(lag[:third])), "last": float(np.median(lag[-third:])),
        "flipped": sum(1 for b in use if b[2] < 0) / len(use),
    }


def selftest():
    """A boat that speeds up and slows down with the strokes, GPS reported late by a known amount."""
    rng = np.random.default_rng(1)
    t = np.arange(0, 300, 1 / FS)
    rate = 0.45 + 0.05 * np.sin(2 * np.pi * t / 120)
    ph = 2 * np.pi * np.cumsum(rate) / FS
    v = 3.5 + 0.4 * np.sin(ph) + 0.12 * np.sin(2 * ph + 0.6)
    accel = np.gradient(v, 1 / FS) + rng.normal(0, 0.6, len(t))
    ok = True
    for delay in (0.0, 0.3, 0.5, 0.8):
        fix_t = np.arange(0, 300, 0.1)
        late = np.interp(fix_t - delay, t, v) + rng.normal(0, 0.03, len(fix_t))
        gps = np.zeros(len(t))
        idx = np.searchsorted(fix_t, t, side="right") - 1
        gps[:] = late[np.maximum(idx, 0)]
        s = summarise(analyse(t, accel, gps))
        print("known delay %.2f s -> measured %.2f s (r %.2f)" % (delay, s["median"], s["corr"]))
        ok = ok and abs(s["median"] - delay) < 0.06
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="*_imu.csv files (default: recordings/*_imu.csv)")
    ap.add_argument("--blocks", action="store_true", help="one line per block as well")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    here = os.path.dirname(os.path.abspath(__file__))
    logs = args.logs or sorted(glob.glob(os.path.join(here, "..", "recordings", "*_imu.csv")))
    if not logs:
        print("no *_imu.csv found")
        return 1
    print("%-26s %6s %6s  %7s  %-13s %6s  %6s %6s  %7s  %s" % (
        "session", "blocks", "used", "lag s", "IQR", "r", "start", "end", "flipped", "logged / assumed"))
    for path in logs:
        name = os.path.basename(path).replace("_imu.csv", "")
        info = read_info(path)
        logged = info.get("gps_speed_delay_s")
        if info.get("build") == "simulation":
            assumed = float(logged) if logged else 0.0
        else:
            assumed = float(logged) if logged else CONFIGURED_DELAY_S
        try:
            blocks = analyse(*load(path))
        except (ValueError, IndexError, OSError) as e:
            print("%-26s skipped: %s" % (name, e))
            continue
        s = summarise(blocks)
        tag = "%s / %.2f" % (logged or "-", assumed)
        if s is None:
            print("%-26s %6d %6d  %s   %s" % (name, len(blocks), 0, "no usable blocks (GPS still, no fix or weak match)", tag))
        else:
            print("%-26s %6d %6d  %7.2f  %5.2f..%-6.2f %6.2f  %6.2f %6.2f  %6.0f%%  %s" % (
                name, s["blocks"], s["used"], s["median"], s["q1"], s["q3"], s["corr"], s["first"], s["last"],
                100 * s["flipped"], tag))
        if args.blocks:
            for at, lag, r, v in blocks:
                print("    at %6.0f s  lag %6.2f s  r %+.2f  speed %.1f m/s" % (at, lag, r, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
