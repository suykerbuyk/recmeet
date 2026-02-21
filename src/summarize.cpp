#include "summarize.h"
#include "http_client.h"

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

namespace {

const char* SYSTEM_PROMPT =
    "You are a precise meeting summarizer. Produce a well-structured Markdown summary. "
    "Use the exact section headings provided. Be thorough but concise. "
    "If a section has no relevant content, write 'None identified.'";

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

    fprintf(stderr, "Requesting summary from %s (model: %s)...\n", api_url.c_str(), model.c_str());

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
                             const std::string& context) {
    fprintf(stderr, "Loading LLM model: %s\n", model_path.filename().c_str());

    llama_backend_init();

    llama_model_params model_params = llama_model_default_params();
    llama_model* model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model) {
        llama_backend_free();
        throw RecmeetError("Failed to load LLM model: " + model_path.string());
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 8192;
    ctx_params.n_threads = 4;

    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        llama_model_free(model);
        llama_backend_free();
        throw RecmeetError("Failed to create LLM context");
    }

    // Build prompt
    std::string prompt = std::string(SYSTEM_PROMPT) + "\n\n" +
                         build_user_prompt(transcript, context);

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

    fprintf(stderr, "Prompt: %d tokens, generating summary...\n", n_prompt);

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

    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        throw RecmeetError("LLM decode failed");
    }

    // Generate
    std::string result;
    int max_tokens = 4096;

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
        llama_decode(ctx, batch);
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
