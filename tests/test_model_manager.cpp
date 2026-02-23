// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "model_manager.h"

#include <fstream>

using namespace recmeet;

TEST_CASE("ensure_whisper_model: throws for unknown model name", "[model_manager]") {
    CHECK_THROWS_AS(ensure_whisper_model("nonexistent"), RecmeetError);
}

TEST_CASE("ensure_whisper_model: returns cached path when file exists", "[model_manager]") {
    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);

    fs::path fake_model = model_dir / "ggml-tiny.bin";
    {
        std::ofstream out(fake_model, std::ios::binary);
        // Write some non-empty content to simulate a cached model
        out << "fake model data for testing purposes";
    }

    fs::path result = ensure_whisper_model("tiny");
    CHECK(result == fake_model);

    fs::remove(fake_model);
}

TEST_CASE("ensure_whisper_model: returns expected filename for each model", "[model_manager]") {
    // Create cached files for all known models to test path construction
    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);

    struct Case { std::string name; std::string filename; };
    std::vector<Case> cases = {
        {"tiny",     "ggml-tiny.bin"},
        {"base",     "ggml-base.bin"},
        {"small",    "ggml-small.bin"},
        {"medium",   "ggml-medium.bin"},
        {"large-v3", "ggml-large-v3.bin"},
    };

    for (const auto& c : cases) {
        fs::path fake = model_dir / c.filename;
        bool existed = fs::exists(fake) && fs::file_size(fake) > 0;
        if (!existed) {
            std::ofstream out(fake, std::ios::binary);
            out << "fake model data";
        }

        fs::path result = ensure_whisper_model(c.name);
        CHECK(result.filename() == c.filename);

        if (!existed)
            fs::remove(fake);
    }
}

TEST_CASE("is_whisper_model_cached: returns false when model not present", "[model_manager]") {
    fs::path model_dir = models_dir() / "whisper";
    fs::path fake = model_dir / "ggml-tiny.bin";
    bool existed = fs::exists(fake);
    if (existed) fs::rename(fake, fake.string() + ".bak");

    CHECK_FALSE(is_whisper_model_cached("tiny"));

    if (existed) fs::rename(fake.string() + ".bak", fake);
}

TEST_CASE("is_whisper_model_cached: returns true when model is cached", "[model_manager]") {
    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);
    fs::path fake = model_dir / "ggml-tiny.bin";

    bool existed = fs::exists(fake) && fs::file_size(fake) > 0;
    if (!existed) {
        std::ofstream out(fake, std::ios::binary);
        out << "fake model data";
    }

    CHECK(is_whisper_model_cached("tiny"));

    if (!existed) fs::remove(fake);
}

TEST_CASE("is_whisper_model_cached: returns false for zero-byte file", "[model_manager]") {
    fs::path model_dir = models_dir() / "whisper";
    fs::create_directories(model_dir);
    fs::path fake = model_dir / "ggml-small.bin";

    bool existed = fs::exists(fake);
    fs::path backup = fake.string() + ".bak";
    if (existed) fs::rename(fake, backup);

    // Create zero-byte file
    { std::ofstream out(fake, std::ios::binary); }

    CHECK_FALSE(is_whisper_model_cached("small"));

    fs::remove(fake);
    if (existed) fs::rename(backup, fake);
}

TEST_CASE("is_whisper_model_cached: throws for unknown model name", "[model_manager]") {
    CHECK_THROWS_AS(is_whisper_model_cached("nonexistent"), RecmeetError);
}

#if RECMEET_USE_LLAMA
TEST_CASE("ensure_llama_model: returns path for existing file", "[model_manager]") {
    auto dir = fs::temp_directory_path() / "recmeet_test_llama";
    fs::create_directories(dir);
    fs::path fake_model = dir / "test.gguf";
    {
        std::ofstream out(fake_model, std::ios::binary);
        out << "fake gguf data";
    }

    fs::path result = ensure_llama_model(fake_model.string());
    CHECK(result == fake_model);

    fs::remove_all(dir);
}

TEST_CASE("ensure_llama_model: throws for missing model", "[model_manager]") {
    CHECK_THROWS_AS(ensure_llama_model("/nonexistent/path/model.gguf"), RecmeetError);
}
#endif
