// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "reprocess_batch.h"

#include "cli.h"
#include "config.h"
#include "config_json.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "log.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"

#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <regex>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace recmeet {

// ---------------------------------------------------------------------------
// Whisper log callback + flush helper. Shared between main.cpp's standalone
// path and the reprocess-batch driver (both need the TTY-aware
// overwrite-in-place behaviour for whisper INFO messages, with a flush
// shim for clean line termination before printing other output).
// ---------------------------------------------------------------------------

namespace {
bool g_whisper_overwrote = false;
} // anonymous namespace

extern "C" void whisper_cli_log_shim(enum ggml_log_level level, const char* text, void*) {
    if (!text || !*text) return;
    static const bool is_tty = isatty(STDERR_FILENO);

    if (is_tty && (level <= GGML_LOG_LEVEL_INFO || level == GGML_LOG_LEVEL_CONT)) {
        size_t len = std::strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            --len;
        if (len > 0) {
            fprintf(stderr, "\r\033[K%.*s", (int)len, text);
            g_whisper_overwrote = true;
        }
    } else {
        if (g_whisper_overwrote) {
            fprintf(stderr, "\n");
            g_whisper_overwrote = false;
        }
        fprintf(stderr, "%s", text);
    }
}

void whisper_flush_line_shim() {
    if (g_whisper_overwrote) {
        fprintf(stderr, "\r\033[K");
        g_whisper_overwrote = false;
    }
}

// ---------------------------------------------------------------------------
// ensure_models_cached_or_fail (factored out of standalone_main's prompt
// blocks at main.cpp:920-1007 + main.cpp:944-952). The reprocess-batch driver
// runs this once at batch start so a missing model fails fast before any
// per-meeting work, and we never block waiting for stdin during a 4-hour
// overnight run. The standalone single-meeting path keeps its own
// prompt-then-download flow because operators DO want the prompt for an
// interactive session.
//
// Returns the empty string on success, or a human-readable error message on
// the first failure encountered.
// ---------------------------------------------------------------------------

std::string ensure_models_cached_or_fail(const Config& cfg) {
    // is_whisper_model_cached throws RecmeetError for unknown model names —
    // catch + fold into the same fail-fast message.
    try {
        if (!is_whisper_model_cached(cfg.whisper_model)) {
            return "whisper model '" + cfg.whisper_model +
                   "' not cached — run `recmeet --download-models` first.";
        }
    } catch (const std::exception& e) {
        return std::string("whisper model unavailable: ") + e.what();
    }

#if RECMEET_USE_SHERPA
    if (cfg.diarize && !is_sherpa_model_cached()) {
        return "sherpa diarization models not cached — "
               "run `recmeet --download-models` first.";
    }
    if (cfg.vad && !is_vad_model_cached()) {
        return "VAD model not cached — run `recmeet --download-models` first.";
    }
#endif

#if RECMEET_USE_LLAMA
    if (!cfg.no_summary && !cfg.llm_model.empty()) {
        try {
            (void)ensure_llama_model(cfg.llm_model);
        } catch (const std::exception& e) {
            return std::string("local LLM model unavailable: ") + e.what();
        }
    }
#endif

    if (!cfg.no_summary && cfg.api_key.empty() && cfg.llm_model.empty()) {
        return "no API key and no local LLM — set XAI_API_KEY (or equivalent), "
               "pass --api-key, configure a local LLM, or pass --no-summary.";
    }

    return "";
}

// ---------------------------------------------------------------------------
// IPC body shared between the single-meeting CLI path (main.cpp's
// `client_record` wrapper, which installs its own SIGINT/SIGTERM handlers)
// and the reprocess-batch dispatcher's daemon-mode path (which installs the
// hybrid `batch_daemon_sigint_handler` that ALSO trips the batch-level stop
// flag — see dispatch_one_reprocess_daemon below).
//
// Returns 0 on success, 1 on iteration error, or `kClientConnectFailedExitCode`
// on connect failure so the batch loop can detect daemon-disappearance and
// abort.
// ---------------------------------------------------------------------------

