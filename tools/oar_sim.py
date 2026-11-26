"""Checks the oar tracker of the StickS3 firmware against simulated rowing.

`Tracker` is a line-by-line port of stick/src/oar_tracker.h. Keep them in step.

The simulation turns a stick about the vertical pin like an oar: a drive from the catch to the finish, a
longer recovery, the blade feathering about the oar's own axis at the finish and squaring up before the
catch, a stick mounted at any angle, gyro bias and noise. It then compares what the tracker finds with
the known turning points.

    python tools\\oar_sim.py              # a crew of sticks with known offsets, prints the errors
    python tools\\oar_sim.py --selftest   # the same, exits 1 when a tolerance is missed
    python tools\\oar_sim.py --plot       # needs matplotlib
"""
import argparse
import math
import sys

import numpy as np

# ---------------------------------------------------------------- tracker (port of oar_tracker.h)

RATE_FILTER_HZ = 8.0
MIN_ARC_DEG = 15.0
STRONG_DPS = 20.0
MIN_PHASE_S = 0.15
MAX_PHASE_S = 6.0
MID_LEARN_STROKES = 4
MID_GAIN = 0.5
MID_MAX_ERR_DEG = 35.0


class Stroke:
    def __init__(self, no, catch_t, finish_t, catch_deg, finish_deg, recovery_s):
        self.no = no
        self.catch_t = catch_t
        self.finish_t = finish_t
        self.catch_deg = catch_deg
        self.finish_deg = finish_deg
        self.recovery_s = recovery_s


