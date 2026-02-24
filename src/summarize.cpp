// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "summarize.h"
#include "http_client.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

#if RECMEET_USE_LLAMA
#include <llama.h>
#include <algorithm>
#include <vector>
#endif

namespace recmeet {

// --- Public JSON helpers (declared in summarize.h) ---

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 32);
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

std::string json_extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find('"', pos + search.size() + 1); // skip ':'
    if (pos == std::string::npos) return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += '\\'; result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

bool is_chat_model(const std::string& id) {
    static const char* skip[] = {
        "embed", "tts", "whisper", "dall-e", "image",
        "video", "moderation", "audio", "realtime",
    };
    for (const auto* s : skip)
        if (id.find(s) != std::string::npos)
            return false;
    return true;
}

std::vector<std::string> fetch_models(const std::string& models_url, const std::string& api_key) {
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key},
    };
    std::string response = http_get(models_url, headers);

    // Extract all "id" values from the response JSON
    std::vector<std::string> models;
    std::string search = "\"id\"";
    size_t pos = 0;
    while ((pos = response.find(search, pos)) != std::string::npos) {
        pos += search.size();
        // Skip to the value string
        pos = response.find('"', pos + 1); // skip ':'
        if (pos == std::string::npos) break;
        pos++; // skip opening quote

        std::string id;
        while (pos < response.size() && response[pos] != '"') {
            id += response[pos];
            pos++;
        }

        if (!id.empty() && is_chat_model(id))
            models.push_back(id);
    }

    std::sort(models.begin(), models.end());
    return models;
}

std::string build_user_prompt(const std::string& transcript, const std::string& context) {
    std::ostringstream oss;
    oss << "Summarize the following meeting transcript.\n\n";

    if (!context.empty()) {
        oss << "## Pre-Meeting Context\n\n" << context << "\n\n";
    }

    oss << "## Required Sections\n\n"
        << "### Overview\n"
        << "A 2-3 sentence high-level summary of what the meeting covered.\n\n"
        << "### Key Points\n"
        << "Bullet list of the most important topics discussed.\n\n"
        << "### Decisions\n"
        << "Bullet list of decisions that were made (who decided what).\n\n"
        << "### Action Items\n"
        << "Bullet list formatted as: **[Owner]** — task description (deadline if mentioned).\n\n"
        << "### Open Questions\n"
        << "Bullet list of unresolved questions or topics deferred to a future meeting.\n\n"
        << "### Participants\n"
        << "List of identifiable speakers/participants (if discernible from context).\n\n"
        << "---\n\n## Transcript\n\n" << transcript;

    return oss.str();
}

namespace {

const char* SYSTEM_PROMPT =
    "You are a precise meeting summarizer. Produce a well-structured Markdown summary. "
    "Use the exact section headings provided. Be thorough but concise. "
    "If a section has no relevant content, write 'None identified.'";

} // anonymous namespace

std::string summarize_http(const std::string& transcript,
                            const std::string& api_url,
                            const std::string& api_key,
                            const std::string& model,
                            const std::string& context) {
    std::string user_prompt = build_user_prompt(transcript, context);

    // Build JSON request body
    std::ostringstream json;
    json << "{"
         << "\"model\":\"" << json_escape(model) << "\","
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\"" << json_escape(SYSTEM_PROMPT) << "\"},"
         << "{\"role\":\"user\",\"content\":\"" << json_escape(user_prompt) << "\"}"
         << "],"
         << "\"temperature\":0.3,"
         << "\"max_tokens\":4096"
         << "}";

    log_info("Requesting summary from %s (model: %s)...", api_url.c_str(), model.c_str());

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key},
    };

    std::string response = http_post_json(api_url, json.str(), headers);

    // Extract content from OpenAI-compatible response
    std::string content = json_extract_string(response, "content");
    if (content.empty())
        throw RecmeetError("Empty summary response from API");

    return content;
}

