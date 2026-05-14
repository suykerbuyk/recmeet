// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ipc_protocol.h"

using namespace recmeet;

// ---------------------------------------------------------------------------
// Request round-trip
// ---------------------------------------------------------------------------

TEST_CASE("IpcRequest: serialize + parse round-trip", "[ipc]") {
    IpcRequest req;
    req.id = 42;
    req.method = "record.start";
    req.params["model"] = std::string("tiny");
    req.params["mic_only"] = true;

    std::string wire = serialize(req);
    REQUIRE(wire.find("\"id\":42") != std::string::npos);
    REQUIRE(wire.find("\"method\":\"record.start\"") != std::string::npos);

    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Request);
    CHECK(msg.request.id == 42);
    CHECK(msg.request.method == "record.start");
    CHECK(json_val_as_string(msg.request.params["model"]) == "tiny");
    CHECK(json_val_as_bool(msg.request.params["mic_only"]) == true);
}

TEST_CASE("IpcRequest: empty params", "[ipc]") {
    IpcRequest req;
    req.id = 1;
    req.method = "status.get";

    std::string wire = serialize(req);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Request);
    CHECK(msg.request.method == "status.get");
    CHECK(msg.request.params.empty());
}

// ---------------------------------------------------------------------------
// Response round-trip
// ---------------------------------------------------------------------------

TEST_CASE("IpcResponse: serialize + parse round-trip", "[ipc]") {
    IpcResponse resp;
    resp.id = 42;
    resp.result["state"] = std::string("idle");
    resp.result["uptime"] = int64_t(3600);

    std::string wire = serialize(resp);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Response);
    CHECK(msg.response.id == 42);
    CHECK(json_val_as_string(msg.response.result["state"]) == "idle");
    CHECK(json_val_as_int(msg.response.result["uptime"]) == 3600);
}

TEST_CASE("IpcResponse: empty result", "[ipc]") {
    IpcResponse resp;
    resp.id = 7;

    std::string wire = serialize(resp);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Response);
    CHECK(msg.response.id == 7);
    CHECK(msg.response.result.empty());
}

// ---------------------------------------------------------------------------
// Error round-trip
// ---------------------------------------------------------------------------

TEST_CASE("IpcError: serialize + parse round-trip", "[ipc]") {
    IpcError err;
    err.id = 42;
    err.code = static_cast<int>(IpcErrorCode::MethodNotFound);
    err.message = "Unknown method";

    std::string wire = serialize(err);
    REQUIRE(wire.find("\"error\"") != std::string::npos);

    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Error);
    CHECK(msg.error.id == 42);
    CHECK(msg.error.code == -32601);
    CHECK(msg.error.message == "Unknown method");
}

// ---------------------------------------------------------------------------
// Event round-trip
// ---------------------------------------------------------------------------

TEST_CASE("IpcEvent: serialize + parse round-trip", "[ipc]") {
    IpcEvent ev;
    ev.event = "phase";
    ev.data["name"] = std::string("transcribing");
    ev.data["progress"] = 0.5;

    std::string wire = serialize(ev);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Event);
    CHECK(msg.event.event == "phase");
    CHECK(json_val_as_string(msg.event.data["name"]) == "transcribing");
    CHECK(json_val_as_double(msg.event.data["progress"]) == 0.5);
}

TEST_CASE("IpcEvent: empty data", "[ipc]") {
    IpcEvent ev;
    ev.event = "state.changed";

    std::string wire = serialize(ev);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Event);
    CHECK(msg.event.event == "state.changed");
    CHECK(msg.event.data.empty());
}