def _norm(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


class Tracker:
    def __init__(self):
        self.started = False
        self.have_u = False
        self.still_ = False
        self.zeroed_ = False
        self.t0 = 0.0
        self.prev_t = 0.0
        self.u = [0.0, 0.0, 1.0]
        self.bias = [0.0, 0.0, 0.0]
        self.still_s = 0.0
        self.theta = 0.0
        self.zero_off = 0.0
        self.sgn = 1.0
        self.wf = 0.0
        self.cov = [0.0] * 6
        self.shaft = [1.0, 0.0, 0.0]
        self.shaft_valid = False

        self.dir = 0
        self.seeded = False
        self.strong_seen = False
        self.lo = self.hi = self.extreme = 0.0
        self.extreme_t = 0.0
        self.cross_t = -1.0

        self.have_last = False
        self.last_kind = 0
        self.catch_kind = 0
        self.last_t = 0.0
        self.last_th = 0.0
        self.seg_after = [0.0, 0.0]
        self.seg_n = [0, 0]
        self.arc_ema = 60.0
        self.arc_n = 0

        self.strokes = 0
        self.last = None
        self.history = []
        self.have_finish = False
        self.have_catch = False
        self.prev_finish_t = 0.0
        self.prev_catch_t = 0.0
        self.rate = 0.0

        self.mid_ref_valid = False
        self.mid_n = 0
        self.mid_sum = 0.0
        self.mid_ref = 0.0

    def update(self, t, a, w):
        if not self.started:
            self.started = True
            self.t0 = self.prev_t = t
            m = _norm(a)
            if m < 0.5:
                return
            self.u = [a[i] / m for i in range(3)]
            self.have_u = True
            return
        t_prev = self.prev_t
        dtd = t - t_prev
        self.prev_t = t
        if not self.have_u:
            m = _norm(a)
            if m < 0.5:
                return
            self.u = [a[i] / m for i in range(3)]
            self.have_u = True
            return
        if dtd <= 0:
            return
        dt = 0.1 if dtd > 0.1 else dtd

        m = _norm(a)
        wc = [w[i] - self.bias[i] for i in range(3)]
        quiet = _norm(wc) < 3.0 and abs(m - 1.0) < 0.05
        self.still_s = self.still_s + dt if quiet else 0.0
        self.still_ = self.still_s > 0.5
        if self.still_:
            k = dt / (0.5 if (t - self.t0) < 10.0 else 5.0)
            for i in range(3):
                self.bias[i] += (w[i] - self.bias[i]) * k

        d2r = 0.017453292
        u = self.u
        wn = _norm(wc)
        if wn > 1e-3:
            phi = wn * d2r * dt
            n = [wc[0] / wn, wc[1] / wn, wc[2] / wn]
            cp, sp = math.cos(phi), math.sin(phi)
            nu = n[0] * u[0] + n[1] * u[1] + n[2] * u[2]
            cr = [n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]]
            for i in range(3):
                u[i] = u[i] * cp - cr[i] * sp + n[i] * nu * (1.0 - cp)
        if abs(m - 1.0) < 0.1:
            k = dt / (0.3 if quiet else 3.0)
            for i in range(3):
                u[i] += k * (a[i] / m - u[i])
        un = _norm(u)
        for i in range(3):
            u[i] /= un

        self._learn_shaft(wc, wn, dt)
        yaw = wc[0] * u[0] + wc[1] * u[1] + wc[2] * u[2]
        sh = self.shaft
        su = sh[0] * u[0] + sh[1] * u[1] + sh[2] * u[2]
        if self.shaft_valid and abs(su) < 0.35:
            ws = wc[0] * sh[0] + wc[1] * sh[1] + wc[2] * sh[2]
            den = 1.0 - su * su
            if den < 0.5:
                den = 0.5
            yaw = (yaw - su * ws) / den
        self.theta += yaw * dt
        alpha = 1.0 - math.exp(-6.2831853 * RATE_FILTER_HZ * dt)
        prev_wf = self.wf
        self.wf += (yaw - self.wf) * alpha
        self._detect(t, t_prev, prev_wf)

    def zero(self):
        self.zero_off = self.theta
        self.zeroed_ = True
        self.mid_ref_valid = False
        self.mid_n = 0
        self.mid_sum = 0.0

    def angle_deg(self):
        return self.sgn * (self.theta - self.zero_off)

    def _learn_shaft(self, wc, wn, dt):
        c, sh = self.cov, self.shaft
        if wn > 5.0:
            k = dt / 20.0
            keep = 1.0 - k
            kw = k * wn * wn * 1e-4  # weighted by the rate squared: the feathering stands out even more
            c[0] = keep * c[0] + kw * wc[0] * wc[0]
            c[1] = keep * c[1] + kw * wc[0] * wc[1]
            c[2] = keep * c[2] + kw * wc[0] * wc[2]
            c[3] = keep * c[3] + kw * wc[1] * wc[1]
            c[4] = keep * c[4] + kw * wc[1] * wc[2]
            c[5] = keep * c[5] + kw * wc[2] * wc[2]
        v = [c[0] * sh[0] + c[1] * sh[1] + c[2] * sh[2],
             c[1] * sh[0] + c[3] * sh[1] + c[4] * sh[2],
             c[2] * sh[0] + c[4] * sh[1] + c[5] * sh[2]]
        vn = _norm(v)
        if vn < 1e-3:
            return
        for i in range(3):
            sh[i] = v[i] / vn
        trace = c[0] + c[3] + c[5]
        lam = sh[0] * v[0] + sh[1] * v[1] + sh[2] * v[2]
        self.shaft_valid = trace > 2000.0 and lam > 0.65 * trace

    def _detect(self, t, t_prev, prev_wf):
        h = 0.15 * self.arc_ema
        h = 6.0 if h < 6.0 else (15.0 if h > 15.0 else h)

        if self.dir == 0:
            if not self.seeded:
                self.seeded = True
                self.lo = self.hi = self.theta
            if self.theta > self.hi:
                self.hi = self.theta
            if self.theta < self.lo:
                self.lo = self.theta
            if self.theta > self.lo + 2 * h:
                self._start_dir(+1)
            elif self.theta < self.hi - 2 * h:
                self._start_dir(-1)
            return

        s = float(self.dir)
        if s * (self.theta - self.extreme) >= 0:
            self.extreme = self.theta
            self.extreme_t = t
        if s * self.wf >= STRONG_DPS:
            self.strong_seen = True
            self.cross_t = -1.0
        elif self.strong_seen and self.cross_t < 0 and s * self.wf <= 0:
            d = prev_wf - self.wf
            f = prev_wf / d if d != 0 else 1.0
            self.cross_t = t_prev + (t - t_prev) * f
        if s * (self.extreme - self.theta) >= h:
            turn_t = self.cross_t - 1.0 / (6.2831853 * RATE_FILTER_HZ) if self.cross_t >= 0 else self.extreme_t
            if self.have_last and turn_t < self.last_t:
                turn_t = self.extreme_t
            self._confirm(self.dir, self.extreme, turn_t)
            self.dir = -self.dir
            self.extreme = self.theta
            self.extreme_t = t
            self.strong_seen = False
            self.cross_t = -1.0

    def _start_dir(self, d):
        self.dir = d
        self.extreme = self.theta
        self.extreme_t = self.prev_t
        self.strong_seen = False
        self.cross_t = -1.0

    def _confirm(self, kind, th, tt):
        pending = 0.0
        if self.have_last and self.last_kind == -kind:
            if abs(th - self.last_th) < MIN_ARC_DEG:
                return
            seg = tt - self.last_t
            if seg < MIN_PHASE_S or seg > MAX_PHASE_S:
                self.have_last = False
            else:
                idx = 0 if self.last_kind > 0 else 1
                if self.seg_n[idx]:
                    self.seg_after[idx] = 0.7 * self.seg_after[idx] + 0.3 * seg
                else:
                    self.seg_after[idx] = seg
                if self.seg_n[idx] < 100:
                    self.seg_n[idx] += 1
                arc = abs(th - self.last_th)
                self.arc_ema = 0.7 * self.arc_ema + 0.3 * arc if self.arc_n else arc
                self.arc_n += 1
                self._classify()
                if self.catch_kind != 0 and kind != self.catch_kind:
                    pending = self._emit(self.last_t, self.last_th, tt, th)
        self.last_kind = kind
        self.last_t = tt
        self.last_th = th
        self.have_last = True
        if pending != 0:
            self.theta += pending
            self.last_th += pending

    def _classify(self):
        if not self.seg_n[0] or not self.seg_n[1]:
            return
        if self.seg_after[0] < 0.9 * self.seg_after[1]:
            self.catch_kind = +1
        elif self.seg_after[1] < 0.9 * self.seg_after[0]:
            self.catch_kind = -1

    def _emit(self, ct, cth, ft, fth):
        if ft - ct < MIN_PHASE_S:
            return 0.0
        self.sgn = 1.0 if cth >= fth else -1.0
        recovery = 0.0
        if self.have_finish and 0.2 <= ct - self.prev_finish_t <= MAX_PHASE_S:
            recovery = ct - self.prev_finish_t
        if self.have_catch and 0.8 < ct - self.prev_catch_t < MAX_PHASE_S * 1.5:
            self.rate = 60.0 / (ct - self.prev_catch_t)
        else:
            self.rate = 0.0
        self.prev_catch_t = ct
        self.have_catch = True
        self.prev_finish_t = ft
        self.have_finish = True
        self.strokes += 1
        self.last = Stroke(self.strokes & 255, ct, ft, self.sgn * (cth - self.zero_off),
                           self.sgn * (fth - self.zero_off), recovery)
        self.history.append(self.last)

        mid = 0.5 * (cth + fth)
        if not self.mid_ref_valid:
            self.mid_sum += mid
            self.mid_n += 1
            if self.mid_n >= MID_LEARN_STROKES:
                self.mid_ref = self.mid_sum / self.mid_n
                self.mid_ref_valid = True
            return 0.0
        err = mid - self.mid_ref
        if abs(err) > MID_MAX_ERR_DEG:
            self.mid_ref_valid = False
            self.mid_n = 0
            self.mid_sum = 0.0
            return 0.0
        return -MID_GAIN * err