#if RECMEET_USE_LLAMA
std::string summarize_local(const std::string& transcript,
                             const fs::path& model_path,
                             const std::string& context,
                             int threads) {
    log_info("Loading LLM model: %s", model_path.filename().c_str());

    llama_backend_init();

    llama_model_params model_params = llama_model_default_params();
    llama_model* model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model) {
        llama_backend_free();
        throw RecmeetError("Failed to load LLM model: " + model_path.string());
    }

    // Use model's native context size, capped to avoid OOM on CPU inference
    constexpr uint32_t MAX_LOCAL_CTX = 32768;
    constexpr int GENERATION_BUDGET = 4096;

    int32_t model_ctx = llama_model_n_ctx_train(model);
    uint32_t n_ctx = std::min(static_cast<uint32_t>(model_ctx), MAX_LOCAL_CTX);

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = n_ctx;
    ctx_params.n_batch = n_ctx;  // allow full-prompt decode in a single batch
    ctx_params.n_threads = threads > 0 ? threads : default_thread_count();

    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        llama_model_free(model);
        llama_backend_free();
        throw RecmeetError("Failed to create LLM context");
    }

    uint32_t actual_ctx = llama_n_ctx(ctx);
    log_info("Context: %u tokens (model native: %d, cap: %u)",
            actual_ctx, model_ctx, MAX_LOCAL_CTX);

    // Build prompt using model's chat template
    std::string user_prompt = build_user_prompt(transcript, context);
    const char* tmpl = llama_model_chat_template(model, nullptr);

    llama_chat_message messages[] = {
        {"system", SYSTEM_PROMPT},
        {"user",   user_prompt.c_str()},
    };

    // First call with length=0 to get required buffer size
    int32_t needed = llama_chat_apply_template(
        tmpl, messages, 2, true, nullptr, 0);
    if (needed < 0) {
        // Fallback: raw concatenation if template not recognized
        log_warn("chat template not available, using raw prompt");
    }

    std::string prompt;
    if (needed > 0) {
        std::vector<char> buf(needed + 1);
        llama_chat_apply_template(tmpl, messages, 2, true, buf.data(), buf.size());
        prompt.assign(buf.data(), needed);
    } else {
        prompt = std::string(SYSTEM_PROMPT) + "\n\n" + user_prompt;
    }

    // Tokenize
    const llama_vocab* vocab = llama_model_get_vocab(model);
    int n_prompt = prompt.size() + 256;
    std::vector<llama_token> tokens(n_prompt);
    n_prompt = llama_tokenize(vocab, prompt.c_str(), prompt.size(),
                              tokens.data(), tokens.size(), true, true);
    if (n_prompt < 0) {
        tokens.resize(-n_prompt);
        n_prompt = llama_tokenize(vocab, prompt.c_str(), prompt.size(),
                                  tokens.data(), tokens.size(), true, true);
    }
    tokens.resize(n_prompt);

    // Truncate prompt to fit within context budget (prevents SIGABRT in llama_decode)
    int max_prompt_tokens = static_cast<int>(actual_ctx) - GENERATION_BUDGET;
    if (max_prompt_tokens < 256) {
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        throw RecmeetError("LLM context too small: " + std::to_string(actual_ctx)
                           + " tokens (need at least " + std::to_string(GENERATION_BUDGET + 256) + ")");
    }

    if (n_prompt > max_prompt_tokens) {
        log_warn("Prompt (%d tokens) exceeds context budget (%d tokens). "
                 "Truncating transcript to fit.", n_prompt, max_prompt_tokens);
        n_prompt = max_prompt_tokens;
        tokens.resize(n_prompt);
    }

    log_info("Prompt: %d tokens, generating summary...", n_prompt);

    // Helper: add a token to a batch
    auto batch_add = [](llama_batch& b, llama_token token, llama_pos pos,
                        llama_seq_id seq_id, bool logits) {
        int i = b.n_tokens;
        b.token[i]    = token;
        b.pos[i]      = pos;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = seq_id;
        b.logits[i]   = logits ? 1 : 0;
        b.n_tokens++;
    };

    // Create batch and process prompt
    llama_batch batch = llama_batch_init(std::max(n_prompt, 512), 0, 1);
    batch.n_tokens = 0;
    for (int i = 0; i < n_prompt; ++i)
        batch_add(batch, tokens[i], i, 0, i == n_prompt - 1);

    int decode_status = llama_decode(ctx, batch);
    if (decode_status != 0) {
        llama_batch_free(batch);
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        if (decode_status == 1)
            throw RecmeetError("LLM decode failed: no KV slot for batch (prompt: "
                               + std::to_string(n_prompt) + " tokens, ctx: "
                               + std::to_string(actual_ctx) + ")");
        throw RecmeetError("LLM decode failed (status " + std::to_string(decode_status) + ")");
    }

    // Generate
    std::string result;
    int max_tokens = GENERATION_BUDGET;

    auto* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

    for (int i = 0; i < max_tokens; ++i) {
        llama_token new_token = llama_sampler_sample(sampler, ctx, -1);

        if (llama_vocab_is_eog(vocab, new_token))
            break;

        char buf[128];
        int n = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        if (n > 0)
            result.append(buf, n);

        batch.n_tokens = 0;
        batch_add(batch, new_token, n_prompt + i, 0, true);
        if (llama_decode(ctx, batch) != 0)
            break;
    }

    llama_sampler_free(sampler);
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    if (result.empty())
        throw RecmeetError("LLM produced no output");

    return result;
}
#endif

} // namespace recmeet