int client_record_no_sigaction(const Config& cfg, const std::string& addr) {
    IpcClient client(addr);
    if (!client.connect()) {
        fprintf(stderr, "Error: daemon not running. Start with: recmeet-daemon\n");
        return kClientConnectFailedExitCode;
    }

    bool is_reprocess = !cfg.reprocess_dir.empty();
    JsonMap params = config_to_map(cfg);

    // Reset function-static state every entry. Three statics had retained
    // values across calls (rev 3 C2-1):
    //   - last_cli_progress: corrupted iteration N+1's progress display
    //   - client_exit_code: contaminated exit code after iteration error
    //   - g_client: non-constant initializer ran only on first call, so
    //     iterations 2..N saw a permanently-null g_client (signal handler
    //     became a no-op).
    static int last_cli_progress = -1;
    static const bool cli_tty = isatty(STDERR_FILENO);
    static int client_exit_code = 0;
    last_cli_progress = -1;
    client_exit_code = 0;

    int64_t my_job_id = 0;

    client.set_event_callback([&client, &my_job_id](const IpcEvent& ev) {
        if (ev.event == "phase") {
            std::string name = json_val_as_string(ev.data.at("name"));
            if (cli_tty && last_cli_progress >= 0)
                fprintf(stderr, "\n");
            last_cli_progress = -1;
            fprintf(stderr, "Phase: %s\n", name.c_str());
        } else if (ev.event == "progress") {
            std::string phase = json_val_as_string(ev.data.at("phase"));
            int percent = static_cast<int>(json_val_as_int(ev.data.at("percent")));
            if (cli_tty) {
                fprintf(stderr, "\r\033[K%s... %d%%",
                        phase.c_str(), percent);
                last_cli_progress = percent;
            } else if (percent - last_cli_progress >= 10 || last_cli_progress < 0) {
                fprintf(stderr, "%s... %d%%\n", phase.c_str(), percent);
                last_cli_progress = percent;
            }
        } else if (ev.event == "state.changed") {
            std::string state = json_val_as_string(ev.data.at("state"));
            auto err_it = ev.data.find("error");
            if (err_it != ev.data.end()) {
                std::string error = json_val_as_string(err_it->second);
                if (!error.empty()) {
                    if (cli_tty && last_cli_progress >= 0)
                        fprintf(stderr, "\n");
                    last_cli_progress = -1;
                    fprintf(stderr, "Error: %s\n", error.c_str());
                    client_exit_code = 1;
                    client.close_connection();
                }
            }
        } else if (ev.event == "job.complete") {
            auto jid_it = ev.data.find("job_id");
            if (my_job_id > 0 && jid_it != ev.data.end()
                && json_val_as_int(jid_it->second) != my_job_id)
                return;
            if (cli_tty && last_cli_progress >= 0)
                fprintf(stderr, "\n");
            last_cli_progress = -1;
            std::string note = json_val_as_string(ev.data.at("note_path"));
            std::string dir = json_val_as_string(ev.data.at("output_dir"));
            if (!note.empty())
                printf("Note: %s\n", note.c_str());
            printf("Output: %s\n", dir.c_str());
        }
    });

    // Publish the live client to the batch-level SIGINT handler. Stored
    // unconditionally for simplicity; the single-meeting wrapper's handler
    // also reads this same atomic to forward Ctrl-C to the daemon.
    g_active_ipc_client.store(&client, std::memory_order_release);

    IpcResponse resp;
    IpcError err;
    if (!client.call("record.start", params, resp, err)) {
        fprintf(stderr, "Error: %s\n", err.message.c_str());
        g_active_ipc_client.store(nullptr, std::memory_order_release);
        return 1;
    }

    {
        auto jid_it = resp.result.find("job_id");
        if (jid_it != resp.result.end())
            my_job_id = json_val_as_int(jid_it->second);
    }

    if (is_reprocess)
        fprintf(stderr, "Reprocessing %s\n", cfg.reprocess_dir.c_str());
    else
        fprintf(stderr, "Recording started. Press Ctrl+C to stop.\n");

    client.read_events("job.complete");

    g_active_ipc_client.store(nullptr, std::memory_order_release);
    return client_exit_code;
}

