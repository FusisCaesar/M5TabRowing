#include "camera.h"

#include <ESP_Video.h>
#include <M5Unified.h>
#include <driver/i2c_master.h>
#include <driver/isp_ccm.h>
#include <driver/ppa.h>

#include <atomic>
#include <utility>

#include "board.h"
#include "display_fb.h"

namespace camera {

namespace {

ESPVideoClass video;
ESPVideoCaptureDevClass capture;
ppa_client_handle_t ppa = nullptr;

TaskHandle_t task = nullptr;
std::atomic<bool> wantOn{false};
std::atomic<Status> state{Status::Off};
std::atomic<float> fpsValue{0};

// Guards `pending` and the capture stream while the UI reads a frame.
SemaphoreHandle_t frameLock = nullptr;
ESPVideoBufferClass pending;  // newest captured frame, not drawn yet

uint32_t drawn = 0, fpsStart = 0;

bool initHardware() {
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_master_get_bus_handle((i2c_port_num_t)M5.In_I2C.getPort(), &bus) != ESP_OK || !bus) {
        Serial.println("[cam] internal I2C bus handle not available");
        return false;
    }
    ESPVideoCamConfigClass cam;
    if (!cam.begin(bus)) return false;
    ESPVideoCSIConfigClass csi;
    // The display already powers the shared MIPI PHY LDO.
    if (!csi.begin(cam, true)) return false;
    if (!video.begin(csi)) {
        Serial.println("[cam] sensor not detected");
        return false;
    }
    if (!capture.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 3)) return false;
    if (!capture.setFormat(ESP_VIDEO_FORMAT_RGB565)) return false;

    ppa_client_config_t pc = {};
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    if (ppa_register_client(&pc, &ppa) != ESP_OK) return false;

    Serial.printf("[cam] ready, sensor %lux%lu\n", (unsigned long)capture.getWidth(),
                  (unsigned long)capture.getHeight());
    return true;
}

void dropPending() {
    xSemaphoreTake(frameLock, portMAX_DELAY);
    if (pending.valid()) pending.end();
    xSemaphoreGive(frameLock);
}

void run(void*) {
    bool inited = false, streaming = false;
    for (;;) {
        if (!wantOn) {
            if (streaming) {
                xSemaphoreTake(frameLock, portMAX_DELAY);
                if (pending.valid()) pending.end();
                capture.stopCapture();
                xSemaphoreGive(frameLock);
                streaming = false;
            }
            if (state != Status::Failed) state = Status::Off;
            fpsValue = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!inited) {
            state = Status::Starting;
            inited = initHardware();
            if (!inited) {
                Serial.println("[cam] init failed");
                state = Status::Failed;
                wantOn = false;
                continue;
            }
        }
        if (!streaming) {
            state = Status::Starting;
            bool started = capture.startCapture();
            // A failed start leaves some buffers queued, so every later start fails too;
            // re-requesting the buffers resets the queue.
            for (int attempt = 0; !started && attempt < 3; attempt++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                started = capture.requestBuffer(3) && capture.startCapture();
            }
            if (!started) {
                Serial.println("[cam] start failed");
                state = Status::Failed;
                wantOn = false;
                continue;
            }
            streaming = true;
        }

        ESPVideoBufferClass frame = capture.captureBuffer();
        if (!frame.valid()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        ESPVideoBufferClass old;
        xSemaphoreTake(frameLock, portMAX_DELAY);
        old = std::move(pending);
        pending = std::move(frame);
        xSemaphoreGive(frameLock);
        if (old.valid()) old.end();
        state = Status::Running;
    }
}

}  // namespace

void begin() {
    frameLock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(run, "camera", 8192, nullptr, 3, &task, 1);
}

void setActive(bool on) {
    if (on == wantOn) return;
    if (on && state == Status::Failed) state = Status::Off;
    wantOn = on;
    if (!on) dropPending();
    xTaskNotifyGive(task);
}

bool active() { return wantOn; }
Status status() { return state; }
float fps() { return fpsValue; }

bool draw(int x, int y, int w, int h, bool mirror, bool rotate180) {
    if (!ppa || xSemaphoreTake(frameLock, 0) != pdTRUE) return false;
    if (!pending.valid()) {
        xSemaphoreGive(frameLock);
        return false;
    }

    fb::Target t;
    if (!fb::map(x, y, w, h, t)) {
        pending.end();
        xSemaphoreGive(frameLock);
        return false;
    }

    int inW = (int)pending.getWidth(), inH = (int)pending.getHeight();
    float s = fmaxf((float)w / inW, (float)h / inH);
    s = ceilf(s * 16.0f) / 16.0f;  // PPA scale steps are 1/16
    int blkW = min(inW, (int)(w / s));
    int blkH = min(inH, (int)(h / s));

    ppa_srm_oper_config_t op = {};
    op.in.buffer = pending.data();
    op.in.pic_w = inW;
    op.in.pic_h = inH;
    op.in.block_w = blkW;
    op.in.block_h = blkH;
    op.in.block_offset_x = (inW - blkW) / 2;
    op.in.block_offset_y = (inH - blkH) / 2;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = t.buffer;
    op.out.buffer_size = t.size;
    op.out.pic_w = t.picW;
    op.out.pic_h = t.picH;
    op.out.block_offset_x = t.x;
    op.out.block_offset_y = t.y;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = fb::angle(t, rotate180 ? 2 : 0);
    op.scale_x = s;
    op.scale_y = s;
    // Mirroring applies to the output block, whose x axis is the display's y axis when rotated by 90.
    op.mirror_x = mirror && !(t.rotation & 1);
    op.mirror_y = mirror && (t.rotation & 1);
    op.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa, &op);
    pending.end();
    xSemaphoreGive(frameLock);

    if (err != ESP_OK) {
        static uint32_t lastLog = 0;
        if (millis() - lastLog > 5000) {
            lastLog = millis();
            Serial.printf("[cam] PPA error %d (in %dx%d block %dx%d scale %.4f out %d,%d)\n", (int)err, inW, inH,
                          blkW, blkH, s, t.x, t.y);
        }
        return false;
    }

    drawn++;
    uint32_t now = millis();
    if (now - fpsStart >= 2000) {
        fpsValue = drawn * 1000.0f / (now - fpsStart);
        drawn = 0;
        fpsStart = now;
    }
    return true;
}

}  // namespace camera

// The prebuilt image tuning folds white balance into the colour matrix and, in warm or dim light,
// asks for gains above the ISP's +-4 limit. Unclamped, the whole update is rejected, white balance
// is never applied and the picture turns green; with saturation the driver clamps instead.
extern "C" esp_err_t __real_esp_isp_ccm_configure(isp_proc_handle_t proc, const esp_isp_ccm_config_t* cfg);
extern "C" esp_err_t __wrap_esp_isp_ccm_configure(isp_proc_handle_t proc, const esp_isp_ccm_config_t* cfg) {
    esp_isp_ccm_config_t c = *cfg;
    c.saturation = true;
    return __real_esp_isp_ccm_configure(proc, &c);
}