# ---------------------------------------------------------------- simulation


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


class Rower:
    """One oar. Angle in degrees, positive towards the stern; the catch is at the angle's maximum."""

    def __init__(self, spm=24.0, drive_frac=0.38, catch_deg=55.0, finish_deg=-35.0, offset_s=0.0,
                 side=+1, feather_deg=90.0, shaft_tilt_deg=6.0):
        self.period = 60.0 / spm
        self.drive = self.period * drive_frac
        self.catch_deg = catch_deg
        self.finish_deg = finish_deg
        self.offset = offset_s
        self.side = side  # +1: the angle as the stick turns about up, -1: the mirror (bow side / other mounting)
        self.feather = math.radians(feather_deg)
        self.shaft_tilt_deg = shaft_tilt_deg

    def phase(self, t):
        return ((t - self.offset) % self.period)

    def angle(self, t):
        p = self.phase(t)
        a, b = self.catch_deg, self.finish_deg
        if p < self.drive:  # drive, catch to finish
            return b + (a - b) * 0.5 * (1 + math.cos(math.pi * p / self.drive))
        r = (p - self.drive) / (self.period - self.drive)  # recovery
        return a + (b - a) * 0.5 * (1 + math.cos(math.pi * r))

    def feathering(self, t):
        """0 = blade square (drive), feather at the finish, back before the catch."""
        p = self.phase(t)
        ramp = 0.18
        if p < self.drive:
            return 0.0
        q = p - self.drive
        rec = self.period - self.drive
        if q < ramp:
            return self.feather * 0.5 * (1 - math.cos(math.pi * q / ramp))
        if q > rec - ramp:
            return self.feather * 0.5 * (1 + math.cos(math.pi * (q - (rec - ramp)) / ramp))
        return self.feather

    def true_turns(self, t_end):
        out = []
        k = 0
        while True:
            c = self.offset + k * self.period
            if c > t_end:
                break
            out.append((c, c + self.drive))
            k += 1
        return out