TEST_CASE("A.4: IpcEvent client_id serialize + parse round-trip", "[ipc][a4]") {
    // Phase A.4 wire-shape contract:
    //   - When `client_id` is non-empty the serializer emits a top-level
    //     `"client_id":"..."` field.
    //   - When empty the field is omitted entirely (NO `"client_id":null`).
    //   - The parser round-trips the field through `IpcEvent::client_id`.

    SECTION("populated client_id appears on the wire and round-trips") {
        IpcEvent ev;
        ev.event = "caption";
        ev.client_id = "c-42-abcdef";
        ev.data["seq"] = int64_t(99);

        std::string wire = serialize(ev);
        // Top-level field, not nested under "data".
        CHECK(wire.find("\"client_id\":\"c-42-abcdef\"") != std::string::npos);
        // Ordering check: client_id appears before the "data" object so a
        // consumer reading the prefix can dispatch without parsing the
        // (potentially large) payload.
        size_t cid_pos  = wire.find("\"client_id\"");
        size_t data_pos = wire.find("\"data\":");
        REQUIRE(cid_pos  != std::string::npos);
        REQUIRE(data_pos != std::string::npos);
        CHECK(cid_pos < data_pos);

        IpcMessage msg;
        REQUIRE(parse_ipc_message(wire, msg));
        REQUIRE(msg.type == IpcMessageType::Event);
        CHECK(msg.event.event == "caption");
        CHECK(msg.event.client_id == "c-42-abcdef");
        CHECK(json_val_as_int(msg.event.data["seq"]) == 99);
    }

    SECTION("empty client_id is omitted from the wire") {
        IpcEvent ev;
        ev.event = "phase";
        ev.data["name"] = std::string("transcribing");
        // client_id left default-constructed (empty).

        std::string wire = serialize(ev);
        CHECK(wire.find("\"client_id\"") == std::string::npos);
        // No `"client_id":null` either — the field is omitted entirely
        // so high-frequency events stay compact.
        CHECK(wire.find("\"client_id\":null") == std::string::npos);

        IpcMessage msg;
        REQUIRE(parse_ipc_message(wire, msg));
        REQUIRE(msg.type == IpcMessageType::Event);
        CHECK(msg.event.client_id.empty());
    }

    SECTION("client_id with embedded special chars escapes safely") {
        // Defensive — the id format never produces these characters but
        // the serializer must never emit invalid JSON if a future format
        // change introduces quotes or backslashes.
        IpcEvent ev;
        ev.event = "test";
        ev.client_id = "weird\"id\\with-escapes";

        std::string wire = serialize(ev);
        IpcMessage msg;
        REQUIRE(parse_ipc_message(wire, msg));
        CHECK(msg.event.client_id == "weird\"id\\with-escapes");
    }
}

// ---------------------------------------------------------------------------
// Special characters
// ---------------------------------------------------------------------------

TEST_CASE("IPC: strings with special characters round-trip", "[ipc]") {
    IpcRequest req;
    req.id = 1;
    req.method = "config.update";
    req.params["path"] = std::string("/home/user/my \"meetings\"/notes");
    req.params["pattern"] = std::string("device\\with\\backslashes");

    std::string wire = serialize(req);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(json_val_as_string(msg.request.params["path"]) == "/home/user/my \"meetings\"/notes");
    CHECK(json_val_as_string(msg.request.params["pattern"]) == "device\\with\\backslashes");
}

TEST_CASE("IPC: newlines and tabs in values", "[ipc]") {
    IpcEvent ev;
    ev.event = "job.complete";
    ev.data["transcript"] = std::string("line1\nline2\ttab");

    std::string wire = serialize(ev);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(json_val_as_string(msg.event.data["transcript"]) == "line1\nline2\ttab");
}

// ---------------------------------------------------------------------------
// Numeric types
// ---------------------------------------------------------------------------

