// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "model_manager.h"
#include "http_client.h"
#include "log.h"

#include <cstdio>
#include <fstream>
#include <map>

namespace recmeet {

namespace {

struct ModelInfo {
    std::string url;
    std::string filename;
};

// Whisper GGUF models hosted on Hugging Face
const std::map<std::string, ModelInfo> WHISPER_MODELS = {
    {"tiny",     {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin",     "ggml-tiny.bin"}},
    {"base",     {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin",     "ggml-base.bin"}},
    {"small",    {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin",    "ggml-small.bin"}},
    {"medium",   {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin",   "ggml-medium.bin"}},
    {"large-v3", {"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3.bin", "ggml-large-v3.bin"}},
};

void download_file(const std::string& url, const fs::path& dest) {
    log_info("Downloading %s ...", url.c_str());

    // Use libcurl to download with progress
    auto data = http_get(url);

    std::ofstream out(dest, std::ios::binary);
    if (!out)
        throw RecmeetError("Cannot write to: " + dest.string());
    out.write(data.data(), data.size());
    out.close();

    log_info("Downloaded: %s (%.1f MB)", dest.filename().c_str(),
            data.size() / (1024.0 * 1024.0));
}

void download_and_extract_tarball(const std::string& url, const fs::path& dest_dir) {
    fs::path tarball = dest_dir / "download.tar.bz2";
    download_file(url, tarball);

    fs::create_directories(dest_dir);
    std::string cmd = "tar xjf " + tarball.string() +
                      " -C " + dest_dir.string() +
                      " --strip-components=1 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        // Try without --strip-components (for single-file downloads)
        cmd = "tar xjf " + tarball.string() +
              " -C " + dest_dir.string() + " 2>/dev/null";
        std::system(cmd.c_str());
    }

    fs::remove(tarball);
}

} // anonymous namespace

bool is_whisper_model_cached(const std::string& model_name) {
    auto it = WHISPER_MODELS.find(model_name);
    if (it == WHISPER_MODELS.end())
        throw RecmeetError("Unknown whisper model: " + model_name +
                           ". Available: tiny, base, small, medium, large-v3");
    fs::path model_path = models_dir() / "whisper" / it->second.filename;
    return fs::exists(model_path) && fs::file_size(model_path) > 0;
}

fs::path ensure_whisper_model(const std::string& model_name) {
    auto it = WHISPER_MODELS.find(model_name);
    if (it == WHISPER_MODELS.end())
        throw RecmeetError("Unknown whisper model: " + model_name +
                           ". Available: tiny, base, small, medium, large-v3");

    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);

    fs::path model_path = model_dir / it->second.filename;
    if (fs::exists(model_path) && fs::file_size(model_path) > 0)
        return model_path;

    download_file(it->second.url, model_path);
    return model_path;
}

fs::path download_whisper_model(const std::string& model_name) {
    auto it = WHISPER_MODELS.find(model_name);
    if (it == WHISPER_MODELS.end())
        throw RecmeetError("Unknown whisper model: " + model_name +
                           ". Available: tiny, base, small, medium, large-v3");

    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);

    fs::path model_path = model_dir / it->second.filename;
    download_file(it->second.url, model_path);
    return model_path;
}

#if RECMEET_USE_LLAMA
fs::path ensure_llama_model(const std::string& model_name) {
    // For now, expect the user to provide a path or a known model name
    // This can be expanded with a model registry similar to whisper
    fs::path model_dir = models_dir() / "llama";
    fs::create_directories(model_dir);

    // If model_name looks like a path, use it directly
    fs::path as_path(model_name);
    if (fs::exists(as_path))
        return as_path;

    // Check if it's already in our model dir
    fs::path model_path = model_dir / model_name;
    if (fs::exists(model_path))
        return model_path;

    throw RecmeetError("LLM model not found: " + model_name +
                       ". Place GGUF file in " + model_dir.string());
}
#endif

#if RECMEET_USE_SHERPA
namespace {

const char* SHERPA_SEGMENTATION_URL =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/"
    "speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2";

const char* SHERPA_EMBEDDING_URL =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/"
    "speaker-recongition-models/"
    "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";

fs::path sherpa_seg_path() {
    return models_dir() / "sherpa" / "segmentation" / "model.onnx";
}

fs::path sherpa_emb_path() {
    return models_dir() / "sherpa" / "embedding" /
           "3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
}

} // anonymous namespace

bool is_sherpa_model_cached() {
    auto seg = sherpa_seg_path();
    auto emb = sherpa_emb_path();
    return fs::exists(seg) && fs::file_size(seg) > 0 &&
           fs::exists(emb) && fs::file_size(emb) > 0;
}

SherpaModelPaths ensure_sherpa_models() {
    auto seg = sherpa_seg_path();
    auto emb = sherpa_emb_path();

    if (!fs::exists(seg) || fs::file_size(seg) == 0) {
        fs::path seg_dir = seg.parent_path();
        fs::create_directories(seg_dir);
        download_and_extract_tarball(SHERPA_SEGMENTATION_URL, seg_dir);
        if (!fs::exists(seg))
            throw RecmeetError("Segmentation model not found after extraction: " + seg.string());
    }

    if (!fs::exists(emb) || fs::file_size(emb) == 0) {
        fs::path emb_dir = emb.parent_path();
        fs::create_directories(emb_dir);
        download_file(SHERPA_EMBEDDING_URL, emb);
    }

    return {seg, emb};
}

SherpaModelPaths download_sherpa_models() {
    auto seg = sherpa_seg_path();
    auto emb = sherpa_emb_path();

    fs::create_directories(seg.parent_path());
    fs::create_directories(emb.parent_path());

    download_and_extract_tarball(SHERPA_SEGMENTATION_URL, seg.parent_path());
    if (!fs::exists(seg))
        throw RecmeetError("Segmentation model not found after extraction: " + seg.string());

    download_file(SHERPA_EMBEDDING_URL, emb);
    return {seg, emb};
}

// --- VAD (Silero) ---

namespace {

const char* SILERO_VAD_URL =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/"
    "asr-models/silero_vad.onnx";

fs::path vad_model_path() {
    return models_dir() / "sherpa" / "vad" / "silero_vad.onnx";
}

} // anonymous namespace

bool is_vad_model_cached() {
    auto p = vad_model_path();
    return fs::exists(p) && fs::file_size(p) > 0;
}

fs::path ensure_vad_model() {
    auto p = vad_model_path();
    if (fs::exists(p) && fs::file_size(p) > 0)
        return p;

    fs::create_directories(p.parent_path());
    download_file(SILERO_VAD_URL, p);
    return p;
}

fs::path download_vad_model() {
    auto p = vad_model_path();
    fs::create_directories(p.parent_path());
    download_file(SILERO_VAD_URL, p);
    return p;
}
#endif

// ---------------------------------------------------------------------------
// Phase 4 — streaming caption (online ASR) models.
//
// Layout mirrors the sherpa-onnx model zoo: each curated model has a
// repo-id under huggingface and ships a `.tar.bz2` whose top-level dir
// matches the repo name. Extraction strips one directory component so the
// files land directly in `<models>/sherpa/online/<short-name>/`.
//
// The two models we surface for V1:
//   * en-2023-06-26 — Phase 0.2-locked Zipformer (~74 MB), the default.
//   * en-small      — 20 M-param fast Zipformer (~28 MB), low-end-host fallback.
//
// `is_caption_model_cached()` is deliberately resilient: model-zoo entries
// don't all use the same `encoder-epoch-99-avg-1.onnx` filename, so we
// prefix-match on `encoder/decoder/joiner` and accept any `*.onnx`
// variant. The same logic gates download skip in `ensure_caption_model`.
// ---------------------------------------------------------------------------

namespace {

struct CaptionModelInfo {
    std::string url;        // tarball URL (sherpa-onnx model zoo on HF)
    std::string size_hint;  // human-readable size for the prompt
};

const std::map<std::string, CaptionModelInfo>& caption_models_table() {
    static const std::map<std::string, CaptionModelInfo> models = {
        {"en-2023-06-26",
            {"https://huggingface.co/csukuangfj/"
             "sherpa-onnx-streaming-zipformer-en-2023-06-26/"
             "resolve/main/sherpa-onnx-streaming-zipformer-en-2023-06-26.tar.bz2",
             "~74 MB"}},
        {"en-small",
            {"https://huggingface.co/csukuangfj/"
             "sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/"
             "resolve/main/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17.tar.bz2",
             "~28 MB"}},
    };
    return models;
}

constexpr const char* DEFAULT_CAPTION_MODEL = "en-2023-06-26";

std::string resolve_caption_model_name(const std::string& name) {
    return name.empty() ? std::string(DEFAULT_CAPTION_MODEL) : name;
}

} // anonymous namespace

std::vector<std::string> known_caption_models() {
    std::vector<std::string> out;
    out.reserve(caption_models_table().size());
    for (const auto& [k, _v] : caption_models_table())
        out.push_back(k);
    return out;
}

std::string caption_model_size_hint(const std::string& name) {
    std::string canonical = resolve_caption_model_name(name);
    const auto& table = caption_models_table();
    auto it = table.find(canonical);
    return it == table.end() ? std::string{} : it->second.size_hint;
}

fs::path caption_model_dir(const std::string& name) {
    return models_dir() / "sherpa" / "online" / resolve_caption_model_name(name);
}

bool is_caption_model_cached(const std::string& name) {
    fs::path dir = caption_model_dir(name);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;

    bool has_enc = false, has_dec = false, has_join = false, has_tok = false;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (!e.is_regular_file()) continue;
        if (fs::file_size(e.path()) == 0) continue;
        std::string n = e.path().filename().string();
        bool is_onnx = n.size() >= 5 &&
                       n.compare(n.size() - 5, 5, ".onnx") == 0;
        if (is_onnx && n.compare(0, 7, "encoder") == 0) has_enc = true;
        if (is_onnx && n.compare(0, 7, "decoder") == 0) has_dec = true;
        if (is_onnx && n.compare(0, 6, "joiner")  == 0) has_join = true;
        if (n == "tokens.txt") has_tok = true;
    }
    return has_enc && has_dec && has_join && has_tok;
}

#if RECMEET_USE_SHERPA
fs::path ensure_caption_model(const std::string& name) {
    std::string canonical = resolve_caption_model_name(name);
    const auto& table = caption_models_table();
    auto it = table.find(canonical);
    if (it == table.end()) {
        std::string available;
        for (const auto& [k, _v] : table) {
            if (!available.empty()) available += ", ";
            available += k;
        }
        throw RecmeetError("Unknown caption model: " + canonical +
                           ". Available: " + available);
    }

    fs::path dir = caption_model_dir(canonical);
    if (is_caption_model_cached(canonical))
        return dir;

    fs::create_directories(dir);
    download_and_extract_tarball(it->second.url, dir);

    if (!is_caption_model_cached(canonical)) {
        throw RecmeetError(
            "Caption model files not found after extraction in: " + dir.string() +
            " (expected encoder-*.onnx, decoder-*.onnx, joiner-*.onnx, tokens.txt)");
    }
    return dir;
}

fs::path download_caption_model(const std::string& name) {
    std::string canonical = resolve_caption_model_name(name);
    const auto& table = caption_models_table();
    auto it = table.find(canonical);
    if (it == table.end()) {
        std::string available;
        for (const auto& [k, _v] : table) {
            if (!available.empty()) available += ", ";
            available += k;
        }
        throw RecmeetError("Unknown caption model: " + canonical +
                           ". Available: " + available);
    }
    fs::path dir = caption_model_dir(canonical);
    fs::create_directories(dir);
    download_and_extract_tarball(it->second.url, dir);
    return dir;
}
#else
fs::path ensure_caption_model(const std::string& /*name*/) {
    throw RecmeetError(
        "captions require RECMEET_USE_SHERPA=ON build "
        "(rebuild with: cmake -B build -DRECMEET_USE_SHERPA=ON)");
}
#endif

std::vector<ModelStatus> list_cached_models() {
    std::vector<ModelStatus> result;

    for (const auto& [name, info] : WHISPER_MODELS) {
        ModelStatus s;
        s.name = name;
        s.category = "whisper";
        s.path = (models_dir() / "whisper" / info.filename).string();
        if (fs::exists(s.path) && fs::file_size(s.path) > 0) {
            s.cached = true;
            s.size_bytes = static_cast<int64_t>(fs::file_size(s.path));
        }
        result.push_back(std::move(s));
    }

#if RECMEET_USE_SHERPA
    {
        ModelStatus s;
        s.name = "segmentation";
        s.category = "sherpa";
        s.path = sherpa_seg_path().string();
        if (fs::exists(s.path) && fs::file_size(s.path) > 0) {
            s.cached = true;
            s.size_bytes = static_cast<int64_t>(fs::file_size(s.path));
        }
        result.push_back(std::move(s));
    }
    {
        ModelStatus s;
        s.name = "embedding";
        s.category = "sherpa";
        s.path = sherpa_emb_path().string();
        if (fs::exists(s.path) && fs::file_size(s.path) > 0) {
            s.cached = true;
            s.size_bytes = static_cast<int64_t>(fs::file_size(s.path));
        }
        result.push_back(std::move(s));
    }
    {
        ModelStatus s;
        s.name = "vad";
        s.category = "vad";
        s.path = vad_model_path().string();
        if (fs::exists(s.path) && fs::file_size(s.path) > 0) {
            s.cached = true;
            s.size_bytes = static_cast<int64_t>(fs::file_size(s.path));
        }
        result.push_back(std::move(s));
    }
#endif

    // Phase 4 — streaming caption models. Surfaced in both build flavors so
    // operators on a no-sherpa build still see the cache layout (the
    // download/run path is sherpa-only, gated above by ensure_caption_model).
    for (const auto& name : known_caption_models()) {
        ModelStatus s;
        s.name = name;
        s.category = "caption";
        s.path = caption_model_dir(name).string();
        s.cached = is_caption_model_cached(name);
        if (s.cached) {
            // Sum size of *.onnx + tokens.txt for the report.
            int64_t total = 0;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(s.path, ec)) {
                if (ec) break;
                if (e.is_regular_file()) total += static_cast<int64_t>(fs::file_size(e.path()));
            }
            s.size_bytes = total;
        }
        result.push_back(std::move(s));
    }

    return result;
}

} // namespace recmeet
