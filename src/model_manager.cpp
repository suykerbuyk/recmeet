#include "model_manager.h"
#include "http_client.h"

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
    fprintf(stderr, "Downloading %s ...\n", url.c_str());

    // Use libcurl to download with progress
    auto data = http_get(url);

    std::ofstream out(dest, std::ios::binary);
    if (!out)
        throw RecmeetError("Cannot write to: " + dest.string());
    out.write(data.data(), data.size());
    out.close();

    fprintf(stderr, "Downloaded: %s (%.1f MB)\n", dest.filename().c_str(),
            data.size() / (1024.0 * 1024.0));
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

} // namespace recmeet
