// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase C.9 — `pipeline.cpp` post-recording-split.
//
// Pre-C.9 this file owned both the live-recording surface (PipeWire/Pulse
// capture + streaming CaptionEngine + WAV writer) and the postprocessing
// pipeline (VAD / transcribe / diarize / summarize / note write). C.9
// retired the daemon-side live-recording path; the daemon no longer needs
// any PipeWire/Pulse dependency. The live-recording half has moved to
// `src/live_recording.cpp` (and the `recmeet_live_capture` library which
// only the CLI binary links). What remains here is the daemon-safe core:
// `resolve_caption_model_dir`, the postprocess helpers, and
// `run_postprocessing()`.

#include "pipeline.h"
#include "caption_engine.h"
#include "caption_vtt.h"
#include "config.h"
#include "diarize.h"
#include "ipc_protocol.h"
#include "speaker_id.h"
#include "vad.h"
#include "audio_file.h"
#include "log.h"
#include "model_manager.h"
#include "transcribe.h"
#include "summarize.h"
#include "note.h"
#include "notify.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace recmeet {

std::string read_context_file(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void save_meeting_context(const fs::path& out_dir, const std::string& context_inline,
                          const fs::path& context_file, const std::string& timestamp,
                          const std::string& meeting_id) {
    if (context_inline.empty() && context_file.empty() && meeting_id.empty()) return;
    fs::path path = timestamp.empty()
        ? out_dir / LEGACY_CONTEXT_NAME
        : out_dir / (std::string(CONTEXT_PREFIX) + timestamp + ".json");
    std::ofstream out(path);
    if (!out) {
        log_warn("Failed to write %s: %s", path.filename().c_str(), path.c_str());
        return;
    }
    out << "{\"context\":\"" << json_escape(context_inline)
        << "\",\"context_file\":\"" << json_escape(context_file.string()) << "\"";
    // C.11: only emit "meeting_id" when non-empty so v1-written context.json
    // shape is preserved byte-for-byte on the no-id path.
    if (!meeting_id.empty()) {
        out << ",\"meeting_id\":\"" << json_escape(meeting_id) << "\"";
    }
    out << "}";
    log_info("Saved %s", path.filename().c_str());
}

namespace {
/// Parse a context.json file and extract the named field. Returns empty
/// string when the file is unreadable, malformed, or the field is absent.
/// Shared by load_meeting_context and load_meeting_id.
std::string read_context_field(const fs::path& path, const std::string& field) {
    if (path.empty()) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string json = buf.str();

    IpcMessage msg;
    std::string wrapped = "{\"id\":0,\"result\":" + json + "}";
    if (parse_ipc_message(wrapped, msg) && msg.type == IpcMessageType::Response) {
        auto it = msg.response.result.find(field);
        if (it != msg.response.result.end())
            return json_val_as_string(it->second);
    }
    return "";
}
} // anonymous namespace

std::string load_meeting_context(const fs::path& out_dir) {
    return read_context_field(find_context_file(out_dir), "context");
}

std::string load_meeting_id(const fs::path& out_dir) {
    std::string id = read_context_field(find_context_file(out_dir), "meeting_id");
    // Defensive: reject a malformed value rather than poison the MeetingIndex.
    // is_valid_meeting_id accepts "" — round-trips fine through this check.
    if (!is_valid_meeting_id(id)) return "";
    return id;
}


fs::path resolve_caption_model_dir(const std::string& name) {
    // Default model directory name pinned by Phase 0.2. Phase 4 will
    // expand this into proper config plumbing; for now this string is the
    // single source of truth.
    constexpr const char* kDefaultCaptionModel = "en-2023-06-26";
    std::string subdir = name.empty() ? std::string(kDefaultCaptionModel) : name;

    const char* home = std::getenv("HOME");
    fs::path base = home && home[0]
        ? fs::path(home) / ".local" / "share" / "recmeet"
        : fs::path("/tmp") / "recmeet";
    return base / "models" / "sherpa" / "online" / subdir;
}


/// Compute weighted progress across VAD segments.
/// Returns overall percent (0-100) given current segment index and within-segment percent.
int vad_weighted_progress(size_t seg_index, int seg_percent, const std::vector<size_t>& seg_samples) {
    if (seg_samples.empty()) return 0;
    size_t total = 0;
    for (auto n : seg_samples) total += n;
    if (total == 0) return 0;

    size_t done = 0;
    for (size_t i = 0; i < seg_index && i < seg_samples.size(); ++i)
        done += seg_samples[i];
    if (seg_index < seg_samples.size())
        done += static_cast<size_t>(seg_samples[seg_index] * seg_percent / 100.0);

    return static_cast<int>(done * 100 / total);
}

std::string build_initial_prompt(const std::vector<std::string>& speaker_names,
                                 const std::string& vocabulary) {
    std::string result;

    for (const auto& name : speaker_names) {
        if (!result.empty()) result += ", ";
        result += name;
    }

    if (!vocabulary.empty()) {
        std::istringstream ss(vocabulary);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto start = token.find_first_not_of(" \t");
            auto end = token.find_last_not_of(" \t");
            if (start == std::string::npos) continue;
            std::string trimmed = token.substr(start, end - start + 1);
            if (!result.empty()) result += ", ";
            result += trimmed;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Phase C.8 — diarization.json artifact writer used by the enroll-mode
// subprocess path. The daemon reads this file back to populate the
// DiarizationCache; the format is purely subprocess→daemon (not on any
// wire), so we hand-roll a minimal flat JSON rather than pulling in a full
// JSON encoder. Schema:
//
//   {
//     "num_speakers": N,
//     "clusters": [
//       { "idx": 0, "duration_ms": 12345, "embedding": [f, f, ...] },
//       ...
//     ]
//   }
//
// Visible for unit tests via the namespace forward decl in pipeline.h is
// NOT exported; the daemon's reader (`load_diarization_json` in
// diarization_cache.cpp's pp_worker helper) only needs the file format,
// not the writer.
// ---------------------------------------------------------------------------
namespace {

void write_diarization_artifact(const fs::path& out_dir,
                                int num_speakers,
                                const std::vector<int64_t>& duration_ms,
                                const std::vector<std::vector<float>>& embeddings) {
    fs::create_directories(out_dir);
    fs::path p = out_dir / "diarization.json";
    std::ofstream out(p);
    if (!out) throw RecmeetError("Could not write " + p.string());
    out << "{\n";
    out << "  \"num_speakers\": " << num_speakers << ",\n";
    out << "  \"clusters\": [\n";
    for (int i = 0; i < num_speakers; ++i) {
        out << "    { \"idx\": " << i
            << ", \"duration_ms\": " << duration_ms[i]
            << ", \"embedding\": [";
        const auto& emb = (i < static_cast<int>(embeddings.size()))
                              ? embeddings[i]
                              : std::vector<float>{};
        for (size_t j = 0; j < emb.size(); ++j) {
            if (j > 0) out << ", ";
            out << emb[j];
        }
        out << "] }";
        if (i + 1 < num_speakers) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // anonymous namespace

PipelineResult run_postprocessing(const JobConfig& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase, ProgressCallback on_progress,
                                  StopToken* stop) {
    log_debug("pipeline: run_postprocessing ENTER (dir=%s)", input.out_dir.c_str());

    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    auto check_cancel = [&]() {
        if (stop && stop->stop_requested())
            throw RecmeetError("Cancelled");
    };

    int threads = cfg.threads > 0 ? cfg.threads : default_thread_count();

    // -----------------------------------------------------------------------
    // Phase C.8 — enroll-mode bypass.
    //
    // When the daemon's `process.submit` carried `mode=enroll`, the
    // resulting Job's `cfg.enroll_mode` is true. We run diarization ONLY
    // (skip transcribe / summarize / note-write), extract one centroid
    // per discovered cluster, and write `diarization.json` to the
    // out_dir so the daemon's pp_worker reads it back and populates the
    // server-side DiarizationCache. The eventual `enroll.finalize` IPC
    // verb consumes the cache to write the speakers DB.
    //
    // We deliberately leave `note_path` empty in the PipelineResult so
    // the daemon's existing job.complete emission logic sees an empty
    // `note_path` (the daemon's enroll-aware lift below replaces the
    // event shape entirely; the legacy field is ignored).
    // -----------------------------------------------------------------------
#if RECMEET_USE_SHERPA
    if (cfg.enroll_mode) {
        log_info("pipeline: enroll-mode — diarize only, no transcribe/summarize");

        auto samples = read_wav_float(input.audio_path);
        if (samples.empty())
            throw RecmeetError("enroll-mode: cannot read audio from "
                               + input.audio_path.string());

        phase("diarizing");
        DiarizeProgressCallback diar_progress;
        if (on_progress) {
            diar_progress = [&on_progress](int done, int total) {
                on_progress("diarizing", total > 0 ? done * 100 / total : 0);
            };
        }
        check_cancel();
        DiarizeResult diar = diarize(samples.data(), samples.size(),
                                     cfg.num_speakers, threads,
                                     cfg.cluster_threshold, diar_progress);

        // Extract one centroid per cluster (raw, non-normalized — matches
        // the CLI --enroll flow's `profile.embeddings.push_back(...)`
        // semantic).
        std::vector<std::vector<float>> centroids(diar.num_speakers);
        std::vector<int64_t> durations_ms(diar.num_speakers, 0);
        for (const auto& seg : diar.segments) {
            if (seg.speaker >= 0 && seg.speaker < diar.num_speakers) {
                durations_ms[seg.speaker] +=
                    static_cast<int64_t>((seg.end - seg.start) * 1000.0);
            }
        }

        if (diar.num_speakers > 0) {
            phase("identifying speakers");
            if (on_progress) on_progress("identifying speakers", 0);
            auto model_paths = ensure_sherpa_models();
            SpeakerEmbeddingSession sess(model_paths.embedding, threads);
            for (int i = 0; i < diar.num_speakers; ++i) {
                check_cancel();
                auto emb = extract_speaker_embedding(
                    sess, samples.data(), samples.size(), diar, i);
                centroids[i] = std::move(emb);
                if (on_progress)
                    on_progress("identifying speakers",
                                (i + 1) * 100 / diar.num_speakers);
            }
        }

        write_diarization_artifact(input.out_dir, diar.num_speakers,
                                   durations_ms, centroids);
        log_info("pipeline: enroll-mode complete — wrote %s/diarization.json "
                 "(%d clusters)",
                 input.out_dir.c_str(), diar.num_speakers);

        PipelineResult r;
        r.output_dir = input.out_dir;
        // r.note_path intentionally empty — enroll runs produce no note.
        phase("complete");
        return r;
    }
#endif  // RECMEET_USE_SHERPA

    // Build initial_prompt from enrolled speaker names + vocabulary hints
    std::string initial_prompt;
    {
        std::vector<std::string> names;
        if (cfg.speaker_id) {
            fs::path db_dir = cfg.speaker_db.empty()
                ? default_speaker_db_dir() : cfg.speaker_db;
            names = list_speakers(db_dir);
        }
        initial_prompt = build_initial_prompt(names, cfg.vocabulary);
        if (!initial_prompt.empty())
            log_info("Vocabulary hints: %s", initial_prompt.c_str());
    }

    // --- Transcribe + Diarize (if not pre-computed) ---
    std::string transcript_text = input.transcript_text;

    if (transcript_text.empty()) {
        log_info("Using %d threads for inference.", threads);

        TranscriptResult result;
        {   // --- audio buffer scope --- freed after diarization, before summarization
            auto samples = read_wav_float(input.audio_path);
            log_info("Audio: %.1fs (%zu samples)",
                    samples.size() / (float)SAMPLE_RATE, samples.size());
            log_debug("pipeline: loaded audio (%.1fs, %zu samples)",
                      samples.size() / (float)SAMPLE_RATE, samples.size());

            {   // --- whisper model scope --- freed before diarization
                log_debug("pipeline: loading whisper model '%s'...", cfg.whisper_model.c_str());
                fs::path model_path = ensure_whisper_model(cfg.whisper_model);
                WhisperModel model(model_path);
                log_debug("pipeline: whisper model loaded");

#if RECMEET_USE_SHERPA
                if (cfg.vad) {
                    phase("detecting speech");
                    notify("Detecting speech...", "VAD segmentation");

                    VadConfig vad_cfg;
                    vad_cfg.threshold = cfg.vad_threshold;
                    vad_cfg.min_silence_duration = cfg.vad_min_silence;
                    vad_cfg.min_speech_duration = cfg.vad_min_speech;
                    vad_cfg.max_speech_duration = cfg.vad_max_speech;

                    log_debug("pipeline: running VAD");
                    auto vad_result = detect_speech(samples, vad_cfg, threads);
                    log_debug("pipeline: VAD complete (%zu speech segments)",
                              vad_result.segments.size());

                    if (!vad_result.segments.empty()) {
                        phase("transcribing");
                        notify("Transcribing...", "Model: " + cfg.whisper_model +
                               " (" + std::to_string(vad_result.segments.size()) + " segments)");

                        // Build sample counts for weighted progress
                        std::vector<size_t> seg_samples;
                        for (const auto& seg : vad_result.segments)
                            seg_samples.push_back(static_cast<size_t>(seg.end_sample - seg.start_sample));

                        log_debug("pipeline: transcribing...");
                        for (size_t si = 0; si < vad_result.segments.size(); ++si) {
                            check_cancel();
                            const auto& seg = vad_result.segments[si];
                            size_t n = seg_samples[si];

                            TranscribeOptions opts;
                            opts.language = cfg.language;
                            opts.initial_prompt = initial_prompt;
                            opts.threads = threads;
                            opts.stop = stop;
                            if (on_progress) {
                                opts.on_progress = [&, si](int pct) {
                                    int overall = vad_weighted_progress(si, pct, seg_samples);
                                    on_progress("transcribing", overall);
                                };
                            }

                            auto seg_result = transcribe(model, samples.data() + seg.start_sample,
                                                         n, seg.start, opts);
                            for (auto& s : seg_result.segments)
                                result.segments.push_back(std::move(s));
                            if (result.language.empty())
                                result.language = seg_result.language;
                        }
                        log_info("Transcribed %zu segments across %zu VAD regions",
                                result.segments.size(), vad_result.segments.size());
                        log_debug("pipeline: transcription complete (%zu segments)",
                                  result.segments.size());
                    } else {
                        log_info("VAD found no speech — skipping transcription.");
                    }
                } else
#endif
                {
                    phase("transcribing");
                    notify("Transcribing...", "Model: " + cfg.whisper_model);

                    TranscribeOptions opts;
                    opts.language = cfg.language;
                    opts.initial_prompt = initial_prompt;
                    opts.threads = threads;
                    opts.stop = stop;
                    if (on_progress) {
                        opts.on_progress = [&](int pct) {
                            on_progress("transcribing", pct);
                        };
                    }

                    log_debug("pipeline: transcribing...");
                    result = transcribe(model, samples.data(), samples.size(), 0.0, opts);
                    log_info("Transcribed %d segments (language: %s)",
                            (int)result.segments.size(), result.language.c_str());
                    log_debug("pipeline: transcription complete (%zu segments)",
                              result.segments.size());
                }
            }   // whisper model freed

#if RECMEET_USE_SHERPA
            check_cancel();
            if (cfg.diarize && !result.segments.empty()) {
                phase("diarizing");
                notify("Diarizing...", "Identifying speakers");
                DiarizeProgressCallback diar_progress;
                if (on_progress) {
                    diar_progress = [&on_progress](int done, int total) {
                        on_progress("diarizing", total > 0 ? done * 100 / total : 0);
                    };
                }

                // T2.2 dispatch: chunked path triggers when audio is large
                // enough to actually produce ≥ 2 real chunks plus 2 minutes
                // of headroom. Below the threshold the single-call path runs
                // unchanged. Threshold formula pinned in plan rev 7 line 429.
                DiarizeChunkConfig chunk_cfg;
                chunk_cfg.chunk_minutes = cfg.chunk_minutes;
                chunk_cfg.overlap_seconds = cfg.chunk_overlap_sec;
                chunk_cfg.stitch_threshold = cfg.stitch_threshold;
                const size_t chunk_threshold_samples = static_cast<size_t>(
                    (chunk_cfg.chunk_minutes * 60.0f
                     + chunk_cfg.overlap_seconds + 120.0f)
                    * static_cast<float>(SAMPLE_RATE));
                const bool use_chunked =
                    samples.size() > chunk_threshold_samples;

                DiarizeResult diar;
                std::map<int, std::vector<float>> chunked_centroids;
                if (use_chunked) {
                    log_debug("pipeline: diarizing chunked "
                              "(%d speakers, %.1f min chunks, %.1f s overlap, "
                              "stitch %.2f, threshold %.2f)",
                              cfg.num_speakers, chunk_cfg.chunk_minutes,
                              chunk_cfg.overlap_seconds,
                              chunk_cfg.stitch_threshold, cfg.cluster_threshold);
                    auto chunked = diarize_chunked(
                        samples.data(), samples.size(),
                        cfg.num_speakers, threads, cfg.cluster_threshold,
                        chunk_cfg, diar_progress);
                    diar = std::move(chunked.diar);
                    chunked_centroids = std::move(chunked.centroids);
                    log_debug("pipeline: chunked diarization complete "
                              "(%zu segments, %zu centroids)",
                              diar.segments.size(), chunked_centroids.size());
                } else {
                    log_debug("pipeline: diarizing (%d speakers)", cfg.num_speakers);
                    diar = diarize(samples.data(), samples.size(),
                                   cfg.num_speakers, threads, cfg.cluster_threshold,
                                   diar_progress);
                    log_debug("pipeline: diarization complete (%zu segments)",
                              diar.segments.size());
                }

                // Speaker identification + embedding extraction
                std::map<int, std::string> speaker_names;
                {
                    fs::path db_dir = cfg.speaker_db.empty()
                        ? default_speaker_db_dir() : cfg.speaker_db;
                    auto db = cfg.speaker_id ? load_speaker_db(db_dir)
                                             : std::vector<SpeakerProfile>{};

                    // Phase event fires regardless of db state — the embedding
                    // extraction call below is the long-running step and the
                    // daemon's progress-staleness watchdog needs the phase
                    // marker to recognize identify-speakers and apply the
                    // heartbeat-as-liveness rule (T1C.1).
                    phase("identifying speakers");
                    if (on_progress) on_progress("identifying speakers", 0);
                    if (!db.empty()) {
                        notify("Identifying speakers...",
                               std::to_string(db.size()) + " enrolled");
                    }
                    log_debug("pipeline: identifying speakers (%s)",
                              use_chunked ? "centroid bypass" : "audio re-extract");
                    IdentifyResult id_result;
                    if (use_chunked) {
                        // Reuse centroids already extracted during chunked
                        // diarization — no second pass over the audio. This
                        // is the bypass T2.2 H1 added to skip the ~10 GB
                        // working-set spike of the per-cluster re-extraction.
                        id_result = identify_speakers_with_centroids(
                            chunked_centroids, db, cfg.speaker_threshold);
                    } else {
                        auto model_paths = ensure_sherpa_models();
                        id_result = identify_speakers(
                            samples.data(), samples.size(), diar, db,
                            model_paths.embedding, cfg.speaker_threshold, threads);
                    }
                    if (on_progress) on_progress("identifying speakers", 100);
                    log_debug("pipeline: speaker ID complete");
                    speaker_names = id_result.names;

                    // Build and save speakers.json
                    std::vector<MeetingSpeaker> meeting_speakers;
                    for (const auto& [sid, emb] : id_result.embeddings) {
                        MeetingSpeaker ms{};
                        ms.cluster_id = sid;
                        ms.embedding = emb;

                        // Compute duration from diarization segments
                        for (const auto& seg : diar.segments)
                            if (seg.speaker == sid)
                                ms.duration_sec += static_cast<float>(seg.end - seg.start);

                        auto name_it = id_result.names.find(sid);
                        if (name_it != id_result.names.end()) {
                            ms.label = name_it->second;
                            ms.identified = true;
                            ms.confidence = id_result.scores[sid];
                        } else {
                            char buf[16];
                            std::snprintf(buf, sizeof(buf), "Speaker_%02d", sid + 1);
                            ms.label = buf;
                            ms.identified = false;
                            ms.confidence = 0.0f;
                        }
                        meeting_speakers.push_back(std::move(ms));
                    }

                    if (!meeting_speakers.empty()) {
                        try {
                            save_meeting_speakers(input.out_dir, meeting_speakers, input.timestamp);
                            log_info("Saved speakers.json with %zu speaker(s)",
                                     meeting_speakers.size());
                        } catch (const std::exception& e) {
                            log_warn("Failed to save speakers.json: %s", e.what());
                        }
                    }
                }

                result.segments = merge_speakers(result.segments, diar, speaker_names);
            }
#endif
        }   // audio buffer freed

        transcript_text = result.to_string();
        if (transcript_text.empty())
            throw RecmeetError("Transcription produced no text.");
    }

    // --- Summarize ---
    check_cancel();
    PipelineResult pipe_result;
    pipe_result.output_dir = input.out_dir;

    std::string summary_text;
    // Merge context: inline > file > saved context.json (reprocess fallback)
    std::string context_text = cfg.context_inline;
    if (!cfg.context_file.empty()) {
        std::string file_ctx = read_context_file(cfg.context_file);
        if (!file_ctx.empty()) {
            if (!context_text.empty()) context_text += "\n\n";
            context_text += file_ctx;
        }
    }
    if (context_text.empty() && !cfg.reprocess_dir.empty()) {
        context_text = load_meeting_context(input.out_dir);
    }

    MeetingMetadata metadata;

    if (!cfg.no_summary) {
        phase("summarizing");

#if RECMEET_USE_LLAMA
        if (!cfg.llm_model.empty()) {  // NOLINT(readability-misleading-indentation)
            // Local summarization
            if (!cfg.batch_mode) notify("Summarizing...", "Local LLM");
            log_debug("pipeline: summarizing (provider=local)");
            try {
                fs::path llm_path = ensure_llama_model(cfg.llm_model);
                summary_text = summarize_local(transcript_text, llm_path, context_text, threads, cfg.llm_mmap);
                log_debug("pipeline: summary complete");
            } catch (const std::exception& e) {
                log_warn("Local summary failed: %s", e.what());
            }
        } else
#endif
        if (!cfg.api_key.empty()) {
            std::string url = cfg.api_url;
            if (url.empty()) {
                const auto* prov = find_provider(cfg.provider);
                if (prov) url = std::string(prov->base_url) + "/chat/completions";
                else url = "https://api.x.ai/v1/chat/completions";
            }
            if (!cfg.batch_mode) notify("Summarizing...", "Sending to " + cfg.api_model);
            log_debug("pipeline: summarizing (provider=%s)", cfg.provider.c_str());
            try {
                summary_text = summarize_http(transcript_text, url,
                                               cfg.api_key, cfg.api_model, context_text);
                log_debug("pipeline: summary complete");
            } catch (const std::exception& e) {
                log_warn("Summary failed: %s", e.what());
                log_warn("Transcript is still available.");
            }
        } else {
            log_warn("No API key and no local LLM — skipping summary.");
        }

        if (!summary_text.empty()) {  // NOLINT(readability-misleading-indentation)
            metadata = extract_meeting_metadata(summary_text);
            summary_text = strip_metadata_block(summary_text);
        }
    } else {
        log_info("Summary skipped (--no-summary).");
    }

    // --- Meeting note output ---
    try {
        auto [date_str, time_str] = resolve_meeting_time(input.out_dir, input.audio_path);

        MeetingData md;
        md.date = date_str;
        md.time = time_str;
        md.summary_text = summary_text;
        md.transcript_text = transcript_text;
        pipe_result.transcript_text = transcript_text;
        md.context_text = context_text;
        md.output_dir = input.out_dir;
        if (!cfg.note_dir.empty()) {
            md.note_dir = cfg.note_dir;
            fs::create_directories(md.note_dir);
        } else {
            md.note_dir = input.out_dir;
        }

        // AI-derived metadata
        md.title = metadata.title;
        md.description = metadata.description;
        md.ai_tags = metadata.tags;
        md.participants = metadata.participants;
        md.duration_seconds = get_audio_duration_seconds(input.audio_path);
        md.whisper_model = cfg.whisper_model;

        pipe_result.note_path = write_meeting_note(cfg.note, md);
    } catch (const std::exception& e) {
        log_warn("Meeting note failed: %s", e.what());
    }

    // --- Done ---
    phase("complete");
    if (!cfg.batch_mode) notify("Meeting complete", input.out_dir.string());
    fprintf(stderr, "\nDone! Files in: %s\n", input.out_dir.c_str());
    log_debug("pipeline: run_postprocessing EXIT");
    return pipe_result;
}

} // namespace recmeet
