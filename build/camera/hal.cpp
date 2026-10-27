#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <system/camera_metadata.h>
#include <unistd.h>
#include <vndk/hardware_buffer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "HoltdroidCamera", __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, "HoltdroidCamera", __VA_ARGS__)

using namespace aidl::android::hardware::camera;
using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::PixelFormat;
using ndk::ScopedAStatus;

namespace {

constexpr const char* kInstance = "android.hardware.camera.provider.ICameraProvider/holtdroid/0";
constexpr const char* kDeviceName = "device@1.1/holtdroid/0";
constexpr char kFeed[] = "holtdroid-camera";
constexpr int kMaxW = 1920, kMaxH = 1080;
constexpr int32_t kJpegMaxSize = 3 << 20;
constexpr int64_t kMinFrame = 33333333;
const std::vector<std::pair<int, int>> kSizes = {{1920, 1080}, {1280, 720}, {640, 480}, {320, 240}};

ScopedAStatus error(common::Status s) { return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s)); }

int64_t now() {
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

class Meta {
  public:
    Meta() : m_(allocate_camera_metadata(64, 4096)) {}
    explicit Meta(const device::CameraMetadata& in) {
        auto* raw = reinterpret_cast<const camera_metadata_t*>(in.metadata.data());
        if (!in.metadata.empty() && validate_camera_metadata_structure(raw, nullptr) == 0) {
            m_ = clone_camera_metadata(raw);
        } else {
            m_ = allocate_camera_metadata(64, 4096);
        }
    }
    Meta(const Meta& o) : m_(clone_camera_metadata(o.m_)) {}
    Meta& operator=(const Meta& o) {
        if (this != &o) {
            free_camera_metadata(m_);
            m_ = clone_camera_metadata(o.m_);
        }
        return *this;
    }
    ~Meta() { free_camera_metadata(m_); }

    bool empty() const { return get_camera_metadata_entry_count(m_) == 0; }

    template <typename T>
    void set(uint32_t tag, const std::vector<T>& v) {
        camera_metadata_entry_t e;
        if (find_camera_metadata_entry(m_, tag, &e) == 0) {
            update_camera_metadata_entry(m_, e.index, v.data(), v.size(), nullptr);
            return;
        }
        if (add_camera_metadata_entry(m_, tag, v.data(), v.size()) != 0) {
            size_t entries = get_camera_metadata_entry_count(m_) * 2 + 8;
            size_t data = get_camera_metadata_data_count(m_) * 2 + v.size() * sizeof(T) + 64;
            camera_metadata_t* bigger = allocate_camera_metadata(entries, data);
            append_camera_metadata(bigger, m_);
            free_camera_metadata(m_);
            m_ = bigger;
            add_camera_metadata_entry(m_, tag, v.data(), v.size());
        }
    }
    template <typename T>
    void set(uint32_t tag, T v) { set(tag, std::vector<T>{v}); }

    int32_t i32(uint32_t tag, int idx, int32_t def) const {
        camera_metadata_ro_entry_t e;
        if (find_camera_metadata_ro_entry(m_, tag, &e) != 0 || int(e.count) <= idx) return def;
        return e.data.i32[idx];
    }
    uint8_t u8(uint32_t tag, uint8_t def) const {
        camera_metadata_ro_entry_t e;
        if (find_camera_metadata_ro_entry(m_, tag, &e) != 0 || e.count == 0) return def;
        return e.data.u8[0];
    }

    device::CameraMetadata aidl() const {
        auto* p = reinterpret_cast<const uint8_t*>(m_);
        return device::CameraMetadata{std::vector<uint8_t>(p, p + get_camera_metadata_size(m_))};
    }

  private:
    camera_metadata_t* m_;
};

std::vector<int32_t> tags(std::initializer_list<uint32_t> l) { return std::vector<int32_t>(l.begin(), l.end()); }

const std::vector<int32_t> kRequestKeys = tags({
    ANDROID_COLOR_CORRECTION_ABERRATION_MODE, ANDROID_CONTROL_AE_ANTIBANDING_MODE, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
    ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
    ANDROID_CONTROL_AF_MODE, ANDROID_CONTROL_AF_TRIGGER, ANDROID_CONTROL_AWB_LOCK, ANDROID_CONTROL_AWB_MODE,
    ANDROID_CONTROL_CAPTURE_INTENT, ANDROID_CONTROL_EFFECT_MODE, ANDROID_CONTROL_MODE, ANDROID_CONTROL_SCENE_MODE,
    ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, ANDROID_FLASH_MODE, ANDROID_JPEG_ORIENTATION, ANDROID_JPEG_QUALITY,
    ANDROID_JPEG_THUMBNAIL_QUALITY, ANDROID_JPEG_THUMBNAIL_SIZE, ANDROID_LENS_FOCAL_LENGTH, ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
    ANDROID_NOISE_REDUCTION_MODE, ANDROID_SCALER_CROP_REGION, ANDROID_STATISTICS_FACE_DETECT_MODE,
});

std::vector<int32_t> resultKeys() {
    auto keys = kRequestKeys;
    for (uint32_t t : {ANDROID_SENSOR_TIMESTAMP, ANDROID_CONTROL_AE_STATE, ANDROID_CONTROL_AF_STATE, ANDROID_CONTROL_AWB_STATE,
                       ANDROID_FLASH_STATE, ANDROID_LENS_STATE, ANDROID_REQUEST_PIPELINE_DEPTH, ANDROID_SENSOR_ROLLING_SHUTTER_SKEW,
                       ANDROID_STATISTICS_SCENE_FLICKER}) {
        keys.push_back(int32_t(t));
    }
    return keys;
}

Meta characteristics() {
    Meta m;
    m.set<uint8_t>(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, {ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
                                                                        ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST});
    m.set<uint8_t>(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, {ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO});
    m.set<uint8_t>(ANDROID_CONTROL_AE_AVAILABLE_MODES, {ANDROID_CONTROL_AE_MODE_ON});
    m.set<int32_t>(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, {15, 30, 30, 30});
    m.set<int32_t>(ANDROID_CONTROL_AE_COMPENSATION_RANGE, {0, 0});
    m.set<camera_metadata_rational_t>(ANDROID_CONTROL_AE_COMPENSATION_STEP, {{1, 3}});
    m.set<uint8_t>(ANDROID_CONTROL_AE_LOCK_AVAILABLE, ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE);
    m.set<uint8_t>(ANDROID_CONTROL_AF_AVAILABLE_MODES, {ANDROID_CONTROL_AF_MODE_OFF});
    m.set<uint8_t>(ANDROID_CONTROL_AVAILABLE_EFFECTS, {ANDROID_CONTROL_EFFECT_MODE_OFF});
    m.set<uint8_t>(ANDROID_CONTROL_AVAILABLE_MODES, {ANDROID_CONTROL_MODE_OFF, ANDROID_CONTROL_MODE_AUTO});
    m.set<uint8_t>(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, {ANDROID_CONTROL_SCENE_MODE_DISABLED});
    m.set<uint8_t>(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, {ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF});
    m.set<uint8_t>(ANDROID_CONTROL_AWB_AVAILABLE_MODES, {ANDROID_CONTROL_AWB_MODE_AUTO});
    m.set<uint8_t>(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE);
    m.set<int32_t>(ANDROID_CONTROL_MAX_REGIONS, {0, 0, 0});
    m.set<uint8_t>(ANDROID_DISTORTION_CORRECTION_AVAILABLE_MODES, {ANDROID_DISTORTION_CORRECTION_MODE_OFF});
    m.set<uint8_t>(ANDROID_FLASH_INFO_AVAILABLE, ANDROID_FLASH_INFO_AVAILABLE_FALSE);
    m.set<uint8_t>(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED);
    m.set<int32_t>(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, {0, 0, 160, 90, 160, 120});
    m.set<int32_t>(ANDROID_JPEG_MAX_SIZE, kJpegMaxSize);
    m.set<uint8_t>(ANDROID_LENS_FACING, ANDROID_LENS_FACING_BACK);
    m.set<float>(ANDROID_LENS_INFO_AVAILABLE_APERTURES, {2.0f});
    m.set<float>(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, {3.3f});
    m.set<uint8_t>(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, {ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF});
    m.set<uint8_t>(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED);
    m.set<float>(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, 0.0f);
    m.set<uint8_t>(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, {ANDROID_NOISE_REDUCTION_MODE_OFF});
    m.set<uint8_t>(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE});
    m.set<int32_t>(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, {0, 3, 1});
    m.set<int32_t>(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, 1);
    m.set<uint8_t>(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, 4);
    m.set<float>(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, 1.0f);
    m.set<uint8_t>(ANDROID_SCALER_CROPPING_TYPE, ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY);
    std::vector<int32_t> configs;
    std::vector<int64_t> durations, stalls;
    for (int32_t f : {int32_t(PixelFormat::IMPLEMENTATION_DEFINED), int32_t(PixelFormat::YCBCR_420_888), int32_t(PixelFormat::BLOB)}) {
        for (auto [w, h] : kSizes) {
            configs.insert(configs.end(), {f, w, h, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT});
            durations.insert(durations.end(), {f, w, h, kMinFrame});
            stalls.insert(stalls.end(), {f, w, h, f == int32_t(PixelFormat::BLOB) ? kMinFrame : 0});
        }
    }
    m.set(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, configs);
    m.set(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations);
    m.set(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stalls);
    m.set<int32_t>(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, {0, 0, kMaxW, kMaxH});
    m.set<int32_t>(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, {kMaxW, kMaxH});
    m.set<int32_t>(ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, {0, 0, kMaxW, kMaxH});
    m.set<float>(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, {4.8f, 2.7f});
    m.set<int64_t>(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, 66666666);
    m.set<uint8_t>(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
    m.set<int32_t>(ANDROID_SENSOR_ORIENTATION, 90);
    m.set<int32_t>(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, {ANDROID_SENSOR_TEST_PATTERN_MODE_OFF});
    m.set<uint8_t>(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, {ANDROID_STATISTICS_FACE_DETECT_MODE_OFF});
    m.set<int32_t>(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, 0);
    m.set<int32_t>(ANDROID_SYNC_MAX_LATENCY, ANDROID_SYNC_MAX_LATENCY_UNKNOWN);
    m.set(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, kRequestKeys);
    m.set(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, resultKeys());
    m.set(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, tags({
        ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, ANDROID_CONTROL_AE_COMPENSATION_RANGE,
        ANDROID_CONTROL_AE_COMPENSATION_STEP, ANDROID_CONTROL_AE_LOCK_AVAILABLE, ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_EFFECTS, ANDROID_CONTROL_AVAILABLE_MODES, ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
        ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, ANDROID_CONTROL_AWB_AVAILABLE_MODES, ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
        ANDROID_CONTROL_MAX_REGIONS, ANDROID_DISTORTION_CORRECTION_AVAILABLE_MODES, ANDROID_FLASH_INFO_AVAILABLE, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, ANDROID_JPEG_MAX_SIZE, ANDROID_LENS_FACING, ANDROID_LENS_INFO_AVAILABLE_APERTURES,
        ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
        ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, ANDROID_SCALER_CROPPING_TYPE, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
        ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, ANDROID_SENSOR_ORIENTATION,
        ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
        ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, ANDROID_SYNC_MAX_LATENCY,
    }));
    return m;
}

Meta defaults(device::RequestTemplate t) {
    uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    switch (t) {
        case device::RequestTemplate::STILL_CAPTURE: intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE; break;
        case device::RequestTemplate::VIDEO_RECORD: intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD; break;
        case device::RequestTemplate::VIDEO_SNAPSHOT: intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT; break;
        case device::RequestTemplate::ZERO_SHUTTER_LAG: intent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG; break;
        case device::RequestTemplate::MANUAL: intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL; break;
        default: break;
    }
    bool video = intent == ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD || intent == ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
    Meta m;
    m.set<uint8_t>(ANDROID_COLOR_CORRECTION_ABERRATION_MODE, ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST);
    m.set<uint8_t>(ANDROID_CONTROL_AE_ANTIBANDING_MODE, ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO);
    m.set<int32_t>(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, 0);
    m.set<uint8_t>(ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_LOCK_OFF);
    m.set<uint8_t>(ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_MODE_ON);
    m.set<uint8_t>(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
    m.set<int32_t>(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, video ? std::vector<int32_t>{30, 30} : std::vector<int32_t>{15, 30});
    m.set<uint8_t>(ANDROID_CONTROL_AF_MODE, ANDROID_CONTROL_AF_MODE_OFF);
    m.set<uint8_t>(ANDROID_CONTROL_AF_TRIGGER, ANDROID_CONTROL_AF_TRIGGER_IDLE);
    m.set<uint8_t>(ANDROID_CONTROL_AWB_LOCK, ANDROID_CONTROL_AWB_LOCK_OFF);
    m.set<uint8_t>(ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_AWB_MODE_AUTO);
    m.set<uint8_t>(ANDROID_CONTROL_CAPTURE_INTENT, intent);
    m.set<uint8_t>(ANDROID_CONTROL_EFFECT_MODE, ANDROID_CONTROL_EFFECT_MODE_OFF);
    m.set<uint8_t>(ANDROID_CONTROL_MODE, ANDROID_CONTROL_MODE_AUTO);
    m.set<uint8_t>(ANDROID_CONTROL_SCENE_MODE, ANDROID_CONTROL_SCENE_MODE_DISABLED);
    m.set<uint8_t>(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    m.set<uint8_t>(ANDROID_FLASH_MODE, ANDROID_FLASH_MODE_OFF);
    m.set<int32_t>(ANDROID_JPEG_ORIENTATION, 0);
    m.set<uint8_t>(ANDROID_JPEG_QUALITY, 90);
    m.set<uint8_t>(ANDROID_JPEG_THUMBNAIL_QUALITY, 90);
    m.set<int32_t>(ANDROID_JPEG_THUMBNAIL_SIZE, {0, 0});
    m.set<float>(ANDROID_LENS_FOCAL_LENGTH, 3.3f);
    m.set<uint8_t>(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF);
    m.set<uint8_t>(ANDROID_NOISE_REDUCTION_MODE, ANDROID_NOISE_REDUCTION_MODE_OFF);
    m.set<int32_t>(ANDROID_SCALER_CROP_REGION, {0, 0, kMaxW, kMaxH});
    m.set<uint8_t>(ANDROID_STATISTICS_FACE_DETECT_MODE, ANDROID_STATISTICS_FACE_DETECT_MODE_OFF);
    return m;
}

struct Frame {
    int w = 0, h = 0;
    std::vector<uint8_t> i420;
};

void colorBars(Frame& f, int w, int h) {
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                       {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    f.w = w, f.h = h;
    f.i420.resize(size_t(w) * h * 3 / 2);
    uint8_t* y = f.i420.data();
    uint8_t* u = y + w * h;
    uint8_t* v = u + (w / 2) * (h / 2);
    for (int c = 0; c < w; c++) y[c] = bars[c * 8 / w][0];
    for (int r = 1; r < h; r++) memcpy(y + r * w, y, w);
    for (int r = 0; r < h / 2; r++) {
        for (int c = 0; c < w / 2; c++) {
            int b = c * 2 * 8 / w;
            u[r * (w / 2) + c] = bars[b][1];
            v[r * (w / 2) + c] = bars[b][2];
        }
    }
}

// Feed takes frames from the agent on the abstract socket @holtdroid-camera,
// which the Pod's containers share and Android's INET firewall doesn't see:
// it connects once a stream needs them,
// asks for "<width> <height> <fps>\n" and reads I420 frames of that size
// until the session closes. Without the agent the device shows color bars.
class Feed {
  public:
    ~Feed() { stop(); }

    void start(int w, int h, int fps) {
        stop();
        std::lock_guard<std::mutex> l(mu_);
        w_ = w, h_ = h, fps_ = fps;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> l(mu_);
            running_ = false;
            if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> l(mu_);
        frame_.reset();
    }

    std::shared_ptr<const Frame> latest() {
        std::lock_guard<std::mutex> l(mu_);
        if (frame_ && now() - got_ < 2000000000) return frame_;
        int w = w_ > 0 ? w_ : 640, h = h_ > 0 ? h_ : 480;
        if (!bars_ || bars_->w != w || bars_->h != h) {
            auto b = std::make_shared<Frame>();
            colorBars(*b, w, h);
            bars_ = b;
        }
        return bars_;
    }

  private:
    void loop() {
        while (true) {
            int w, h, fps;
            {
                std::lock_guard<std::mutex> l(mu_);
                if (!running_) return;
                w = w_, h = h_, fps = fps_;
            }
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            memcpy(a.sun_path + 1, kFeed, sizeof(kFeed) - 1);
            socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof(kFeed) - 1);
            bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&a), len) == 0;
            if (!ok && !warned_) {
                ERR("no frames from @%s: %s", kFeed, strerror(errno));
                warned_ = true;
            }
            if (ok) {
                warned_ = false;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    fd_ = fd;
                }
                char hello[64];
                int n = snprintf(hello, sizeof(hello), "%d %d %d\n", w, h, fps);
                if (write(fd, hello, n) == n) read(fd, w, h);
                std::lock_guard<std::mutex> l(mu_);
                fd_ = -1;
            }
            if (fd >= 0) close(fd);
            for (int i = 0; i < 20; i++) {
                {
                    std::lock_guard<std::mutex> l(mu_);
                    if (!running_) return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void read(int fd, int w, int h) {
        while (true) {
            auto f = std::make_shared<Frame>();
            f->w = w, f->h = h;
            f->i420.resize(size_t(w) * h * 3 / 2);
            size_t got = 0;
            while (got < f->i420.size()) {
                ssize_t n = ::read(fd, f->i420.data() + got, f->i420.size() - got);
                if (n <= 0) return;
                got += size_t(n);
            }
            std::lock_guard<std::mutex> l(mu_);
            frame_ = std::move(f);
            got_ = now();
        }
    }

    std::mutex mu_;
    std::thread thread_;
    bool running_ = false;
    int fd_ = -1;
    bool warned_ = false;
    int w_ = 0, h_ = 0, fps_ = 30;
    std::shared_ptr<const Frame> frame_, bars_;
    int64_t got_ = 0;
};

// scale draws src into a w×h picture with the same aspect ratio, cropped
// at the center, reading through get and writing through put.
template <typename Put>
void scale(const Frame& src, int w, int h, Put put) {
    double sa = double(src.w) / src.h, da = double(w) / h;
    int cw = src.w, ch = src.h;
    if (sa > da) cw = int(src.h * da);
    else ch = int(src.w / da);
    int ox = (src.w - cw) / 2, oy = (src.h - ch) / 2;
    const uint8_t* sy = src.i420.data();
    const uint8_t* su = sy + src.w * src.h;
    const uint8_t* sv = su + (src.w / 2) * (src.h / 2);
    for (int r = 0; r < h; r++) {
        int yr = oy + r * ch / h;
        for (int c = 0; c < w; c++) {
            int xc = ox + c * cw / w;
            put(r, c, sy[yr * src.w + xc], su[(yr / 2) * (src.w / 2) + xc / 2], sv[(yr / 2) * (src.w / 2) + xc / 2]);
        }
    }
}

struct Crop {
    std::vector<int> x, y;
};

// crop maps each destination column and row to a source one, keeping the
// aspect ratio and cutting the rest at the center.
Crop crop(int sw, int sh, int w, int h) {
    double sa = double(sw) / sh, da = double(w) / h;
    int cw = sw, ch = sh;
    if (sa > da) cw = int(sh * da);
    else ch = int(sw / da);
    int ox = (sw - cw) / 2, oy = (sh - ch) / 2;
    Crop c;
    c.x.resize(w);
    c.y.resize(h);
    for (int i = 0; i < w; i++) c.x[i] = ox + int(int64_t(i) * cw / w);
    for (int i = 0; i < h; i++) c.y[i] = oy + int(int64_t(i) * ch / h);
    return c;
}

void plane(const uint8_t* src, int sstride, const Crop& m, const AHardwareBuffer_Plane& d, int w, int h, bool same) {
    auto* dst = static_cast<uint8_t*>(d.data);
    for (int r = 0; r < h; r++) {
        const uint8_t* s = src + size_t(m.y[r]) * sstride;
        uint8_t* o = dst + size_t(r) * d.rowStride;
        if (same && d.pixelStride == 1) {
            memcpy(o, s, w);
        } else if (d.pixelStride == 1) {
            for (int c = 0; c < w; c++) o[c] = s[m.x[c]];
        } else {
            for (int c = 0; c < w; c++) o[c * d.pixelStride] = s[m.x[c]];
        }
    }
}

void fillYuv(const Frame& src, const AHardwareBuffer_Planes& p, int w, int h) {
    bool same = src.w == w && src.h == h;
    Crop luma = crop(src.w, src.h, w, h), chroma = crop(src.w / 2, src.h / 2, w / 2, h / 2);
    const uint8_t* sy = src.i420.data();
    const uint8_t* su = sy + src.w * src.h;
    const uint8_t* sv = su + (src.w / 2) * (src.h / 2);
    plane(sy, src.w, luma, p.planes[0], w, h, same);
    plane(su, src.w / 2, chroma, p.planes[1], w / 2, h / 2, same);
    plane(sv, src.w / 2, chroma, p.planes[2], w / 2, h / 2, same);
}

std::vector<uint8_t> jpeg(const Frame& src, int w, int h, int quality) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    scale(src, w, h, [&](int r, int c, uint8_t y, uint8_t u, uint8_t v) {
        float Yf = 1.164f * (y - 16), Uf = u - 128.0f, Vf = v - 128.0f;
        uint8_t* px = &rgb[(size_t(r) * w + c) * 3];
        px[0] = uint8_t(std::clamp(Yf + 1.596f * Vf, 0.0f, 255.0f));
        px[1] = uint8_t(std::clamp(Yf - 0.392f * Uf - 0.813f * Vf, 0.0f, 255.0f));
        px[2] = uint8_t(std::clamp(Yf + 2.017f * Uf, 0.0f, 255.0f));
    });
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* o = static_cast<std::vector<uint8_t>*>(ctx);
            o->insert(o->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        },
        &out, w, h, 3, rgb.data(), quality);
    return out;
}

// The handle helpers live in libcutils, which Android 13 and 14 keep in the
// VNDK; the layout is fixed, so the HAL makes its own handles.
native_handle_t* newHandle(int fds, int ints) {
    auto* n = static_cast<native_handle_t*>(malloc(sizeof(native_handle_t) + sizeof(int) * (fds + ints)));
    n->version = sizeof(native_handle_t);
    n->numFds = fds;
    n->numInts = ints;
    return n;
}

void freeHandle(native_handle_t* n) {
    for (int i = 0; i < n->numFds; i++) close(n->data[i]);
    free(n);
}

native_handle_t* fromAidl(const NativeHandle& h) {
    native_handle_t* n = newHandle(int(h.fds.size()), int(h.ints.size()));
    for (size_t i = 0; i < h.fds.size(); i++) n->data[i] = dup(h.fds[i].get());
    for (size_t i = 0; i < h.ints.size(); i++) n->data[h.fds.size() + i] = h.ints[i];
    return n;
}

struct PendingBuffer {
    int32_t streamId = 0;
    int64_t bufferId = 0;
    native_handle_t* handle = nullptr;
    int fence = -1;
};

struct Pending {
    int32_t frameNumber = 0;
    device::CameraMetadata settings;
    std::vector<PendingBuffer> buffers;
};

Pending pending(const device::CaptureRequest& r) {
    Pending p{r.frameNumber, r.settings, {}};
    for (auto& b : r.outputBuffers) {
        PendingBuffer o{b.streamId, b.bufferId, nullptr, -1};
        if (!b.buffer.fds.empty()) o.handle = fromAidl(b.buffer);
        if (!b.acquireFence.fds.empty()) o.fence = dup(b.acquireFence.fds[0].get());
        p.buffers.push_back(o);
    }
    return p;
}

void release(Pending& p) {
    for (auto& b : p.buffers) {
        if (b.handle) {
            freeHandle(b.handle);
            b.handle = nullptr;
        }
        if (b.fence >= 0) close(b.fence);
        b.fence = -1;
    }
}

void waitFence(int fd) {
    if (fd < 0) return;
    pollfd p{fd, POLLIN, 0};
    poll(&p, 1, 3000);
}

class Session : public device::BnCameraDeviceSession {
  public:
    Session(std::shared_ptr<device::ICameraDeviceCallback> cb) : cb_(std::move(cb)) {
        worker_ = std::thread([this] { work(); });
    }
    ~Session() override { shutdown(); }

    ScopedAStatus close() override {
        shutdown();
        return ScopedAStatus::ok();
    }

    ScopedAStatus configureStreams(const device::StreamConfiguration& cfg, std::vector<device::HalStream>* out) override {
        std::unique_lock<std::mutex> l(mu_);
        idle_.wait(l, [this] { return queue_.empty() && !busy_; });
        int fw = 0, fh = 0;
        std::map<int, device::Stream> streams;
        for (auto& s : cfg.streams) {
            if (s.streamType != device::StreamType::OUTPUT) return error(common::Status::ILLEGAL_ARGUMENT);
            bool blob = s.format == PixelFormat::BLOB;
            if (!blob && s.format != PixelFormat::IMPLEMENTATION_DEFINED && s.format != PixelFormat::YCBCR_420_888) {
                return error(common::Status::ILLEGAL_ARGUMENT);
            }
            streams[s.id] = s;
            if (int64_t(s.width) * s.height > int64_t(fw) * fh) fw = s.width, fh = s.height;
            device::HalStream h;
            h.id = s.id;
            h.overrideFormat = s.format == PixelFormat::IMPLEMENTATION_DEFINED ? PixelFormat::YCBCR_420_888 : s.format;
            h.producerUsage = BufferUsage(int64_t(BufferUsage::CPU_WRITE_OFTEN) | int64_t(BufferUsage::CPU_READ_OFTEN));
            h.maxBuffers = 4;
            h.overrideDataSpace = s.dataSpace;
            out->push_back(h);
        }
        for (auto it = buffers_.begin(); it != buffers_.end();) {
            if (!streams.count(it->first.first)) {
                AHardwareBuffer_release(it->second);
                it = buffers_.erase(it);
            } else {
                ++it;
            }
        }
        streams_ = std::move(streams);
        if (fw > 0) feed_.start(std::min(fw, kMaxW), std::min(fh, kMaxH), 30);
        LOG("configured %zu streams, feed %dx%d", streams_.size(), fw, fh);
        return ScopedAStatus::ok();
    }

    ScopedAStatus constructDefaultRequestSettings(device::RequestTemplate t, device::CameraMetadata* out) override {
        *out = defaults(t).aidl();
        return ScopedAStatus::ok();
    }

    ScopedAStatus flush() override {
        std::deque<Pending> dropped;
        {
            std::lock_guard<std::mutex> l(mu_);
            dropped.swap(queue_);
        }
        for (auto& r : dropped) {
            fail(r);
            release(r);
        }
        std::unique_lock<std::mutex> l(mu_);
        idle_.wait(l, [this] { return queue_.empty() && !busy_; });
        return ScopedAStatus::ok();
    }

    ScopedAStatus getCaptureRequestMetadataQueue(MQDescriptor<int8_t, SynchronizedReadWrite>*) override {
        return ScopedAStatus::ok();
    }
    ScopedAStatus getCaptureResultMetadataQueue(MQDescriptor<int8_t, SynchronizedReadWrite>*) override {
        return ScopedAStatus::ok();
    }

    ScopedAStatus isReconfigurationRequired(const device::CameraMetadata&, const device::CameraMetadata&, bool* out) override {
        *out = true;
        return ScopedAStatus::ok();
    }

    ScopedAStatus processCaptureRequest(const std::vector<device::CaptureRequest>& reqs, const std::vector<device::BufferCache>& remove,
                                        int32_t* out) override {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& c : remove) {
            auto it = buffers_.find({c.streamId, c.bufferId});
            if (it != buffers_.end()) {
                AHardwareBuffer_release(it->second);
                buffers_.erase(it);
            }
        }
        for (auto& r : reqs) queue_.push_back(pending(r));
        *out = int32_t(reqs.size());
        cv_.notify_all();
        return ScopedAStatus::ok();
    }

    ScopedAStatus repeatingRequestEnd(int32_t, const std::vector<int32_t>&) override { return ScopedAStatus::ok(); }
    ScopedAStatus signalStreamFlush(const std::vector<int32_t>&, int32_t) override { return ScopedAStatus::ok(); }
    ScopedAStatus switchToOffline(const std::vector<int32_t>&, device::CameraOfflineSessionInfo*,
                                  std::shared_ptr<device::ICameraOfflineSession>*) override {
        return error(common::Status::OPERATION_NOT_SUPPORTED);
    }

  private:
    void shutdown() {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (closed_) return;
            closed_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        std::deque<Pending> left;
        {
            std::lock_guard<std::mutex> l(mu_);
            left.swap(queue_);
        }
        for (auto& r : left) {
            fail(r);
            release(r);
        }
        feed_.stop();
        std::lock_guard<std::mutex> l(mu_);
        for (auto& [k, b] : buffers_) AHardwareBuffer_release(b);
        buffers_.clear();
        LOG("session closed");
    }

    void work() {
        int64_t next = now();
        while (true) {
            Pending r;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return closed_ || !queue_.empty(); });
                if (closed_) return;
                r = std::move(queue_.front());
                queue_.pop_front();
                busy_ = true;
            }
            Meta settings(r.settings);
            if (settings.empty()) settings = last_;
            else last_ = settings;
            int fps = std::clamp(settings.i32(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, 1, 30), 1, 30);
            int64_t t = now();
            if (next > t) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(next - t));
                t = now();
            }
            next = std::max(next, t) + 1000000000LL / fps;
            capture(r, settings, t);
            release(r);
            std::lock_guard<std::mutex> l(mu_);
            busy_ = false;
            idle_.notify_all();
        }
    }

    AHardwareBuffer* buffer(const PendingBuffer& b, const device::Stream& s) {
        std::lock_guard<std::mutex> l(mu_);
        auto key = std::make_pair(b.streamId, b.bufferId);
        auto it = buffers_.find(key);
        if (it != buffers_.end() && b.handle == nullptr) return it->second;
        if (b.handle == nullptr) return nullptr;
        if (it != buffers_.end()) {
            AHardwareBuffer_release(it->second);
            buffers_.erase(it);
        }
        bool blob = s.format == PixelFormat::BLOB;
        AHardwareBuffer_Desc d{};
        d.width = blob ? uint32_t(s.bufferSize > 0 ? s.bufferSize : kJpegMaxSize) : uint32_t(s.width);
        d.height = blob ? 1 : uint32_t(s.height);
        d.layers = 1;
        d.format = blob ? AHARDWAREBUFFER_FORMAT_BLOB : AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
        d.usage = uint64_t(s.usage) | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        AHardwareBuffer* ahb = nullptr;
        int err = AHardwareBuffer_createFromHandle(&d, b.handle, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
        if (err != 0) {
            ERR("import buffer %lld of stream %d: %d", (long long)b.bufferId, b.streamId, err);
            return nullptr;
        }
        buffers_[key] = ahb;
        return ahb;
    }

    bool write(const PendingBuffer& b, const Frame& frame, const Meta& settings) {
        device::Stream s;
        {
            std::lock_guard<std::mutex> l(mu_);
            auto it = streams_.find(b.streamId);
            if (it == streams_.end()) return false;
            s = it->second;
        }
        AHardwareBuffer* ahb = buffer(b, s);
        if (ahb == nullptr) return false;
        waitFence(b.fence);
        if (s.format == PixelFormat::BLOB) {
            void* p = nullptr;
            if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p) != 0) return false;
            AHardwareBuffer_Desc d;
            AHardwareBuffer_describe(ahb, &d);
            auto img = jpeg(frame, s.width, s.height, settings.u8(ANDROID_JPEG_QUALITY, 90));
            size_t cap = d.width;
            bool ok = img.size() + sizeof(device::CameraBlob) <= cap;
            if (ok) {
                memcpy(p, img.data(), img.size());
                struct {
                    int32_t id;
                    int32_t size;
                } trailer{int32_t(device::CameraBlobId::JPEG), int32_t(img.size())};
                memcpy(static_cast<uint8_t*>(p) + cap - sizeof(trailer), &trailer, sizeof(trailer));
            }
            AHardwareBuffer_unlock(ahb, nullptr);
            return ok;
        }
        AHardwareBuffer_Planes planes;
        if (AHardwareBuffer_lockPlanes(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &planes) != 0) return false;
        fillYuv(frame, planes, s.width, s.height);
        AHardwareBuffer_unlock(ahb, nullptr);
        return true;
    }

    void capture(const Pending& r, Meta settings, int64_t t) {
        device::NotifyMsg shutter;
        shutter.set<device::NotifyMsg::shutter>(device::ShutterMsg{r.frameNumber, t, t});
        cb_->notify({shutter});

        auto frame = feed_.latest();
        device::CaptureResult res;
        res.frameNumber = r.frameNumber;
        res.partialResult = 1;
        res.inputBuffer.streamId = -1;
        std::vector<device::NotifyMsg> errors;
        for (auto& b : r.buffers) {
            device::StreamBuffer o;
            o.streamId = b.streamId;
            o.bufferId = b.bufferId;
            o.status = write(b, *frame, settings) ? device::BufferStatus::OK : device::BufferStatus::ERROR;
            if (o.status == device::BufferStatus::ERROR) {
                device::NotifyMsg e;
                e.set<device::NotifyMsg::error>(device::ErrorMsg{r.frameNumber, b.streamId, device::ErrorCode::ERROR_BUFFER});
                errors.push_back(e);
            }
            res.outputBuffers.push_back(std::move(o));
        }
        if (!errors.empty()) cb_->notify(errors);
        settings.set<int64_t>(ANDROID_SENSOR_TIMESTAMP, t);
        settings.set<uint8_t>(ANDROID_CONTROL_AE_STATE, ANDROID_CONTROL_AE_STATE_CONVERGED);
        settings.set<uint8_t>(ANDROID_CONTROL_AF_STATE, ANDROID_CONTROL_AF_STATE_INACTIVE);
        settings.set<uint8_t>(ANDROID_CONTROL_AWB_STATE, ANDROID_CONTROL_AWB_STATE_CONVERGED);
        settings.set<uint8_t>(ANDROID_FLASH_STATE, ANDROID_FLASH_STATE_UNAVAILABLE);
        settings.set<uint8_t>(ANDROID_LENS_STATE, ANDROID_LENS_STATE_STATIONARY);
        settings.set<uint8_t>(ANDROID_REQUEST_PIPELINE_DEPTH, 2);
        settings.set<int64_t>(ANDROID_SENSOR_ROLLING_SHUTTER_SKEW, 10000000);
        settings.set<uint8_t>(ANDROID_STATISTICS_SCENE_FLICKER, ANDROID_STATISTICS_SCENE_FLICKER_NONE);
        res.result = settings.aidl();
        std::vector<device::CaptureResult> out;
        out.push_back(std::move(res));
        cb_->processCaptureResult(out);
    }

    void fail(const Pending& r) {
        device::NotifyMsg e;
        e.set<device::NotifyMsg::error>(device::ErrorMsg{r.frameNumber, -1, device::ErrorCode::ERROR_REQUEST});
        cb_->notify({e});
        device::CaptureResult res;
        res.frameNumber = r.frameNumber;
        res.inputBuffer.streamId = -1;
        for (auto& b : r.buffers) {
            device::StreamBuffer o;
            o.streamId = b.streamId;
            o.bufferId = b.bufferId;
            o.status = device::BufferStatus::ERROR;
            res.outputBuffers.push_back(std::move(o));
        }
        std::vector<device::CaptureResult> out;
        out.push_back(std::move(res));
        cb_->processCaptureResult(out);
    }

    std::shared_ptr<device::ICameraDeviceCallback> cb_;
    std::mutex mu_;
    std::condition_variable cv_, idle_;
    std::deque<Pending> queue_;
    std::map<int, device::Stream> streams_;
    std::map<std::pair<int, int64_t>, AHardwareBuffer*> buffers_;
    bool closed_ = false, busy_ = false;
    Meta last_;
    Feed feed_;
    std::thread worker_;
};

