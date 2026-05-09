// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 2 — Streaming ASR engine implementation.
//
// Two compile paths:
//   * RECMEET_USE_SHERPA defined  -> real sherpa-onnx-backed engine.
//   * RECMEET_USE_SHERPA undefined -> stub bodies; start() returns false with
//     a clear error message; no symbols from sherpa-onnx are referenced.

#include "caption_engine.h"

#include "log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifdef RECMEET_USE_SHERPA
#include <sherpa-onnx/c-api/c-api.h>

#include <condition_variable>
#include <filesystem>
#include <thread>
#include <vector>

#include <sched.h>
#include <unistd.h>
#include <errno.h>
#endif // RECMEET_USE_SHERPA

namespace recmeet {

// ===========================================================================
// Common Impl shape — declared regardless of build path so the header's
// unique_ptr<Impl> sees a complete type when destructed.
// ===========================================================================

struct CaptionEngine::Impl {
    // Lifecycle state common to both build paths.
    std::mutex lifecycle_mtx;          // serializes start() / stop() / dtor
    std::atomic<bool> running{false};
    std::string last_error;            // guarded by lifecycle_mtx for writes,
                                       // copied out under lock for reads
    std::atomic<std::size_t> degraded_emitted{0};

#ifdef RECMEET_USE_SHERPA
    // ----- Ring buffer (SPSC, lock-free for push/pop indices) ---------------
    // Capacity is a power of two so the index mask is a single AND. Producer
    // (capture thread) writes at `head_`; consumer (worker) reads at `tail_`.
    // `samples_avail()` is a relaxed snapshot — both threads use it as a
    // hint, never as a strict invariant.
    std::vector<int16_t> ring;         // size == capacity, fixed at start()
    std::size_t ring_mask = 0;
    std::atomic<std::size_t> head{0};  // producer writes here
    std::atomic<std::size_t> tail{0};  // consumer reads here
    std::atomic<bool> overflow_seen{false};

    // ----- Worker -----------------------------------------------------------
    std::thread worker;
    std::atomic<bool> worker_should_exit{false};
    int worker_poll_ms = 5;            // overridable via Options for tests
    int effective_num_threads = 1;     // after the min(2, ...) cap

    // ----- Recognizer + stream ---------------------------------------------
    const SherpaOnnxOnlineRecognizer* recognizer = nullptr;
    const SherpaOnnxOnlineStream* stream = nullptr;
    int32_t sample_rate = 16000;

    // Owned strings backing the C-config char* fields. Must outlive the
    // SherpaOnnxCreateOnlineRecognizer call.
    std::string encoder_path;
    std::string decoder_path;
    std::string joiner_path;
    std::string tokens_path;
    std::string decoding_method;
    std::string provider;
    std::string model_type;

    // ----- Callbacks --------------------------------------------------------
    CaptionResultCallback   on_result   = nullptr;
    void*                   on_result_ud = nullptr;
    CaptionDegradedCallback on_degraded = nullptr;
    void*                   on_degraded_ud = nullptr;