// ---------------------------------------------------------------------------
// Atomics + signal handler scaffolding for Phase 3.
//
// The standalone-mode `batch_sigint_handler` install/restore at batch entry
// is Phase 3's responsibility — TODOs are clearly marked at the install/
// restore sites below. The daemon-mode `batch_daemon_sigint_handler` is
// installed per-iteration by `dispatch_one_reprocess` (Phase 2) so SIGINT
// during a daemon-routed iteration both stops the meeting AND sets the
// batch-level stop flag (resolves rev 2 C2-4).
// ---------------------------------------------------------------------------

static std::atomic<bool> g_batch_stop_requested{false};
static std::atomic<StopToken*> g_active_iter_stop{nullptr};
// External linkage — set by client_record_no_sigaction (in main.cpp).
std::atomic<IpcClient*> g_active_ipc_client{nullptr};

extern "C" void batch_daemon_sigint_handler(int) {
    g_batch_stop_requested.store(true, std::memory_order_release);
    StopToken* tok = g_active_iter_stop.load(std::memory_order_acquire);
    if (tok) tok->request();
    IpcClient* c = g_active_ipc_client.load(std::memory_order_acquire);
    if (c) {
        IpcResponse r;
        IpcError e;
        c->call("record.stop", r, e, 5000);
    }
}

// Standalone-mode batch-level SIGINT/SIGTERM handler. Trips the batch-stop
// flag and (if an iteration is in flight) requests cancellation on its
// StopToken. No mutex, no IPC call — daemon-mode iterations install
// `batch_daemon_sigint_handler` per-iteration via dispatch_one_reprocess
// (which nests under this batch-level handler when also installed).
//
// Both atomics use release/acquire so the loop's between-iteration check
// and the in-flight pipeline's StopToken poll both observe the flip
// without locking. StopToken::request() is itself an atomic store
// (util.h:40), so this whole handler stays inside the POSIX
// async-signal-safe set.
extern "C" void batch_sigint_handler(int) {
    g_batch_stop_requested.store(true, std::memory_order_release);
    StopToken* tok = g_active_iter_stop.load(std::memory_order_acquire);
    if (tok) tok->request();
}

// Test-only hooks. Defined here so the Catch2 binary can invoke the handler
// against this translation unit's file-static atomics (otherwise the test
// would only see its own copy). Production code never touches these.
namespace test_hooks {
extern "C" void test_batch_sigint_handler(int sig) {
    ::recmeet::batch_sigint_handler(sig);
}
void set_active_iter_stop(StopToken* tok) {
    g_active_iter_stop.store(tok, std::memory_order_release);
}
bool batch_stop_requested() {
    return g_batch_stop_requested.load(std::memory_order_acquire);
}
void reset_batch_stop_requested() {
    g_batch_stop_requested.store(false, std::memory_order_release);
}
} // namespace test_hooks

// ---------------------------------------------------------------------------
// Classification — enumerate immediate subdirs matching the meeting-dir
// pattern and decide WILL-REPROCESS / SKIP-note-exists / SKIP-no-audio.
// ---------------------------------------------------------------------------

namespace {

// Canonical meeting directory pattern: YYYY-MM-DD_HH-MM with optional `_N`
// collision suffix (treated as anomalies — see plan "_N suffix" notes).
const std::regex& meeting_dir_regex() {
    static const std::regex re(R"(^(\d{4}-\d{2}-\d{2}_\d{2}-\d{2})(_\d+)?$)");
    return re;
}

/// Resolve the directory we expect a meeting note to live under. Mirrors
/// `note.cpp:160-167`: if cfg.note_dir is set we add /YYYY/MM/, otherwise the
/// note lives in the meeting directory itself.
fs::path resolve_note_parent(const Config& cfg, const fs::path& meeting_dir,
                             const std::string& timestamp) {
    if (cfg.note_dir.empty()) {
        return meeting_dir;
    }
    fs::path parent = cfg.note_dir;
    if (timestamp.size() >= 7) {
        std::string year = timestamp.substr(0, 4);
        std::string month = timestamp.substr(5, 2);
        parent = parent / year / month;
    }
    return parent;
}

/// Glob `<note_parent>/Meeting_<timestamp>*.md` (the trailing `*` matters —
/// notes carry an AI-derived title suffix). Returns true if at least one
/// match exists.
bool note_exists_for(const fs::path& note_parent, const std::string& timestamp) {
    std::error_code ec;
    if (!fs::is_directory(note_parent, ec)) return false;

    const std::string prefix = "Meeting_" + timestamp;
    for (const auto& entry : fs::directory_iterator(note_parent, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < prefix.size() + 3) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".md") != 0) continue;
        return true;
    }
    return false;
}

} // anonymous namespace

