"""Replay a raw IMU log through the stroke detector with different settings.

This is a line-by-line port of src/stroke_detector.cpp, so thresholds can be
tuned offline on recorded outings and then copied into the Settings screen.

Usage:
  python tools/replay.py S0007_imu.csv                 # replay with defaults
  python tools/replay.py S0007_imu.csv --k 2.0 --floor 0.10 --minima
  python tools/replay.py S0007_imu.csv --sweep         # try a grid of K / floor values
  python tools/replay.py --synthetic 120               # self-test on simulated rowing
  python tools/replay.py S0007_imu.csv --plot 60 90    # plot 60-90 s (needs matplotlib)
"""
import argparse
import csv
import math
import random
import statistics

FS = 100.0
DT = 1.0 / FS
GRAVITY_TAU_S = 2.0
AXIS_COV_TAU_S = 8.0
SKEW_TAU_S = 20.0
SKEW_FLIP_THRESHOLD = 0.25
SKEW_LOWPASS_HZ = 6.0
BANDPASS = (0.1, 3.0)
RMS_TAU_S = 5.0
MINIMA_FACTOR = 0.6
MINIMA_COUNT = 5
MIN_STROKE_INTERVAL_S = 1.0
MAX_DIP_DURATION_S = 0.8
FINISH_FRACTION = 0.35
RATE_AVG_STROKES = 3
STROKE_TIMEOUT_S = 6.0
RESCUE_WINDOW_LO = 0.75
RESCUE_ACCEPT_LO = 0.90
RESCUE_WINDOW_HI = 1.35
RESCUE_THRESHOLD_FACTOR = 0.5
RESCUE_THRESHOLD_FLOOR_G = 0.06
RESCUE_MIN_INTERVALS = 2
RESCUE_CHAIN_MAX = 120
WARMUP_S = 3.0
MAX_DRIVE_S = 2.5
STANDARD_GRAVITY = 9.80665
GPS_DIR_MIN_SPEED_MPS = 1.0
GPS_DIR_INTEGRATOR_TAU_S = 1.0
GPS_DIR_HP_TAU_S = 1.0
GPS_DIR_CORR_TAU_S = 15.0
GPS_DIR_MIN_CORR = 0.3
GPS_DIR_MIN_TIME_S = 10.0


def ema_alpha(tau):
    return DT / (tau + DT)


class Biquad:
    def __init__(self, kind, fc, q=0.70710678):
        w0 = 2 * math.pi * fc / FS
        c, alpha = math.cos(w0), math.sin(w0) / (2 * q)
        a0 = 1 + alpha
        if kind == "lp":
            self.b0 = (1 - c) * 0.5 / a0
            self.b1 = (1 - c) / a0
        else:
            self.b0 = (1 + c) * 0.5 / a0
            self.b1 = -(1 + c) / a0
        self.b2 = self.b0
        self.a1 = -2 * c / a0
        self.a2 = (1 - alpha) / a0
        self.z1 = self.z2 = 0.0

    def __call__(self, x):
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def norm(a):
    return math.sqrt(dot(a, a))