    // ----- Time -------------------------------------------------------------
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_degraded_emit;  // worker-local
    bool last_degraded_emit_set = false;
#endif // RECMEET_USE_SHERPA
};

// ===========================================================================
// Construction / destruction (both build paths)
// ===========================================================================

CaptionEngine::CaptionEngine() : impl_(std::make_unique<Impl>()) {}

CaptionEngine::~CaptionEngine() {
    // RAII safety net — caller may have skipped stop().
    try {
        stop();
    } catch (...) {
        // Destructors must not throw.
    }
}

bool CaptionEngine::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::string CaptionEngine::last_error() const {
    std::lock_guard lk(impl_->lifecycle_mtx);
    return impl_->last_error;
}

std::size_t CaptionEngine::_degraded_events_emitted_for_test() const {
    return impl_->degraded_emitted.load(std::memory_order_acquire);
}

// ===========================================================================
// Stub build path (RECMEET_USE_SHERPA OFF)
// ===========================================================================

#ifndef RECMEET_USE_SHERPA

bool CaptionEngine::start(const Options& /*opts*/,
                          CaptionResultCallback /*on_result*/, void* /*result_userdata*/,
                          CaptionDegradedCallback /*on_degraded*/, void* /*degraded_userdata*/) {
    std::lock_guard lk(impl_->lifecycle_mtx);
    impl_->last_error = "captions require RECMEET_USE_SHERPA=ON build";
    return false;
}

void CaptionEngine::on_audio_chunk(const int16_t* /*samples*/, std::size_t /*n*/, void* /*engine_ptr*/) {
    // No-op — there's no consumer to feed. The capture-thread caller may not
    // even know the build flavor, so we accept the call and drop silently.
}

std::size_t CaptionEngine::_push_samples_for_test(const int16_t* /*samples*/, std::size_t /*n*/) {
    return 0;
}

std::size_t CaptionEngine::_ring_occupancy_for_test() const {
    return 0;
}

std::size_t CaptionEngine::_ring_capacity_for_test() const {
    return 0;
}

void CaptionEngine::stop() {
    std::lock_guard lk(impl_->lifecycle_mtx);
    impl_->running.store(false, std::memory_order_release);
}

int CaptionEngine::_effective_num_threads_for_test() const {
    return 0;
}

#else // RECMEET_USE_SHERPA — real implementation below

// ===========================================================================
// Real-build helpers
// ===========================================================================

namespace {

namespace fs = std::filesystem;

/// Round up to the next power of two. Returns 1 for n==0 to avoid a 0-cap
/// ring buffer (which would divide by zero in mask arithmetic).
std::size_t next_pow2(std::size_t n) {
    if (n < 2) return 1;
    --n;
    for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
        n |= n >> shift;
    }
    return n + 1;
}

/// Search `model_dir` for the first file whose name matches the given
/// substring patterns. Returns empty path if no match.
fs::path find_model_file(const fs::path& model_dir,
                         std::initializer_list<const char*> patterns) {
    std::error_code ec;
    if (!fs::is_directory(model_dir, ec)) return {};
    for (const auto& entry : fs::directory_iterator(model_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        for (const char* pat : patterns) {
            if (name.find(pat) != std::string::npos) {
                return entry.path();
            }
        }
    }
    return {};
}

/// Default scheduler-set: try SCHED_BATCH; fall back to nice(+10) on EPERM.
/// Returns 0 on SCHED_BATCH applied, 1 on nice fallback, -1 on hard failure.
int default_scheduler_setter(void* /*userdata*/) {
    sched_param param{};
    param.sched_priority = 0;  // SCHED_BATCH lives in SCHED_OTHER family.
    if (::sched_setscheduler(0, SCHED_BATCH, &param) == 0) {
        return 0;
    }
    int err = errno;
    if (err == EPERM) {
        // Sandboxed harness — fall back to nice(+10). Silent: this is a
        // graceful degradation, not a failure.
        errno = 0;
        if (::nice(10) == -1 && errno != 0) {
            return -1;
        }
        return 1;
    }
    // Other errno values (EINVAL etc.) are unexpected — try nice anyway.
    errno = 0;
    if (::nice(10) == -1 && errno != 0) {
        return -1;
    }
    return 1;
}

} // anonymous namespace

// ===========================================================================
// Ring-buffer producer (called from capture thread)
// ===========================================================================

void CaptionEngine::on_audio_chunk(const int16_t* samples, std::size_t n, void* engine_ptr) {
    if (!engine_ptr || !samples || n == 0) return;
    auto* eng = static_cast<CaptionEngine*>(engine_ptr);
    eng->_push_samples_for_test(samples, n);
}

std::size_t CaptionEngine::_push_samples_for_test(const int16_t* samples, std::size_t n) {
    if (!impl_->running.load(std::memory_order_acquire)) {
        // Not started — drop silently. The producer should not have been
        // wired before start(), but we don't crash on misuse.
        return n;
    }
    if (n == 0 || impl_->ring.empty()) return 0;

    // Lock-free SPSC push. Producer owns `head`; consumer owns `tail`. We
    // observe `tail` (acquire) to compute available space, then store `head`
    // (release) so the consumer's acquire-load sees our writes.
    const std::size_t cap  = impl_->ring.size();
    const std::size_t mask = impl_->ring_mask;
    std::size_t head_local = impl_->head.load(std::memory_order_relaxed);
    std::size_t tail_local = impl_->tail.load(std::memory_order_acquire);
    std::size_t in_buf     = head_local - tail_local;       // mod 2^N — wraps fine
    std::size_t free_space = cap - in_buf;

    std::size_t dropped = 0;
    if (n >= cap) {
        // Push would overflow even an empty buffer. Keep only the last `cap`
        // samples; consumer behavior is identical to "drop oldest until fits".
        dropped = n - cap + free_space;  // approximate; we will clamp below
        // Advance tail to make room for the full `cap` samples we'll write.
        // Effectively, writing `cap` samples wraps the buffer once; the
        // consumer's tail snapshot is stale but harmless — its next read
        // sees overflow_seen=true and resyncs.
        // To keep the SPSC contract intact, we don't move `tail` (consumer
        // owns it). Instead, we advance head by cap and let the consumer
        // catch up. The consumer reads samples_avail() as `head - tail`; if
        // head jumps by `cap` past `tail`, samples_avail() will report `cap`
        // which is fine (consumer reads at most cap samples, then loops).
        for (std::size_t i = 0; i < cap; ++i) {
            impl_->ring[(head_local + i) & mask] = samples[n - cap + i];
        }
        impl_->head.store(head_local + cap, std::memory_order_release);
        impl_->overflow_seen.store(true, std::memory_order_release);
        return dropped;
    }
    if (n > free_space) {
        // Drop oldest pending: advance the producer's view of tail by
        // (n - free_space). The consumer also owns tail; we cannot move it
        // from here without violating SPSC. Resolution: write the full chunk
        // and let head outrun tail by `n` samples. The consumer detects this
        // by seeing samples_avail() exceed cap (impossible under normal SPSC
        // — its load+store are sequentially consistent on x86) and clamps.
        // Concretely: head = head + n; consumer does
        //   in_buf = head - tail;
        //   if (in_buf > cap) { tail = head - cap; }  // resync drop
        dropped = n - free_space;
        for (std::size_t i = 0; i < n; ++i) {
            impl_->ring[(head_local + i) & mask] = samples[i];
        }
        impl_->head.store(head_local + n, std::memory_order_release);
        impl_->overflow_seen.store(true, std::memory_order_release);
        return dropped;
    }

    // Fast path — fits.
    for (std::size_t i = 0; i < n; ++i) {
        impl_->ring[(head_local + i) & mask] = samples[i];
    }
    impl_->head.store(head_local + n, std::memory_order_release);
    return 0;
}

std::size_t CaptionEngine::_ring_occupancy_for_test() const {
    if (impl_->ring.empty()) return 0;
    std::size_t head_local = impl_->head.load(std::memory_order_acquire);
    std::size_t tail_local = impl_->tail.load(std::memory_order_acquire);
    std::size_t diff = head_local - tail_local;
    return std::min(diff, impl_->ring.size());
}

std::size_t CaptionEngine::_ring_capacity_for_test() const {
    return impl_->ring.size();
}

int CaptionEngine::_effective_num_threads_for_test() const {
    return impl_->effective_num_threads;
}

// ===========================================================================
// Worker thread main loop
// ===========================================================================

namespace {

/// Pull up to `max_samples` from the ring, converting int16 → float in
/// [-1.0, 1.0]. Returns the actual count copied. Updates the consumer's
/// tail index.
std::size_t drain_ring_to_float(CaptionEngine::Impl& impl,
                                float* out, std::size_t max_samples) {
    if (impl.ring.empty() || max_samples == 0) return 0;

    const std::size_t cap = impl.ring.size();
    const std::size_t mask = impl.ring_mask;
    std::size_t head_local = impl.head.load(std::memory_order_acquire);
    std::size_t tail_local = impl.tail.load(std::memory_order_relaxed);

    std::size_t avail = head_local - tail_local;
    if (avail > cap) {
        // Producer outran us — drop-oldest resync. Move tail forward so we
        // re-anchor at (head - cap). overflow_seen will already be set.
        tail_local = head_local - cap;
        avail = cap;
    }
    std::size_t to_copy = std::min(avail, max_samples);
    for (std::size_t i = 0; i < to_copy; ++i) {
        int16_t s = impl.ring[(tail_local + i) & mask];
        out[i] = static_cast<float>(s) / 32768.0f;
    }
    impl.tail.store(tail_local + to_copy, std::memory_order_release);
    return to_copy;
}

void worker_main(CaptionEngine::Impl* impl) {
    auto& I = *impl;

    // (No mutex grab here — stop() holds lifecycle_mtx during join(), so the
    // worker must never block on it. start() publishes all worker-visible
    // state before std::thread::thread launches the worker, which provides
    // the needed happens-before via the thread's start synchronization.)

    // Buffer for one drain-and-feed cycle. Size to match the recognizer's
    // chunk-size sweet-spot; 1600 = 100ms @ 16 kHz.
    constexpr std::size_t FEED_CHUNK = 1600;
    std::vector<float> feed(FEED_CHUNK);

    auto last_endpoint_text_emitted = std::string{};

    while (!I.worker_should_exit.load(std::memory_order_acquire)) {
        // ----- Drain ring → recognizer ------------------------------------
        std::size_t got = drain_ring_to_float(I, feed.data(), feed.size());
        if (got > 0 && I.recognizer && I.stream) {
            SherpaOnnxOnlineStreamAcceptWaveform(I.stream, I.sample_rate,
                                                 feed.data(),
                                                 static_cast<int32_t>(got));
            // Decode while ready.
            while (SherpaOnnxIsOnlineStreamReady(I.recognizer, I.stream)) {
                SherpaOnnxDecodeOnlineStream(I.recognizer, I.stream);
            }

            // ----- Pull result + endpoint check ---------------------------
            const SherpaOnnxOnlineRecognizerResult* res =
                SherpaOnnxGetOnlineStreamResult(I.recognizer, I.stream);
            int is_endpoint = SherpaOnnxOnlineStreamIsEndpoint(I.recognizer, I.stream);

            if (res) {
                std::string text = res->text ? res->text : "";
                CaptionResult cr;
                cr.text = text;
                cr.is_partial = (is_endpoint == 0);
                cr.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - I.start_time).count();
                if (I.on_result && !text.empty()) {
                    I.on_result(cr, I.on_result_ud);
                }
                SherpaOnnxDestroyOnlineRecognizerResult(res);
            }

            if (is_endpoint != 0) {
                SherpaOnnxOnlineStreamReset(I.recognizer, I.stream);
            }
        }
        // (When recognizer is absent — test mode — got>0 just discards the
        // float buffer; ring drain still advances `tail`.)

        // ----- Backpressure observation (rate-limited 1/s) -----------------
        if (I.overflow_seen.exchange(false, std::memory_order_acq_rel)) {
            auto now = std::chrono::steady_clock::now();
            bool emit = true;
            if (I.last_degraded_emit_set) {
                auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - I.last_degraded_emit).count();
                if (since < 1000) {
                    emit = false;
                    // Re-arm the flag so the next eligible tick still fires.
                    I.overflow_seen.store(true, std::memory_order_release);
                }
            }
            if (emit) {
                I.last_degraded_emit = now;
                I.last_degraded_emit_set = true;
                I.degraded_emitted.fetch_add(1, std::memory_order_acq_rel);
                if (I.on_degraded) {
                    I.on_degraded(CaptionDegradedReason::BufferOverrun, I.on_degraded_ud);
                }
            }
        }