std::vector<BatchEntry> classify_batch_entries(
    const fs::path& parent_dir, const Config& cfg) {

    std::vector<BatchEntry> entries;
    std::error_code ec;
    if (!fs::is_directory(parent_dir, ec)) return entries;

    for (const auto& entry : fs::directory_iterator(parent_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        std::string name = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(name, m, meeting_dir_regex())) continue;

        BatchEntry be;
        be.dir = entry.path();
        be.timestamp = m[1].str();  // canonical YYYY-MM-DD_HH-MM (suffix stripped)

        fs::path audio = find_audio_file(be.dir);
        if (audio.empty()) {
            be.kind = BatchEntryKind::SkipNoAudio;
        } else {
            fs::path note_parent = resolve_note_parent(cfg, be.dir, be.timestamp);
            if (note_exists_for(note_parent, be.timestamp)) {
                be.kind = BatchEntryKind::SkipNoteExists;
            } else {
                be.kind = BatchEntryKind::WillReprocess;
            }
        }
        entries.push_back(std::move(be));
    }

    std::sort(entries.begin(), entries.end(),
              [](const BatchEntry& a, const BatchEntry& b) {
                  // Primary key: timestamp; secondary: dir name for deterministic
                  // ordering of `_N` collision suffixes against their canonical
                  // counterpart.
                  if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                  return a.dir.filename().string() < b.dir.filename().string();
              });

    return entries;
}

// ---------------------------------------------------------------------------
// Dispatch helper — runs a single meeting via the locked-once dispatcher
// mode. Returns an Outcome describing the result; the loop assembles these
// into the end-of-batch summary.
// ---------------------------------------------------------------------------