TEST_CASE("IPC: integer and double values", "[ipc]") {
    IpcResponse resp;
    resp.id = 1;
    resp.result["count"] = int64_t(0);
    resp.result["negative"] = int64_t(-42);
    resp.result["pi"] = 3.14159;
    resp.result["flag"] = true;
    resp.result["nothing"] = std::monostate{};

    std::string wire = serialize(resp);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(json_val_as_int(msg.response.result["count"]) == 0);
    CHECK(json_val_as_int(msg.response.result["negative"]) == -42);
    CHECK_THAT(json_val_as_double(msg.response.result["pi"]),
               Catch::Matchers::WithinAbs(3.14159, 0.0001));
    CHECK(json_val_as_bool(msg.response.result["flag"]) == true);
    // null comes back as monostate
    CHECK(std::holds_alternative<std::monostate>(msg.response.result["nothing"]));
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_CASE("IPC: parse rejects garbage input", "[ipc]") {
    IpcMessage msg;
    CHECK_FALSE(parse_ipc_message("", msg));
    CHECK_FALSE(parse_ipc_message("not json at all", msg));
    CHECK_FALSE(parse_ipc_message("{malformed", msg));
    CHECK_FALSE(parse_ipc_message("{}", msg)); // no known fields
}

// ---------------------------------------------------------------------------
// JsonVal helpers: type coercion / defaults
// ---------------------------------------------------------------------------

TEST_CASE("json_val_as_string: returns default for non-string", "[ipc]") {
    CHECK(json_val_as_string(JsonVal{int64_t(42)}, "def") == "def");
    CHECK(json_val_as_string(JsonVal{true}, "def") == "def");
}

TEST_CASE("json_val_as_int: coerces double to int", "[ipc]") {
    CHECK(json_val_as_int(JsonVal{3.7}) == 3);
    CHECK(json_val_as_int(JsonVal{std::string("nope")}, 99) == 99);
}

TEST_CASE("json_val_as_double: coerces int to double", "[ipc]") {
    CHECK(json_val_as_double(JsonVal{int64_t(7)}) == 7.0);
}

TEST_CASE("json_val_as_bool: returns default for non-bool", "[ipc]") {
    CHECK(json_val_as_bool(JsonVal{int64_t(1)}, false) == false);
    CHECK(json_val_as_bool(JsonVal{std::string("true")}, true) == true);
}

// ---------------------------------------------------------------------------
// Speaker IPC message round-trips
// ---------------------------------------------------------------------------

TEST_CASE("IPC: speakers.list request round-trip", "[ipc]") {
    IpcRequest req;
    req.id = 100;
    req.method = "speakers.list";

    std::string wire = serialize(req);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(msg.type == IpcMessageType::Request);
    CHECK(msg.request.method == "speakers.list");
}

TEST_CASE("IPC: speakers.remove request round-trip", "[ipc]") {
    IpcRequest req;
    req.id = 101;
    req.method = "speakers.remove";
    req.params["name"] = std::string("Alice");

    std::string wire = serialize(req);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(msg.type == IpcMessageType::Request);
    CHECK(msg.request.method == "speakers.remove");
    CHECK(json_val_as_string(msg.request.params["name"]) == "Alice");
}

TEST_CASE("IPC: speakers.reset response round-trip", "[ipc]") {
    IpcResponse resp;
    resp.id = 102;
    resp.result["ok"] = true;
    resp.result["removed"] = int64_t(3);

    std::string wire = serialize(resp);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    CHECK(msg.type == IpcMessageType::Response);
    CHECK(json_val_as_bool(msg.response.result["ok"]) == true);
    CHECK(json_val_as_int(msg.response.result["removed"]) == 3);
}

// ---------------------------------------------------------------------------
// Progress event
// ---------------------------------------------------------------------------

TEST_CASE("IPC: progress event round-trip", "[ipc]") {
    IpcEvent ev;
    ev.event = "progress";
    ev.data["phase"] = std::string("transcribing");
    ev.data["percent"] = int64_t(42);

    std::string wire = serialize(ev);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Event);
    CHECK(msg.event.event == "progress");
    CHECK(json_val_as_string(msg.event.data["phase"]) == "transcribing");
    CHECK(json_val_as_int(msg.event.data["percent"]) == 42);
}

