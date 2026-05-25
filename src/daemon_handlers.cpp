// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2a — production IPC handler registrations.
//
// `register_daemon_handlers(server)` was extracted verbatim from
// `src/daemon.cpp` line ~2077..4495 (the inline `server.on("X", ...)`
// registrations that ran in `main()` during startup). The motion is pure
// code motion: handler behavior, registration order, and the per-handler
// capture pattern are byte-equivalent to the pre-extraction state. The
// only refactor surface is the lambda capture of `server`: the lambdas now
// capture `[&server, ...]` from the function parameter rather than from
// `main()`'s local.
//
// Phase 2b (a follow-on dispatch) makes the IPC integration tests call
// `register_daemon_handlers()` directly so the production code path is
// what those tests exercise. Until then, the tests register their own
// stub handlers and this file is the single producer of the verb set.

#include "daemon_handlers.h"
#include "daemon_handlers_internal.h"

#include "backend_info.h"
#include "config.h"
#include "config_json.h"
#include "diarization_cache.h"
#include "fetch_artifacts.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "log.h"
#include "meeting_index.h"
#include "meetings_browse.h"
#include "model_manager.h"
#include "ndjson_parse.h"
#include "session_manager.h"
#include "session_merge.h"
#include "speaker_id.h"
#include "streaming_session.h"
#include "summarize.h"
#include "upload_session.h"
#include "util.h"

#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using namespace recmeet;

