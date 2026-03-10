// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "log.h"
#include "speaker_id.h"
#include "util.h"
#include "version.h"

#include <httplib.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static httplib::Server* g_server = nullptr;

static void signal_handler(int) {
    if (g_server) g_server->stop();
}

// ---------------------------------------------------------------------------
// JSON helpers (same minimal approach as speaker_id.cpp)
// ---------------------------------------------------------------------------

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string json_ok() {
    return R"({"ok":true})";
}

static std::string json_ok_removed(int n) {
    return R"({"ok":true,"removed":)" + std::to_string(n) + "}";
}

static std::string json_error(const std::string& msg) {
    return R"({"error":")" + escape_json(msg) + R"("})";
}

// Extract a string value from JSON body: "key": "value"
static std::string json_get_str(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    // skip whitespace and colon
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    if (pos >= body.size() || body[pos] != '"') return "";
    ++pos;
    auto end = body.find('"', pos);
    if (end == std::string::npos) return "";
    return body.substr(pos, end - pos);
}

// Extract an integer value from JSON body: "key": 123
static int json_get_int(const std::string& body, const std::string& key, int def = -1) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    if (pos >= body.size()) return def;
    return std::atoi(body.c_str() + pos);
}

// ---------------------------------------------------------------------------
// Path safety: validate meeting_dir is a simple name (no path traversal)
// ---------------------------------------------------------------------------