class StrokeDetector:
    def __init__(self, k=1.8, floor=0.12, minima=False):
        self.k, self.floor, self.use_minima = k, floor, minima
        self.aG, self.aC = ema_alpha(GRAVITY_TAU_S), ema_alpha(AXIS_COV_TAU_S)
        self.aS, self.aR = ema_alpha(SKEW_TAU_S), ema_alpha(RMS_TAU_S)
        self.bp = [(Biquad("hp", BANDPASS[0]), Biquad("lp", BANDPASS[1])) for _ in range(3)]
        self.skew_lp = Biquad("lp", SKEW_LOWPASS_HZ)
        self.init = False
        self.cov = [0.0] * 6  # xx xy xz yy yz zz
        self.sign = 1
        self.m1 = self.m2 = self.m3 = self.ms = 0.0
        self.skew_n = 0
        self.gps_locked = False
        self.v_int = self.gps_mean = 0.0
        self.cxy = self.cxx2 = self.cyy2 = 0.0
        self.gps_dir_n = 0
        self.in_dip = False
        self.in_rescue = False
        self.rescue_chain = 0
        self.last_catch = -1.0
        self.intervals, self.minima = [], []
        self.seq = 0
        self.phase = None
        self.reset_cycle()

    def reset_cycle(self):
        self.drive_peak = 0.0
        self.finish_t = -1.0
        self.drive_time = 0.0
        self.rec = [0.0] * 6  # n, t, x, tt, tx, xx
        self.v_sum2, self.v_n = 0.0, 0

    def rms(self):
        return math.sqrt(self.ms)

    def skew(self):
        var = self.m2 - self.m1 ** 2
        c3 = self.m3 - 3 * self.m1 * self.m2 + 2 * self.m1 ** 3
        return c3 / var ** 1.5 if var > 1e-8 else 0.0

    def gps_corr(self):
        d = math.sqrt(self.cxx2 * self.cyy2)
        return self.cxy / d if d > 1e-12 else 0.0

    def rate(self, now):
        if self.last_catch < 0 or not self.intervals or now - self.last_catch > STROKE_TIMEOUT_S:
            return 0.0
        return 60.0 * len(self.intervals) / sum(self.intervals)

    def thr_mag(self):
        mag = max(self.k * self.rms(), self.floor)
        if self.use_minima and len(self.minima) >= MINIMA_COUNT:
            mag = max(self.floor, MINIMA_FACTOR * -min(min(self.minima), 0.0))
        return mag

    def rescue_period(self):
        if self.last_catch < 0 or len(self.intervals) < RESCUE_MIN_INTERVALS:
            return None
        period = sum(self.intervals) / len(self.intervals)
        return period if period > 0 else None

    def in_rescue_window(self, t, lo_factor):
        if self.rescue_chain >= RESCUE_CHAIN_MAX:
            return False
        period = self.rescue_period()
        if period is None or t - self.last_catch > STROKE_TIMEOUT_S:
            return False
        lo = self.last_catch + lo_factor * period
        hi = self.last_catch + RESCUE_WINDOW_HI * period
        cap = self.last_catch + STROKE_TIMEOUT_S
        if hi > cap:
            hi = cap
        return lo <= t <= hi

    def accept_catch(self, t, catch_t, dip_min, rescued):
        """Shared tail of a normal and a rescued catch. None when the interval is too short."""
        if self.last_catch >= 0 and catch_t - self.last_catch < MIN_STROKE_INTERVAL_S:
            return None
        interval = 0.0 if self.last_catch < 0 else catch_t - self.last_catch
        if interval > STROKE_TIMEOUT_S:
            interval = 0.0
        self.seq += 1
        event = {"seq": self.seq, "t": catch_t, "interval": interval, "drive": 0.0, "recovery": 0.0,
                 "peak": dip_min, "check": 0.0, "bounce": 0.0, "rescued": rescued}
        if interval > 0:
            if self.finish_t > self.last_catch and self.drive_time > 0:
                event["drive"] = self.drive_time
                event["recovery"] = catch_t - self.finish_t
            n, st, sx, stt, stx, sxx = self.rec
            if n >= 10:
                ctt, ctx, cxx = stt - st * st / n, stx - st * sx / n, sxx - sx * sx / n
                resid = cxx - ctx * ctx / ctt if ctt > 1e-9 else cxx
                event["check"] = math.sqrt(max(resid, 0) / n) * 1000
            if self.v_n:
                event["bounce"] = math.sqrt(self.v_sum2 / self.v_n) * 1000
            self.intervals = (self.intervals + [interval])[-RATE_AVG_STROKES:]
        else:
            self.intervals = []
        self.minima = (self.minima + [dip_min])[-MINIMA_COUNT:]
        self.last_catch = catch_t
        event["rate"] = self.rate(t)
        self.reset_cycle()
        self.phase = "drive"
        self.rescue_chain = self.rescue_chain + 1 if rescued and interval > 0 else 0
        return event

    def update_axis(self, h, gu):
        c = self.cov
        prods = (h[0] * h[0], h[0] * h[1], h[0] * h[2], h[1] * h[1], h[1] * h[2], h[2] * h[2])
        for i in range(6):
            c[i] += (prods[i] - c[i]) * self.aC
        a = self.axis
        v = (c[0] * a[0] + c[1] * a[1] + c[2] * a[2],
             c[1] * a[0] + c[3] * a[1] + c[4] * a[2],
             c[2] * a[0] + c[4] * a[1] + c[5] * a[2])
        v = sub(v, scale(gu, dot(v, gu)))
        n = norm(v)
        if n > 1e-12:
            v = scale(v, 1 / n)
            if dot(v, a) < 0:
                v = scale(v, -1)
            self.axis = v
            return
        a = sub(a, scale(gu, dot(a, gu)))
        n = norm(a)
        if n < 1e-3:
            trial = (1, 0, 0) if abs(gu[0]) < 0.9 else (0, 1, 0)
            a = sub(trial, scale(gu, dot(trial, gu)))
            n = norm(a)
        self.axis = scale(a, 1 / n)

    def process(self, t, ax, ay, az, gps=-1.0):
        """Returns (surge, threshold, stroke_event_or_None, finish_detected). gps < 0 means no fix."""
        a = (ax, ay, az)
        if not self.init:
            self.init, self.t0, self.g = True, t, a
            self.prev_t, self.prev_x = t, 0.0
            gn = norm(a)
            gu = scale(a, 1 / gn) if gn > 0.1 else (0, 0, 1)
            trial = (1, 0, 0) if abs(gu[0]) < 0.9 else (0, 1, 0)
            ax0 = sub(trial, scale(gu, dot(trial, gu)))
            self.axis = scale(ax0, 1 / norm(ax0))

        self.g = tuple(g + (x - g) * self.aG for g, x in zip(self.g, a))
        gn = norm(self.g)
        gu = scale(self.g, 1 / gn) if gn > 0.1 else (0, 0, 1)
        lin = sub(a, self.g)
        bp = tuple(lp(hp(x)) for (hp, lp), x in zip(self.bp, lin))
        vert = dot(bp, gu)
        h = sub(bp, scale(gu, vert))
        self.update_axis(h, gu)

        p = dot(h, self.axis)
        lin_h = sub(lin, scale(gu, dot(lin, gu)))
        p_lin = dot(lin_h, self.axis)
        pw = self.skew_lp(p_lin)
        warm = t - self.t0 >= WARMUP_S
        if warm:
            self.skew_n += 1
            a_s = max(self.aS, 1.0 / self.skew_n)
            self.m1 += (pw - self.m1) * a_s
            self.m2 += (pw * pw - self.m2) * a_s
            self.m3 += (pw * pw * pw - self.m3) * a_s
        flipped = False
        if warm and not self.gps_locked and self.sign * self.skew() > SKEW_FLIP_THRESHOLD:
            self.sign, flipped = -self.sign, True

        if warm and gps >= GPS_DIR_MIN_SPEED_MPS:
            self.v_int += (p_lin * STANDARD_GRAVITY - self.v_int / GPS_DIR_INTEGRATOR_TAU_S) * DT
            self.gps_mean += (gps - self.gps_mean) * ema_alpha(GPS_DIR_HP_TAU_S)
            vg = gps - self.gps_mean
            a_c = ema_alpha(GPS_DIR_CORR_TAU_S)
            self.cxy += (self.v_int * vg - self.cxy) * a_c
            self.cxx2 += (self.v_int ** 2 - self.cxx2) * a_c
            self.cyy2 += (vg * vg - self.cyy2) * a_c
            self.gps_dir_n += 1
            corr = self.gps_corr()
            if self.gps_dir_n >= GPS_DIR_MIN_TIME_S * FS and abs(corr) >= GPS_DIR_MIN_CORR:
                want = 1 if corr > 0 else -1
                self.gps_locked = True
                if want != self.sign:
                    self.sign, flipped = want, True

        x = self.sign * p
        self.ms += (x * x - self.ms) * self.aR
        if flipped:
            self.in_dip, self.in_rescue, self.phase = False, False, None
            self.reset_cycle()

        normal_mag = self.thr_mag()
        normal_thr_mag = self.dip_thr if self.in_dip else normal_mag
        relax_mag = max(RESCUE_THRESHOLD_FACTOR * normal_mag, RESCUE_THRESHOLD_FLOOR_G)
        relax_useful = relax_mag + 1e-4 < normal_mag
        shown = normal_thr_mag
        if warm and not self.in_dip:
            if self.in_rescue:
                shown = self.rescue_thr
            elif relax_useful and self.in_rescue_window(t, RESCUE_WINDOW_LO):
                shown = relax_mag
        thr = -shown
        self.v_sum2 += vert * vert
        self.v_n += 1
        finish = False

        if self.phase == "drive":
            self.drive_peak = max(self.drive_peak, x)
            fin = FINISH_FRACTION * self.drive_peak
            if self.drive_peak > 0.5 * self.floor and self.prev_x >= fin > x:
                tf = self.prev_t + (fin - self.prev_x) / (x - self.prev_x) * (t - self.prev_t)
                self.finish_t, self.drive_time = tf, tf - self.last_catch
                self.phase, finish = "recovery", True
            elif t - self.last_catch > MAX_DRIVE_S:
                self.phase = None
        elif self.phase == "recovery" and not self.in_dip and not self.in_rescue:
            rt = t - self.finish_t
            for i, v in enumerate((1, rt, x, rt * rt, rt * x, x * x)):
                self.rec[i] += v

        event = None
        normal_thr = -normal_thr_mag
        if not warm:
            self.in_dip = False
            self.in_rescue = False
        elif not self.in_dip and self.prev_x >= normal_thr > x:
            # Deep enough for the normal threshold: this dip belongs to it, not the gap-fill.
            self.t_down = self.prev_t + (normal_thr - self.prev_x) / (x - self.prev_x) * (t - self.prev_t)
            self.dip_min, self.dip_thr, self.in_dip = x, normal_thr_mag, True
            self.in_rescue = False
        elif not self.in_dip and not self.in_rescue and relax_useful and self.in_rescue_window(t, RESCUE_WINDOW_LO) \
                and self.prev_x >= -relax_mag > x:
            self.rescue_t_down = self.prev_t + (-relax_mag - self.prev_x) / (x - self.prev_x) * (t - self.prev_t)
            self.rescue_min, self.rescue_thr, self.in_rescue = x, relax_mag, True
        elif not self.in_dip and self.in_rescue:
            rescue_thr = -self.rescue_thr
            self.rescue_min = min(self.rescue_min, x)
            if x >= rescue_thr:
                t_up = self.prev_t + (rescue_thr - self.prev_x) / (x - self.prev_x) * (t - self.prev_t)
                catch_t = 0.5 * (self.rescue_t_down + t_up)
                self.in_rescue = False
                if self.in_rescue_window(catch_t, RESCUE_ACCEPT_LO):
                    event = self.accept_catch(t, catch_t, self.rescue_min, True)
            elif t - self.rescue_t_down > MAX_DIP_DURATION_S:
                self.in_rescue = False
        elif self.in_dip:
            self.dip_min = min(self.dip_min, x)
            if x >= normal_thr:
                t_up = self.prev_t + (normal_thr - self.prev_x) / (x - self.prev_x) * (t - self.prev_t)
                catch_t = 0.5 * (self.t_down + t_up)
                self.in_dip = False
                self.in_rescue = False
                event = self.accept_catch(t, catch_t, self.dip_min, False)
            elif t - self.t_down > MAX_DIP_DURATION_S:
                self.in_dip = False

        self.prev_t, self.prev_x = t, x
        return x, thr, event, finish


