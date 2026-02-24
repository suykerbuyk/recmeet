// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "device_enum.h"
#include "util.h"

#include <pulse/pulseaudio.h>
#include <cstring>
#include <regex>

namespace recmeet {

namespace {

struct EnumContext {
    std::vector<AudioSource> sources;
    bool done = false;
    bool error = false;
    pa_mainloop* mainloop = nullptr;
};

void source_info_cb(pa_context*, const pa_source_info* info, int eol, void* userdata) {
    auto* ctx = static_cast<EnumContext*>(userdata);
    if (eol > 0) {
        ctx->done = true;
        pa_mainloop_quit(ctx->mainloop, 0);
        return;
    }
    if (!info) return;

    AudioSource src;
    src.name = info->name;
    src.description = info->description ? info->description : "";
    src.is_monitor = (info->monitor_of_sink != PA_INVALID_INDEX);
    ctx->sources.push_back(std::move(src));
}

void context_state_cb(pa_context* c, void* userdata) {
    auto* ctx = static_cast<EnumContext*>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    switch (state) {
        case PA_CONTEXT_READY: {
            pa_operation* op = pa_context_get_source_info_list(c, source_info_cb, userdata);
            if (op) pa_operation_unref(op);
            break;
        }
        case PA_CONTEXT_FAILED:
            ctx->error = true;
            ctx->done = true;
            pa_mainloop_quit(ctx->mainloop, 1);
            break;
        case PA_CONTEXT_TERMINATED:
            // Normal shutdown after disconnect — not an error
            if (!ctx->done) {
                ctx->error = true;
                pa_mainloop_quit(ctx->mainloop, 1);
            }
            ctx->done = true;
            break;
        default:
            break;
    }
}

struct ServerInfoContext {
    std::string default_source_name;
    bool done = false;
    bool error = false;
    pa_mainloop* mainloop = nullptr;
};

void server_info_cb(pa_context*, const pa_server_info* info, void* userdata) {
    auto* ctx = static_cast<ServerInfoContext*>(userdata);
    if (info && info->default_source_name)
        ctx->default_source_name = info->default_source_name;
    ctx->done = true;
    pa_mainloop_quit(ctx->mainloop, 0);
}

void server_context_state_cb(pa_context* c, void* userdata) {
    auto* ctx = static_cast<ServerInfoContext*>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    switch (state) {
        case PA_CONTEXT_READY: {
            pa_operation* op = pa_context_get_server_info(c, server_info_cb, userdata);
            if (op) pa_operation_unref(op);
            break;
        }
        case PA_CONTEXT_FAILED:
            ctx->error = true;
            ctx->done = true;
            pa_mainloop_quit(ctx->mainloop, 1);
            break;
        case PA_CONTEXT_TERMINATED:
            if (!ctx->done) {
                ctx->error = true;
                pa_mainloop_quit(ctx->mainloop, 1);
            }
            ctx->done = true;
            break;
        default:
            break;
    }
}

bool is_monitor_source(const AudioSource& src) {
    const std::string suffix = ".monitor";
    bool has_monitor_suffix = src.name.size() >= suffix.size() &&
        src.name.compare(src.name.size() - suffix.size(), suffix.size(), suffix) == 0;
    return src.is_monitor || has_monitor_suffix;
}

} // anonymous namespace

std::vector<AudioSource> list_sources() {
    EnumContext ectx;

    pa_mainloop* ml = pa_mainloop_new();
    if (!ml) throw DeviceError("Failed to create PulseAudio mainloop");
    ectx.mainloop = ml;

    pa_mainloop_api* api = pa_mainloop_get_api(ml);
    pa_context* ctx = pa_context_new(api, "recmeet-enum");
    if (!ctx) {
        pa_mainloop_free(ml);
        throw DeviceError("Failed to create PulseAudio context");
    }

    pa_context_set_state_callback(ctx, context_state_cb, &ectx);

    int connect_ret = pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
    if (connect_ret < 0) {
        const char* err = pa_strerror(pa_context_errno(ctx));
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        throw DeviceError(std::string("Failed to connect to PulseAudio/PipeWire: ") + err);
    }

    // Run the mainloop — callbacks will quit it when done
    int ret = 0;
    int run_ret = pa_mainloop_run(ml, &ret);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    (void)run_ret;
    if (ectx.error)
        throw DeviceError("PulseAudio connection failed during source enumeration");

    return ectx.sources;
}

std::string get_default_source_name() {
    ServerInfoContext sctx;

    pa_mainloop* ml = pa_mainloop_new();
    if (!ml) return "";
    sctx.mainloop = ml;

    pa_mainloop_api* api = pa_mainloop_get_api(ml);
    pa_context* ctx = pa_context_new(api, "recmeet-default");
    if (!ctx) {
        pa_mainloop_free(ml);
        return "";
    }

    pa_context_set_state_callback(ctx, server_context_state_cb, &sctx);

    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return "";
    }

    int ret = 0;
    pa_mainloop_run(ml, &ret);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    if (sctx.error) return "";
    return sctx.default_source_name;
}

DetectedSources detect_sources(const std::string& pattern) {
    DetectedSources result;
    result.all = list_sources();

    if (!pattern.empty()) {
        // Explicit regex path
        std::regex re(pattern, std::regex_constants::icase);

        for (const auto& src : result.all) {
            if (!std::regex_search(src.name, re))
                continue;
            if (is_monitor_source(src)) {
                if (result.monitor.empty())
                    result.monitor = src.name;
            } else {
                if (result.mic.empty())
                    result.mic = src.name;
            }
        }

        return result;
    }

    // Empty pattern: use system default source
    std::string default_name = get_default_source_name();

    if (!default_name.empty()) {
        for (const auto& src : result.all) {
            if (src.name == default_name) {
                if (is_monitor_source(src))
                    result.monitor = src.name;
                else
                    result.mic = src.name;
                break;
            }
        }
    }

    // Fill whichever slot(s) are still empty from first available
    for (const auto& src : result.all) {
        if (result.mic.empty() && !is_monitor_source(src))
            result.mic = src.name;
        if (result.monitor.empty() && is_monitor_source(src))
            result.monitor = src.name;
        if (!result.mic.empty() && !result.monitor.empty())
            break;
    }

    return result;
}

} // namespace recmeet