// ---------------------------------------------------------------------------
// Socket path
// ---------------------------------------------------------------------------

TEST_CASE("default_socket_path: returns a non-empty path", "[ipc]") {
    std::string path = default_socket_path();
    CHECK_FALSE(path.empty());
    CHECK(path.find("daemon.sock") != std::string::npos);
}

// ---------------------------------------------------------------------------
// parse_ipc_address
// ---------------------------------------------------------------------------

TEST_CASE("parse_ipc_address: empty string → default Unix", "[ipc]") {
    IpcAddress addr;
    REQUIRE(parse_ipc_address("", addr));
    CHECK(addr.transport == IpcTransport::Unix);
    CHECK(addr.socket_path == default_socket_path());
    CHECK(addr.port == 0);
}

TEST_CASE("parse_ipc_address: Unix socket path", "[ipc]") {
    IpcAddress addr;
    REQUIRE(parse_ipc_address("/tmp/foo.sock", addr));
    CHECK(addr.transport == IpcTransport::Unix);
    CHECK(addr.socket_path == "/tmp/foo.sock");
}

TEST_CASE("parse_ipc_address: host:port → TCP", "[ipc]") {
    SECTION("IP address") {
        IpcAddress addr;
        REQUIRE(parse_ipc_address("192.168.1.5:9090", addr));
        CHECK(addr.transport == IpcTransport::Tcp);
        CHECK(addr.host == "192.168.1.5");
        CHECK(addr.port == 9090);
    }
    SECTION("localhost") {
        IpcAddress addr;
        REQUIRE(parse_ipc_address("localhost:8080", addr));
        CHECK(addr.transport == IpcTransport::Tcp);
        CHECK(addr.host == "localhost");
        CHECK(addr.port == 8080);
    }
    SECTION("bind-all") {
        IpcAddress addr;
        REQUIRE(parse_ipc_address("0.0.0.0:5000", addr));
        CHECK(addr.transport == IpcTransport::Tcp);
        CHECK(addr.host == "0.0.0.0");
        CHECK(addr.port == 5000);
    }
}

TEST_CASE("parse_ipc_address: port 0 → rejected", "[ipc]") {
    IpcAddress addr;
    CHECK_FALSE(parse_ipc_address("localhost:0", addr));
}

TEST_CASE("parse_ipc_address: port > 65535 → rejected", "[ipc]") {
    IpcAddress addr;
    CHECK_FALSE(parse_ipc_address("localhost:70000", addr));
}

TEST_CASE("parse_ipc_address: no host (:9090) → rejected", "[ipc]") {
    IpcAddress addr;
    CHECK_FALSE(parse_ipc_address(":9090", addr));
}

TEST_CASE("parse_ipc_address: bare IPv6 → rejected", "[ipc]") {
    IpcAddress addr;
    CHECK_FALSE(parse_ipc_address("::1:9090", addr));
    CHECK_FALSE(parse_ipc_address("fe80::1:9090", addr));
}

TEST_CASE("parse_ipc_address: IPv6 bracket notation → rejected (future)", "[ipc]") {
    IpcAddress addr;
    // Bracket notation reserved for future IPv6 support
    CHECK_FALSE(parse_ipc_address("[::1]:9090", addr));
}

TEST_CASE("default_ipc_address: returns Unix transport", "[ipc]") {
    IpcAddress addr = default_ipc_address();
    CHECK(addr.transport == IpcTransport::Unix);
    CHECK(addr.socket_path == default_socket_path());
}

// ===========================================================================
// Phase C.1 — framed wire protocol
//
// Every frame on the wire is `<1-byte discriminator><body>`. These tests
// cover the encode helpers (`frame_ndjson` / `frame_binary`) and the
// `FrameReader` state machine: round-trips for all four discriminators,
// big-endian length correctness, partial-read accumulation, NDJSON-while-
// binary-in-flight interleaving, unknown-discriminator rejection,
// oversized-frame rejection, and the zero-length payload edge case.
// ===========================================================================

