#include "audio_capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>

#include <cstring>

namespace recmeet {

// Implementation struct — defined here in the .cpp, used via opaque pointer.
struct PwCaptureImpl {
    std::string target;
    bool capture_sink;

    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;

    std::mutex buf_mtx;
    std::vector<int16_t> buffer;
    bool running = false;
};

static void on_process(void* userdata) {
    auto* impl = static_cast<PwCaptureImpl*>(userdata);
    pw_buffer* b = pw_stream_dequeue_buffer(impl->stream);
    if (!b) return;

    spa_buffer* buf = b->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(impl->stream, b);
        return;
    }

    auto* samples = static_cast<const int16_t*>(buf->datas[0].data);
    uint32_t n_bytes = buf->datas[0].chunk->size;
    uint32_t n_samples = n_bytes / sizeof(int16_t);

    {
        std::lock_guard lk(impl->buf_mtx);
        impl->buffer.insert(impl->buffer.end(), samples, samples + n_samples);
    }

    pw_stream_queue_buffer(impl->stream, b);
}

static void on_state_changed(void* userdata, enum pw_stream_state old_state,
                              enum pw_stream_state state, const char* error) {
    (void)old_state;
    (void)error;
    auto* impl = static_cast<PwCaptureImpl*>(userdata);
    if (state == PW_STREAM_STATE_STREAMING)
        impl->running = true;
    else if (state == PW_STREAM_STATE_UNCONNECTED || state == PW_STREAM_STATE_ERROR)
        impl->running = false;
}

static const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

// Remap Impl pointer type
struct PipeWireCapture::Impl : PwCaptureImpl {};

PipeWireCapture::PipeWireCapture(const std::string& target, bool capture_sink)
    : impl_(new Impl) {
    impl_->target = target;
    impl_->capture_sink = capture_sink;
    pw_init(nullptr, nullptr);
}

PipeWireCapture::~PipeWireCapture() {
    stop();
    delete impl_;
}

void PipeWireCapture::start() {
    impl_->loop = pw_thread_loop_new("recmeet-capture", nullptr);
    if (!impl_->loop)
        throw RecmeetError("Failed to create PipeWire thread loop");

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        nullptr);

    // Target the specific source
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, impl_->target.c_str());

    // For sink monitor capture: tell PipeWire to capture the sink's output
    if (impl_->capture_sink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    impl_->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(impl_->loop),
        "recmeet-audio",
        props,
        &stream_events,
        static_cast<PwCaptureImpl*>(impl_));

    if (!impl_->stream) {
        pw_thread_loop_destroy(impl_->loop);
        impl_->loop = nullptr;
        throw RecmeetError("Failed to create PipeWire stream");
    }

    // Set up audio format: S16LE, 16kHz, mono
    uint8_t param_buf[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, param_buf, sizeof(param_buf));

    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.rate = SAMPLE_RATE;
    info.channels = CHANNELS;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    int ret = pw_stream_connect(
        impl_->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (ret < 0) {
        pw_stream_destroy(impl_->stream);
        impl_->stream = nullptr;
        pw_thread_loop_destroy(impl_->loop);
        impl_->loop = nullptr;
        throw RecmeetError("Failed to connect PipeWire stream: " + std::string(strerror(-ret)));
    }

    pw_thread_loop_start(impl_->loop);
}

void PipeWireCapture::stop() {
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop);
    }
    if (impl_->stream) {
        pw_stream_destroy(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->loop) {
        pw_thread_loop_destroy(impl_->loop);
        impl_->loop = nullptr;
    }
    impl_->running = false;
}

std::vector<int16_t> PipeWireCapture::drain() {
    std::lock_guard lk(impl_->buf_mtx);
    std::vector<int16_t> out;
    out.swap(impl_->buffer);
    return out;
}

bool PipeWireCapture::is_running() const {
    return impl_->running;
}

} // namespace recmeet