PRELUDE_S = 2.0  # from square to the first catch


def log_map(d):
    """Rotation vector of a rotation matrix."""
    c = (np.trace(d) - 1.0) * 0.5
    c = max(-1.0, min(1.0, c))
    ang = math.acos(c)
    v = np.array([d[2, 1] - d[1, 2], d[0, 2] - d[2, 0], d[1, 0] - d[0, 1]]) * 0.5
    sn = math.sin(ang)
    if sn < 1e-9:
        return v
    return v * (ang / sn)


def simulate(rower, seconds=90.0, fs=100.0, mount=(0.4, -0.7, 1.1), bias=(0.4, -0.3, 0.2), seed=1,
             gyro_noise=0.15, accel_noise=0.02, still_s=3.0, centripetal=True):
    rng = np.random.default_rng(seed)
    r_mount = rot_z(mount[0]) @ rot_y(mount[1]) @ rot_x(mount[2])
    n = int(seconds * fs)
    dt = 1.0 / fs
    row0 = still_s + PRELUDE_S

    def orient(t):
        # Still and square, then the approach to the first catch, then rowing.
        if t < still_s:
            return r_mount
        if t < row0:
            x = (t - still_s) / PRELUDE_S
            th = math.radians(rower.catch_deg * rower.side * 0.5 * (1 - math.cos(math.pi * x)))
            return rot_z(th) @ r_mount
        tr = t - row0
        th = math.radians(rower.angle(tr) * rower.side)
        return rot_z(th) @ rot_y(-math.radians(rower.shaft_tilt_deg)) @ rot_x(rower.feathering(tr)) @ r_mount

    ts, accs, gyros = [], [], []
    r_prev = orient(-dt)
    for i in range(n):
        t = i * dt
        r = orient(t)
        om = log_map(r_prev.T @ r) / dt  # body rate, rad/s
        r_prev = r
        acc = r.T @ np.array([0.0, 0.0, 1.0])
        if centripetal and t >= row0:
            # The oar swings the stick on an arc, about 1 m from the pin.
            tr = t - row0
            w = math.radians((rower.angle(tr + 0.005) - rower.angle(tr - 0.005)) / 0.01 * rower.side)
            a = math.radians(rower.angle(tr) * rower.side)
            radial = np.array([math.cos(a), math.sin(a), 0.0])
            acc = acc + r.T @ (-radial * w * w * 1.0 / 9.81)
        gy = np.degrees(om) + np.array(bias) + rng.normal(0, gyro_noise, 3)
        ac = acc + rng.normal(0, accel_noise, 3)
        ts.append(t)
        accs.append(ac)
        gyros.append(gy)
    return np.array(ts), np.array(accs), np.array(gyros)


def run(rower, **kw):
    ts, accs, gyros = simulate(rower, **kw)
    tr = Tracker()
    still_s = kw.get("still_s", 3.0)
    angles = []
    zeroed = False
    for t, a, g in zip(ts, accs, gyros):
        if not zeroed and t >= still_s - 0.3:
            tr.zero()  # the oar is square to the boat
            zeroed = True
        tr.update(float(t), [float(x) for x in a], [float(x) for x in g])
        angles.append(tr.angle_deg())
    return tr, ts, np.array(angles), still_s + PRELUDE_S


