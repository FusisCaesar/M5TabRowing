#pragma once

#include <stdint.h>

namespace cfg {

// ---- Pins -------------------------------------------------------------
// Grove PORT.A (default: external I2C). Re-used as a UART for the GPS.
constexpr int GPS_TX_PIN = 53;
constexpr int GPS_RX_PIN = 54;

// Camera image needs a half turn to be upright with the default (unflipped) display.
constexpr bool CAM_ROTATE_180 = false;

// microSD in SDIO 4-bit mode.
constexpr int SD_CLK_PIN = 43;
constexpr int SD_CMD_PIN = 44;
constexpr int SD_D0_PIN = 39;
constexpr int SD_D1_PIN = 40;
constexpr int SD_D2_PIN = 41;
constexpr int SD_D3_PIN = 42;
constexpr int SD_FREQ_KHZ = 20000;

// ---- Clock --------------------------------------------------------------
// Logs are in UTC; the screen shows local time in this POSIX time zone (Europe/Amsterdam).
constexpr const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.google.com";

// ---- IMU / stroke detection -------------------------------------------
constexpr float IMU_RATE_HZ = 100.0f;
constexpr float IMU_DT_S = 1.0f / IMU_RATE_HZ;

constexpr float GRAVITY_TAU_S = 2.0f;
constexpr float AXIS_COV_TAU_S = 8.0f;
constexpr float SKEW_TAU_S = 20.0f;
constexpr float SKEW_FLIP_THRESHOLD = 0.25f;
constexpr float SKEW_LOWPASS_HZ = 6.0f;

// GPS direction check: integrated surge must rise and fall with GPS speed.
constexpr float GPS_DIR_MIN_SPEED_MPS = 1.0f;
constexpr float GPS_DIR_INTEGRATOR_TAU_S = 1.0f;
constexpr float GPS_DIR_HP_TAU_S = 1.0f;
constexpr float GPS_DIR_CORR_TAU_S = 15.0f;
constexpr float GPS_DIR_MIN_CORR = 0.3f;
constexpr float GPS_DIR_MIN_TIME_S = 10.0f;
// The receiver reports speed about half a second late (measured on the water), so the
// surge is held back this long before the two are compared. Compared without the delay
// they are a fifth of a stroke apart: the match sits near zero and can tip either way.
constexpr float GPS_SPEED_DELAY_S = 0.5f;
constexpr int GPS_DIR_DELAY_SAMPLES = (int)(GPS_SPEED_DELAY_S * IMU_RATE_HZ + 0.5f);

// The direction of travel in the device's own frame is learned from GPS the same way and then
// held: surge is measured along it and nothing else, so a sideways push is not a forward one.
// A running average, so a unit that is remounted finds its way again.
constexpr float FORWARD_TAU_S = 120.0f;
constexpr float FORWARD_MIN_TIME_S = 30.0f;

// Roll and yaw from the gyro: its rate about forward and about up, minus its slow average (bias and
// steady turns), integrated with a leak. The angle is how far the boat is off its mean attitude.
constexpr float GYRO_BIAS_TAU_S = 30.0f;
constexpr float ATTITUDE_LEAK_TAU_S = 3.0f;

constexpr float BANDPASS_LOW_HZ = 0.1f;
constexpr float BANDPASS_HIGH_HZ = 3.0f;

constexpr float RMS_TAU_S = 5.0f;
constexpr float THRESHOLD_K_DEFAULT = 1.8f;
constexpr float THRESHOLD_FLOOR_G_DEFAULT = 0.12f;
constexpr float MINIMA_FACTOR = 0.6f;
constexpr int MINIMA_COUNT = 5;

constexpr float MIN_STROKE_INTERVAL_S = 1.0f;
constexpr float MAX_DIP_DURATION_S = 0.8f;
constexpr float FINISH_FRACTION = 0.35f;
constexpr int RATE_AVG_STROKES = 3;
constexpr float STROKE_TIMEOUT_S = 6.0f;

// Gap-fill for a catch the normal threshold missed. Once two real intervals have
// set the rhythm, a shallower dip counts too, but only inside the window where
// the next stroke is expected. A rescued catch keeps the rhythm, so a run of
// light strokes stays tracked; a normal catch or a pause clears the chain.
constexpr float RESCUE_WINDOW_LO = 0.75f;          // x recent average interval; where a dip may start
constexpr float RESCUE_ACCEPT_LO = 0.90f;          // the catch itself must be this late, or a recovery
                                                   // wiggle counts and the real catch is blocked
constexpr float RESCUE_WINDOW_HI = 1.35f;
constexpr float RESCUE_THRESHOLD_FACTOR = 0.5f;    // x the normal threshold
constexpr float RESCUE_THRESHOLD_FLOOR_G = 0.06f;  // relaxed threshold never goes below this
constexpr int RESCUE_MIN_INTERVALS = 2;
constexpr int RESCUE_CHAIN_MAX = 120;

constexpr int TECHNIQUE_AVG_STROKES = 5;

constexpr float FUSION_CROSSOVER_HZ = 0.3f;

constexpr float STANDARD_GRAVITY = 9.80665f;

// ---- GPS ----------------------------------------------------------------
constexpr uint32_t GPS_TARGET_BAUD = 115200;
constexpr uint32_t GPS_RATE_MS_FAST = 100;   // 10 Hz, needs >= 38400 baud
constexpr uint32_t GPS_RATE_MS_SLOW = 200;   // 5 Hz fallback at 9600 baud
constexpr float GPS_MAX_HDOP = 4.0f;
constexpr int GPS_MIN_SATS = 4;
constexpr uint32_t GPS_FIX_TIMEOUT_MS = 2000;
constexpr uint32_t GPS_SILENCE_RESCAN_MS = 5000;
constexpr float GPS_MOVING_SPEED_MPS = 0.6f;
constexpr float GPS_MIN_STEP_M = 3.0f;
constexpr float GPS_SPEED_ZERO_MPS = 0.3f;

// ---- Metrics ------------------------------------------------------------
constexpr float SPLIT_IDLE_WINDOW_S = 3.0f;
constexpr float MIN_SPLIT_SPEED_MPS = 0.5f;
constexpr int SPLIT_AVG_STROKES = 3;  // displayed split averages this many strokes
constexpr int SPEED_HISTORY = 256;   // ~25 s at 10 Hz

// ---- Map / track --------------------------------------------------------
constexpr float TRACK_MIN_STEP_M = 3.0f;      // one breadcrumb per this many metres
constexpr int TRACK_MAX_POINTS = 20000;       // ~60 km of track
constexpr int MAP_MAX_FILES = 24;             // .rmap files scanned on the SD card
// Zoom levels as the real width shown across the map, in metres.
constexpr int MAP_ZOOM_M[] = {100, 200, 400, 800, 1600, 3200};
constexpr int MAP_ZOOM_COUNT = sizeof(MAP_ZOOM_M) / sizeof(MAP_ZOOM_M[0]);
constexpr int MAP_ZOOM_DEFAULT = 2;           // 400 m across
constexpr uint32_t MAP_FRAME_MS = 200;        // redraw the map at most this often

// ---- UI -----------------------------------------------------------------
constexpr uint32_t TOUCH_HOLD_MS = 800;
constexpr uint32_t UI_FRAME_MS = 50;
constexpr int PLOT_SAMPLES = 1024;   // power of two

}  // namespace cfg