namespace {

Outcome dispatch_one_reprocess_daemon(const Config& cfg, const std::string& addr) {
    Outcome o;
    auto t0 = std::chrono::steady_clock::now();

    // Save the previous SIGINT/SIGTERM handlers so we can restore them on the
    // way out — the batch driver may have its own (Phase 3 standalone handler)
    // or there may be no batch-level installed handler yet.
    struct sigaction prev_int{};
    struct sigaction prev_term{};
    sigaction(SIGINT, nullptr, &prev_int);
    sigaction(SIGTERM, nullptr, &prev_term);

    struct sigaction sa{};
    sa.sa_handler = batch_daemon_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // `client_record_no_sigaction` owns its IpcClient and publishes it to
    // `g_active_ipc_client` so `batch_daemon_sigint_handler` can call
    // record.stop on the live socket on Ctrl-C. On connect failure it
    // returns `kClientConnectFailed`; the loop maps that to DaemonUnreachable.
    int rc = 0;
    try {
        rc = client_record_no_sigaction(cfg, addr);
    } catch (...) {
        sigaction(SIGINT, &prev_int, nullptr);
        sigaction(SIGTERM, &prev_term, nullptr);
        g_active_ipc_client.store(nullptr, std::memory_order_release);
        throw;
    }

    sigaction(SIGINT, &prev_int, nullptr);
    sigaction(SIGTERM, &prev_term, nullptr);
    g_active_ipc_client.store(nullptr, std::memory_order_release);

    auto t1 = std::chrono::steady_clock::now();
    o.duration_seconds = std::chrono::duration<double>(t1 - t0).count();

    if (rc == 0) {
        o.kind = Outcome::Kind::Ok;
    } else if (rc == kClientConnectFailedExitCode) {
        o.kind = Outcome::Kind::DaemonUnreachable;
        o.error_message = "daemon connect failed";
    } else {
        o.kind = Outcome::Kind::Failed;
        o.error_message = "daemon iteration failed (exit " + std::to_string(rc) + ")";
    }
    return o;
}

Outcome dispatch_one_reprocess_standalone(const Config& cfg, StopToken& iter_stop) {
    Outcome o;
    auto t0 = std::chrono::steady_clock::now();

    try {
        auto result = run_pipeline(cfg, iter_stop);
        o.note_path = result.note_path;
        o.kind = Outcome::Kind::Ok;
    } catch (const RecmeetError& e) {
        o.kind = Outcome::Kind::Failed;
        o.error_message = e.what();
    } catch (const std::exception& e) {
        o.kind = Outcome::Kind::Failed;
        o.error_message = e.what();
    }

    whisper_flush_line_shim();
    auto t1 = std::chrono::steady_clock::now();
    o.duration_seconds = std::chrono::duration<double>(t1 - t0).count();
    return o;
}

Outcome dispatch_one_reprocess(const Config& cfg, BatchDispatchMode mode,
                               const std::string& addr, StopToken& iter_stop) {
    if (mode == BatchDispatchMode::Daemon) {
        return dispatch_one_reprocess_daemon(cfg, addr);
    }
    return dispatch_one_reprocess_standalone(cfg, iter_stop);
}

const char* kind_label(BatchEntryKind k) {
    switch (k) {
        case BatchEntryKind::WillReprocess:  return "WILL-REPROCESS";
        case BatchEntryKind::SkipNoteExists: return "SKIP-note-exists";
        case BatchEntryKind::SkipNoAudio:    return "SKIP-no-audio";
    }
    return "?";
}

std::string format_duration(double seconds) {
    int total = static_cast<int>(seconds + 0.5);
    int m = total / 60;
    int s = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    return std::string(buf);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Batch driver
// ---------------------------------------------------------------------------

int run_reprocess_batch(const CliResult& cli) {
    const Config& orig_cfg = cli.cfg;

    // 1. Validate parent dir exists and is a directory.
    if (orig_cfg.reprocess_batch_dir.empty()) {
        fprintf(stderr, "Error: --reprocess-batch requires a parent directory.\n");
        return 1;
    }
    std::error_code ec;
    if (!fs::exists(orig_cfg.reprocess_batch_dir, ec)) {
        fprintf(stderr, "Error: --reprocess-batch parent directory does not exist: %s\n",
                orig_cfg.reprocess_batch_dir.c_str());
        return 1;
    }
    if (!fs::is_directory(orig_cfg.reprocess_batch_dir, ec)) {
        fprintf(stderr, "Error: --reprocess-batch path is not a directory: %s\n",
                orig_cfg.reprocess_batch_dir.c_str());
        return 1;
    }

    // 2. Canonicalize so log lines use absolute paths.
    fs::path parent_dir = fs::canonical(orig_cfg.reprocess_batch_dir, ec);
    if (ec) {
        fprintf(stderr, "Error: cannot canonicalize parent directory: %s (%s)\n",
                orig_cfg.reprocess_batch_dir.c_str(), ec.message().c_str());
        return 1;
    }

    // 3. Enumerate + classify (uses orig_cfg, not yet `batch_mode`-set).
    auto entries = classify_batch_entries(parent_dir, orig_cfg);

    // 4. Tally counts up front for the header line.
    size_t will_count = 0, skip_note = 0, skip_audio = 0;
    for (const auto& e : entries) {
        switch (e.kind) {
            case BatchEntryKind::WillReprocess:  ++will_count; break;
            case BatchEntryKind::SkipNoteExists: ++skip_note; break;
            case BatchEntryKind::SkipNoAudio:    ++skip_audio; break;
        }
    }

    fprintf(stderr, "Reprocess batch: %s (%zu meetings to process, %zu already done, %zu anomalous)\n\n",
            parent_dir.c_str(), will_count, skip_note, skip_audio);

    // 5. Dry-run path: print classification table + counts, exit 0.
    if (orig_cfg.reprocess_batch_dry_run) {
        for (const auto& e : entries) {
            fprintf(stderr, "  %-16s  %s\n", kind_label(e.kind), e.dir.c_str());
        }
        fprintf(stderr, "\nDry run — no meetings reprocessed.\n");
        return 0;
    }

    if (will_count == 0) {
        fprintf(stderr, "Nothing to reprocess.\n");
        return 0;
    }

    // 6. Once-per-batch model precheck. Operates on orig_cfg (which still
    // mirrors what each iteration's pipeline will see, modulo `batch_mode`).
    {
        std::string err = ensure_models_cached_or_fail(orig_cfg);
        if (!err.empty()) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
    }

    // 7. Lock the dispatcher mode at batch entry. Mid-batch daemon up/down
    //    is treated as DaemonUnreachable rather than silently switching.
    BatchDispatchMode mode;
    if (cli.daemon_mode == DaemonMode::Force) {
        mode = BatchDispatchMode::Daemon;
    } else if (cli.daemon_mode == DaemonMode::Disable) {
        mode = BatchDispatchMode::Standalone;
    } else {
        mode = daemon_running(cli.daemon_addr) ? BatchDispatchMode::Daemon
                                               : BatchDispatchMode::Standalone;
    }

    // 8. Standalone-mode prelude. Daemon mode skips whisper_log + notify_init
    //    here because notifications come from the batch driver's own end-of-
    //    batch notify() (see step 14) and per-iteration whisper logs come
    //    from the daemon's subprocess (which deliberately silences whisper).
    if (mode == BatchDispatchMode::Standalone) {
        whisper_log_set(whisper_cli_log_shim, nullptr);
    }

    // 9. notify_init for end-of-batch summary notification (step 14).
    //    This runs in the operator's terminal `recmeet` process regardless of
    //    mode — the daemon subprocess never sees this code path.
    notify_init();

    // 10. Set per-batch flag on the working config copy. Not persisted; only
    //     propagates through IPC + subprocess JSON for this run.
    Config cfg_for_iter = orig_cfg;
    cfg_for_iter.batch_mode = true;

    // 11. Install batch-level SIGINT/SIGTERM handler for standalone mode.
    //     Daemon mode's `dispatch_one_reprocess` installs the hybrid daemon
    //     handler per iteration, so between iterations the default
    //     disposition is in effect (Ctrl-C terminates — acceptable; no work
    //     in flight). The standalone path needs the handler installed once
    //     for the entire batch so SIGINT during run_pipeline trips iter_stop
    //     and SIGINT between iterations trips g_batch_stop_requested for the
    //     loop's between-iteration check.
    //
    //     RAII guard restores the previous sigaction on any exit path
    //     (normal return, exception, early return below).
    struct SigGuard {
        struct sigaction prev_int{};
        struct sigaction prev_term{};
        bool installed = false;
        SigGuard(bool install) {
            sigaction(SIGINT, nullptr, &prev_int);
            sigaction(SIGTERM, nullptr, &prev_term);
            if (install) {
                struct sigaction sa{};
                sa.sa_handler = batch_sigint_handler;
                sigemptyset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(SIGINT, &sa, nullptr);
                sigaction(SIGTERM, &sa, nullptr);
                installed = true;
            }
        }
        ~SigGuard() {
            sigaction(SIGINT, &prev_int, nullptr);
            sigaction(SIGTERM, &prev_term, nullptr);
        }
        SigGuard(const SigGuard&) = delete;
        SigGuard& operator=(const SigGuard&) = delete;
    };
    // Reset the batch-stop flag so a re-entrant call (e.g. tests, or a
    // future daemonised batch driver) sees a clean slate.
    g_batch_stop_requested.store(false, std::memory_order_release);
    SigGuard sig_guard(mode == BatchDispatchMode::Standalone);

    // 12. Loop over WILL-REPROCESS meetings.
    struct IterReport {
        const BatchEntry* entry;
        Outcome outcome;
    };
    std::vector<IterReport> reports;
    bool aborted_daemon_unreachable = false;
    size_t failed = 0, ok = 0, cancelled = 0;
    size_t idx = 0;
    bool any_started = false;

    for (auto& e : entries) {
        if (e.kind != BatchEntryKind::WillReprocess) continue;
        ++idx;

        if (g_batch_stop_requested.load(std::memory_order_acquire)) {
            // Phase 3 path. Phase 2 never sets the flag from the standalone
            // side, but the daemon-mode handler can. Bail cleanly.
            break;
        }

        any_started = true;
        Config iter_cfg = cfg_for_iter;
        iter_cfg.reprocess_dir = e.dir;
        iter_cfg.reprocess_batch_dir.clear();  // single-meeting semantics inside dispatch

        StopToken iter_stop;
        g_active_iter_stop.store(&iter_stop, std::memory_order_release);

        fprintf(stderr, "[%zu/%zu] %s — reprocessing...\n",
                idx, will_count, e.timestamp.c_str());

        Outcome o = dispatch_one_reprocess(iter_cfg, mode, cli.daemon_addr, iter_stop);

        g_active_iter_stop.store(nullptr, std::memory_order_release);

        if (mode == BatchDispatchMode::Standalone) {
            whisper_flush_line_shim();
        }

        if (o.kind == Outcome::Kind::DaemonUnreachable) {
            fprintf(stderr,
                    "Error: daemon disappeared mid-batch (iteration %zu/%zu); "
                    "run `recmeet --status` to investigate\n",
                    idx, will_count);
            aborted_daemon_unreachable = true;
            reports.push_back({&e, std::move(o)});
            break;
        }

        // If SIGINT tripped iter_stop mid-flight, the dispatch path will have
        // returned Failed (run_pipeline throws RecmeetError("Cancelled"); the
        // daemon path returns the exit code from client_record_no_sigaction).
        // Re-classify those as Cancelled so the summary reads correctly and
        // exit code 130 is reported, not 1. Note: this checks BOTH the
        // per-iteration token AND the batch flag — either being tripped
        // implies the user hit Ctrl-C during this iteration.
        if (o.kind == Outcome::Kind::Failed
            && (iter_stop.stop_requested()
                || g_batch_stop_requested.load(std::memory_order_acquire))) {
            o.kind = Outcome::Kind::Cancelled;
        }

        if (o.kind == Outcome::Kind::Ok) {
            ++ok;
            fprintf(stderr, "[%zu/%zu] %s — DONE",
                    idx, will_count, e.timestamp.c_str());
            if (!o.note_path.empty()) {
                fprintf(stderr, " -> %s", o.note_path.c_str());
            }
            fprintf(stderr, " (%s)\n\n", format_duration(o.duration_seconds).c_str());
        } else if (o.kind == Outcome::Kind::Cancelled) {
            ++cancelled;
            fprintf(stderr, "[%zu/%zu] %s — CANCELLED (%s)\n\n",
                    idx, will_count, e.timestamp.c_str(),
                    format_duration(o.duration_seconds).c_str());
        } else {
            ++failed;
            fprintf(stderr, "[%zu/%zu] %s — FAIL: %s (%s)\n\n",
                    idx, will_count, e.timestamp.c_str(),
                    o.error_message.empty() ? "unknown error" : o.error_message.c_str(),
                    format_duration(o.duration_seconds).c_str());
        }

        reports.push_back({&e, std::move(o)});
    }

    (void)any_started;

    // 13. Previous SIGACTION restore is handled by sig_guard's destructor
    //     when this function returns (covers normal return, early return,
    //     and exception unwind paths uniformly).

    // 14. Final tally + single end-of-batch desktop notification.
    //     "skipped" here means meetings inside the WILL-REPROCESS set that
    //     never started because the loop bailed early (SIGINT between
    //     iterations or daemon disappearance). Cancelled is reported
    //     separately so the operator can tell partial work apart from
    //     not-yet-attempted.
    size_t not_started = will_count - ok - failed - cancelled;
    fprintf(stderr,
            "Summary: %zu ok, %zu failed, %zu cancelled, %zu skipped "
            "(out of %zu to process)\n",
            ok, failed, cancelled, not_started, will_count);
    if (failed > 0) {
        fprintf(stderr, "Failed:\n");
        for (const auto& r : reports) {
            if (r.outcome.kind == Outcome::Kind::Failed) {
                fprintf(stderr, "  %s — %s\n", r.entry->timestamp.c_str(),
                        r.outcome.error_message.empty() ? "unknown error"
                                                        : r.outcome.error_message.c_str());
            }
        }
    }
    if (cancelled > 0) {
        fprintf(stderr, "Cancelled:\n");
        for (const auto& r : reports) {
            if (r.outcome.kind == Outcome::Kind::Cancelled) {
                fprintf(stderr, "  %s\n", r.entry->timestamp.c_str());
            }
        }
    }

    {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "%zu ok, %zu failed, %zu cancelled, %zu skipped",
                      ok, failed, cancelled, not_started);
        notify("Reprocess batch complete", body);
    }

    notify_cleanup();

    if (aborted_daemon_unreachable) return 1;
    if (g_batch_stop_requested.load(std::memory_order_acquire)) return 130;
    return failed > 0 ? 1 : 0;
}

} // namespace recmeet