class Device : public device::BnCameraDevice {
  public:
    ScopedAStatus getCameraCharacteristics(device::CameraMetadata* out) override {
        *out = characteristics().aidl();
        return ScopedAStatus::ok();
    }
    ScopedAStatus getPhysicalCameraCharacteristics(const std::string&, device::CameraMetadata*) override {
        return error(common::Status::ILLEGAL_ARGUMENT);
    }
    ScopedAStatus getResourceCost(common::CameraResourceCost* out) override {
        out->resourceCost = 100;
        return ScopedAStatus::ok();
    }
    ScopedAStatus isStreamCombinationSupported(const device::StreamConfiguration& cfg, bool* out) override {
        int blobs = 0, others = 0;
        *out = true;
        for (auto& s : cfg.streams) {
            bool known = std::find(kSizes.begin(), kSizes.end(), std::make_pair(s.width, s.height)) != kSizes.end();
            if (s.format == PixelFormat::BLOB) blobs++;
            else if (s.format == PixelFormat::IMPLEMENTATION_DEFINED || s.format == PixelFormat::YCBCR_420_888) others++;
            else *out = false;
            if (!known || s.streamType != device::StreamType::OUTPUT) *out = false;
        }
        if (blobs > 1 || others > 3) *out = false;
        return ScopedAStatus::ok();
    }
    ScopedAStatus open(const std::shared_ptr<device::ICameraDeviceCallback>& cb,
                       std::shared_ptr<device::ICameraDeviceSession>* out) override {
        std::lock_guard<std::mutex> l(mu_);
        if (auto s = session_.lock()) s->close();
        auto s = ndk::SharedRefBase::make<Session>(cb);
        session_ = s;
        *out = s;
        LOG("opened");
        return ScopedAStatus::ok();
    }
    ScopedAStatus openInjectionSession(const std::shared_ptr<device::ICameraDeviceCallback>&,
                                       std::shared_ptr<device::ICameraInjectionSession>*) override {
        return error(common::Status::OPERATION_NOT_SUPPORTED);
    }
    ScopedAStatus setTorchMode(bool) override { return error(common::Status::OPERATION_NOT_SUPPORTED); }
    ScopedAStatus turnOnTorchWithStrengthLevel(int32_t) override { return error(common::Status::OPERATION_NOT_SUPPORTED); }
    ScopedAStatus getTorchStrengthLevel(int32_t*) override { return error(common::Status::OPERATION_NOT_SUPPORTED); }

