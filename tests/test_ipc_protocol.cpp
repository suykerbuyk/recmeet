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
