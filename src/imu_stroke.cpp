#include "imu_stroke.h"

#include <M5Unified.h>

#include <atomic>

#include "board.h"
#include "config.h"
#include "fusion.h"
#include "logger.h"
#include "sim.h"

namespace imu {

namespace {

StrokeDetector detector;
SpeedFusion fusion;
QueueHandle_t strokeQueue = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

Live liveState;
StrokeParams pendingParams;
bool paramsDirty = false;
std::atomic<bool> fusionEnabled{false};
float gpsSpeed = 0;
bool gpsValid = false;

PlotPoint plot[cfg::PLOT_SAMPLES];
std::atomic<uint32_t> plotHead{0};

void readAccel(double t, float& ax, float& ay, float& az, float& gx, float& gy, float& gz) {
#if SIM_MODE
    sim::accel(t, ax, ay, az);
    gx = gy = gz = 0;
#else
    (void)t;
    board::I2CLock lock;
    M5.Imu.update();
    M5.Imu.getAccel(&ax, &ay, &az);
    M5.Imu.getGyro(&gx, &gy, &gz);
#endif
}

void task(void*) {
    const TickType_t period = pdMS_TO_TICKS(1000 / (int)cfg::IMU_RATE_HZ);
    TickType_t wake = xTaskGetTickCount();
    uint32_t samplesInWindow = 0;
    int64_t windowStart = board::nowUs();
    uint32_t overruns = 0;
    DetectorSample out;
    StrokeEvent ev;
    char line[160];

    for (;;) {
        if (xTaskDelayUntil(&wake, period) == pdFALSE) overruns++;

        double t = board::nowS();
        float ax, ay, az, gx, gy, gz;
        readAccel(t, ax, ay, az, gx, gy, gz);

        float gpsV;
        bool gpsOk;
        portENTER_CRITICAL(&mux);
        if (paramsDirty) {
            detector.setParams(pendingParams);
            paramsDirty = false;
        }
        gpsV = gpsSpeed;
        gpsOk = gpsValid;
        portEXIT_CRITICAL(&mux);

        detector.setGps(gpsV, gpsOk);
        bool stroke = detector.process(t, ax, ay, az, out, ev);

        fusion.setGps(gpsV, gpsOk && fusionEnabled);
        fusion.step(out.surgeLinear * cfg::STANDARD_GRAVITY, cfg::IMU_DT_S);

        if (stroke) {
            ev.fusedDistanceM = fusion.active() ? fusion.distance() : -1.0;
            xQueueSend(strokeQueue, &ev, 0);
        }

        uint32_t head = plotHead.load(std::memory_order_relaxed);
        plot[head & (cfg::PLOT_SAMPLES - 1)] = {out.surge, out.threshold,
                                                (uint8_t)((out.catchDetected ? 1 : 0) |
                                                          (out.finishDetected ? 2 : 0))};
        plotHead.store(head + 1, std::memory_order_release);

        if (logger::rawActive()) {
            snprintf(line, sizeof(line), "%.3f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.4f,%.4f,%d,%.2f,%.4f,%.4f,%d\n",
                     t, ax, ay, az, gx, gy, gz, out.surge, out.threshold, out.catchDetected ? 1 : 0,
                     gpsOk ? gpsV : -1.0f, out.surgeLinear, out.vertical, out.finishDetected ? 1 : 0);
            logger::write(logger::Stream::Raw, line);
        }

        samplesInWindow++;
        int64_t now = board::nowUs();
        float hz = -1;
        if (now - windowStart >= 1000000) {
            hz = samplesInWindow * 1e6f / (float)(now - windowStart);
            samplesInWindow = 0;
            windowStart = now;
        }

        dsp::Vec3 ax3 = detector.axis(), g3 = detector.gravity();
        portENTER_CRITICAL(&mux);
        liveState.rate = detector.rate(t);
        liveState.strokes = detector.strokeCount();
        liveState.lastCatch = detector.lastCatchTime();
        liveState.rms = detector.rms();
        liveState.skew = detector.skew();
        liveState.sign = detector.sign();
        liveState.directionFromGps = detector.directionFromGps();
        liveState.gpsCorrelation = detector.gpsCorrelation();
        liveState.axis[0] = ax3.x;
        liveState.axis[1] = ax3.y;
        liveState.axis[2] = ax3.z;
        liveState.gravity[0] = g3.x;
        liveState.gravity[1] = g3.y;
        liveState.gravity[2] = g3.z;
        liveState.accel[0] = ax;
        liveState.accel[1] = ay;
        liveState.accel[2] = az;
        liveState.threshold = out.threshold;
        if (hz >= 0) liveState.sampleHz = hz;
        liveState.overruns = overruns;
        liveState.fusionActive = fusion.active();
        liveState.fusedSpeed = fusion.speed();
        liveState.fusedDistance = fusion.distance();
        portEXIT_CRITICAL(&mux);
    }
}

}  // namespace

void begin(const StrokeParams& p, bool fusionOn) {
    detector.setParams(p);
    fusion.setCrossoverHz(cfg::FUSION_CROSSOVER_HZ);
    fusionEnabled = fusionOn;
    strokeQueue = xQueueCreate(16, sizeof(StrokeEvent));
    xTaskCreatePinnedToCore(task, "imu", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
}

void setParams(const StrokeParams& p) {
    portENTER_CRITICAL(&mux);
    pendingParams = p;
    paramsDirty = true;
    portEXIT_CRITICAL(&mux);
}

void setFusionEnabled(bool on) { fusionEnabled = on; }

void setGpsSpeed(float speedMps, bool valid) {
    portENTER_CRITICAL(&mux);
    gpsSpeed = speedMps;
    gpsValid = valid;
    portEXIT_CRITICAL(&mux);
}

bool popStroke(StrokeEvent& ev) { return strokeQueue && xQueueReceive(strokeQueue, &ev, 0) == pdTRUE; }

Live live() {
    portENTER_CRITICAL(&mux);
    Live copy = liveState;
    portEXIT_CRITICAL(&mux);
    return copy;
}

int copyPlot(PlotPoint* dst, int n) {
    uint32_t head = plotHead.load(std::memory_order_acquire);
    if (n > cfg::PLOT_SAMPLES - 1) n = cfg::PLOT_SAMPLES - 1;
    if ((uint32_t)n > head) n = (int)head;
    uint32_t start = head - n;
    for (int i = 0; i < n; i++) dst[i] = plot[(start + i) & (cfg::PLOT_SAMPLES - 1)];
    return n;
}

}  // namespace imu