  private:
    std::mutex mu_;
    std::weak_ptr<Session> session_;
};

class Provider : public provider::BnCameraProvider {
  public:
    ScopedAStatus setCallback(const std::shared_ptr<provider::ICameraProviderCallback>&) override { return ScopedAStatus::ok(); }
    ScopedAStatus getVendorTags(std::vector<common::VendorTagSection>*) override { return ScopedAStatus::ok(); }
    ScopedAStatus getCameraIdList(std::vector<std::string>* out) override {
        out->push_back(kDeviceName);
        return ScopedAStatus::ok();
    }
    ScopedAStatus getCameraDeviceInterface(const std::string& name, std::shared_ptr<device::ICameraDevice>* out) override {
        if (name != kDeviceName) return error(common::Status::ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> l(mu_);
        if (!device_) device_ = ndk::SharedRefBase::make<Device>();
        *out = device_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus notifyDeviceStateChange(int64_t) override { return ScopedAStatus::ok(); }
    ScopedAStatus getConcurrentCameraIds(std::vector<provider::ConcurrentCameraIdCombination>*) override { return ScopedAStatus::ok(); }
    ScopedAStatus isConcurrentStreamCombinationSupported(const std::vector<provider::CameraIdAndStreamCombination>&, bool* out) override {
        *out = false;
        return ScopedAStatus::ok();
    }

  private:
    std::mutex mu_;
    std::shared_ptr<Device> device_;
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    auto p = ndk::SharedRefBase::make<Provider>();
    binder_status_t s = AServiceManager_registerLazyService(p->asBinder().get(), kInstance);
    if (s != STATUS_OK) {
        ERR("register %s: %d", kInstance, s);
        return 1;
    }
    LOG("registered %s", kInstance);
    ABinderProcess_joinThreadPool();
    return 0;
}