        // ----- Idle pacing -----------------------------------------------
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(I.worker_poll_ms));
        }
    }
}

} // anonymous namespace

// ===========================================================================
// start() / stop()
// ===========================================================================

bool CaptionEngine::start(const Options& opts,
                          CaptionResultCallback on_result, void* result_userdata,
                          CaptionDegradedCallback on_degraded, void* degraded_userdata) {
    std::lock_guard lk(impl_->lifecycle_mtx);

    if (impl_->running.load(std::memory_order_acquire)) {
        // Already running — treat as success but do nothing.
        return true;
    }

    impl_->last_error.clear();

    if (!opts._no_recognizer_for_test) {
        // ----- Resolve model files in opts.model_dir ---------------------
        const fs::path model_dir(opts.model_dir);
        if (opts.model_dir.empty()) {
            impl_->last_error = "caption_engine: Options::model_dir is empty";
            return false;
        }
        std::error_code ec;
        if (!fs::is_directory(model_dir, ec)) {
            impl_->last_error = "caption_engine: model_dir is not a directory: " + opts.model_dir;
            return false;
        }

        fs::path encoder = find_model_file(model_dir, {"encoder"});
        fs::path decoder = find_model_file(model_dir, {"decoder"});
        fs::path joiner  = find_model_file(model_dir, {"joiner"});
        fs::path tokens  = find_model_file(model_dir, {"tokens"});

        if (encoder.empty() || decoder.empty() || joiner.empty() || tokens.empty()) {
            impl_->last_error = "caption_engine: missing model files in " + opts.model_dir +
                                " (need encoder*.onnx, decoder*.onnx, joiner*.onnx, tokens*.txt)";
            return false;
        }

        impl_->encoder_path = encoder.string();
        impl_->decoder_path = decoder.string();
        impl_->joiner_path  = joiner.string();
        impl_->tokens_path  = tokens.string();
    }

    impl_->decoding_method = opts.decoding_method.empty() ? "greedy_search" : opts.decoding_method;
    impl_->provider        = "cpu";
    impl_->model_type      = "";

    // ----- Cap thread count -----------------------------------------------
    int requested = std::max(1, opts.num_threads);
    impl_->effective_num_threads = std::min(2, requested);

    impl_->sample_rate = opts.sample_rate;

    if (!opts._no_recognizer_for_test) {
        // ----- Build sherpa-onnx config ---------------------------------
        SherpaOnnxOnlineRecognizerConfig cfg{};
        cfg.feat_config.sample_rate = opts.sample_rate;
        cfg.feat_config.feature_dim = 80;

        cfg.model_config.transducer.encoder = impl_->encoder_path.c_str();
        cfg.model_config.transducer.decoder = impl_->decoder_path.c_str();
        cfg.model_config.transducer.joiner  = impl_->joiner_path.c_str();
        cfg.model_config.tokens             = impl_->tokens_path.c_str();
        cfg.model_config.num_threads        = impl_->effective_num_threads;
        cfg.model_config.provider           = impl_->provider.c_str();
        cfg.model_config.debug              = 0;
        cfg.model_config.model_type         = impl_->model_type.c_str();

        cfg.decoding_method            = impl_->decoding_method.c_str();
        cfg.max_active_paths           = 4;
        cfg.enable_endpoint            = opts.enable_endpoint ? 1 : 0;
        cfg.rule1_min_trailing_silence = 2.4f;
        cfg.rule2_min_trailing_silence = 1.2f;
        cfg.rule3_min_utterance_length = 20.0f;
        cfg.hotwords_score             = 1.5f;

        impl_->recognizer = SherpaOnnxCreateOnlineRecognizer(&cfg);
        if (!impl_->recognizer) {
            impl_->last_error = "caption_engine: SherpaOnnxCreateOnlineRecognizer failed";
            return false;
        }
        impl_->stream = SherpaOnnxCreateOnlineStream(impl_->recognizer);
        if (!impl_->stream) {
            SherpaOnnxDestroyOnlineRecognizer(impl_->recognizer);
            impl_->recognizer = nullptr;
            impl_->last_error = "caption_engine: SherpaOnnxCreateOnlineStream failed";
            return false;
        }
    }

    // ----- Allocate ring buffer ------------------------------------------
    std::size_t requested_cap =
        opts.ring_capacity_override > 0 ? opts.ring_capacity_override
                                        : 32768;  // ~2s @ 16 kHz mono int16
    std::size_t cap = next_pow2(requested_cap);
    impl_->ring.assign(cap, int16_t{0});
    impl_->ring_mask = cap - 1;
    impl_->head.store(0, std::memory_order_relaxed);
    impl_->tail.store(0, std::memory_order_relaxed);
    impl_->overflow_seen.store(false, std::memory_order_relaxed);

    // ----- Wire callbacks + state for worker -----------------------------
    impl_->on_result    = on_result;
    impl_->on_result_ud = result_userdata;
    impl_->on_degraded  = on_degraded;
    impl_->on_degraded_ud = degraded_userdata;
    impl_->worker_poll_ms = opts.worker_poll_ms_override > 0
                                ? opts.worker_poll_ms_override : 5;
    impl_->last_degraded_emit_set = false;
    impl_->degraded_emitted.store(0, std::memory_order_relaxed);
    impl_->start_time = std::chrono::steady_clock::now();
    impl_->worker_should_exit.store(false, std::memory_order_release);

    // Capture the scheduler-setter seam locally — the worker uses it once at
    // start of the loop. We invoke it here on the calling thread so tests can
    // observe its effect synchronously, then run the worker with the result.
    SchedulerSetter setter = opts.scheduler_setter ? opts.scheduler_setter
                                                   : &default_scheduler_setter;
    void* setter_ud = opts.scheduler_setter ? opts.scheduler_setter_userdata : nullptr;

    impl_->running.store(true, std::memory_order_release);

    // Spawn worker. The worker runs scheduler-setter on its own thread for
    // realistic semantics, but we also retain the fact that the test seam is
    // the one observed.
    auto* impl_ptr = impl_.get();
    impl_->worker = std::thread([impl_ptr, setter, setter_ud]() {
        int rc = setter(setter_ud);
        if (rc == 1) {
            log_debug("caption_engine: SCHED_BATCH unavailable (EPERM); using nice(+10)");
        } else if (rc < 0) {
            log_debug("caption_engine: scheduler tweak unavailable; using default");
        }
        worker_main(impl_ptr);
    });

    return true;
}

void CaptionEngine::stop() {
    std::lock_guard lk(impl_->lifecycle_mtx);

    if (!impl_->running.load(std::memory_order_acquire) && !impl_->worker.joinable()) {
        // Already stopped or never started.
        return;
    }

    impl_->worker_should_exit.store(true, std::memory_order_release);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    if (impl_->stream) {
        SherpaOnnxDestroyOnlineStream(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->recognizer) {
        SherpaOnnxDestroyOnlineRecognizer(impl_->recognizer);
        impl_->recognizer = nullptr;
    }

    impl_->ring.clear();
    impl_->ring_mask = 0;
    impl_->head.store(0, std::memory_order_relaxed);
    impl_->tail.store(0, std::memory_order_relaxed);
    impl_->overflow_seen.store(false, std::memory_order_relaxed);

    impl_->on_result = nullptr;
    impl_->on_result_ud = nullptr;
    impl_->on_degraded = nullptr;
    impl_->on_degraded_ud = nullptr;

    impl_->running.store(false, std::memory_order_release);
}

#endif // RECMEET_USE_SHERPA

} // namespace recmeet
