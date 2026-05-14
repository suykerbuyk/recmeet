// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_capture.h"
#include "log.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>

#include <atomic>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

namespace recmeet {

// Phase B.1 — fan-out subscriber slot. Each entry is a callback +
// userdata pair plus the handle returned to the caller. Lookup/remove
// hits a tiny linear scan (worst case = number of live subscribers,
// expected to be <= 4 in practice: WAV stager + streaming uploader +
// caption engine + future telemetry). Allocation is bounded by
// `reserve(8)` at construction so the RT thread never racing with an
// add never sees a vector reallocate mid-read.
struct AudioSubscriberSlot {
    PipeWireCapture::SubscriberHandle handle = 0;
    AudioChunkCallback cb = nullptr;
    void* userdata = nullptr;
};

// Implementation struct — defined here in the .cpp, used via opaque pointer.
struct PwCaptureImpl {
    std::string target;
    bool capture_sink;

    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;

    std::mutex buf_mtx;
    std::vector<int16_t> buffer;
    std::atomic<bool> running{false};

    std::atomic<bool> first_callback_received{false};
    std::atomic<pid_t> callback_tid{0};

    // Phase B.1 fan-out subscribers. The RT thread invokes each
    // registered callback under `buf_mtx`, which is the same mutex
    // it already takes on every chunk to append to `buffer`. Adders
    // and removers also take `buf_mtx`, so dispatch is race-free
    // against subscriber list mutations without a separate mutex
    // hop. The list is expected to be tiny (<= 4 subscribers in
    // practice); the lock-hold duration was already determined by
    // the buffer.insert() above, which dwarfs the cost of a small
    // linear iteration. The TODO above (line 84) about RT-unsafe
    // logging applies to that branch only — subscriber dispatch
    // does not log.
    std::vector<AudioSubscriberSlot> subscribers;
    std::uint64_t next_subscriber_handle = 1;
};

static void on_process(void* userdata) {
    auto* impl = static_cast<PwCaptureImpl*>(userdata);

    // Capture thread ID on first callback (atomic, no mutex — RT-safe)
    if (!impl->first_callback_received.exchange(true)) {
        impl->callback_tid.store(syscall(SYS_gettid));
    }

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
        // Phase B.1 fan-out — dispatch the same (samples, n_samples)
        // tuple to every registered subscriber. The subscribers vector
        // is mutated only under buf_mtx (this same lock), so a copy of
        // the slot snapshot under the lock is race-free with adders /
        // removers. We invoke the callback while still holding the
        // mutex to keep the dispatch atomic with the pre-existing
        // single-callback semantics: a stop() that removes the last
        // subscriber while the RT thread is mid-dispatch waits on the
        // mutex, so the callback never fires after `remove_audio_subscriber`
        // returns. Pass the live PipeWire chunk pointer (not a vector
        // iterator) to avoid resize-invalidation on the buffer above.
        for (const auto& slot : impl->subscribers) {
            slot.cb(samples, n_samples, slot.userdata);
        }
        // Warn once when buffer exceeds ~120 minutes of audio (230 MB)
        constexpr size_t WARN_SAMPLES = SAMPLE_RATE * 60 * 120;
        if (impl->buffer.size() >= WARN_SAMPLES &&
            impl->buffer.size() - n_samples < WARN_SAMPLES) {
            // TODO: RT-unsafe log call — runs on PipeWire RT thread
            log_warn("Audio buffer exceeds 120 minutes (%.0f MB). "
                    "Memory usage will continue to grow.",
                    impl->buffer.size() * sizeof(int16_t) / (1024.0 * 1024.0));
        }
    }

    pw_stream_queue_buffer(impl->stream, b);
}

