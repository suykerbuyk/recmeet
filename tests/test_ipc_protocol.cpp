// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ipc_protocol.h"
#include "util.h"

using namespace recmeet;

// ---------------------------------------------------------------------------
// Request round-trip
// ---------------------------------------------------------------------------

TEST_CASE("IpcRequest: serialize + parse round-trip", "[ipc]") {
    IpcRequest req;
    req.id = 42;
    req.method = "process.submit";
    req.params["model"] = std::string("tiny");
    req.params["mic_only"] = true;

    std::string wire = serialize(req);
    REQUIRE(wire.find("\"id\":42") != std::string::npos);
    REQUIRE(wire.find("\"method\":\"process.submit\"") != std::string::npos);

    IpcMessage msg;
    REQUIRE(parse_ipc_message(wire, msg));
    REQUIRE(msg.type == IpcMessageType::Request);
    CHECK(msg.request.id == 42);
    CHECK(msg.request.method == "process.submit");
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
    // v2-coexistence Phase 3: default_ipc_address now resolves to
    // server_socket_path() (~/.config/recmeet-server/server.sock by default).
    CHECK(addr.socket_path == server_socket_path());
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
    // v2-coexistence Phase 3: default_ipc_address now resolves to
    // server_socket_path() (~/.config/recmeet-server/server.sock by default).
    CHECK(addr.socket_path == server_socket_path());
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
    req.method = "process.submit";
    req.params["model"] = std::string("tiny");

    std::string wire = frame_ndjson(serialize(req));

    FrameReader reader;
    Frame f = pull_one(reader, wire);
    CHECK(f.type == FrameType::Ndjson);

    IpcMessage msg;
    REQUIRE(parse_ipc_message(f.payload, msg));
    REQUIRE(msg.type == IpcMessageType::Request);
    CHECK(msg.request.id == 7);
    CHECK(msg.request.method == "process.submit");
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

// ---------------------------------------------------------------------------
// C.1 frame-integrity gap (audit iter 156): the FrameReader state machine
// uses a length prefix to delimit binary frames, so a peer that connects,
// sends `<type><len>` plus a SHORT payload, then drops can leave the reader
// in a partial-frame state. The contract is that such a frame must NEVER
// be surfaced as a complete `Frame` via FrameStatus::Ok — the reader stays
// at NeedMore, and `in_frame()` reports the mid-frame state so the
// connection layer (which observes the EOF independently) can close the
// fd without dispatching the truncated payload.
//
// The state machine itself does not see EOF — that is the caller's job —
// so the assertions here are: (1) after feeding `<type><len><partial>`,
// next() returns NeedMore on every poll; (2) `in_frame()` is true and the
// current mode is Binary; (3) the partial payload is held inside the
// reader's buffer, NOT yielded; (4) feeding ARBITRARILY more partial bytes
// (still below `expected_len_`) leaves us in the same state — there is no
// stale-yield bug. Together these make a frame whose connection has gone
// silent definitively "discardable, never dispatched" once the caller
// detects EOF.
// ---------------------------------------------------------------------------
TEST_CASE("C.1: truncated binary frame stays mid-frame and never yields a Frame",
          "[ipc][c1][frame-integrity]") {
    FrameReader reader;
    Frame f;

    // Hand-build a header declaring a 100-byte payload, then feed only 50
    // of those payload bytes. The remaining 50 bytes never arrive
    // (simulating a dropped connection).
    constexpr uint32_t kDeclaredLen = 100;
    constexpr size_t   kPartialLen  = 50;
    std::string wire;
    wire += static_cast<char>(FrameType::BinaryUpload);
    wire += static_cast<char>((kDeclaredLen >> 24) & 0xFF);
    wire += static_cast<char>((kDeclaredLen >> 16) & 0xFF);
    wire += static_cast<char>((kDeclaredLen >>  8) & 0xFF);
    wire += static_cast<char>( kDeclaredLen        & 0xFF);
    wire.append(kPartialLen, 'P');  // 50 bytes of payload only

    reader.feed(wire);

    // (1) next() must report NeedMore, not Ok or any terminal status — the
    // frame is incomplete, but the reader has done nothing wrong.
    CHECK(reader.next(f) == FrameStatus::NeedMore);

    // (2) the reader is mid-frame, and specifically in Binary mode (the
    // length header was consumed cleanly; we are accumulating payload).
    CHECK(reader.in_frame());
    CHECK_FALSE(reader.in_ndjson_frame());

    // (3) the partial payload is buffered — `buffered()` reflects the
    // 50 bytes that have arrived. Note: the 1-byte discriminator and the
    // 4-byte length header have already been consumed by the state
    // machine, so the buffered count equals the partial payload size.
    CHECK(reader.buffered() == kPartialLen);

    // (4) calling next() again on an unchanged buffer is idempotent — it
    // does NOT yield a half-frame just because we polled twice.
    Frame f2;
    CHECK(reader.next(f2) == FrameStatus::NeedMore);
    CHECK(reader.in_frame());

    // Feed a few more bytes (still below the declared length). The reader
    // must remain mid-frame and continue to refuse to yield.
    reader.feed(std::string(10, 'X'));
    CHECK(reader.next(f2) == FrameStatus::NeedMore);
    CHECK(reader.in_frame());
    CHECK(reader.buffered() == kPartialLen + 10);

    // At this point, the connection layer would observe EOF on the socket
    // and close the fd. The truncated frame is held inside `reader` and is
    // discarded along with the FrameReader instance — never surfaced as a
    // completed Frame. The post-EOF cleanup (closing the fd, removing the
    // reader from the per-connection state) is the connection layer's
    // responsibility; FrameReader's job here is simply to refuse to yield.

    // Sanity belt-and-braces: completing the frame after the gap closes it
    // off cleanly, proving the buffered partial wasn't dropped or
    // corrupted. (This is the recovery path for a slow but non-malicious
    // peer; truncation-on-EOF is the malicious-or-broken peer path above.)
    reader.feed(std::string(kDeclaredLen - kPartialLen - 10, 'Q'));
    Frame done;
    REQUIRE(reader.next(done) == FrameStatus::Ok);
    CHECK(done.type == FrameType::BinaryUpload);
    CHECK(done.payload.size() == kDeclaredLen);
    CHECK_FALSE(reader.in_frame());
}

// A truncated frame whose declared length is split across a feed boundary
// must also stay mid-frame, not yield, and resume correctly when the
// remainder arrives. Companion case to the truncation test above — they
// share the [frame-integrity] tag so the entire integrity surface runs as
// one filter.
TEST_CASE("C.1: truncated length-prefix bytes also stay mid-frame",
          "[ipc][c1][frame-integrity]") {
    FrameReader reader;
    Frame f;

    // Feed the discriminator + only 2 of the 4 length bytes. The reader
    // moves into BinaryLen and stays there — it must not guess the length
    // from partial header data.
    std::string head;
    head += static_cast<char>(FrameType::BinaryArtifact);
    head += static_cast<char>(0x00);
    head += static_cast<char>(0x00);
    reader.feed(head);

    CHECK(reader.next(f) == FrameStatus::NeedMore);
    CHECK(reader.in_frame());

    // Complete the length header (declares 8 bytes) and feed the payload.
    std::string tail;
    tail += static_cast<char>(0x00);
    tail += static_cast<char>(0x08);  // declared len = 8
    tail.append(8, 'Z');
    reader.feed(tail);

    REQUIRE(reader.next(f) == FrameStatus::Ok);
    CHECK(f.type == FrameType::BinaryArtifact);
    CHECK(f.payload == std::string(8, 'Z'));
    CHECK_FALSE(reader.in_frame());
}

// ---------------------------------------------------------------------------
// P3-1 — Pure-mode streams (no interleave). The existing C.1 tests cover
// interleaved 0x00 NDJSON + 0x01/0x02/0x03 binary frames but never pin the
// pure-NDJSON or pure-binary back-to-back case — i.e. confirm the
// FrameReader does not erroneously slip into the "other mode" between
// frames of the same discriminator. Surfaced by the iter-156 audit.
// ---------------------------------------------------------------------------
TEST_CASE("C.1: pure NDJSON stream (no binary frames) round-trips",
          "[ipc][c1][frame-modes]") {
    // Five back-to-back NDJSON frames — feed them all at once into the
    // reader and assert each is yielded in order with no binary surfacing.
    std::string wire;
    wire += frame_ndjson("{\"id\":1}");
    wire += frame_ndjson("{\"id\":2}");
    wire += frame_ndjson("{\"id\":3}");
    wire += frame_ndjson("{\"id\":4}");
    wire += frame_ndjson("{\"id\":5}");

    FrameReader reader;
    reader.feed(wire);

    for (int i = 1; i <= 5; ++i) {
        Frame f;
        INFO("frame index=" << i);
        REQUIRE(reader.next(f) == FrameStatus::Ok);
        CHECK(f.type == FrameType::Ndjson);
        CHECK(f.payload == std::string("{\"id\":") + std::to_string(i) + "}");
    }
    // Reader is back at a clean boundary with no extra frames lurking.
    Frame trailing;
    CHECK(reader.next(trailing) == FrameStatus::NeedMore);
    CHECK_FALSE(reader.in_frame());
}

TEST_CASE("C.1: pure binary stream (no NDJSON frames) round-trips",
          "[ipc][c1][frame-modes]") {
    // Three back-to-back 0x01 BinaryUpload frames — distinct payload sizes
    // so a misaligned length-prefix read would be observable.
    std::string p1(16, 'a');
    std::string p2(64, 'b');
    std::string p3(128, 'c');

    std::string wire;
    wire += frame_binary(FrameType::BinaryUpload, p1);
    wire += frame_binary(FrameType::BinaryUpload, p2);
    wire += frame_binary(FrameType::BinaryUpload, p3);

    FrameReader reader;
    reader.feed(wire);

    Frame f1, f2, f3;
    REQUIRE(reader.next(f1) == FrameStatus::Ok);
    CHECK(f1.type == FrameType::BinaryUpload);
    CHECK(f1.payload == p1);

    REQUIRE(reader.next(f2) == FrameStatus::Ok);
    CHECK(f2.type == FrameType::BinaryUpload);
    CHECK(f2.payload == p2);

    REQUIRE(reader.next(f3) == FrameStatus::Ok);
    CHECK(f3.type == FrameType::BinaryUpload);
    CHECK(f3.payload == p3);

    // Reader is back at a clean boundary; no NDJSON should have surfaced.
    Frame trailing;
    CHECK(reader.next(trailing) == FrameStatus::NeedMore);
    CHECK_FALSE(reader.in_frame());
    CHECK_FALSE(reader.in_ndjson_frame());
}