void register_daemon_handlers(recmeet::IpcServer& server) {
    // Handler block extracted from src/daemon.cpp (Phase 2a).
    // Order matches the original registration order verbatim.

    server.on("status.get", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        fill_state_fields(resp.result);
        // C.7: `queue_depth` reports the postprocess slot's queued count
        // (the successor of the pre-C.7 single g_job_queue size).
        resp.result["queue_depth"] = static_cast<int64_t>(
            g_jobs ? g_jobs->queued_count(JobKind::Postprocess) : 0);
        return true;
    });

    // Phase C.9 — `sources.list` is gone. Audio-device enumeration is a
    // client-local concern in the thin-client model: the tray (and the CLI
    // in standalone mode) link `recmeet_capture` and call its `list_sources()`
    // directly. The daemon no longer links PipeWire/PulseAudio so it could
    // not honour this verb anyway.

    server.on("config.reload", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            // Mirror startup + SIGHUP: run legacy migration first so an
            // operator who edited config.yaml mid-flight gets it migrated
            // and applied (no-op once daemon.yaml exists).
            migrate_legacy_config_if_present();
            {
                std::lock_guard<std::mutex> lk(g_server_config_mu);
                g_server_config = load_server_config();
            }
            resp.result["ok"] = true;
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // Phase A.6: `config.update` IPC removed. Per-request config overrides
    // are replaced by per-`client_id` session state established once at
    // connect via `session.init` and refreshed via `session.update_*`.
    // The handler is deliberately not re-registered; the IPC dispatcher
    // returns "Method not found" for any stray `config.update` request,
    // and tests pin that behavior (see test_ipc_integration.cpp).

    // -----------------------------------------------------------------
    // Phase A.6 session handshake handlers
    //
    // `session.init` is the post-auth handshake the thin-client tray and
    // CLI use to establish credentials + preferences on the per-`client_id`
    // slot the daemon will consult at `enqueue_postprocess()` time. Wire
    // shape per the plan body:
    //   {"credentials": {"provider", "api_key", "api_keys": {...}},
    //    "preferences": {"output_dir", "note_dir", "language", "vocabulary",
    //                    "mic_source", "monitor_source", "whisper_model",
    //                    "summarization_backend", "llm_model",
    //                    "captions_enabled", "caption_latency_ms"}}
    //
    // All fields optional. Validation enforced at this layer:
    //   * `summarization_backend ∈ {"", "http", "local"}`  → reject otherwise
    //   * `caption_latency_ms ∈ [200, 2000]`               → reject otherwise
    //
    // On success the response is {"ok": true, "session_active": true}.
    auto parse_credentials_into = [](const JsonMap& src, SessionCredentials& dst) {
        auto pit = src.find("provider");
        if (pit != src.end()) dst.provider = json_val_as_string(pit->second);
        auto kit = src.find("api_key");
        if (kit != src.end()) dst.api_key = json_val_as_string(kit->second);
        // Per-provider key map arrives as a nested object stringified by
        // the parser (parse_json_object stores nested objects as their
        // raw JSON for the outer map). We pick known providers out of
        // that raw string so a future addition only updates one site.
        auto m = src.find("api_keys");
        if (m != src.end()) {
            std::string raw = json_val_as_string(m->second);
            JsonMap nested;
            if (!raw.empty() && raw[0] == '{') {
                // Reuse the IPC protocol's parser via a fake response wrap.
                std::string wrapped = "{\"id\":0,\"result\":" + raw + "}";
                IpcMessage tmp;
                if (parse_ipc_message(wrapped, tmp) && tmp.type == IpcMessageType::Response)
                    nested = std::move(tmp.response.result);
            }
            for (const auto& [k, v] : nested) {
                std::string val = json_val_as_string(v);
                if (!val.empty()) dst.api_keys[k] = val;
            }
        }
    };

    // Validation helper that returns true on success; on failure populates
    // `err` and returns false. Used by all three session handlers so the
    // shape rules (latency range, backend value-set) cannot drift between
    // them.
    auto validate_prefs_payload = [](const JsonMap& src, IpcError& err) {
        auto bit = src.find("summarization_backend");
        if (bit != src.end()) {
            std::string b = json_val_as_string(bit->second);
            if (!b.empty() && b != "http" && b != "local") {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "summarization_backend must be 'http' or 'local'";
                return false;
            }
        }
        auto lit = src.find("caption_latency_ms");
        if (lit != src.end()) {
            int64_t v = json_val_as_int(lit->second);
            if (v < 200 || v > 2000) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "caption_latency_ms must be in [200, 2000]";
                return false;
            }
        }
        return true;
    };

    auto parse_preferences_into = [](const JsonMap& src, SessionPreferences& dst) {
        auto get_str = [&](const char* k, std::string& out) {
            auto it = src.find(k);
            if (it != src.end()) out = json_val_as_string(it->second);
        };
        get_str("output_dir",            dst.output_dir);
        get_str("note_dir",              dst.note_dir);
        get_str("language",              dst.language);
        get_str("vocabulary",            dst.vocabulary);
        get_str("mic_source",            dst.mic_source);
        get_str("monitor_source",        dst.monitor_source);
        get_str("whisper_model",         dst.whisper_model);
        get_str("summarization_backend", dst.summarization_backend);
        get_str("llm_model",             dst.llm_model);
        auto cit = src.find("captions_enabled");
        if (cit != src.end()) dst.captions_enabled = json_val_as_bool(cit->second);
        auto lit = src.find("caption_latency_ms");
        if (lit != src.end()) dst.caption_latency_ms =
            static_cast<int>(json_val_as_int(lit->second));
    };

    // Helper: pull a nested sub-object out of req.params at `key` into a
    // JsonMap. The IPC parser leaves nested objects as their raw JSON
    // string in the outer map; we re-parse via the response-wrap trick.
    auto pull_nested = [](const JsonMap& outer, const char* key, JsonMap& dst) {
        auto it = outer.find(key);
        if (it == outer.end()) return false;
        std::string raw = json_val_as_string(it->second);
        if (raw.empty() || raw[0] != '{') return false;
        std::string wrapped = "{\"id\":0,\"result\":" + raw + "}";
        IpcMessage tmp;
        if (!parse_ipc_message(wrapped, tmp) || tmp.type != IpcMessageType::Response)
            return false;
        dst = std::move(tmp.response.result);
        return true;
    };

    server.on("session.init", [&server, parse_credentials_into, parse_preferences_into,
                               validate_prefs_payload, pull_nested]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.init: no client_id stamped on request";
            return false;
        }

        // Validate first; on rejection the slot is unchanged so the
        // client can retry with a corrected payload without dropping a
        // prior good session state (this handler is also valid on a
        // fresh connection where there is no prior state to preserve,
        // so the symmetry is just for code clarity).
        JsonMap prefs_map;
        bool have_prefs = pull_nested(req.params, "preferences", prefs_map);
        if (have_prefs && !validate_prefs_payload(prefs_map, err)) {
            err.id = req.id;
            return false;
        }

        // Parse and apply.
        SessionCredentials creds;
        JsonMap creds_map;
        if (pull_nested(req.params, "credentials", creds_map))
            parse_credentials_into(creds_map, creds);

        SessionPreferences prefs;
        // Preserve the struct defaults (caption_latency_ms = 500) when
        // the client omits the field; only overwrite from the request.
        if (have_prefs)
            parse_preferences_into(prefs_map, prefs);

        server.set_session_credentials(req.client_id, creds);
        server.set_session_preferences(req.client_id, prefs);

        resp.result["ok"] = true;
        resp.result["session_active"] = true;
        // A2.1 — surface the daemon's runtime-effective captions capability
        // to the client. The struct field is named `captions_enabled`
        // (server SoT, AND'd with capability at startup), but the wire
        // field is named `captions_supported` because from the client's
        // perspective the semantics are "is the server able to caption
        // right now" — a capability, not a toggle. Read under
        // g_server_config_mu (the same mutex that A1.4's startup gate
        // wrote under).
        bool captions_supported = false;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            captions_supported = g_server_config.captions_enabled;
        }
        resp.result["captions_supported"] = captions_supported;
        return true;
    });

    server.on("session.update_credentials",
              [&server, parse_credentials_into]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.update_credentials: no client_id stamped on request";
            return false;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        // Snapshot prior slot so the merge preserves untouched fields.
        // If the slot does not exist yet, `creds` / `prefs` default-
        // construct — the update behaves like a partial session.init.
        server.get_session(req.client_id, creds, prefs);
        // Overlay only the fields actually present in the request.
        parse_credentials_into(req.params, creds);
        if (!server.set_session_credentials(req.client_id, creds)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session slot unavailable";
            return false;
        }
        resp.result["ok"] = true;
        return true;
    });

    server.on("session.update_prefs",
              [&server, parse_preferences_into, validate_prefs_payload]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.update_prefs: no client_id stamped on request";
            return false;
        }
        if (!validate_prefs_payload(req.params, err)) {
            err.id = req.id;
            return false;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        server.get_session(req.client_id, creds, prefs);
        // Overlay only the fields actually present in the request — for
        // strings this is "non-empty value overwrites"; for the boolean
        // and integer we explicitly check key presence so a request that
        // omits them preserves the prior value (and so a request that
        // sets caption_latency_ms but omits captions_enabled does NOT
        // implicitly set captions_enabled=false).
        auto bit = req.params.find("captions_enabled");
        auto lit = req.params.find("caption_latency_ms");
        SessionPreferences from_req;
        parse_preferences_into(req.params, from_req);
        // String fields: non-empty overlays.
        if (!from_req.output_dir.empty())            prefs.output_dir = from_req.output_dir;
        if (!from_req.note_dir.empty())              prefs.note_dir = from_req.note_dir;
        if (!from_req.language.empty())              prefs.language = from_req.language;
        if (!from_req.vocabulary.empty())            prefs.vocabulary = from_req.vocabulary;
        if (!from_req.mic_source.empty())            prefs.mic_source = from_req.mic_source;
        if (!from_req.monitor_source.empty())        prefs.monitor_source = from_req.monitor_source;
        if (!from_req.whisper_model.empty())         prefs.whisper_model = from_req.whisper_model;
        if (!from_req.summarization_backend.empty()) prefs.summarization_backend = from_req.summarization_backend;
        if (!from_req.llm_model.empty())             prefs.llm_model = from_req.llm_model;
        if (bit != req.params.end()) prefs.captions_enabled   = from_req.captions_enabled;
        if (lit != req.params.end()) prefs.caption_latency_ms = from_req.caption_latency_ms;
        if (!server.set_session_preferences(req.client_id, prefs)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session slot unavailable";
            return false;
        }
        resp.result["ok"] = true;
        return true;
    });

    // ---------------------------------------------------------------------------
    // Phase C.9 — REMOVED handlers: record.start, record.stop, job.context.
    //
    // Until C.9 the daemon owned a live-recording path: record.start spawned a
    // worker thread that called pipeline::run_recording (PipeWire/PulseAudio
    // capture + optional CaptionEngine), and record.stop / job.context fed it.
    // C.9 retired the entire path. The tray now captures audio locally (via
    // recmeet_capture) and either:
    //   - submits a finished WAV via  + 0x01 upload frames
    //     (the daemon enqueues a postprocess Job; pp_worker_loop drains it),
    //   - opens a streaming-caption session via  + 0x03 audio
    //     frames (the streaming_session manager owns the CaptionEngine).
    //
    // record.stop / job.context callers should migrate to process.cancel /
    // (per-submit) context fields in process.submit. The wire-side 
    // boolean on state.changed is also gone in C.9 (IPC_PROTOCOL_VERSION bump).
    // ---------------------------------------------------------------------------

    // --- Phase C.10a — streaming-caption handlers ---
    //
    // process.stream         — open a streaming-caption session. The client
    //                          then sends `0x03` audio frames; the daemon
    //                          feeds them into a CaptionEngine and broadcasts
    //                          `caption` / `caption.degraded` events.
    // process.stream.cancel  — discard the streaming job + its buffered audio
    //                          (unlinks the temp WAV), release the slot.
    //
    // C.10a scope: this is the streaming *core*. process.stream.commit and
    // the stream->postprocess handoff are C.10b; converting the caption
    // events to send_to_client() is C.3/C.10b.

    server.on("process.stream",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        // Parse the request shape. Unknown / missing fields fall back to the
        // StreamRequest struct defaults. `speaker_hints` is reserved for v2
        // multi-server — accepted and ignored (we simply never read it).
        StreamRequest sr;
        auto gs = [&](const char* k, const std::string& def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_string(it->second) : def;
        };
        auto gi = [&](const char* k, int64_t def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_int(it->second) : def;
        };
        auto gb = [&](const char* k, bool def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_bool(it->second) : def;
        };
        sr.format            = gs("format", sr.format);
        sr.sample_rate       = static_cast<int32_t>(gi("sample_rate", sr.sample_rate));
        sr.channels          = static_cast<int32_t>(gi("channels", sr.channels));
        sr.context           = gs("context", sr.context);
        sr.language          = gs("language", sr.language);
        sr.captions_enabled  = gb("captions_enabled", sr.captions_enabled);
        sr.latency_budget_ms = static_cast<int>(gi("latency_budget_ms",
                                                   sr.latency_budget_ms));
        // C.11 — optional meeting_id. Empty is the v1-client fallback;
        // non-empty must be a canonical UUID v4 or we reject at the wire
        // boundary (defense-in-depth so a malformed id never poisons the
        // MeetingIndex via the streaming session's frozen state).
        sr.meeting_id        = gs("meeting_id", "");
        if (!is_valid_meeting_id(sr.meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream: 'meeting_id' must be a canonical "
                          "lowercase UUID v4 (or absent)";
            return false;
        }

        // Phase C.10b — snapshot the per-client postprocess config at
        // `process.stream` time, exactly as `process.submit` does at
        // submit time. The streaming session freezes this snapshot so a
        // later `process.stream.commit` builds its postprocess Job from
        // the preferences live when the stream STARTED (not from a
        // concurrently-reloaded daemon Config). The default temp_dir
        // (system temp) is used here; production daemons can swap it
        // later via a config knob.
        //
        // Phase E.2 W2.2b — build the per-job JobConfig by merging the
        // server snapshot with this client's session credentials +
        // preferences. Legacy clients that never called `session.init`
        // get default-constructed creds/prefs — make_job_config treats
        // those as "no overlay, use srv as fallback", which is the
        // correct backward-compatible behavior.
        ServerConfig srv_snapshot;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            srv_snapshot = g_server_config;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        server.get_session(req.client_id, creds, prefs);
        PostprocessInput input{};
        JobConfig stream_pp_cfg = make_job_config_with_real_env(
            srv_snapshot, creds, prefs, input);

        auto cr = g_streaming->create(req.client_id, sr, {}, stream_pp_cfg);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        resp.result["job_id"] = cr.job_id;
        resp.result["stream_token"] = cr.stream_token;
        log_info("daemon: process.stream OK (job=%ld client=%s)",
                 (long)cr.job_id, req.client_id.c_str());
        return true;
    });

    server.on("process.stream.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        auto it = req.params.find("stream_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.cancel: missing 'stream_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        if (!g_streaming->cancel(req.client_id, token)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.cancel: unknown stream_token "
                          "(or not owned by this client)";
            return false;
        }
        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.10b — process.stream.commit (finalize handoff) ---
    //
    // process.stream.commit — finalize a streaming session: flush + close
    //                         the temp WAV, hand the accumulated audio off
    //                         to a fresh Postprocess job (transcribe +
    //                         diarize + summarize), release the streaming
    //                         slot. Request: { stream_token }. Response:
    //                         { job_id: <postprocess job_id>, ok: true }.
    //                         The client monitors the new job via
    //                         `progress.job` + `process.fetch` exactly as
    //                         it would for a `process.submit` job.
    //
    // Validation chain (narrowest reject first):
    //   1. Missing `stream_token`              → InvalidParams
    //   2. Unknown stream_token                → InvalidParams
    //   3. Stream not owned by req.client_id   → PermissionDenied
    //   4. Session not in a committable state  → InvalidParams
    //
    // The full action is documented at StreamingSessionManager::commit;
    // the daemon handler is a thin shell over it.
    server.on("process.stream.commit",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        auto it = req.params.find("stream_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.commit: missing 'stream_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        auto cr = g_streaming->commit(req.client_id, token);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        // `job_id` on the wire is the NEW postprocess job_id (not the
        // streaming job's id). The client uses it for progress.job +
        // process.fetch follow-ups.
        resp.result["job_id"] = cr.postprocess_job_id;
        resp.result["ok"] = true;
        broadcast_state_inline(server);
        log_info("daemon: process.stream.commit OK (postprocess_job=%ld "
                 "client=%s)",
                 (long)cr.postprocess_job_id, req.client_id.c_str());
        return true;
    });

    // --- Phase C.2 — process.submit (batch postprocess upload) ---
    //
    // process.submit         — open an upload session. The client then sends
    //                          `0x01` upload frames; on completion the daemon
    //                          enqueues a postprocess job (which the existing
    //                          pp_worker_loop drains exactly as a legacy
    //                          record.start handoff did).
    // process.submit.cancel  — discard an in-flight upload session (unlinks
    //                          the staging dir, releases the reservation).
    //                          C.5 will add cancel-for-running-postprocess.
    //
    // C.2 scope: opening and finalizing an UPLOAD. Cancelling a postprocess
    // job that is already enqueued/running is C.5. Fetching artifacts is
    // C.4. Removing record.start is C.9.
    server.on("process.submit",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_uploads) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "upload subsystem unavailable";
            return false;
        }
        SubmitRequest sr;
        auto gs = [&](const char* k, const std::string& def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_string(it->second) : def;
        };
        auto gi = [&](const char* k, int64_t def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_int(it->second) : def;
        };
        sr.audio_size   = gi("audio_size", 0);
        sr.format       = gs("format", sr.format);
        sr.sample_rate  = static_cast<int32_t>(gi("sample_rate", sr.sample_rate));
        sr.channels     = static_cast<int32_t>(gi("channels", sr.channels));
        sr.context      = gs("context", sr.context);
        sr.mode         = gs("mode", sr.mode);
        sr.enroll_name  = gs("enroll_name", sr.enroll_name);  // C.8
        // C.11 — optional client-minted meeting_id (UUID v4). Empty is the
        // v1-client fallback; non-empty must validate or we reject at the
        // wire boundary so a malformed id never reaches UploadSession or
        // the MeetingIndex.
        sr.meeting_id   = gs("meeting_id", "");
        if (!is_valid_meeting_id(sr.meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit: 'meeting_id' must be a canonical "
                          "lowercase UUID v4 (or absent)";
            return false;
        }
        // `speaker_hints` is accepted-and-ignored in C.2 (reserved for v2
        // multi-server). We deliberately do not read it.

        // Snapshot the per-client postprocess config for the eventual Job.
        // Phase E.2 W2.2b — build the per-job JobConfig by merging the
        // server snapshot with this client's session credentials +
        // preferences. The upload manager freezes this snapshot for the
        // upload's lifetime so a concurrent config.reload between submit
        // and finalize can't surprise us. Legacy clients that never
        // called `session.init` get default-constructed creds/prefs —
        // make_job_config treats those as "no overlay, use srv as
        // fallback".
        ServerConfig srv_snapshot;
        size_t max_upload_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            srv_snapshot = g_server_config;
            max_upload_bytes = srv_snapshot.max_upload_bytes;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        server.get_session(req.client_id, creds, prefs);
        PostprocessInput input{};
        JobConfig cfg = make_job_config_with_real_env(
            srv_snapshot, creds, prefs, input);

        auto cr = g_uploads->create(req.client_id, sr, cfg, max_upload_bytes);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        resp.result["job_id"]       = cr.job_id;
        resp.result["upload_token"] = cr.upload_token;
        resp.result["max_size"]     = cr.max_size;
        log_info("daemon: process.submit OK (job=%ld client=%s "
                 "audio_size=%lld format=%s)",
                 (long)cr.job_id, req.client_id.c_str(),
                 (long long)sr.audio_size, sr.format.c_str());
        return true;
    });

    server.on("process.submit.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_uploads) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "upload subsystem unavailable";
            return false;
        }
        auto it = req.params.find("upload_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit.cancel: missing 'upload_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        if (!g_uploads->cancel(req.client_id, token)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit.cancel: unknown upload_token "
                          "(or not owned by this client)";
            return false;
        }
        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.12 — process.reprocess (re-run pipeline on resident meeting) ---
    //
    // process.reprocess { meeting_id, transcribe?=true, diarize?=true,
    //                     identify?=true, summarize?=true,
    //                     summary_style?, vocabulary? } -> { job_id }
    //
    // Resolves a meeting via the server-resident MeetingIndex (C.11.2) and
    // enqueues a Postprocess job that re-runs the pipeline against that
    // meeting's on-disk audio — no client-side audio re-upload required.
    // Pattern 4 of the convergence-principle flow patterns (V2-STRATEGY.md
    // "Meeting identity and the client-server audio contract"): the canonical
    // audio is server-resident; the client only needs to name the meeting.
    //
    // Per-stage flags let the operator rerun selected stages without redoing
    // the whole pipeline. Default (no flags) reruns the full pipeline exactly
    // as `--reprocess <dir>` does today.
    //
    // v1 scope note for `transcribe=false`: accepted on the wire for forward
    // compatibility but currently ignored — the subprocess always re-runs
    // transcription. Real skip-transcription support requires either threading
    // PostprocessInput through write_job_config or a new Config flag honored
    // by the subprocess's run_recording reprocess-mode path; both deferred to
    // a follow-up so this handler stays surgical. A client that sends
    // `transcribe=false` should expect re-transcription cost on this revision.
    //
    // Validation chain (narrowest first):
    //   1. Missing meeting_id                    -> InvalidParams
    //   2. meeting_id fails is_valid_meeting_id  -> InvalidParams
    //      (is_valid_meeting_id accepts "" as the v1 "no id" sentinel, so the
    //       empty-reject in step 1 must precede this format check)
    //   3. meeting_id not in MeetingIndex        -> InvalidParams (not_found)
    //   4. Resolved dir vanished from disk       -> InternalError
    //   5. No audio file in resolved dir         -> InternalError
    //
    // Per-stage flag translation (matches the existing Config knobs used by
    // the subprocess; no new Config schema):
    //   diarize=false    -> cfg.diarize    = false
    //   identify=false   -> cfg.speaker_id = false
    //   summarize=false  -> cfg.no_summary = true
    //   vocabulary=<s>   -> cfg.vocabulary = s   (replaces the per-session
    //                                             vocabulary for this job)
    //   summary_style    -> reserved; accepted, ignored (NoteConfig today
    //                       carries only `domain` + `tags`, no style field)
    //
    // The job's `meeting_id` is stamped from the request so job.list /
    // job.status reconcile by content key, and so process.cancel / process.fetch
    // address the resulting job via the C.11 binding.
    server.on("process.reprocess",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }
        if (!g_meeting_index) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "meeting index unavailable";
            return false;
        }

        // (1) meeting_id required.
        std::string meeting_id;
        {
            auto it = req.params.find("meeting_id");
            if (it != req.params.end()) meeting_id = json_val_as_string(it->second);
        }
        if (meeting_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: missing 'meeting_id'";
            return false;
        }
        // (2) Format validation. `is_valid_meeting_id("")` is true (v1 "no id"
        // sentinel); the step-1 empty reject ensures we never reach here with
        // an empty id, so the format check is the real gate.
        if (!is_valid_meeting_id(meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: 'meeting_id' must be a canonical "
                          "lowercase UUID v4";
            return false;
        }

        // (3) Lookup. A miss is the "client claims it exists but the server
        // has never seen it" case: typo, wrong server, or a meeting whose dir
        // was unlinked since startup rebuild. InvalidParams matches the
        // unknown-job_id precedent on process.fetch / job.status.
        auto hit = g_meeting_index->find(meeting_id);
        if (!hit.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: unknown meeting_id " + meeting_id;
            return false;
        }
        fs::path meeting_dir = *hit;

        // (4) Dir must still exist on disk. The index is in-memory only; an
        // operator who `rm -rf`'d a meeting after startup-rebuild would still
        // have a stale binding here. We do NOT unbind() — that would race
        // concurrent submits with the same meeting_id — we just refuse.
        std::error_code ec;
        if (!fs::is_directory(meeting_dir, ec)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.reprocess: meeting_id " + meeting_id
                        + " is indexed at " + meeting_dir.string()
                        + " but the directory no longer exists";
            return false;
        }

        // (5) Resolve audio. find_audio_file prefers timestamped audio_*.wav
        // and falls back to legacy audio.wav — both v1+v2 producer paths
        // write one. A meeting dir with neither is structurally broken (or
        // mid-upload, in which case the operator should wait).
        fs::path audio_path = find_audio_file(meeting_dir);
        if (audio_path.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.reprocess: no audio file found in "
                        + meeting_dir.string();
            return false;
        }

        // (6) Build cfg from live snapshot + per-stage flag overrides.
        // Phase E.2 W2.2b — build the per-job JobConfig by merging the
        // server snapshot with this client's session credentials +
        // preferences. pp_worker_loop sets reprocess_dir from
        // job.input.out_dir on every dequeue, so the default empty
        // reprocess_dir from PostprocessInput{} is fine here; the
        // per-stage flag mutations below remain identical.
        ServerConfig srv_snapshot;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            srv_snapshot = g_server_config;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        server.get_session(req.client_id, creds, prefs);
        PostprocessInput merge_input{};
        JobConfig cfg = make_job_config_with_real_env(
            srv_snapshot, creds, prefs, merge_input);

        // transcribe is accepted but ignored in v1 (see header note).
        {
            auto it = req.params.find("transcribe");
            if (it != req.params.end()) (void)json_val_as_bool(it->second, true);
        }
        // diarize: false -> skip diarization. Note this also disables
        // identification downstream (speaker_id needs diarization clusters).
        {
            auto it = req.params.find("diarize");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.diarize = false;
        }
        // identify: false -> skip cross-session voiceprint matching even when
        // diarization runs. Useful for "re-summarize with fresh diarization
        // but don't relabel known speakers".
        {
            auto it = req.params.find("identify");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.speaker_id = false;
        }
        // summarize: false -> skip summary step. Note inverts: cfg.no_summary
        // is the daemon-side flag, request carries the positive "summarize".
        {
            auto it = req.params.find("summarize");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.no_summary = true;
        }
        // vocabulary: when present (even empty), REPLACES the per-session
        // vocabulary. An empty string clears the prompt for this job.
        {
            auto it = req.params.find("vocabulary");
            if (it != req.params.end())
                cfg.vocabulary = json_val_as_string(it->second);
        }
        // summary_style: reserved; consumed defensively so an unknown-param
        // strict mode (future) does not break clients that send it.
        {
            auto it = req.params.find("summary_style");
            if (it != req.params.end()) (void)json_val_as_string(it->second);
        }

        // (7) Build the postprocess input. `derive_meeting_timestamp` returns
        // empty for legacy / non-canonical dirs; downstream code handles
        // empty (resolve_meeting_time falls back to file mtime).
        PostprocessInput input;
        input.out_dir    = meeting_dir;
        input.audio_path = audio_path;
        input.timestamp  = derive_meeting_timestamp(meeting_dir);

        Job job;
        job.input      = std::move(input);
        job.cfg        = std::move(cfg);
        job.meeting_id = meeting_id;

        int64_t job_id = g_jobs->enqueue(std::move(job), JobKind::Postprocess,
                                         req.client_id);

        resp.result["job_id"] = job_id;
        log_info("daemon: process.reprocess OK (job=%ld client=%s "
                 "meeting_id=%s dir=%s)",
                 (long)job_id, req.client_id.c_str(),
                 meeting_id.c_str(), meeting_dir.string().c_str());
        return true;
    });

    // --- Phase C.5 — process.cancel (general, job_id-routed) ---
    //
    // process.cancel — unified cancellation surface. Request: { job_id }.
    //                  Cancels a job regardless of lifecycle state (Queued /
    //                  WaitingOnDownload / WaitingForUpload / Running) and
    //                  regardless of which slot it lives in. Routes to the
    //                  right teardown path by inspecting the job's kind +
    //                  state via JobQueue::status.
    //
    //   kind / state                        → action
    //   ─────────────────────────────────── ──────────────────────────────
    //   Postprocess + Queued                → g_jobs->cancel(job_id)
    //                                         lazy FIFO removal at next
    //                                         pick_runnable_locked.
    //   Postprocess + WaitingOnDownload     → g_jobs->cancel(job_id);
    //                                         finish_download will see the
    //                                         Cancelled verdict and skip
    //                                         re-arming.
    //   Postprocess + WaitingForUpload      → g_jobs->cancel(job_id) AND
    //                                         g_uploads->cancel_by_job_id —
    //                                         tear down staging file + slot
    //                                         reservation.
    //   Postprocess + Running               → g_jobs->cancel(job_id) AND
    //                                         g_pp_stop.request() AND
    //                                         kill(g_pp_child_pid, SIGTERM)
    //                                         if a child is alive (mirrors
    //                                         the record.stop path).
    //   Streaming + any non-terminal        → g_streaming->cancel_by_job_id
    //                                         (stops engine + WAV + slot).
    //   ModelDownload + non-terminal       → g_jobs->cancel(job_id); the
    //                                         download worker observes via
    //                                         finish_download's Cancelled
    //                                         guard and dependents fail
    //                                         cleanly through the existing
    //                                         finish_download path.
    //   any kind + terminal                → InvalidParams "already
    //                                         in terminal state".
    //
    // Scope discipline — process.cancel is NEW; the narrower verbs
    // process.stream.cancel and process.submit.cancel stay in place for
    // backward compatibility (and as token-anchored entry points). C.5 does
    // NOT remove them — they coexist with the general verb.
    //
    // record.stop (legacy) is left untouched — it remains the v1 path that
    // C.9 will retire. The two coexist until then; this handler is the v2
    // thin-client cancellation entry point.
    server.on("process.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp,
                        IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: 'job_id' must be a positive integer";
            return false;
        }

        // (2) Snapshot the job. We read kind/state under JobQueue's mutex
        // via status(); after we release that snapshot we are racing the
        // worker, but the manager-side teardown paths all re-validate.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        const JobKind  kind  = snap->kind;
        const JobState state = snap->state;

        // (3) Ownership: only the originating client may cancel its own job.
        // Uses the C.3 binding (job_id → client_id). An empty req.client_id
        // (defensive — the IPC server stamps a client_id on every accepted
        // connection) would fail this check naturally because no live job
        // is bound to "".
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "process.cancel: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // (4) Terminal-state reject — there is nothing to cancel.
        if (state == JobState::Done ||
            state == JobState::Failed ||
            state == JobState::Cancelled) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: job is already in terminal "
                          "state " + std::string(job_state_name(state));
            return false;
        }

        // (5) Dispatch by kind + state.
        log_info("daemon: process.cancel job=%ld kind=%s state=%s "
                 "client=%s",
                 (long)job_id, job_kind_name(kind),
                 job_state_name(state), req.client_id.c_str());

        switch (kind) {
        case JobKind::Postprocess: {
            switch (state) {
            case JobState::Queued:
            case JobState::WaitingOnDownload:
                // Lazy FIFO removal at the next pick_runnable_locked, OR
                // observed by finish_download as a Cancelled dependent.
                g_jobs->cancel(job_id);
                break;
            case JobState::WaitingForUpload:
                // Tear down the in-flight upload first (so the staging
                // file is unlinked and the slot reservation released),
                // then mark the JobQueue entry Cancelled. The upload
                // manager's cancel_by_job_id() also calls g_jobs->cancel
                // internally; calling cancel() here is idempotent (the
                // second cancel hits the Cancelled-state guard and is a
                // no-op). Order matters: tear down resources before
                // settling state, so an observer racing on status()
                // sees a clean lifecycle.
                if (g_uploads) g_uploads->cancel_by_job_id(job_id);
                g_jobs->cancel(job_id);
                break;
            case JobState::Running: {
                // Mark Cancelled FIRST so the pp_worker_loop's exit-code-2
                // path (which calls finish(false, "cancelled")) preserves
                // the Cancelled verdict via finish()'s state guard. Then
                // poke the subprocess: g_pp_stop is the cooperative
                // signal; SIGTERM is the kick if a child PID is live.
                g_jobs->cancel(job_id);
                g_pp_stop.request();
                pid_t child = g_pp_child_pid.load();
                if (child > 0) ::kill(child, SIGTERM);
                break;
            }
            default:
                // Unreachable — terminal states reject above.
                break;
            }
            break;
        }
        case JobKind::Streaming: {
            // The streaming manager owns the slot + engine + temp WAV. Its
            // cancel_by_job_id runs the full teardown (engine stop, WAV
            // close+unlink, JobQueue::cancel, JobQueue::finish to release
            // the running marker). Returns false only if the session is
            // no longer live — a benign race; the JobQueue verdict is
            // either already Cancelled or terminal, which the post-status
            // re-check above caught.
            if (g_streaming) g_streaming->cancel_by_job_id(job_id);
            else             g_jobs->cancel(job_id);
            break;
        }
        case JobKind::ModelDownload: {
            // The model_download worker checks status before finishing —
            // a Cancelled verdict propagates through finish_download(),
            // and any parked Postprocess dependents land Failed via the
            // existing finish_download dependent path.
            g_jobs->cancel(job_id);
            break;
        }
        case JobKind::_count:
            // Unreachable — sentinel.
            break;
        }

        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.4 — process.fetch (artifact download) ---
    //
    // process.fetch — download the artifacts of a completed postprocess job.
    //                 Request:  { "job_id": int64 }
    //                 Response: { "job_id": int64,
    //                             "artifacts": [
    //                               {"name": "...", "size": int64,
    //                                "content_type": "..."}, ...],
    //                             "total_size": int64 }
    //                 Then, in the same `client_id`'s outbound stream, one
    //                 `0x02` BinaryArtifact frame per `artifacts[]` entry,
    //                 in the same order. NDJSON events for unrelated jobs
    //                 may interleave fine — the client demultiplexes by
    //                 counting `0x02` frames specifically.
    //
    // Validation chain (in this order — narrowest reject first):
    //   1. Missing/non-positive job_id        -> InvalidParams
    //   2. Unknown job_id                     -> InvalidParams
    //   3. job not owned by req.client_id     -> PermissionDenied
    //   4. job not in Done state              -> JobNotReady
    //   5. out_dir missing/unreadable         -> InternalError
    //   6. any artifact > max_binary_frame_bytes -> InternalError
    //                                            (chunking deferred — C.4
    //                                             rejects with a clear msg)
    //
    // Scope discipline (NOT in this handler — later phases):
    //   * Cancellation                        — C.5 (process.cancel)
    //   * job.status / job.list verbs         — C.6
    //   * Cleanup / retention of old artifacts— out of v1 scope
    //   * Chunked transfer for huge artifacts — future; today they reject
    server.on("process.fetch",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id is required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: 'job_id' must be a positive integer";
            return false;
        }

        // (2) The job_id must be known.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        const Job& job = *snap;

        // (3) Ownership: the requester must be the originator. We do NOT
        // expose another client's artifacts even if the job_id is guessed.
        // The check goes through `client_for_job()` (the C.3 binding) so a
        // raced disconnect-then-reconnect mints a fresh client_id and is
        // correctly rejected — the new connection does not inherit the
        // departed client's job artifacts.
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "process.fetch: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // (4) State must be terminal-Done. Other states are valid lifecycle
        // points but produce no artifacts to ship.
        if (job.state != JobState::Done) {
            err.code = static_cast<int>(IpcErrorCode::JobNotReady);
            err.message = std::string("process.fetch: fetch is only valid for "
                                      "Done jobs; current state=")
                        + job_state_name(job.state);
            return false;
        }

        // (5) `out_dir` must exist on disk. Enumeration handles the absent /
        // unreadable cases and returns an explanatory error string.
        const fs::path& out_dir = job.input.out_dir;
        if (out_dir.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: job has no out_dir on record";
            return false;
        }
        std::string enum_err;
        std::vector<ArtifactInfo> arts = enumerate_artifacts(out_dir, &enum_err);
        if (!enum_err.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: " + enum_err;
            return false;
        }

        // (6) Cap check: each artifact rides one `0x02` frame. C.1's transport
        // cap (default 16 MiB) bounds the payload of a single frame. A note
        // at typical sizes (<100 KB) clears this trivially, but a malformed
        // / huge artifact would not — reject rather than truncate.
        const size_t frame_cap = server.max_binary_frame_bytes();
        for (const auto& a : arts) {
            if (static_cast<size_t>(a.size) > frame_cap) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: artifact '" + a.name
                            + "' (" + std::to_string(a.size)
                            + " bytes) exceeds max_binary_frame_bytes ("
                            + std::to_string(frame_cap)
                            + "); raise [ipc] max_upload_bytes on the daemon "
                              "or omit this artifact";
                return false;
            }
        }

        // Read the bytes BEFORE sending the metadata response. If a read
        // fails (file vanished between enumerate and read) we want the
        // failure to surface as the response error, not as a torn binary
        // stream halfway through. This sequence preserves the wire invariant
        // "metadata first → exactly N `0x02` frames after". On success we
        // hand the buffers to the post-response binary fan-out below.
        std::vector<std::string> bodies;
        bodies.reserve(arts.size());
        int64_t total_size = 0;
        for (const auto& a : arts) {
            std::ifstream in(a.path, std::ios::binary);
            if (!in) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: cannot open artifact '"
                            + a.name + "' for read";
                return false;
            }
            std::string body;
            body.reserve(static_cast<size_t>(a.size));
            body.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
            total_size += static_cast<int64_t>(body.size());
            bodies.push_back(std::move(body));
        }

        // Build the metadata JSON. We emit `artifacts` as a raw JSON array
        // string because the JsonMap value type is flat — nested arrays
        // are stored as their raw substring and round-trip cleanly through
        // the parser on the client side (see parse_json_object's nested
        // object/array branch in ipc_protocol.cpp).
        std::string arr = "[";
        for (size_t i = 0; i < arts.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"name\":\"" + json_escape(arts[i].name) + "\""
                +  ",\"size\":" + std::to_string(arts[i].size)
                +  ",\"content_type\":\"" + json_escape(arts[i].content_type)
                +  "\"}";
        }
        arr += "]";

        resp.result["job_id"]     = job_id;
        resp.result["artifacts"]  = arr;
        resp.result["total_size"] = total_size;

        // The metadata response is dispatched by the IPC machinery once we
        // return `true`. The binary fan-out must happen AFTER that response
        // hits the client's outbound queue (or at least, after the response
        // is enqueued on the same fd) so the client's "wait for response,
        // then expect N binary frames" pump path is happy. We `post()` the
        // binary sends onto the poll thread so they enqueue on the SAME
        // outbound queue AFTER this handler's response — preserving order
        // because `send_to()` appends to the per-fd `outbound` deque.
        //
        // We capture `bodies` + `arts` BY MOVE into the posted callback so
        // no copying happens for large artifacts.
        const std::string client_id = req.client_id;
        auto bodies_p = std::make_shared<std::vector<std::string>>(std::move(bodies));
        auto arts_p   = std::make_shared<std::vector<ArtifactInfo>>(std::move(arts));
        server.post([&server, client_id, bodies_p, arts_p]() {
            // Iterate `artifacts[]` order (which is `bodies` order — built
            // in lockstep above). Each `send_binary_to_client` enqueues one
            // `0x02` frame on the client's outbound queue; the queue
            // preserves enqueue order, so the wire sees them in the same
            // order as the metadata array.
            for (size_t i = 0; i < bodies_p->size(); ++i) {
                server.send_binary_to_client(client_id,
                                             FrameType::BinaryArtifact,
                                             (*bodies_p)[i],
                                             MessageClass::Response);
            }
            log_debug("daemon: process.fetch dispatched %zu artifact frame(s) "
                      "to client=%s", bodies_p->size(), client_id.c_str());
        });

        log_info("daemon: process.fetch OK (job=%ld client=%s "
                 "artifacts=%zu total_size=%lld)",
                 (long)job_id, req.client_id.c_str(),
                 arts_p->size(), (long long)total_size);
        return true;
    });

    // --- Phase C.6 — job.status / job.list (read-only queries) ---
    //
    // Two thin read-only verbs that surface the JobQueue registry to clients.
    // Both wire over the existing `JobQueue::status` / `JobQueue::list_by_client`
    // / `JobQueue::client_for_job` API — no new mutation paths and no new
    // wire-shape surface beyond the response body.
    //
    // job.status — request  : { "job_id": int64 }
    //              response : { "job_id"   : int64,
    //                           "kind"     : "postprocess"|"streaming"|"model_download",
    //                           "state"    : "queued"|"waiting_on_download"|
    //                                        "waiting_for_upload"|"running"|
    //                                        "done"|"failed"|"cancelled",
    //                           "client_id": "<owner>",
    //                           "model_id" : "<empty for non-download>",
    //                           "error"    : "<empty unless state==failed>" }
    //              Validation chain:
    //                1. Missing/non-positive job_id   -> InvalidParams
    //                2. Unknown job_id                -> InvalidParams
    //                3. job not owned by req.client_id -> PermissionDenied
    //              Use case (D.3): on reconnect a client re-syncs the state of
    //              a single known job_id. Terminal jobs are retained in the
    //              registry (C.7) so a Done/Failed/Cancelled verdict survives.
    //
    // job.list — request  : {} (no params; scoped server-side by req.client_id)
    //            response : { "jobs": [ ...same shape as job.status... ],
    //                         "count": int64 }
    //            Use case (D.5): on tray restart a client re-syncs every job
    //            it owns across all kinds, including terminal jobs. Ordering
    //            is ascending job_id (std::map iteration order inside
    //            `list_by_client`); we preserve it on the wire.
    //
    // Scope discipline: `Job::input` (PostprocessInput) and `Job::cfg` (Config)
    // are NOT serialized. They carry config secrets (api_keys, output_dir
    // paths) and are not part of the wire surface. The intentional positive
    // assertion in the test suite is that these keys are ABSENT from the
    // response.
    //
    // Serialization approach for the `jobs[]` array follows the same precedent
    // as `process.fetch`'s `artifacts[]` (a few hundred lines up): we build
    // a raw JSON-array substring and stash it as a JsonVal::string. The
    // JsonMap value type is flat — nested objects/arrays round-trip cleanly
    // through `parse_json_object`'s nested-substring branch — so emitting
    // pre-serialized JSON via a string is the established C.4 pattern. We
    // factor the per-job object out into a local `serialize_job_object`
    // helper so `job.status` (single object) and `job.list` (array element)
    // share the exact same field set, ordering, and escaping behavior. A
    // divergence between the two responses would be a Phase D re-sync
    // correctness bug.

    auto serialize_job_object = [](const Job& job) -> std::string {
        std::string out;
        out.reserve(240);
        out += "{\"job_id\":";
        out += std::to_string(job.job_id);
        out += ",\"kind\":\"";
        out += job_kind_name(job.kind);
        out += "\",\"state\":\"";
        out += job_state_name(job.state);
        out += "\",\"client_id\":\"";
        out += json_escape(job.client_id);
        out += "\",\"model_id\":\"";
        out += json_escape(job.model_id);
        out += "\",\"error\":\"";
        out += json_escape(job.error);
        // C.11 — meeting_id emitted unconditionally (empty string for
        // v1-shaped clients + daemon-internal jobs like model downloads).
        // The client uses this to reconcile by content key per
        // docs/V2-STRATEGY.md "Meeting identity".
        out += "\",\"meeting_id\":\"";
        out += json_escape(job.meeting_id);
        // C.14 — phase + progress, cached by JobQueue::update_progress from
        // every daemon-side emission site (pp_worker_loop phase / progress
        // handlers + UploadProgressSink). Empty cached phase falls back to
        // a state-derived default so a D.3 reconnect re-sync always carries
        // a meaningful phase string without waiting for the next event.
        out += "\",\"phase\":\"";
        out += json_escape(job.phase.empty()
                               ? std::string(default_phase_for_state(job.state))
                               : job.phase);
        out += "\",\"progress\":";
        out += std::to_string(job.progress);
        out += "}";
        return out;
    };

    server.on("job.status",
              [](const IpcRequest& req, IpcResponse& resp,
                 IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: 'job_id' must be a positive integer";
            return false;
        }

        // (2) Snapshot — the registry retains terminal jobs (C.7), so a
        // Done/Failed/Cancelled job is fully queryable here.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }

        // (3) Ownership — same posture as process.fetch / process.cancel.
        // We do NOT leak another client's job state even on a guessed id.
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "job.status: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // Single-job response — emit each field flat under `result`. The
        // IpcResponse top-level wrapper is `{"id":N,"result":{...}}`, so we
        // expose the per-job fields directly at the result level (not
        // nested under a "job" key) — that matches the doc-comment shape
        // above. We emit `model_id`/`error` unconditionally even when
        // empty: a missing key vs. an empty string would be an unnecessary
        // client-side branch and the wire byte cost is negligible.
        //
        // `serialize_job_object` produces a raw JSON-object string used by
        // job.list's array elements (raw-substring C.4 pattern). For
        // job.status the response wrapper is itself an object, so we use
        // the flat JsonMap path instead — the resulting wire shape is
        // byte-for-byte the same set of {kind/state/client_id/model_id/
        // error/job_id} fields the array-element form emits.
        resp.result["job_id"]    = static_cast<int64_t>(snap->job_id);
        resp.result["kind"]      = std::string(job_kind_name(snap->kind));
        resp.result["state"]     = std::string(job_state_name(snap->state));
        resp.result["client_id"] = snap->client_id;
        resp.result["model_id"]  = snap->model_id;
        resp.result["error"]     = snap->error;
        // C.11 — emit unconditionally; matches serialize_job_object so the
        // job.status object shape is byte-equivalent to a job.list entry.
        resp.result["meeting_id"] = snap->meeting_id;
        // C.14 — phase + progress, same source-of-truth + fallback as
        // serialize_job_object. Keep the two paths byte-equivalent (a
        // job.status response and a job.list array element MUST carry the
        // same field set — D.3 / D.5 re-sync correctness).
        resp.result["phase"] = snap->phase.empty()
            ? std::string(default_phase_for_state(snap->state))
            : snap->phase;
        resp.result["progress"] = static_cast<int64_t>(snap->progress);

        log_debug("daemon: job.status job=%ld kind=%s state=%s client=%s "
                  "phase=%s progress=%d",
                  (long)job_id, job_kind_name(snap->kind),
                  job_state_name(snap->state), req.client_id.c_str(),
                  snap->phase.empty() ? default_phase_for_state(snap->state)
                                      : snap->phase.c_str(),
                  snap->progress);
        return true;
    });

    server.on("job.list",
              [serialize_job_object](const IpcRequest& req, IpcResponse& resp,
                                     IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // No params required — server-side scoping is by req.client_id.
        // An empty req.client_id (defensive — the IPC server stamps a
        // client_id on every accepted connection) would naturally return
        // an empty list because no live job binding is "".
        std::vector<Job> jobs = g_jobs->list_by_client(req.client_id);

        // Build the `jobs[]` raw JSON array. Same C.4 raw-substring pattern
        // as `process.fetch`'s `artifacts[]` — pre-serialize the array, then
        // stash it on the response as a string. The client-side parser
        // (`parse_json_object`'s nested-`[...]` branch) treats it as a raw
        // substring and re-parses on the receive side. Ordering is the
        // ascending-job_id order `list_by_client` returns (std::map
        // iteration order); we preserve it byte-for-byte on the wire.
        std::string arr = "[";
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (i > 0) arr += ",";
            arr += serialize_job_object(jobs[i]);
        }
        arr += "]";

        resp.result["jobs"]  = arr;
        resp.result["count"] = static_cast<int64_t>(jobs.size());

        log_debug("daemon: job.list client=%s count=%zu",
                  req.client_id.c_str(), jobs.size());
        return true;
    });

    // --- Phase C.8 — enroll.finalize (voiceprint enrollment second step) ---
    //
    // Two flows feed `enroll.finalize`. Both end at the same code path here:
    //
    //   Flow (a) — fresh enrollment audio:
    //     client → process.submit { mode="enroll", enroll_name=Alice, ... }
    //                              → daemon reserves Postprocess job_id
    //     client → 0x01 upload frames (audio)
    //     daemon → enqueue Postprocess job with cfg.enroll_mode = true
    //              → subprocess runs diarize-only, writes diarization.json
    //              → daemon reads diarization.json, populates
    //                DiarizationCache[job_id], emits job.complete with
    //                speakers[] payload + enroll_mode=true.
    //     client picks `target_speaker` from speakers[].
    //     client → enroll.finalize { job_id, target_speaker, enroll_name }.
    //              → handler extracts embedding for target_speaker from
    //                cached centroids, appends to SpeakerProfile under
    //                enroll_name, persists speakers DB.
    //
    //   Flow (b) — reuse existing diarization from a prior postprocess job:
    //     client → enroll.finalize { existing_job_id, target_speaker,
    //                                enroll_name }.
    //              → handler uses the same cache lookup.
    //
    // Both flows share the cache-lookup + extract + persist body below.
    //
    // Validation chain (matches `process.fetch` posture):
    //   1. Missing job_id / target_speaker / enroll_name → InvalidParams.
    //   2. Unknown job_id                                → InvalidParams.
    //   3. Job not owned by req.client_id                → PermissionDenied.
    //   4. Job state not Done                            → JobNotReady.
    //   5. Diarization cache miss or expired             → InvalidParams
    //                                                      "diarization no
    //                                                       longer cached
    //                                                       (TTL=...)".
    //   6. target_speaker out of cluster range           → InvalidParams.
    //
    // Action: load the speakers DB, find or create the profile for
    // `enroll_name`, append the cluster's centroid as a new embedding,
    // bump `updated`, save. Returns `{ok, enroll_name, embedding_count}`.
    server.on("enroll.finalize",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_jobs || !g_diar_cache) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "enroll.finalize: subsystem unavailable";
            return false;
        }
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'job_id' must be a positive integer";
            return false;
        }
        auto sit = req.params.find("target_speaker");
        if (sit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'target_speaker'";
            return false;
        }
        const int64_t target_speaker = json_val_as_int(sit->second, -1);
        if (target_speaker < 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'target_speaker' must be >= 0";
            return false;
        }
        auto nit = req.params.find("enroll_name");
        if (nit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'enroll_name'";
            return false;
        }
        std::string enroll_name = json_val_as_string(nit->second);
        if (enroll_name.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'enroll_name' must be non-empty";
            return false;
        }

        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "enroll.finalize: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }
        if (snap->state != JobState::Done) {
            err.code = static_cast<int>(IpcErrorCode::JobNotReady);
            err.message = std::string(
                              "enroll.finalize: finalize is only valid for "
                              "Done jobs; current state=")
                        + job_state_name(snap->state);
            return false;
        }

        auto entry = g_diar_cache->get(job_id);
        if (!entry.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: diarization no longer cached "
                          "(TTL=" + std::to_string(g_diar_cache->ttl_secs())
                        + "s; re-submit the job)";
            return false;
        }
        if (target_speaker >= static_cast<int64_t>(entry->clusters.size())) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: target_speaker "
                        + std::to_string(target_speaker)
                        + " out of range (clusters=0.."
                        + std::to_string(entry->clusters.size()) + ")";
            return false;
        }
        const auto& cluster = entry->clusters[target_speaker];
        if (cluster.embedding.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "enroll.finalize: cluster "
                        + std::to_string(target_speaker)
                        + " has no cached embedding (subprocess may have "
                          "skipped extraction)";
            return false;
        }

        // Append the cluster centroid to the SpeakerProfile keyed by
        // `enroll_name`. Mirrors the CLI's `profile.embeddings.push_back(...)`
        // semantic — the on-disk format stays append-one-embedding.
        fs::path db_dir;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            db_dir = g_server_config.speaker_db.empty()
                ? default_speaker_db_dir() : g_server_config.speaker_db;
        }
        try {
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile profile;
            for (const auto& p : profiles) {
                if (p.name == enroll_name) {
                    profile = p;
                    break;
                }
            }
            if (profile.name.empty()) {
                profile.name = enroll_name;
                profile.created = iso_now();
            }
            profile.embeddings.push_back(cluster.embedding);
            profile.updated = iso_now();
            if (profile.created.empty()) profile.created = profile.updated;
            save_speaker(db_dir, profile);

            resp.result["ok"] = true;
            resp.result["enroll_name"] = enroll_name;
            resp.result["embedding_count"] =
                static_cast<int64_t>(profile.embeddings.size());
            log_info("daemon: enroll.finalize OK (job=%ld client=%s "
                     "name=%s target=%d count=%zu)",
                     (long)job_id, req.client_id.c_str(),
                     enroll_name.c_str(), (int)target_speaker,
                     profile.embeddings.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = std::string("enroll.finalize: ") + e.what();
            return false;
        }
    });

    // --- Speaker management handlers ---

    server.on("speakers.list", [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        try {
            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }
            auto profiles = load_speaker_db(db_dir);
            std::string arr = "[";
            for (size_t i = 0; i < profiles.size(); ++i) {
                if (i > 0) arr += ",";
                arr += "{\"name\":\"" + json_escape(profiles[i].name)
                    + "\",\"enrollments\":" + std::to_string(profiles[i].embeddings.size())
                    + ",\"created\":\"" + json_escape(profiles[i].created)
                    + "\",\"updated\":\"" + json_escape(profiles[i].updated) + "\"}";
            }
            arr += "]";
            resp.result["speakers"] = arr;
            resp.result["count"] = static_cast<int64_t>(profiles.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    server.on("speakers.remove", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        auto it = req.params.find("name");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "Missing 'name' parameter";
            return false;
        }
        std::string name = json_val_as_string(it->second);
        fs::path db_dir;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            db_dir = g_server_config.speaker_db.empty()
                ? default_speaker_db_dir() : g_server_config.speaker_db;
        }
        if (remove_speaker(db_dir, name)) {
            resp.result["ok"] = true;
            resp.result["name"] = name;
            return true;
        } else {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "Speaker '" + name + "' not found";
            return false;
        }
    });

    server.on("speakers.reset", [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        try {
            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }
            int count = reset_speakers(db_dir);
            resp.result["ok"] = true;
            resp.result["removed"] = static_cast<int64_t>(count);
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // --- Phase E.6.1 — speakers.* / meetings.* IPC surface ---
    //
    // These eight verbs back the tray-bundled WebUI (E.6). All meeting
    // lookups resolve `meeting_id → meeting_dir_path` via
    // `g_meeting_index->find()` (NOT a dir-name parameter); the WebUI
    // keys all URLs off meeting_id. `db_dir` is resolved the same way as
    // the speakers.list/remove/reset handlers above: snapshot
    // g_server_config, fall back to default_speaker_db_dir() on empty.

    // speakers.get { name } → { name, enrollments, created, updated,
    //                           embedding_dim, embedding_count }
    //
    // Slim shape: the raw embedding blobs are NOT returned (browser only
    // renders count + dimension; biometric data shouldn't leak over a
    // possibly-tunneled WebUI). embedding_dim is the first vector's
    // length, 0 when there are no embeddings. embedding_count is
    // embeddings.size().
    server.on("speakers.get", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            auto it = req.params.find("name");
            std::string name = (it != req.params.end()) ? json_val_as_string(it->second) : "";
            if (!is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.get: invalid 'name'";
                return false;
            }
            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }
            auto profiles = load_speaker_db(db_dir);
            const SpeakerProfile* found = nullptr;
            for (const auto& p : profiles) {
                if (p.name == name) { found = &p; break; }
            }
            if (!found) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.get: not_found";
                return false;
            }
            const int64_t emb_count = static_cast<int64_t>(found->embeddings.size());
            const int64_t emb_dim = emb_count > 0
                ? static_cast<int64_t>(found->embeddings[0].size()) : 0;
            resp.result["name"] = found->name;
            resp.result["enrollments"] = emb_count;
            resp.result["created"] = found->created;
            resp.result["updated"] = found->updated;
            resp.result["embedding_dim"] = emb_dim;
            resp.result["embedding_count"] = emb_count;
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // speakers.enroll { name, meeting_id, cluster_id }
    //   → { ok, duration_sec, confidence, warning? }
    //
    // Resolve meeting_id via g_meeting_index->find() (NOT dir name); find
    // the cluster_id within the meeting's speakers.json; reject when
    // duration_sec < 5.0f (web.cpp:457-463 quality gate); append the
    // cluster's embedding to the named speaker profile, creating the
    // profile if absent. `warning` fires when 0 < confidence < 0.5f.
    server.on("speakers.enroll", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            if (!g_meeting_index) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "speakers.enroll: meeting index unavailable";
                return false;
            }
            std::string name;
            std::string meeting_id;
            int64_t cluster_id = -1;
            {
                auto it = req.params.find("name");
                if (it != req.params.end()) name = json_val_as_string(it->second);
                auto it2 = req.params.find("meeting_id");
                if (it2 != req.params.end()) meeting_id = json_val_as_string(it2->second);
                auto it3 = req.params.find("cluster_id");
                if (it3 != req.params.end()) cluster_id = json_val_as_int(it3->second, -1);
            }
            if (!is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: invalid 'name'";
                return false;
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: 'meeting_id' must be a canonical "
                              "lowercase UUID v4";
                return false;
            }
            if (cluster_id < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: 'cluster_id' missing or negative";
                return false;
            }
            auto hit = g_meeting_index->find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: unknown meeting_id " + meeting_id;
                return false;
            }
            const fs::path meeting_path = *hit;

            auto speakers = load_meeting_speakers(meeting_path);
            const MeetingSpeaker* spk = nullptr;
            for (const auto& s : speakers) {
                if (s.cluster_id == static_cast<int>(cluster_id)) { spk = &s; break; }
            }
            if (!spk) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: cluster_id not found in meeting speakers";
                return false;
            }
            if (spk->embedding.empty()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: cluster has no embedding data";
                return false;
            }
            if (spk->duration_sec < 5.0f) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.enroll: speaker has less than 5 seconds of audio — "
                              "enrollment would be unreliable";
                return false;
            }

            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile profile;
            for (const auto& p : profiles) {
                if (p.name == name) { profile = p; break; }
            }
            if (profile.name.empty()) {
                profile.name = name;
                profile.created = iso_now();
            }
            profile.updated = iso_now();
            profile.embeddings.push_back(spk->embedding);
            save_speaker(db_dir, profile);

            resp.result["ok"] = true;
            resp.result["duration_sec"] = static_cast<double>(spk->duration_sec);
            resp.result["confidence"] = static_cast<double>(spk->confidence);
            if (spk->confidence > 0.0f && spk->confidence < 0.5f) {
                std::string warning = "Low confidence ("
                    + std::to_string(spk->confidence).substr(0, 4)
                    + ") — this enrollment may be inaccurate";
                resp.result["warning"] = warning;
            }
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // speakers.remove_embedding { name, index } → { ok, remaining }
    //
    // Validate 0 <= index < embeddings.size(); erase by index; if the
    // resulting profile is empty, remove the profile entirely via
    // remove_speaker() and return remaining:0. Same wire shape as
    // web.cpp:514's body — `index` is an int parameter, NOT an embedding
    // blob to L2-match.
    server.on("speakers.remove_embedding",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            std::string name;
            int64_t index = -1;
            {
                auto it = req.params.find("name");
                if (it != req.params.end()) name = json_val_as_string(it->second);
                auto it2 = req.params.find("index");
                if (it2 != req.params.end()) index = json_val_as_int(it2->second, -1);
            }
            if (!is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.remove_embedding: invalid 'name'";
                return false;
            }
            if (index < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.remove_embedding: 'index' missing or negative";
                return false;
            }

            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile* found = nullptr;
            for (auto& p : profiles) {
                if (p.name == name) { found = &p; break; }
            }
            if (!found) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.remove_embedding: not_found";
                return false;
            }
            if (static_cast<std::size_t>(index) >= found->embeddings.size()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.remove_embedding: index out of range";
                return false;
            }
            found->embeddings.erase(found->embeddings.begin() + static_cast<std::ptrdiff_t>(index));
            if (found->embeddings.empty()) {
                remove_speaker(db_dir, name);
                resp.result["ok"] = true;
                resp.result["remaining"] = static_cast<int64_t>(0);
            } else {
                found->updated = iso_now();
                save_speaker(db_dir, *found);
                resp.result["ok"] = true;
                resp.result["remaining"] =
                    static_cast<int64_t>(found->embeddings.size());
            }
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // speakers.relabel { meeting_id, cluster_id, new_label, update_profile? }
    //   → { ok, old_label }
    //
    // Resolve meeting_id via find(); load_meeting_speakers; find cluster_id;
    // capture old_label. When update_profile != false AND the cluster has
    // an embedding: remove the embedding from the old profile via the
    // L2-distance blob match helper at speaker_id.h:43 (remove_embedding —
    // NOT an index removal — the daemon holds the blob via the meeting
    // speakers.json), then add it to the new profile (create if absent).
    // Then mutate the meeting speaker: label = new_label, identified = true,
    // confidence = 1.0f. Save via save_meeting_speakers with the meeting's
    // derive_meeting_timestamp.
    server.on("speakers.relabel",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            if (!g_meeting_index) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "speakers.relabel: meeting index unavailable";
                return false;
            }
            std::string meeting_id;
            std::string new_label;
            int64_t cluster_id = -1;
            bool update_profile = true;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end()) meeting_id = json_val_as_string(it->second);
                auto it2 = req.params.find("cluster_id");
                if (it2 != req.params.end()) cluster_id = json_val_as_int(it2->second, -1);
                auto it3 = req.params.find("new_label");
                if (it3 != req.params.end()) new_label = json_val_as_string(it3->second);
                auto it4 = req.params.find("update_profile");
                if (it4 != req.params.end())
                    update_profile = json_val_as_bool(it4->second, true);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.relabel: 'meeting_id' must be a canonical "
                              "lowercase UUID v4";
                return false;
            }
            if (cluster_id < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.relabel: 'cluster_id' missing or negative";
                return false;
            }
            if (new_label.empty() || !is_safe_dirname(new_label)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.relabel: invalid 'new_label'";
                return false;
            }
            auto hit = g_meeting_index->find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.relabel: unknown meeting_id " + meeting_id;
                return false;
            }
            const fs::path meeting_path = *hit;

            auto meeting_speakers = load_meeting_speakers(meeting_path);
            MeetingSpeaker* spk = nullptr;
            for (auto& s : meeting_speakers) {
                if (s.cluster_id == static_cast<int>(cluster_id)) { spk = &s; break; }
            }
            if (!spk) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.relabel: cluster_id not found in meeting speakers";
                return false;
            }

            const std::string old_label = spk->label;

            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                db_dir = g_server_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_server_config.speaker_db;
            }

            if (update_profile && !spk->embedding.empty()) {
                if (spk->identified && !old_label.empty())
                    remove_embedding(db_dir, old_label, spk->embedding);

                auto profiles = load_speaker_db(db_dir);
                SpeakerProfile profile;
                for (const auto& p : profiles) {
                    if (p.name == new_label) { profile = p; break; }
                }
                if (profile.name.empty()) {
                    profile.name = new_label;
                    profile.created = iso_now();
                }
                profile.updated = iso_now();
                profile.embeddings.push_back(spk->embedding);
                save_speaker(db_dir, profile);
            }

            spk->label = new_label;
            spk->identified = true;
            spk->confidence = 1.0f;
            save_meeting_speakers(meeting_path, meeting_speakers,
                                  derive_meeting_timestamp(meeting_path));

            resp.result["ok"] = true;
            resp.result["old_label"] = old_label;
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // speakers.batch_reidentify → { ok, async: true, started_at }
    //
    // Async by construction. Sync execution on the poll thread would block
    // every other client's health check + caption broadcast for 60+ s on
    // a 100-meeting batch. We spawn a detached std::thread that runs
    // load_speaker_db + discover_meetings + per-meeting re_identify_meeting
    // + save_meeting_speakers, then clears the running flag. A second
    // call while one is in flight is rejected with InvalidParams
    // "batch in progress".
    server.on("speakers.batch_reidentify",
              [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        bool expected = false;
        if (!g_batch_reidentify_running.compare_exchange_strong(expected, true)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "speakers.batch_reidentify: batch in progress";
            return false;
        }

        fs::path db_dir;
        fs::path meetings_root;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            db_dir = g_server_config.speaker_db.empty()
                ? default_speaker_db_dir() : g_server_config.speaker_db;
            meetings_root = g_server_config.meetings_root;
        }

#if RECMEET_USE_SHERPA
        std::thread worker([db_dir, meetings_root]() {
            try {
                auto db = load_speaker_db(db_dir);
                if (db.empty()) {
                    g_batch_reidentify_running.store(false);
                    return;
                }
                auto meetings = discover_meetings(meetings_root);
                for (const auto& mtg : meetings) {
                    if (!mtg.has_speakers) continue;
                    const fs::path mp = meetings_root / mtg.name;
                    auto spks = load_meeting_speakers(mp);
                    if (spks.empty()) continue;
                    auto result = re_identify_meeting(spks, db);
                    if (!result.empty()) {
                        save_meeting_speakers(mp, result,
                                              derive_meeting_timestamp(mp));
                    }
                }
            } catch (...) {
                // Swallow — flag is the only thing other callers observe.
            }
            g_batch_reidentify_running.store(false);
        });
        worker.detach();
#else
        // Without sherpa we have no re_identify_meeting; clear the flag
        // immediately and report success (no-op batch). Matches the
        // best-effort character of the speakers.* family on non-sherpa
        // builds.
        g_batch_reidentify_running.store(false);
#endif

        resp.result["ok"] = true;
        resp.result["async"] = true;
        resp.result["started_at"] = iso_now();
        return true;
    });

    // meetings.list → { meetings: <json_array_string>, count }
    //
    // Server-side equivalent of web.cpp's discover_meetings(). Iterates
    // g_server_config.meetings_root and emits one MeetingInfo per dir
    // (name, optional meeting_id, has_audio, has_speakers, has_summary,
    // mtime_iso). meeting_id is omitted/null for legacy V1 meetings
    // (pre-C.11 context.json without meeting_id) — the WebUI hides
    // mutation buttons for these (migration note #2).
    server.on("meetings.list", [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        try {
            fs::path meetings_root;
            {
                std::lock_guard<std::mutex> lock(g_server_config_mu);
                meetings_root = g_server_config.meetings_root;
            }
            auto meetings = discover_meetings(meetings_root);
            std::string arr = "[";
            for (std::size_t i = 0; i < meetings.size(); ++i) {
                if (i > 0) arr += ",";
                const auto& m = meetings[i];
                arr += "{\"name\":\"" + json_escape(m.name) + "\"";
                if (m.meeting_id.has_value())
                    arr += ",\"meeting_id\":\"" + json_escape(*m.meeting_id) + "\"";
                else
                    arr += ",\"meeting_id\":null";
                arr += ",\"has_audio\":";
                arr += (m.has_audio ? "true" : "false");
                arr += ",\"has_speakers\":";
                arr += (m.has_speakers ? "true" : "false");
                arr += ",\"has_summary\":";
                arr += (m.has_summary ? "true" : "false");
                arr += ",\"mtime_iso\":\"" + json_escape(m.mtime_iso) + "\"}";
            }
            arr += "]";
            resp.result["meetings"] = arr;
            resp.result["count"] = static_cast<int64_t>(meetings.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // meetings.speakers { meeting_id } → { speakers: <json_array_string>, count }
    //
    // Resolve meeting_id via find(). Serialize each MeetingSpeaker as
    // {cluster_id, label, identified, confidence, duration_sec}. The
    // embedding blob is omitted — large, never rendered, and the relabel /
    // enroll verbs that need it call load_meeting_speakers themselves.
    server.on("meetings.speakers",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            if (!g_meeting_index) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "meetings.speakers: meeting index unavailable";
                return false;
            }
            std::string meeting_id;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end())
                    meeting_id = json_val_as_string(it->second);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "meetings.speakers: 'meeting_id' must be a canonical "
                              "lowercase UUID v4";
                return false;
            }
            auto hit = g_meeting_index->find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "meetings.speakers: unknown meeting_id " + meeting_id;
                return false;
            }
            const fs::path meeting_path = *hit;
            auto speakers = load_meeting_speakers(meeting_path);
            std::string arr = "[";
            for (std::size_t i = 0; i < speakers.size(); ++i) {
                if (i > 0) arr += ",";
                const auto& s = speakers[i];
                arr += "{\"cluster_id\":" + std::to_string(s.cluster_id)
                    + ",\"label\":\"" + json_escape(s.label) + "\""
                    + ",\"identified\":" + (s.identified ? "true" : "false")
                    + ",\"confidence\":" + std::to_string(s.confidence)
                    + ",\"duration_sec\":" + std::to_string(s.duration_sec)
                    + "}";
            }
            arr += "]";
            resp.result["speakers"] = arr;
            resp.result["count"] = static_cast<int64_t>(speakers.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // meetings.read_note { meeting_id } → { path, content }
    //
    // Read Meeting_*.md from the meeting directory ONLY. No note_dir
    // fallback (deliberate behavior change — see plan migration note #1).
    // If no Meeting_*.md is found, return InvalidParams "not_found".
    server.on("meetings.read_note",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            if (!g_meeting_index) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "meetings.read_note: meeting index unavailable";
                return false;
            }
            std::string meeting_id;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end())
                    meeting_id = json_val_as_string(it->second);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "meetings.read_note: 'meeting_id' must be a canonical "
                              "lowercase UUID v4";
                return false;
            }
            auto hit = g_meeting_index->find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "meetings.read_note: unknown meeting_id " + meeting_id;
                return false;
            }
            const fs::path meeting_path = *hit;

            // Find Meeting_*.md in the meeting dir. No note_dir fallback.
            fs::path note_path;
            std::error_code ec;
            if (fs::is_directory(meeting_path, ec)) {
                for (const auto& entry : fs::directory_iterator(meeting_path, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    const std::string fname = entry.path().filename().string();
                    static constexpr const char* PREFIX = "Meeting_";
                    static constexpr const char* SUFFIX = ".md";
                    const std::size_t pre_n = std::strlen(PREFIX);
                    const std::size_t suf_n = std::strlen(SUFFIX);
                    if (fname.size() < pre_n + suf_n) continue;
                    if (fname.compare(0, pre_n, PREFIX) != 0) continue;
                    if (fname.compare(fname.size() - suf_n, suf_n, SUFFIX) != 0) continue;
                    note_path = entry.path();
                    break;
                }
            }
            if (note_path.empty()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "meetings.read_note: not_found";
                return false;
            }

            std::ifstream in(note_path, std::ios::binary);
            if (!in) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "meetings.read_note: cannot open note file";
                return false;
            }
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            resp.result["path"] = note_path.filename().string();
            resp.result["content"] = content;
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // --- Model management handlers ---

    server.on("models.list", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        auto models = list_cached_models();
        std::string arr = "[";
        for (size_t i = 0; i < models.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"name\":\"" + json_escape(models[i].name)
                + "\",\"category\":\"" + json_escape(models[i].category)
                + "\",\"cached\":" + (models[i].cached ? "true" : "false")
                + ",\"size_bytes\":" + std::to_string(models[i].size_bytes)
                + ",\"path\":\"" + json_escape(models[i].path) + "\"}";
        }
        arr += "]";
        resp.result["models"] = arr;
        return true;
    });

    // C.7: models.ensure / models.update no longer spawn an ad-hoc
    // g_dl_worker thread guarded by g_downloading. They enqueue
    // JobKind::ModelDownload jobs into the JobQueue's model_download slot;
    // the long-lived model_dl_worker_loop drains it. The slot's capacity-1
    // FIFO is the single admission point — explicit downloads queue behind
    // any auto-triggered one (and vice-versa) instead of racing.
    server.on("models.ensure", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        // Phase E.2 W2.2b — pure ServerConfig consumer; no session merge
        // needed. `whisper_model`, `diarize`, `vad`, and
        // `allow_client_downloads` are all server-resident fields.
        ServerConfig srv;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            srv = g_server_config;
        }
        // Download initiation policy (C.7): operator-disablable via
        // `[server] allow_client_downloads`. Any PSK-authenticated client
        // may trigger downloads when enabled (the default).
        if (!srv.allow_client_downloads) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "Client-initiated model downloads are disabled "
                          "([server] allow_client_downloads=false)";
            return false;
        }

        auto it = req.params.find("whisper_model");
        std::string whisper_model = (it != req.params.end())
            ? json_val_as_string(it->second) : srv.whisper_model;

        std::vector<std::string> enqueued;
        auto enqueue_if_missing = [&](const std::string& model_id, bool missing) {
            if (!missing) return;
            Job j;
            j.model_id = model_id;
            j.force_download = false;   // ensure semantics: no-op if cached.
            g_jobs->enqueue(std::move(j), JobKind::ModelDownload, req.client_id);
            enqueued.push_back(model_id);
        };

        try {
            enqueue_if_missing("whisper/" + whisper_model,
                               !is_whisper_model_cached(whisper_model));
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = e.what();
            return false;
        }
