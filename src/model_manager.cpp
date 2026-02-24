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
#endif

} // namespace recmeet
