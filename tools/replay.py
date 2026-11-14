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
import bisect
import csv
from array import array
import math
import os
import random
import shutil
import statistics
import time

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
GPS_SPEED_DELAY_S = 0.5
# Forward in the device's frame, learned from GPS as it goes and then held (as the firmware does).
FORWARD_TAU_S = 120.0
FORWARD_MIN_TIME_S = 30.0
GYRO_BIAS_TAU_S = 30.0
ATTITUDE_LEAK_TAU_S = 3.0
# Recordings only, not in the firmware: with the whole outing to look at, forward is
# settled once and used from the first stroke.
HINDSIGHT_MIN_S = 30.0
HINDSIGHT_HYSTERESIS = 0.2
# With forward known from GPS, surge is measured along it and nothing else: a sideways push
# stays out of the surge. Off: surge along the axis the detector tracks by itself.
LOCK_FORWARD = True


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


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    return math.sqrt(dot(a, a))


class StrokeDetector:
    def __init__(self, k=1.8, floor=0.12, minima=False, forward=None, gps_delay=GPS_SPEED_DELAY_S):
        self.k, self.floor, self.use_minima = k, floor, minima
        # The firmware has one fixed delay. A recording can say otherwise: see gps_delay_of().
        self.delay_n = max(1, int(gps_delay * FS + 0.5) + 1)
        # Forward as a unit vector in the device's frame. Given: it decides the sign and
        # the skew and GPS checks are left out. None: the firmware's own behaviour.
        self.forward = forward
        self.h_v = (0.0, 0.0, 0.0)
        self.h_ring = [(0.0, 0.0, 0.0)] * self.delay_n
        self.h_head = 0
        self.h_c = [0.0, 0.0, 0.0]
        self.h_m = [0.0] * 6  # xx xy xz yy yz zz
        self.h_gg = 0.0
        self.h_n = 0
        # The same, as the device keeps it: a running average, so it follows a remounted unit.
        self.o_c = (0.0, 0.0, 0.0)
        self.o_m = [0.0] * 6
        self.o_gg = 0.0
        self.o_n = 0
        self.fwd_ready = False
        self.fwd_online = (1.0, 0.0, 0.0)
        self.p_lin = 0.0
        self.lat_lin = 0.0  # gravity removed, g, positive towards starboard (to the right of forward)
        self.roll = self.yaw = 0.0  # degrees off the mean: starboard side down, bow to starboard
        self.gyro_bias = (0.0, 0.0, 0.0)
        self.gyro_n = 0
        self.aB = ema_alpha(GYRO_BIAS_TAU_S)
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
        self.v_delay = [0.0] * self.delay_n  # one longer than the delay: written first, then read
        self.v_delay_head = 0
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

    def learn_forward(self, lin_h, vg):
        """Recordings only: which way in the device's frame speeds the boat up.

        The same comparison as the GPS direction check, but on the whole horizontal
        acceleration instead of its projection on the surge axis. That axis can come
        back the other way round after a pause; this does not depend on it.
        """
        v = tuple(vi + (a * STANDARD_GRAVITY - vi / GPS_DIR_INTEGRATOR_TAU_S) * DT for vi, a in zip(self.h_v, lin_h))
        self.h_v = v
        self.h_ring[self.h_head] = v
        self.h_head = (self.h_head + 1) % self.delay_n
        old = self.h_ring[self.h_head]
        for i in range(3):
            self.h_c[i] += old[i] * vg
        prods = (old[0] * old[0], old[0] * old[1], old[0] * old[2], old[1] * old[1], old[1] * old[2], old[2] * old[2])
        for i in range(6):
            self.h_m[i] += prods[i]
        self.h_gg += vg * vg
        self.h_n += 1
        self.o_n += 1
        a = max(ema_alpha(FORWARD_TAU_S), 1.0 / self.o_n)
        self.o_c = tuple(c + (o * vg - c) * a for c, o in zip(self.o_c, old))
        self.o_m = [m + (p - m) * a for m, p in zip(self.o_m, prods)]
        self.o_gg += (vg * vg - self.o_gg) * a
        size = norm(self.o_c)
        if self.o_n >= FORWARD_MIN_TIME_S * FS and size > 1e-12:
            f = scale(self.o_c, 1 / size)
            m = self.o_m
            var = (m[0] * f[0] * f[0] + m[3] * f[1] * f[1] + m[5] * f[2] * f[2]
                   + 2 * (m[1] * f[0] * f[1] + m[2] * f[0] * f[2] + m[4] * f[1] * f[2]))
            d = math.sqrt(max(var, 0.0) * self.o_gg)
            if d > 1e-12 and size / d >= GPS_DIR_MIN_CORR:
                self.fwd_online, self.fwd_ready = f, True

    def learned_forward(self):
        """(unit vector, correlation with GPS speed, seconds of GPS it rests on), or None."""
        size = norm(self.h_c)
        if not self.h_n or size < 1e-12:
            return None
        f = scale(self.h_c, 1 / size)
        m = self.h_m
        var = (m[0] * f[0] * f[0] + m[3] * f[1] * f[1] + m[5] * f[2] * f[2]
               + 2 * (m[1] * f[0] * f[1] + m[2] * f[0] * f[2] + m[4] * f[1] * f[2]))
        d = math.sqrt(max(var, 0.0) * self.h_gg)
        return f, (size / d if d > 1e-12 else 0.0), self.h_n * DT

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

    def process(self, t, ax, ay, az, gps=-1.0, gyro=None):
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
        lock = self.forward if self.forward is not None else (self.fwd_online if self.fwd_ready else None)
        if lock is not None and LOCK_FORWARD:
            fh = sub(lock, scale(gu, dot(lock, gu)))
            if norm(fh) > 0.2:
                self.axis = scale(fh, 1 / norm(fh))

        p = dot(h, self.axis)
        lin_h = sub(lin, scale(gu, dot(lin, gu)))
        p_lin = dot(lin_h, self.axis)
        self.p_lin = p_lin
        pw = self.skew_lp(p_lin)
        warm = t - self.t0 >= WARMUP_S
        if warm:
            self.skew_n += 1
            a_s = max(self.aS, 1.0 / self.skew_n)
            self.m1 += (pw - self.m1) * a_s
            self.m2 += (pw * pw - self.m2) * a_s
            self.m3 += (pw * pw * pw - self.m3) * a_s
        flipped = False
        if self.forward is not None:
            if self.sign * dot(self.axis, self.forward) < -HINDSIGHT_HYSTERESIS:
                self.sign, flipped = -self.sign, True
        elif warm and not self.gps_locked and self.sign * self.skew() > SKEW_FLIP_THRESHOLD:
            self.sign, flipped = -self.sign, True

        if warm and gps >= GPS_DIR_MIN_SPEED_MPS:
            self.v_int += (p_lin * STANDARD_GRAVITY - self.v_int / GPS_DIR_INTEGRATOR_TAU_S) * DT
            self.v_delay[self.v_delay_head] = self.v_int
            self.v_delay_head = (self.v_delay_head + 1) % self.delay_n
            v_old = self.v_delay[self.v_delay_head]
            self.gps_mean += (gps - self.gps_mean) * ema_alpha(GPS_DIR_HP_TAU_S)
            vg = gps - self.gps_mean
            a_c = ema_alpha(GPS_DIR_CORR_TAU_S)
            self.cxy += (v_old * vg - self.cxy) * a_c
            self.cxx2 += (v_old ** 2 - self.cxx2) * a_c
            self.cyy2 += (vg * vg - self.cyy2) * a_c
            self.gps_dir_n += 1
            self.learn_forward(lin_h, vg)
            corr = self.gps_corr()
            if self.forward is None and LOCK_FORWARD and self.fwd_ready:
                # Forward is known as a direction, so the axis points along it and surge is positive.
                self.gps_locked = True
                if self.sign != 1:
                    self.sign, flipped = 1, True
            elif self.forward is None and self.gps_dir_n >= GPS_DIR_MIN_TIME_S * FS and abs(corr) >= GPS_DIR_MIN_CORR:
                want = 1 if corr > 0 else -1
                self.gps_locked = True
                if want != self.sign:
                    self.sign, flipped = want, True

        x = self.sign * p
        # Sideways: perpendicular to forward, in the horizontal plane. Right of forward is
        # forward x up, and gu points up (a device at rest reads +1 g upwards).
        self.lat_lin = dot(lin_h, cross(scale(self.axis, self.sign), gu))
        if gyro is not None:
            # Rate about forward and about up, less its slow average, integrated with a leak. Right-hand
            # rotation about forward lifts the port side, about up it turns the bow to port.
            self.gyro_n += 1
            a_b = max(self.aB, 1.0 / self.gyro_n)
            self.gyro_bias = tuple(b + (w - b) * a_b for b, w in zip(self.gyro_bias, gyro))
            r = sub(gyro, self.gyro_bias)
            leak = 1.0 / ATTITUDE_LEAK_TAU_S
            self.roll += (dot(r, scale(self.axis, self.sign)) - self.roll * leak) * DT
            self.yaw += (-dot(r, gu) - self.yaw * leak) * DT
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
    GPS speed is held between 10 Hz fixes and arrives late, like the real receiver.
    """
    rnd = random.Random(seed)
    w = 2 * math.pi / 120.0
    tilt = math.radians(35)
    rows = []
    gps_v = -1.0
    fixes = []
    late = int(round(GPS_SPEED_DELAY_S * 10))
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
            fixes.append(3.2 + 0.08 * (rate - 20.0) + 0.35 * math.sin(2 * math.pi * (tau / period - 0.15))
                         + rnd.uniform(-0.05, 0.05))
            gps_v = fixes[max(0, len(fixes) - 1 - late)]
        rows.append((t, ax, ay, az, 0, gps_v))
    return rows


# ---- analysis -------------------------------------------------------------

def _run_once(rows, det, keep_trace):
    """keep_trace True keeps a row per sample; "surge" keeps only the unfiltered surge, as one
    float per sample, which is all a long recording can afford to hold."""
    events = []
    trace = array("d") if keep_trace == "surge" else []
    # Sideways, roll and yaw, one value per sample like the surge. Single precision is plenty.
    kept = keep_trace == "surge"
    det.sway, det.roll_trace, det.yaw_trace = (array("f"), array("f"), array("f")) if kept else (None, None, None)
    for t, ax, ay, az, _, gps, *gyro in rows:
        x, thr, ev, fin = det.process(t, ax, ay, az, gps, gyro if gyro else None)
        if ev:
            events.append(ev)
        if kept:
            trace.append(det.sign * det.p_lin)
            det.sway.append(det.lat_lin)
            det.roll_trace.append(det.roll)
            det.yaw_trace.append(det.yaw)
        elif keep_trace:
            trace.append((t, x, thr, ev is not None, fin, det.sign * det.p_lin))
    return events, trace


def gps_delay_of(info):
    """How late GPS speed is in a recording, from its info file.

    Sessions written before the delay was logged: the receiver on the water is
    half a second late, the simulator of that time was not late at all.
    """
    try:
        return float(info["gps_speed_delay_s"])
    except (KeyError, TypeError, ValueError):
        return 0.0 if info.get("build") == "simulation" else GPS_SPEED_DELAY_S


def direction_note(det):
    h = getattr(det, "hindsight", None)
    if h:
        return "forward from GPS over the whole recording, r %.2f" % h["corr"]
    return "forward from %s as it went, r %+.2f" % ("GPS" if det.gps_locked else "the stroke shape", det.gps_corr())


def run(rows, k, floor, minima, keep_trace=False, hindsight=False, gps_delay=GPS_SPEED_DELAY_S):
    """Replays a log. Trace rows: t, surge, threshold, catch, finish, unfiltered surge.

    Without hindsight this is what the device does. With it, forward is first
    learned from GPS over the whole recording and the log is replayed with that
    direction from the first sample, so the strokes before the device was sure
    come out the right way up too. det.hindsight says whether that happened.
    """
    if hindsight and iter(rows) is rows:  # a one-shot iterator; the log is read twice
        rows = list(rows)
    det = StrokeDetector(k, floor, minima, gps_delay=gps_delay)
    events, trace = _run_once(rows, det, keep_trace)
    det.hindsight = None
    if hindsight:
        learned = det.learned_forward()
        if learned and learned[1] >= GPS_DIR_MIN_CORR and learned[2] >= HINDSIGHT_MIN_S:
            live = [e["t"] for e in events]
            det = StrokeDetector(k, floor, minima, forward=learned[0], gps_delay=gps_delay)
            events, trace = _run_once(rows, det, keep_trace)
            moved = 0
            for e in events:
                i = bisect.bisect_left(live, e["t"])
                near = [abs(e["t"] - m) for m in live[max(0, i - 1):i + 1]]
                if not near or min(near) >= 0.3:
                    moved += 1
            det.hindsight = {"corr": learned[1], "seconds": learned[2], "moved": moved}
    return events, trace, det


MIN_SPLIT_SPEED_MPS = 0.5
SPLIT_AVG_STROKES = 3
STROKE_HEADER = (
    "elapsed_s,stroke,rate_spm,interval_s,drive_s,recovery_s,ratio,split_s,speed_mps,dps_m,"
    "distance_m,peak_decel_g,check_mg,bounce_mg,lat,lon,t_s,rescued,split3_s\n"
)


def _sibling(imu_path, stream):
    if imu_path.endswith("_imu.csv"):
        return imu_path[: -len("_imu.csv")] + "_" + stream + ".csv"
    return os.path.splitext(imu_path)[0] + "_" + stream + ".csv"


def _read_info(path):
    if not os.path.exists(path):
        return {}
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return {r["key"]: r["value"] for r in csv.DictReader(f) if r.get("key")}


def _newline(path):
    with open(path, "rb") as f:
        chunk = f.read(4096)
    return "\r\n" if b"\r\n" in chunk else "\n"


def _split_from_speed(speed):
    return 500.0 / speed if speed >= MIN_SPLIT_SPEED_MPS else 0.0


def _mean_speed(times, speeds, t0, t1):
    i = bisect.bisect_left(times, t0)
    j = bisect.bisect_right(times, t1)
    total = 0.0
    n = 0
    for v in speeds[i:j]:
        if v >= 0:
            total += v
            n += 1
    return total / n if n >= 3 else -1.0


def _seconds_track(path):
    if not os.path.exists(path):
        return []
    out = []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                out.append((float(r["elapsed_s"]), float(r.get("distance_m") or 0),
                            float(r.get("lat") or 0), float(r.get("lon") or 0)))
            except (TypeError, ValueError):
                continue
    return out


def _place(track, elapsed):
    if not track:
        return 0.0, 0.0, 0.0
    if elapsed <= track[0][0]:
        return track[0][1], track[0][2], track[0][3]
    if elapsed >= track[-1][0]:
        return track[-1][1], track[-1][2], track[-1][3]
    for i in range(1, len(track)):
        a, b = track[i - 1], track[i]
        if elapsed <= b[0]:
            span = b[0] - a[0] or 1.0
            u = (elapsed - a[0]) / span
            dist = a[1] + (b[1] - a[1]) * u
            src = b if u >= 0.5 else a
            return dist, src[2], src[3]
    return track[-1][1], track[-1][2], track[-1][3]


def _write_lines(path, lines):
    nl = _newline(path) if os.path.exists(path) else "\n"
    tmp = path + ".tmp"
    with open(tmp, "w", newline="", encoding="utf-8") as f:
        f.write(nl.join(line.rstrip("\r\n") for line in lines) + nl)
    os.replace(tmp, path)


REBUILT_KEY = "strokes_rebuilt"
ORIGINAL_DIR = "original"


def _keep_originals(imu_path, info):
    """Before the first rebuild, copies the device's own files to original/ next to them."""
    if info.get(REBUILT_KEY):
        return []  # what is here was already rebuilt, so it is not the device's to keep
    folder = os.path.join(os.path.dirname(os.path.abspath(imu_path)), ORIGINAL_DIR)
    kept = []
    for stream in ("strokes", "seconds", "laps"):
        src = _sibling(imu_path, stream)
        dst = os.path.join(folder, os.path.basename(src))
        if os.path.exists(src) and not os.path.exists(dst):
            os.makedirs(folder, exist_ok=True)
            shutil.copy2(src, dst)
            kept.append(dst)
    return kept