# ---- data sources ---------------------------------------------------------

def load_log(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append((float(r["t_s"]), float(r["ax_g"]), float(r["ay_g"]), float(r["az_g"]),
                         int(r.get("catch") or 0), float(r.get("gps_mps") or -1)))
    return rows


def synthetic(seconds, seed=1, facing=1.0, catch_depth=0.45, gps=True):
    """Port of src/sim.cpp: rate sweeps 20 -> 32 -> 20 spm over two minutes.

    facing=-1 mounts the device the other way round; catch_depth (g) models smoother rowing.
    GPS speed is held between 10 Hz fixes like the real receiver.
    """
    rnd = random.Random(seed)
    w = 2 * math.pi / 120.0
    tilt = math.radians(35)
    rows = []
    gps_v = -1.0
    for i in range(int(seconds * FS)):
        t = i * DT
        rate = 26.0 - 6.0 * math.cos(w * t)
        cycles = (26.0 * t - 6.0 * math.sin(w * t) / w) / 60.0
        period = 60.0 / rate
        tau = (cycles - math.floor(cycles)) * period
        tw = tau - period if tau > period * 0.5 else tau
        surge = facing * (-catch_depth * math.exp(-0.5 * (tw / 0.05) ** 2)
                          + 0.22 * math.exp(-0.5 * ((tau - 0.45) / 0.18) ** 2) - 0.04)
        vert = 0.03 * math.sin(2 * math.pi * tau / period)
        vib = 0.02 * math.sin(2 * math.pi * 8 * t)
        g = 1 + vert
        noise = lambda: rnd.uniform(-0.015, 0.015)
        ax = noise() + vib
        ay = math.sin(tilt) * g - math.cos(tilt) * surge + noise() + vib
        az = math.cos(tilt) * g + math.sin(tilt) * surge + noise()
        if gps and i % 10 == 0:
            gps_v = (3.2 + 0.08 * (rate - 20.0) + 0.35 * math.sin(2 * math.pi * (tau / period - 0.15))
                     + rnd.uniform(-0.05, 0.05))
        rows.append((t, ax, ay, az, 0, gps_v))
    return rows


# ---- analysis -------------------------------------------------------------

def run(rows, k, floor, minima, keep_trace=False):
    det = StrokeDetector(k, floor, minima)
    events, trace = [], []
    for t, ax, ay, az, _, gps in rows:
        x, thr, ev, fin = det.process(t, ax, ay, az, gps)
        if ev:
            events.append(ev)
        if keep_trace:
            trace.append((t, x, thr, ev is not None, fin))
    return events, trace, det


def summarize(events, rows, label=""):
    rates = [e["rate"] for e in events if e["rate"] > 0]
    drives = [e["drive"] for e in events if e["drive"] > 0]
    recs = [e["recovery"] for e in events if e["recovery"] > 0]
    device = [r[0] for r in rows if r[4]]
    rescued = sum(1 for e in events if e.get("rescued"))
    line = f"{label}strokes {len(events):4d}  rescued {rescued:3d}"
    if rates:
        line += f"  rate mean {statistics.mean(rates):5.1f} spm  min {min(rates):5.1f}  max {max(rates):5.1f}"
    if drives:
        line += f"  drive {statistics.median(drives):.2f} s  recovery {statistics.median(recs):.2f} s"
    if device:
        mine = [e["t"] for e in events]
        matched = sum(1 for d in device if any(abs(d - m) < 0.3 for m in mine))
        line += f"  | device catches {len(device)}, matched {matched}"
    print(line)


def plot(trace, t0, t1):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        raise SystemExit("plotting needs matplotlib: pip install matplotlib")
    sel = [p for p in trace if t0 <= p[0] - trace[0][0] <= t1]
    ts = [p[0] - trace[0][0] for p in sel]
    plt.figure(figsize=(14, 5))
    plt.plot(ts, [p[1] for p in sel], label="surge (g)")
    plt.plot(ts, [p[2] for p in sel], "r--", label="threshold")
    for p, t in zip(sel, ts):
        if p[3]:
            plt.axvline(t, color="g", alpha=0.6)
        if p[4]:
            plt.axvline(t, color="c", alpha=0.4, ymin=0.4, ymax=0.6)
    plt.xlabel("time (s)")
    plt.legend()
    plt.grid(alpha=0.3)
    plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="S####_imu.csv from the SD card")
    ap.add_argument("--synthetic", type=float, metavar="SECONDS")
    ap.add_argument("--k", type=float, default=1.8, help="threshold factor x RMS")
    ap.add_argument("--floor", type=float, default=0.12, help="threshold floor in g")
    ap.add_argument("--minima", action="store_true", help="threshold from last 5 catch depths")
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--strokes", metavar="CSV", help="write detected strokes to CSV")
    ap.add_argument("--plot", nargs=2, type=float, metavar=("FROM", "TO"))
    args = ap.parse_args()

    if args.synthetic:
        rows = synthetic(args.synthetic)
    elif args.log:
        rows = load_log(args.log)
    else:
        ap.error("give a log file or --synthetic")
    print(f"{len(rows)} samples, {(rows[-1][0] - rows[0][0]):.0f} s")

    if args.sweep:
        for minima in (False, True):
            for k in (1.4, 1.6, 1.8, 2.0, 2.2, 2.5):
                for floor in (0.06, 0.09, 0.12, 0.15):
                    events, _, _ = run(rows, k, floor, minima)
                    summarize(events, rows, f"K {k:.1f} floor {floor:.2f} minima {int(minima)}  ")
        return

    events, trace, det = run(rows, args.k, args.floor, args.minima, keep_trace=bool(args.plot))
    summarize(events, rows)
    print(f"surge axis {tuple(round(a * det.sign, 2) for a in det.axis)}  skew {det.skew():+.2f}  "
          f"direction from {'GPS' if det.gps_locked else 'skew'} (corr {det.gps_corr():+.2f})")
    if args.strokes:
        with open(args.strokes, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(events[0].keys()))
            w.writeheader()
            w.writerows(events)
        print(f"wrote {args.strokes}")
    if args.plot:
        plot(trace, *args.plot)


if __name__ == "__main__":
    main()