// Helper: feed `wire` to a reader and pull exactly one frame, asserting Ok.
namespace {
Frame pull_one(FrameReader& r, const std::string& wire) {
    r.feed(wire);
    Frame f;
    REQUIRE(r.next(f) == FrameStatus::Ok);
    return f;
}
} // namespace

TEST_CASE("C.1: frame_ndjson encodes the 0x00 discriminator + newline", "[ipc][c1]") {
    std::string wire = frame_ndjson("{\"id\":1}");
    REQUIRE(wire.size() == 1 + 8 + 1);                    // 0x00 + json + '\n'
    CHECK(wire[0] == static_cast<char>(FrameType::Ndjson));
    CHECK(wire[0] == '\0');
    CHECK(wire.substr(1, 8) == "{\"id\":1}");
    CHECK(wire.back() == '\n');
}

TEST_CASE("C.1: 0x00 NDJSON round-trip through FrameReader", "[ipc][c1]") {
    // Existing NDJSON still works end-to-end through the new reader: take a
    // real serialized request, frame it, decode it, and parse it back.
    IpcRequest req;
    req.id = 7;
    req.method = "record.start";
    req.params["model"] = std::string("tiny");

    std::string wire = frame_ndjson(serialize(req));

    FrameReader reader;
    Frame f = pull_one(reader, wire);
    CHECK(f.type == FrameType::Ndjson);

    IpcMessage msg;
    REQUIRE(parse_ipc_message(f.payload, msg));
    REQUIRE(msg.type == IpcMessageType::Request);
    CHECK(msg.request.id == 7);
    CHECK(msg.request.method == "record.start");
    CHECK(json_val_as_string(msg.request.params["model"]) == "tiny");

    // Reader is back at a frame boundary with nothing buffered.
    Frame again;
    CHECK(reader.next(again) == FrameStatus::NeedMore);
    CHECK_FALSE(reader.in_frame());
}

TEST_CASE("C.1: 0x01/0x02/0x03 binary frame encode/decode round-trip", "[ipc][c1]") {
    struct Case { FrameType type; const char* name; };
    auto cases = { Case{FrameType::BinaryUpload,   "0x01 upload"},
                   Case{FrameType::BinaryArtifact, "0x02 artifact"},
                   Case{FrameType::StreamAudio,    "0x03 stream"} };

    for (const auto& c : cases) {
        SECTION(c.name) {
            // Payload includes embedded NUL and a '\n' — binary frames are
            // length-prefixed, so neither byte must terminate the frame.
            std::string payload = std::string("PCM\0\ndata\xFF", 9);
            std::string wire = frame_binary(c.type, payload);

            // Wire layout: discriminator + 4-byte length + payload.
            REQUIRE(wire.size() == 1 + 4 + payload.size());
            CHECK(static_cast<uint8_t>(wire[0]) == static_cast<uint8_t>(c.type));

            FrameReader reader;
            Frame f = pull_one(reader, wire);
            CHECK(f.type == c.type);
            CHECK(f.payload == payload);
        }
    }
}

TEST_CASE("C.1: binary frame length is big-endian", "[ipc][c1]") {
    // A 258-byte payload → length 0x00000102. The four length bytes after
    // the discriminator must be 00 00 01 02 (most significant first).
    std::string payload(258, 'x');
    std::string wire = frame_binary(FrameType::BinaryUpload, payload);

    REQUIRE(wire.size() == 1 + 4 + 258);
    CHECK(static_cast<unsigned char>(wire[1]) == 0x00);
    CHECK(static_cast<unsigned char>(wire[2]) == 0x00);
    CHECK(static_cast<unsigned char>(wire[3]) == 0x01);
    CHECK(static_cast<unsigned char>(wire[4]) == 0x02);

    FrameReader reader;
    Frame f = pull_one(reader, wire);
    CHECK(f.payload.size() == 258);
}