def _mark_rebuilt(info_path, when):
    """Notes in the session's info file that its strokes no longer come from the device."""
    lines = ["key,value"]
    if os.path.exists(info_path):
        with open(info_path, newline="", encoding="utf-8", errors="replace") as f:
            lines = [line for line in f.read().splitlines() if line.strip()]
    lines = [line for line in lines if not line.startswith(REBUILT_KEY + ",")]
    lines.append("%s,%s" % (REBUILT_KEY, when))
    _write_lines(info_path, lines)


def rebuild_session(imu_path):
    """Replace a session's stroke file with what the current detector finds.

    Keeps the session's own K, floor and minima. Speed, split and position come
    from the recording. The per-second rate and each lap's stroke count follow
    the new catches, so the rest of the session agrees with the stroke file.
    The device's own files are copied to original/ first.
    """
    info_path = _sibling(imu_path, "info")
    info = _read_info(info_path)
    k = float(info.get("threshold_k", 1.8))
    floor = float(info.get("threshold_floor_g", 0.12))
    minima = info.get("minima_threshold") == "1"
    rows = load_log(imu_path)
    if not rows:
        raise SystemExit("no samples in %s" % imu_path)
    origin = float(info["start_t_s"]) if info.get("start_t_s") else rows[0][0]
    events, _, det = run(rows, k, floor, minima, hindsight=True, gps_delay=gps_delay_of(info))
    kept = _keep_originals(imu_path, info)
    times = [r[0] for r in rows]
    speeds = [r[5] for r in rows]
    track = _seconds_track(_sibling(imu_path, "seconds"))

    paces = []
    split3 = 0.0
    lines = [STROKE_HEADER.rstrip("\n")]
    rescued = 0
    for n, ev in enumerate(events, start=1):
        elapsed = ev["t"] - origin
        interval, drive, rec = ev["interval"], ev["drive"], ev["recovery"]
        ratio = rec / drive if drive > 0 else 0.0
        if interval > 0:
            mean_v = _mean_speed(times, speeds, ev["t"] - interval, ev["t"])
            speed = mean_v if mean_v > 0 else 0.0
            raw = _split_from_speed(speed)
            dps = speed * interval if mean_v > 0 else 0.0
            if mean_v > 0:
                paces.append((interval, mean_v))
                paces = paces[-SPLIT_AVG_STROKES:]
                dist = sum(i * s for i, s in paces)
                span = sum(i for i, _ in paces)
                split3 = _split_from_speed(dist / span) if span else 0.0
            elif not paces:
                split3 = 0.0
        else:
            speed = raw = dps = split3 = 0.0
            paces = []
        if ev["rescued"]:
            rescued += 1
        dist_m, lat, lon = _place(track, elapsed)
        lines.append(
            "%.2f,%d,%.1f,%.3f,%.3f,%.3f,%.2f,%.1f,%.2f,%.2f,%.1f,%.3f,%.1f,%.1f,%.7f,%.7f,%.3f,%d,%.1f"
            % (elapsed, n, ev["rate"], interval, drive, rec, ratio, raw, speed, dps, dist_m,
               ev["peak"], ev["check"], ev["bounce"], lat, lon, ev["t"], 1 if ev["rescued"] else 0, split3)
        )
    strokes_path = _sibling(imu_path, "strokes")
    _write_lines(strokes_path, lines)
    _rewrite_rates(_sibling(imu_path, "seconds"), events, origin)
    laps = _rewrite_laps(_sibling(imu_path, "laps"), events, origin)
    _mark_rebuilt(info_path, time.strftime("%Y-%m-%d"))
    print("%s  strokes %d  rescued %d  laps %d  (K %.2f, floor %.2f g%s, %s)"
          % (os.path.basename(strokes_path), len(events), rescued, laps, k, floor,
             ", minima" if minima else "", direction_note(det)))
    if kept:
        print("  device files kept in %s" % os.path.dirname(kept[0]))
    return len(events), rescued