#if RECMEET_USE_SHERPA
        if (srv.diarize)
            enqueue_if_missing("sherpa/diarization", !is_sherpa_model_cached());
        if (srv.vad)
            enqueue_if_missing("vad", !is_vad_model_cached());
#endif

        resp.result["ok"] = true;
        resp.result["enqueued"] = static_cast<int64_t>(enqueued.size());
        return true;
    });

    server.on("models.update", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        bool allow_dl;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            allow_dl = g_server_config.allow_client_downloads;
        }
        if (!allow_dl) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "Client-initiated model downloads are disabled "
                          "([server] allow_client_downloads=false)";
            return false;
        }

        // Refresh every currently-cached model: one ModelDownload job per
        // model, force_download=true so it re-fetches even when cached.
        auto models = list_cached_models();
        std::vector<std::string> enqueued;
        bool sherpa_enqueued = false;
        for (const auto& m : models) {
            if (!m.cached) continue;
            std::string model_id;
            if (m.category == "whisper") {
                model_id = "whisper/" + m.name;
            }
#if RECMEET_USE_SHERPA
            else if (m.category == "sherpa") {
                if (sherpa_enqueued) continue;
                model_id = "sherpa/diarization";
                sherpa_enqueued = true;
            } else if (m.category == "vad") {
                model_id = "vad";
            }
#endif
            else {
                continue;
            }
            Job j;
            j.model_id = model_id;
            j.force_download = true;
            g_jobs->enqueue(std::move(j), JobKind::ModelDownload, req.client_id);
            enqueued.push_back(model_id);
        }

        resp.result["ok"] = true;
        resp.result["enqueued"] = static_cast<int64_t>(enqueued.size());
        return true;
    });

    // --- Phase C.13 — admin.evict (operator session-revocation) ---
    //
    // admin.evict { prefix } — forced eviction of a resume_token by prefix.
    //                          Sole caller today is `recmeet-daemon --evict`
    //                          (a separate CLI invocation routed through the
    //                          Unix socket; peer-credential trust is the gate
    //                          — Unix clients bypass PSK at accept time per
    //                          src/ipc_server.cpp:501-513). TCP-exposed
    //                          deployments inherit PSK gating automatically
    //                          (the verb has no special privilege check; the
    //                          security boundary is the socket itself).
    //
    // Request:  { "prefix": "<8+ hex chars>" }
    // Response: { "evicted":          "<full 64-char token>",
    //             "client_id":        "<owner_id>",
    //             "owned_jobs_failed": [<job_ids>] }
    //
    // Validation (H-4):
    //   - prefix < 8 hex chars   → InvalidParams "prefix too short"
    //   - 0 matches              → InvalidParams "no matching session"
    //   - > 1 match              → InvalidParams "ambiguous prefix"
    //   - exact-one              → evict + return full token + client_id
    //
    // SCAFFOLD: the eviction primitive on SessionManager is live (drops the
    // resume_token → ResumeSession binding); the owned-jobs-fail teardown
    // (which shares the same code path as the GC sweep — see
    // gc_worker_loop's TODO block) is deferred to the C.13 implementation
    // pass. owned_jobs_failed returns an empty array for now.
    server.on("admin.evict",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_sessions) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "admin.evict: session manager unavailable";
            return false;
        }
        auto it = req.params.find("prefix");
        if (it == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "admin.evict: missing 'prefix'";
            return false;
        }
        std::string prefix = json_val_as_string(it->second);
        EvictResult r = g_sessions->evict_by_prefix(prefix);
        switch (r.kind) {
        case EvictResult::Kind::PrefixTooShort:
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "admin.evict: prefix too short (min 8 hex chars)";
            return false;
        case EvictResult::Kind::NoMatch:
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "admin.evict: no matching session for prefix "
                        + SessionManager::log_prefix(prefix);
            return false;
        case EvictResult::Kind::Ambiguous:
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "admin.evict: ambiguous prefix " + prefix
                        + " — please specify more chars";
            return false;
        case EvictResult::Kind::Evicted: {
            // Same teardown path the GC sweep runs (shared body —
            // single-source-of-truth for the orphan-job + WAV-archive
            // policy). Returns the list of job_ids that were marked
            // Cancelled; the response carries that as
            // `owned_jobs_failed` so the operator sees the blast radius.
            std::vector<int64_t> canceled = teardown_orphan_jobs(
                SessionManager::log_prefix(r.token), r.client_id);
            resp.result["evicted"]   = r.token;
            resp.result["client_id"] = r.client_id;
            std::string arr = "[";
            for (size_t i = 0; i < canceled.size(); ++i) {
                if (i > 0) arr += ",";
                arr += std::to_string(canceled[i]);
            }
            arr += "]";
            resp.result["owned_jobs_failed"] = arr;
            log_info("daemon: admin.evict OK (prefix=%s client=%s "
                     "canceled=%zu)",
                     SessionManager::log_prefix(r.token).c_str(),
                     r.client_id.c_str(), canceled.size());
            return true;
        }
        }
        // Unreachable.
        err.code = static_cast<int>(IpcErrorCode::InternalError);
        err.message = "admin.evict: unexpected EvictResult kind";
        return false;
    });

}
