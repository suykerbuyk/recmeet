// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase B.1 — recmeet_capture library tests.
//
// Covers:
//   1. Fan-out subscriber delivery: a single _inject_for_test() chunk
//      reaches every registered subscriber with the same (samples, n)
//      tuple. Verifies the C.10 streaming-uploader hook will share the
//      same audio frames as the tray's WAV stager.
//   2. Subscriber add / remove correctness: handles are unique,
//      removed subscribers stop receiving frames, the legacy
//      set_audio_callback() wrapper preserves single-subscriber
//      semantics.
//   3. Handle hygiene: remove with handle 0 is a no-op; an unknown
//      handle is a no-op; clear via set_audio_callback(nullptr).

#include <catch2/catch_test_macros.hpp>

#include "audio_capture.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace recmeet;

namespace {

// Test sink — records every (samples, n) invocation as a flat vector.
// Plain ints avoid heap allocation during the callback for tests that
// care about the RT-safety contract; the helper itself is allocation-
// free under the per-call append.
struct SubscriberSink {
    int call_count = 0;
    std::vector<int16_t> received;     // flattened samples seen
    std::size_t last_n = 0;
    const int16_t* last_ptr = nullptr;
};

void sink_cb(const int16_t* samples, std::size_t n, void* userdata) {
    auto* s = static_cast<SubscriberSink*>(userdata);
    s->call_count++;
    s->last_n = n;
    s->last_ptr = samples;
    s->received.insert(s->received.end(), samples, samples + n);
}

} // anonymous namespace

TEST_CASE("B.1: add_audio_subscriber fan-outs the same frames to two sinks",
          "[capture][b1]") {
    PipeWireCapture cap("test-source");

    SubscriberSink a, b;
    auto ha = cap.add_audio_subscriber(&sink_cb, &a);
    auto hb = cap.add_audio_subscriber(&sink_cb, &b);

    REQUIRE(ha != 0);
    REQUIRE(hb != 0);
    REQUIRE(ha != hb);

    const int16_t chunk1[] = {1, 2, 3, 4, 5};
    cap._inject_for_test(chunk1, 5);

    const int16_t chunk2[] = {10, 20, 30};
    cap._inject_for_test(chunk2, 3);

    REQUIRE(a.call_count == 2);
    REQUIRE(b.call_count == 2);
    REQUIRE(a.received == b.received);
    REQUIRE(a.received.size() == 8);
    REQUIRE(a.received[0] == 1);
    REQUIRE(a.received[7] == 30);
}

TEST_CASE("B.1: remove_audio_subscriber stops further fires for that sink",
          "[capture][b1]") {
    PipeWireCapture cap("test-source");

    SubscriberSink a, b;
    auto ha = cap.add_audio_subscriber(&sink_cb, &a);
    auto hb = cap.add_audio_subscriber(&sink_cb, &b);

    const int16_t chunk[] = {7, 7, 7};
    cap._inject_for_test(chunk, 3);
    REQUIRE(a.call_count == 1);
    REQUIRE(b.call_count == 1);

    cap.remove_audio_subscriber(ha);
    cap._inject_for_test(chunk, 3);

    CHECK(a.call_count == 1);   // gone after remove
    CHECK(b.call_count == 2);

    // Removing again is harmless; removing zero is harmless.
    cap.remove_audio_subscriber(ha);
    cap.remove_audio_subscriber(0);
    cap._inject_for_test(chunk, 3);
    CHECK(a.call_count == 1);
    CHECK(b.call_count == 3);

    cap.remove_audio_subscriber(hb);
    cap._inject_for_test(chunk, 3);
    CHECK(a.call_count == 1);
    CHECK(b.call_count == 3);   // no remaining subscribers
}

TEST_CASE("B.1: set_audio_callback wrapper clears existing subscribers",
          "[capture][b1]") {
    PipeWireCapture cap("test-source");

    SubscriberSink a, b;
    cap.add_audio_subscriber(&sink_cb, &a);
    cap.add_audio_subscriber(&sink_cb, &b);

    const int16_t chunk[] = {42};
    cap._inject_for_test(chunk, 1);
    REQUIRE(a.call_count == 1);
    REQUIRE(b.call_count == 1);

    // Legacy entry point: installs a single subscriber, clearing the
    // existing list. After this call neither a nor b should receive
    // further frames; only c does.
    SubscriberSink c;
    cap.set_audio_callback(&sink_cb, &c);

    cap._inject_for_test(chunk, 1);
    CHECK(a.call_count == 1);
    CHECK(b.call_count == 1);
    CHECK(c.call_count == 1);

    // nullptr disarms entirely.
    cap.set_audio_callback(nullptr, nullptr);
    cap._inject_for_test(chunk, 1);
    CHECK(c.call_count == 1);
}

TEST_CASE("B.1: subscriber receives the source pointer (not a buffer iterator)",
          "[capture][b1]") {
    // Phase B.1 contract: the callback is handed the live PipeWire chunk
    // pointer (or _inject_for_test's source pointer), not a vector
    // iterator that another thread could resize-invalidate. We assert
    // the pointer-identity property under _inject_for_test because that
    // is the test-side mirror of the same RT-path code at on_process().
    PipeWireCapture cap("test-source");
    SubscriberSink s;
    cap.add_audio_subscriber(&sink_cb, &s);

    const int16_t chunk[] = {99, 88, 77};
    cap._inject_for_test(chunk, 3);

    REQUIRE(s.call_count == 1);
    CHECK(s.last_n == 3);
    CHECK(s.last_ptr == chunk);
}