def _rate_at(events, origin, elapsed):
    chosen = None
    for ev in events:
        if ev["t"] - origin <= elapsed:
            chosen = ev
        else:
            break
    if chosen is None or elapsed - (chosen["t"] - origin) > STROKE_TIMEOUT_S:
        return 0.0
    return chosen["rate"]


def _rewrite_rates(path, events, origin):
    if not os.path.exists(path):
        return
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    if not lines:
        return
    header = lines[0].split(",")
    try:
        ri = header.index("rate_spm")
        ei = header.index("elapsed_s")
    except ValueError:
        return
    out = [lines[0]]
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split(",")
        try:
            elapsed = float(parts[ei])
        except (ValueError, IndexError):
            out.append(line)
            continue
        parts[ri] = "%.1f" % _rate_at(events, origin, elapsed)
        out.append(",".join(parts))
    _write_lines(path, out)


def _rewrite_laps(path, events, origin):
    if not os.path.exists(path):
        return 0
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    if len(lines) < 2:
        return 0
    header = lines[0].split(",")
    try:
        ei = header.index("elapsed_s")
        ri = header.index("avg_rate_spm")
        si = header.index("strokes")
    except ValueError:
        return 0
    ends = []
    for line in lines[1:]:
        if not line.strip():
            continue
        try:
            ends.append(float(line.split(",")[ei]))
        except (ValueError, IndexError):
            ends.append(None)
    out = [lines[0]]
    prev = -1.0
    written = 0
    data_i = 0
    for line in lines[1:]:
        if not line.strip():
            continue
        parts = line.split(",")
        end = ends[data_i]
        data_i += 1
        if end is None:
            out.append(line)
            continue
        if data_i == len(ends):
            mine = [ev for ev in events if ev["t"] - origin > prev]
        else:
            mine = [ev for ev in events if prev < ev["t"] - origin <= end]
        prev = end
        rates = [ev["rate"] for ev in mine if ev["rate"] > 0]
        parts[si] = str(len(mine))
        parts[ri] = "%.1f" % (sum(rates) / len(rates) if rates else 0.0)
        out.append(",".join(parts))
        written += 1
    _write_lines(path, out)
    return written


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
    ap.add_argument("--rebuild", action="store_true",
                    help="rewrite this session's strokes, per-second rate and lap counts")
    ap.add_argument("--hindsight", action="store_true",
                    help="settle forward from GPS over the whole recording first (--rebuild always does)")
    ap.add_argument("--plot", nargs=2, type=float, metavar=("FROM", "TO"))
    args = ap.parse_args()

    delay = GPS_SPEED_DELAY_S
    if args.synthetic:
        rows = synthetic(args.synthetic)
    elif args.log:
        rows = load_log(args.log)
        delay = gps_delay_of(_read_info(_sibling(args.log, "info")))
    else:
        ap.error("give a log file or --synthetic")
    print(f"{len(rows)} samples, {(rows[-1][0] - rows[0][0]):.0f} s")

    if args.rebuild:
        if not args.log:
            ap.error("--rebuild needs a session log")
        rebuild_session(args.log)
        return

    if args.sweep:
        for minima in (False, True):
            for k in (1.4, 1.6, 1.8, 2.0, 2.2, 2.5):
                for floor in (0.06, 0.09, 0.12, 0.15):
                    events, _, _ = run(rows, k, floor, minima, gps_delay=delay)
                    summarize(events, rows, f"K {k:.1f} floor {floor:.2f} minima {int(minima)}  ")
        return

    events, trace, det = run(rows, args.k, args.floor, args.minima, keep_trace=bool(args.plot),
                             hindsight=args.hindsight, gps_delay=delay)
    summarize(events, rows)
    print(f"surge axis {tuple(round(a * det.sign, 2) for a in det.axis)}  skew {det.skew():+.2f}  "
          f"{direction_note(det)}")
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