def errors(rower, tr, still_s, skip=6):
    """Timing errors (ms) of catch and finish against the simulated turning points."""
    truth = [(c + still_s, f + still_s) for c, f in rower.true_turns(600)]
    ce, fe, ca, fa = [], [], [], []
    for s in tr.history[skip:]:
        k = min(range(len(truth)), key=lambda i: abs(truth[i][0] - s.catch_t))
        ce.append((s.catch_t - truth[k][0]) * 1000)
        fe.append((s.finish_t - truth[k][1]) * 1000)
        ca.append(s.catch_deg - rower.catch_deg)
        fa.append(s.finish_deg - rower.finish_deg)
    return np.array(ce), np.array(fe), np.array(ca), np.array(fa)


def report(name, rower, tr, still_s):
    ce, fe, ca, fa = errors(rower, tr, still_s)
    if not len(ce):
        print(f"{name}: no strokes found")
        return None
    print(f"{name:34s} strokes {tr.strokes:3d}  catch {ce.mean():+6.1f} ms (sd {ce.std():4.1f})  "
          f"finish {fe.mean():+6.1f} ms (sd {fe.std():4.1f})  catch {ca.mean():+5.1f} deg  finish {fa.mean():+5.1f} deg")
    return ce, fe, ca, fa


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--seconds", type=float, default=90.0)
    args = ap.parse_args()

    cases = [
        ("stroke side, 24 spm", Rower(spm=24, offset_s=0.00, side=+1), dict(mount=(0.4, -0.7, 1.1))),
        ("bow side (mirrored), 24 spm", Rower(spm=24, offset_s=0.00, side=-1), dict(mount=(2.0, 0.5, -0.4))),
        ("seat 3, 40 ms late, 24 spm", Rower(spm=24, offset_s=0.04, side=+1), dict(mount=(-1.2, 0.3, 2.2))),
        ("seat 4, 30 ms early, 24 spm", Rower(spm=24, offset_s=-0.03, side=-1), dict(mount=(0.1, 1.0, 0.6))),
        ("rate 18 spm", Rower(spm=18, offset_s=0.0), dict()),
        ("rate 32 spm", Rower(spm=32, drive_frac=0.45, catch_deg=50, finish_deg=-30), dict()),
        ("small arc 60 deg", Rower(spm=22, catch_deg=35, finish_deg=-25), dict()),
        ("big bias 1.5 dps", Rower(spm=24), dict(bias=(1.5, -1.0, 0.8))),
    ]
    bad = 0
    results = {}
    for i, (name, rower, kw) in enumerate(cases):
        tr, ts, ang, still_s = run(rower, seconds=args.seconds, seed=i + 1, **kw)
        r = report(name, rower, tr, still_s)
        results[name] = (rower, tr, ts, ang, still_s)
        if r is None:
            bad += 1
            continue
        ce, fe, ca, fa = r
        # The errors are against the rower's own turning points, offset included: the tracker is not told it.
        if abs(ce.mean()) > 20 or ce.std() > 15 or abs(fe.mean()) > 20 or fe.std() > 15:
            print("   -> timing out of tolerance")
            bad += 1
        if abs(ca.mean()) > 12 or abs(fa.mean()) > 12:
            print("   -> angle out of tolerance")
            bad += 1

    # Togetherness between two sticks: the difference of their catches should come out as the offset.
    a = results["stroke side, 24 spm"][1].history
    b = results["seat 3, 40 ms late, 24 spm"][1].history
    diffs = []
    for s in b[6:]:
        k = min(a, key=lambda x: abs(x.catch_t - s.catch_t))
        diffs.append((s.catch_t - k.catch_t) * 1000)
    if diffs:
        print(f"seat 3 against stroke seat: {np.mean(diffs):+.1f} ms (true +40)  sd {np.std(diffs):.1f}")
        if abs(np.mean(diffs) - 40) > 20:
            print("   -> offset out of tolerance")
            bad += 1

    if args.plot:
        import matplotlib.pyplot as plt
        rower, tr, ts, ang, still_s = results["stroke side, 24 spm"]
        plt.plot(ts, ang, label="angle (tracker)")
        for s in tr.history:
            plt.axvline(s.catch_t, color="g", lw=0.5)
            plt.axvline(s.finish_t, color="c", lw=0.5)
        plt.xlim(still_s, still_s + 15)
        plt.legend()
        plt.show()

    if args.selftest and bad:
        print(f"{bad} checks failed")
        sys.exit(1)
    print("ok" if not bad else f"{bad} checks failed")


if __name__ == "__main__":
    main()