TEST_CASE("C.1: partial-read accumulation — frame split across feeds", "[ipc][c1]") {
    std::string payload(1000, 'Z');
    std::string wire = frame_binary(FrameType::StreamAudio, payload);
    REQUIRE(wire.size() == 1005);

    FrameReader reader;
    Frame f;

    // Feed one byte at a time for the first 7 bytes (discriminator + length
    // header + a couple payload bytes) — every intermediate state must
    // report NeedMore, never produce a frame and never error.
    for (size_t i = 0; i < 7; ++i) {
        reader.feed(wire.data() + i, 1);
        CHECK(reader.next(f) == FrameStatus::NeedMore);
    }
    // Feed the rest in two uneven chunks.
    reader.feed(wire.data() + 7, 500);
    CHECK(reader.next(f) == FrameStatus::NeedMore);
    reader.feed(wire.data() + 507, wire.size() - 507);

    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.type == FrameType::StreamAudio);
    CHECK(f.payload == payload);
}

TEST_CASE("C.1: NDJSON line split across feeds accumulates", "[ipc][c1]") {
    std::string wire = frame_ndjson("{\"event\":\"caption\"}");
    FrameReader reader;
    Frame f;

    // Discriminator alone → mid-frame, NeedMore.
    reader.feed(wire.data(), 1);
    CHECK(reader.next(f) == FrameStatus::NeedMore);
    CHECK(reader.in_ndjson_frame());

    // Half the JSON, still no '\n'.
    reader.feed(wire.data() + 1, 8);
    CHECK(reader.next(f) == FrameStatus::NeedMore);

    // The rest, including the terminating '\n'.
    reader.feed(wire.data() + 9, wire.size() - 9);
    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.payload == "{\"event\":\"caption\"}");
}

TEST_CASE("C.1: NDJSON interleaved with an in-flight binary stream", "[ipc][c1]") {
    // The core C.1 guarantee: a client can send a 0x00 NDJSON command
    // (e.g. process.cancel) in between two 0x03 audio frames. The reader
    // assembles all three in order with no "binary mode" lock.
    std::string audio1 = frame_binary(FrameType::StreamAudio, std::string(64, 'a'));
    std::string ndjson = frame_ndjson("{\"id\":9,\"method\":\"process.cancel\",\"params\":{}}");
    std::string audio2 = frame_binary(FrameType::StreamAudio, std::string(32, 'b'));

    FrameReader reader;
    reader.feed(audio1);
    reader.feed(ndjson);
    reader.feed(audio2);

    Frame f1, f2, f3, f4;
    REQUIRE(reader.next(f1) == FrameStatus::Ok);
    CHECK(f1.type == FrameType::StreamAudio);
    CHECK(f1.payload == std::string(64, 'a'));

    REQUIRE(reader.next(f2) == FrameStatus::Ok);
    CHECK(f2.type == FrameType::Ndjson);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(f2.payload, msg));
    CHECK(msg.type == IpcMessageType::Request);
    CHECK(msg.request.method == "process.cancel");

    REQUIRE(reader.next(f3) == FrameStatus::Ok);
    CHECK(f3.type == FrameType::StreamAudio);
    CHECK(f3.payload == std::string(32, 'b'));

    CHECK(reader.next(f4) == FrameStatus::NeedMore);
}