static bool is_safe_dirname(const std::string& name) {
    if (name.empty()) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

static std::string speaker_profile_to_json(const SpeakerProfile& p) {
    std::ostringstream out;
    out << "{";
    out << "\"name\":\"" << escape_json(p.name) << "\",";
    out << "\"enrollments\":" << p.embeddings.size() << ",";
    out << "\"created\":\"" << escape_json(p.created) << "\",";
    out << "\"updated\":\"" << escape_json(p.updated) << "\"";
    out << "}";
    return out.str();
}

static std::string speaker_profile_detail_to_json(const SpeakerProfile& p) {
    std::ostringstream out;
    out << "{";
    out << "\"name\":\"" << escape_json(p.name) << "\",";
    out << "\"enrollments\":" << p.embeddings.size() << ",";
    out << "\"created\":\"" << escape_json(p.created) << "\",";
    out << "\"updated\":\"" << escape_json(p.updated) << "\",";
    out << "\"embedding_dim\":" << (p.embeddings.empty() ? 0 : p.embeddings[0].size());
    out << "}";
    return out.str();
}

struct MeetingInfo {
    std::string name;
    bool has_speakers_json;
    int speaker_count;
    std::string date;
};

static std::string meeting_info_to_json(const MeetingInfo& m) {
    std::ostringstream out;
    out << "{";
    out << "\"name\":\"" << escape_json(m.name) << "\",";
    out << "\"has_speakers_json\":" << (m.has_speakers_json ? "true" : "false") << ",";
    out << "\"speaker_count\":" << m.speaker_count << ",";
    out << "\"date\":\"" << escape_json(m.date) << "\"";
    out << "}";
    return out.str();
}

#if RECMEET_USE_SHERPA
static std::string meeting_speaker_to_json(const MeetingSpeaker& s) {
    std::ostringstream out;
    out << "{";
    out << "\"cluster_id\":" << s.cluster_id << ",";
    out << "\"label\":\"" << escape_json(s.label) << "\",";
    out << "\"identified\":" << (s.identified ? "true" : "false") << ",";
    out << "\"duration_sec\":" << s.duration_sec << ",";
    out << "\"confidence\":" << s.confidence;
    out << "}";
    return out.str();
}
#endif

// ---------------------------------------------------------------------------
// Meeting discovery
// ---------------------------------------------------------------------------

static std::vector<MeetingInfo> discover_meetings(const fs::path& output_dir) {
    std::vector<MeetingInfo> meetings;
    if (!fs::is_directory(output_dir)) return meetings;

    for (const auto& entry : fs::directory_iterator(output_dir)) {
        if (!entry.is_directory()) continue;

        auto audio_path = find_audio_file(entry.path());
        if (audio_path.empty()) continue;

        MeetingInfo info;
        info.name = entry.path().filename().string();

        auto spk_path = entry.path() / "speakers.json";
        info.has_speakers_json = fs::exists(spk_path);
        info.speaker_count = 0;

#if RECMEET_USE_SHERPA
        if (info.has_speakers_json) {
            auto spks = load_meeting_speakers(entry.path());
            info.speaker_count = static_cast<int>(spks.size());
        }
#endif

        // Extract date from directory name (YYYY-MM-DD_HH-MM format)
        auto dirname = info.name;
        if (dirname.size() >= 10 && dirname[4] == '-' && dirname[7] == '-')
            info.date = dirname.substr(0, 10);
        else
            info.date = "";

        meetings.push_back(std::move(info));
    }

    // Sort by name descending (most recent first, since names are date-based)
    std::sort(meetings.begin(), meetings.end(),
              [](const MeetingInfo& a, const MeetingInfo& b) {
                  return a.name > b.name;
              });

    return meetings;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    fprintf(stderr,
        "Usage: recmeet-web [OPTIONS]\n"
        "\n"
        "Recmeet speaker management web server.\n"
        "\n"
        "Options:\n"
        "  --port PORT          Listen port (default: 8384)\n"
        "  --bind ADDR          Bind address (default: 127.0.0.1)\n"
        "  --web-root DIR       Static file directory\n"
        "  --config PATH        Config file path\n"
        "  --log-level LEVEL    none|error|warn|info (default: none)\n"
        "  --help               Show this message\n"
        "  --version            Show version\n"
    );
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Parse CLI args
    std::string config_path;
    std::string web_root;
    std::string log_level_str;
    int port = -1;
    std::string bind_addr;

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string(argv[i]);
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--version" || arg == "-V") {
            printf("recmeet-web %s\n", RECMEET_VERSION);
            return 0;
        } else if (arg == "--port") {
            auto v = next();
            if (!v) { fprintf(stderr, "--port requires a value\n"); return 1; }
            port = std::atoi(v);
        } else if (arg == "--bind") {
            auto v = next();
            if (!v) { fprintf(stderr, "--bind requires a value\n"); return 1; }
            bind_addr = v;
        } else if (arg == "--web-root") {
            auto v = next();
            if (!v) { fprintf(stderr, "--web-root requires a value\n"); return 1; }
            web_root = v;
        } else if (arg == "--config") {
            auto v = next();
            if (!v) { fprintf(stderr, "--config requires a value\n"); return 1; }
            config_path = v;
        } else if (arg == "--log-level") {
            auto v = next();
            if (!v) { fprintf(stderr, "--log-level requires a value\n"); return 1; }
            log_level_str = v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage();
            return 1;
        }
    }

    // Load config
    Config cfg = load_config(config_path);

    // CLI overrides
    if (port > 0) cfg.web_port = port;
    if (!bind_addr.empty()) cfg.web_bind = bind_addr;
    if (!log_level_str.empty()) cfg.log_level_str = log_level_str;

    // Init logging
    log_init(parse_log_level(cfg.log_level_str), cfg.log_dir);

    // Resolve speaker DB directory
    fs::path speaker_db_dir = cfg.speaker_db.empty()
        ? default_speaker_db_dir() : cfg.speaker_db;

    // Resolve output directory
    fs::path output_dir = cfg.output_dir;
    if (output_dir.is_relative())
        output_dir = fs::current_path() / output_dir;

    // Resolve web root
    fs::path web_root_path;
    if (!web_root.empty()) {
        web_root_path = web_root;
    } else {
        // Try relative to binary first
        auto exe_dir = fs::read_symlink("/proc/self/exe").parent_path();
        auto rel = exe_dir / ".." / "share" / "recmeet" / "web";
        if (fs::is_directory(rel)) {
            web_root_path = fs::canonical(rel);
        }
#ifdef RECMEET_WEB_ROOT
        else if (fs::is_directory(RECMEET_WEB_ROOT)) {
            web_root_path = RECMEET_WEB_ROOT;
        }
#endif
    }

    // Mutex for thread-safe speaker DB writes
    std::mutex speaker_mu;

    // Create server
    httplib::Server server;
    g_server = &server;

    // CORS headers for local dev
    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    });

    // OPTIONS preflight
    server.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // Health check
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // --- Speaker endpoints ---

    server.Get("/api/speakers", [&](const httplib::Request&, httplib::Response& res) {
        auto profiles = load_speaker_db(speaker_db_dir);
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < profiles.size(); ++i) {
            if (i > 0) out << ",";
            out << speaker_profile_to_json(profiles[i]);
        }
        out << "]";
        res.set_content(out.str(), "application/json");
    });

    server.Get(R"(/api/speakers/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        auto profiles = load_speaker_db(speaker_db_dir);
        for (const auto& p : profiles) {
            if (p.name == name) {
                res.set_content(speaker_profile_detail_to_json(p), "application/json");
                return;
            }
        }
        res.status = 404;
        res.set_content(json_error("speaker not found"), "application/json");
    });

    server.Delete(R"(/api/speakers/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto name = req.matches[1].str();
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(speaker_mu);
        if (remove_speaker(speaker_db_dir, name)) {
            res.set_content(json_ok(), "application/json");
        } else {
            res.status = 404;
            res.set_content(json_error("speaker not found"), "application/json");
        }
    });

    server.Post("/api/speakers/reset", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(speaker_mu);
        int removed = reset_speakers(speaker_db_dir);
        res.set_content(json_ok_removed(removed), "application/json");
    });

    server.Post("/api/speakers/enroll", [&](const httplib::Request& req, httplib::Response& res) {
        auto name = json_get_str(req.body, "name");
        auto meeting_dir_name = json_get_str(req.body, "meeting_dir");
        int cluster_id = json_get_int(req.body, "cluster_id", -1);

        if (name.empty() || meeting_dir_name.empty() || cluster_id < 0) {
            res.status = 400;
            res.set_content(json_error("missing required fields: name, meeting_dir, cluster_id"),
                            "application/json");
            return;
        }

        if (!is_safe_dirname(meeting_dir_name)) {
            res.status = 400;
            res.set_content(json_error("invalid meeting_dir"), "application/json");
            return;
        }

        auto meeting_path = output_dir / meeting_dir_name;
        if (!fs::is_directory(meeting_path)) {
            res.status = 404;
            res.set_content(json_error("meeting directory not found"), "application/json");
            return;
        }

#if RECMEET_USE_SHERPA
        auto meeting_speakers = load_meeting_speakers(meeting_path);
        if (meeting_speakers.empty()) {
            res.status = 404;
            res.set_content(json_error("no speakers.json in meeting directory"),
                            "application/json");
            return;
        }

        // Find the speaker with matching cluster_id
        const MeetingSpeaker* found = nullptr;
        for (const auto& s : meeting_speakers) {
            if (s.cluster_id == cluster_id) {
                found = &s;
                break;
            }
        }
        if (!found) {
            res.status = 404;
            res.set_content(json_error("cluster_id not found in meeting speakers"),
                            "application/json");
            return;
        }
        if (found->embedding.empty()) {
            res.status = 400;
            res.set_content(json_error("speaker has no embedding data"), "application/json");
            return;
        }

        std::lock_guard<std::mutex> lock(speaker_mu);

        // Load or create profile
        SpeakerProfile profile;
        auto profiles = load_speaker_db(speaker_db_dir);
        for (const auto& p : profiles) {
            if (p.name == name) {
                profile = p;
                break;
            }
        }

        if (profile.name.empty()) {
            profile.name = name;
            profile.created = iso_now();
        }
        profile.updated = iso_now();
        profile.embeddings.push_back(found->embedding);

        save_speaker(speaker_db_dir, profile);
        res.set_content(json_ok(), "application/json");
#else
        res.status = 501;
        res.set_content(json_error("enrollment requires sherpa-onnx support"), "application/json");
#endif
    });

    // --- Meeting endpoints ---

    server.Get("/api/meetings", [&](const httplib::Request&, httplib::Response& res) {
        auto meetings = discover_meetings(output_dir);
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < meetings.size(); ++i) {
            if (i > 0) out << ",";
            out << meeting_info_to_json(meetings[i]);
        }
        out << "]";
        res.set_content(out.str(), "application/json");
    });

    server.Get(R"(/api/meetings/([^/]+)/speakers)", [&](const httplib::Request& req, httplib::Response& res) {
        auto dir_name = req.matches[1].str();
        if (!is_safe_dirname(dir_name)) {
            res.status = 400;
            res.set_content(json_error("invalid meeting directory name"), "application/json");
            return;
        }

        auto meeting_path = output_dir / dir_name;
        if (!fs::is_directory(meeting_path)) {
            res.status = 404;
            res.set_content(json_error("meeting directory not found"), "application/json");
            return;
        }

#if RECMEET_USE_SHERPA
        auto speakers = load_meeting_speakers(meeting_path);
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < speakers.size(); ++i) {
            if (i > 0) out << ",";
            out << meeting_speaker_to_json(speakers[i]);
        }
        out << "]";
        res.set_content(out.str(), "application/json");
#else
        res.set_content("[]", "application/json");
#endif
    });

    // Serve meeting note as rendered HTML page
    server.Get(R"(/api/meetings/([^/]+)/note)", [&](const httplib::Request& req, httplib::Response& res) {
        auto dir_name = req.matches[1].str();
        if (!is_safe_dirname(dir_name)) {
            res.status = 400;
            res.set_content(json_error("invalid meeting directory name"), "application/json");
            return;
        }

        auto meeting_path = output_dir / dir_name;
        if (!fs::is_directory(meeting_path)) {
            res.status = 404;
            res.set_content(json_error("meeting directory not found"), "application/json");
            return;
        }

        // Find Meeting_*.md — check meeting dir first, then note_dir/YYYY/MM/
        fs::path note_path;
        auto starts_with = [](const std::string& s, const std::string& prefix) {
            return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
        };
        auto ends_with = [](const std::string& s, const std::string& suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        auto find_note = [&](const fs::path& dir) -> fs::path {
            if (!fs::is_directory(dir)) return {};
            for (const auto& entry : fs::directory_iterator(dir)) {
                auto fname = entry.path().filename().string();
                if (starts_with(fname, "Meeting_") && ends_with(fname, ".md"))
                    return entry.path();
            }
            return {};
        };

        note_path = find_note(meeting_path);

        // Try note_dir/YYYY/MM/ if not found in meeting dir
        // Match on dir_name prefix (e.g. "2026-03-08_09-28") since multiple
        // meetings may share the same month subdirectory
        if (note_path.empty() && !cfg.note_dir.empty() && dir_name.size() >= 7) {
            auto year = dir_name.substr(0, 4);
            auto month = dir_name.substr(5, 2);
            auto note_subdir = fs::path(cfg.note_dir) / year / month;
            if (fs::is_directory(note_subdir)) {
                for (const auto& entry : fs::directory_iterator(note_subdir)) {
                    auto fname = entry.path().filename().string();
                    // Note filename: Meeting_YYYY-MM-DD_HH-MM[_Title].md
                    if (starts_with(fname, "Meeting_" + dir_name) && ends_with(fname, ".md")) {
                        note_path = entry.path();
                        break;
                    }
                }
            }
        }

        if (note_path.empty()) {
            res.status = 404;
            res.set_content(json_error("no meeting note found"), "application/json");
            return;
        }

        std::ifstream in(note_path);
        if (!in) {
            res.status = 500;
            res.set_content(json_error("failed to read note file"), "application/json");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

        res.set_content("{\"path\":\"" + escape_json(note_path.filename().string()) +
                        "\",\"content\":\"" + escape_json(content) + "\"}",
                        "application/json");
    });

    // --- Static file serving (SPA) ---
    if (!web_root_path.empty() && fs::is_directory(web_root_path)) {
        server.set_mount_point("/", web_root_path.string());
    }

    // Signal handling
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    fprintf(stderr, "recmeet-web %s listening on %s:%d\n",
            RECMEET_VERSION, cfg.web_bind.c_str(), cfg.web_port);
    if (!web_root_path.empty())
        fprintf(stderr, "  web root: %s\n", web_root_path.string().c_str());
    fprintf(stderr, "  speaker db: %s\n", speaker_db_dir.string().c_str());
    fprintf(stderr, "  output dir: %s\n", output_dir.string().c_str());

    if (!server.listen(cfg.web_bind, cfg.web_port)) {
        fprintf(stderr, "Failed to listen on %s:%d\n", cfg.web_bind.c_str(), cfg.web_port);
        return 1;
    }

    log_shutdown();
    return 0;
}