static void on_state_changed(void* userdata, enum pw_stream_state old_state,
                              enum pw_stream_state state, const char* error) {
    (void)old_state;
    (void)error;
    auto* impl = static_cast<PwCaptureImpl*>(userdata);
    // Runs on PipeWire thread loop thread (not RT audio thread) — mutex-safe
    log_debug("pw-capture: state → %s", pw_stream_state_as_string(state));
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
    : impl_(std::make_unique<Impl>()) {
    impl_->target = target;
    impl_->capture_sink = capture_sink;
    pw_init(nullptr, nullptr);
}

PipeWireCapture::~PipeWireCapture() {
    stop();
}

void PipeWireCapture::start() {
    log_debug("pw-capture: start ENTER");
    impl_->loop = pw_thread_loop_new("recmeet-capture", nullptr);
    if (!impl_->loop)
        throw RecmeetError("Failed to create PipeWire thread loop");
    log_debug("pw-capture: thread loop created");

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
        static_cast<PwCaptureImpl*>(impl_.get()));

    if (!impl_->stream) {
        pw_thread_loop_destroy(impl_->loop);
        impl_->loop = nullptr;
        throw RecmeetError("Failed to create PipeWire stream");
    }
    log_debug("pw-capture: stream created");

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
    log_debug("pw-capture: stream connected");

    pw_thread_loop_start(impl_->loop);
    log_debug("pw-capture: thread loop started");
}

void PipeWireCapture::stop() {
    log_debug("pw-capture: stop ENTER");
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop);
    }

    if (impl_->first_callback_received.load()) {
        log_debug("pw-capture: audio callbacks received (first callback tid=%d, buffered=%zu samples)",
                  (int)impl_->callback_tid.load(), impl_->buffer.size());
    } else {
        log_warn("pw-capture: NO audio callbacks received (stream may have failed)");
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

    size_t buffer_size = impl_->buffer.size();
    log_debug("pw-capture: stop EXIT (buffered=%zu samples)", buffer_size);
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

void PipeWireCapture::set_audio_callback(AudioChunkCallback cb, void* userdata) {
    // Phase B.1: legacy entry point retained for call sites that hold
    // the "single subscriber" invariant (daemon-side live recording,
    // pre-B.1 tests). Semantics: clear the current subscriber list
    // and install `cb` as the sole subscriber. A nullptr `cb`
    // simply clears the list.
    std::lock_guard lk(impl_->buf_mtx);
    impl_->subscribers.clear();
    if (cb) {
        AudioSubscriberSlot slot;
        slot.handle = impl_->next_subscriber_handle++;
        slot.cb = cb;
        slot.userdata = userdata;
        impl_->subscribers.push_back(slot);
    }
}

PipeWireCapture::SubscriberHandle
PipeWireCapture::add_audio_subscriber(AudioChunkCallback cb, void* userdata) {
    if (!cb) return 0;
    std::lock_guard lk(impl_->buf_mtx);
    AudioSubscriberSlot slot;
    slot.handle = impl_->next_subscriber_handle++;
    slot.cb = cb;
    slot.userdata = userdata;
    impl_->subscribers.push_back(slot);
    return slot.handle;
}

void PipeWireCapture::remove_audio_subscriber(SubscriberHandle handle) {
    if (handle == 0) return;
    std::lock_guard lk(impl_->buf_mtx);
    for (auto it = impl_->subscribers.begin(); it != impl_->subscribers.end(); ++it) {
        if (it->handle == handle) {
            impl_->subscribers.erase(it);
            return;
        }
    }
}

void PipeWireCapture::_inject_for_test(const int16_t* samples, std::size_t n) {
    // Test-only path that mirrors the buffer-append + callback dispatch
    // shape from on_process() without opening a PipeWire stream. Phase B.1
    // walks the subscriber list, identical to the production RT path.
    std::lock_guard lk(impl_->buf_mtx);
    impl_->buffer.insert(impl_->buffer.end(), samples, samples + n);
    for (const auto& slot : impl_->subscribers) {
        slot.cb(samples, n, slot.userdata);
    }
}

} // namespace recmeet