TEST_CASE("C.1: unknown discriminator is rejected cleanly", "[ipc][c1]") {
    FrameReader reader;
    Frame f;

    SECTION("0x04 — first reserved value") {
        char bad = 0x04;
        reader.feed(&bad, 1);
        CHECK(reader.next(f) == FrameStatus::BadDiscriminator);
    }
    SECTION("0x7B '{' — a pre-C.1 peer's un-prefixed JSON") {
        // A v1 peer sends raw `{...}` — byte 0 is '{' (0x7B), which is in
        // the reserved range. The reader must reject, not misparse.
        std::string stale = "{\"id\":1,\"method\":\"x\"}\n";
        reader.feed(stale);
        CHECK(reader.next(f) == FrameStatus::BadDiscriminator);
    }
    SECTION("0xFF — high byte") {
        char bad = static_cast<char>(0xFF);
        reader.feed(&bad, 1);
        CHECK(reader.next(f) == FrameStatus::BadDiscriminator);
    }
}

TEST_CASE("C.1: oversized binary frame is rejected before buffering payload", "[ipc][c1]") {
    // Use a small cap so the test does not have to materialize megabytes.
    FrameReader reader(/*max_binary_frame_bytes=*/1024);
    Frame f;

    // Hand-craft a header declaring a 2048-byte payload (> 1024 cap). No
    // payload bytes follow — the reader must reject on the length header
    // alone, proving it does not wait for (or buffer) the payload.
    std::string header;
    header += static_cast<char>(FrameType::BinaryUpload);
    header += static_cast<char>(0x00);
    header += static_cast<char>(0x00);
    header += static_cast<char>(0x08);  // 0x0800 == 2048
    header += static_cast<char>(0x00);
    reader.feed(header);
    CHECK(reader.next(f) == FrameStatus::FrameTooLarge);

    // A frame exactly at the cap is still accepted.
    FrameReader ok_reader(/*max_binary_frame_bytes=*/1024);
    std::string wire = frame_binary(FrameType::BinaryUpload, std::string(1024, 'q'));
    Frame g = pull_one(ok_reader, wire);
    CHECK(g.payload.size() == 1024);
}

TEST_CASE("C.1: zero-length binary payload is a valid frame", "[ipc][c1]") {
    std::string wire = frame_binary(FrameType::BinaryArtifact, "");
    // Discriminator + 4 zero length bytes, no payload.
    REQUIRE(wire.size() == 5);
    CHECK(static_cast<unsigned char>(wire[1]) == 0x00);
    CHECK(static_cast<unsigned char>(wire[2]) == 0x00);
    CHECK(static_cast<unsigned char>(wire[3]) == 0x00);
    CHECK(static_cast<unsigned char>(wire[4]) == 0x00);

    FrameReader reader;
    Frame f = pull_one(reader, wire);
    CHECK(f.type == FrameType::BinaryArtifact);
    CHECK(f.payload.empty());

    // And the reader is cleanly back at a boundary afterwards.
    Frame again;
    CHECK(reader.next(again) == FrameStatus::NeedMore);
    CHECK_FALSE(reader.in_frame());
}

TEST_CASE("C.1: multiple frames in a single feed drain in order", "[ipc][c1]") {
    // Back-to-back frames in one buffer — the reader must yield them one
    // at a time without losing or merging any.
    std::string blob;
    blob += frame_ndjson("{\"id\":1}");
    blob += frame_binary(FrameType::BinaryUpload, "abc");
    blob += frame_ndjson("{\"id\":2}");

    FrameReader reader;
    reader.feed(blob);

    Frame f;
    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.type == FrameType::Ndjson);
    CHECK(f.payload == "{\"id\":1}");

    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.type == FrameType::BinaryUpload);
    CHECK(f.payload == "abc");

    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.type == FrameType::Ndjson);
    CHECK(f.payload == "{\"id\":2}");

    CHECK(reader.next(f) == FrameStatus::NeedMore);
}

TEST_CASE("C.1: IPC_PROTOCOL_VERSION bumped for the breaking frame change", "[ipc][c1]") {
    // C.1 is a breaking wire change; the A.5 handshake gates it. The
    // version must have moved past the pre-C.1 value of 1.
    CHECK(IPC_PROTOCOL_VERSION >= 2);
}
