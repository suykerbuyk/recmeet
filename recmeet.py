#!/usr/bin/env python3
"""recmeet — Improved meeting recorder with transcription and Grok summarization."""

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time
import wave
from datetime import datetime
from pathlib import Path

import requests
from dotenv import load_dotenv

API_URL = "https://api.x.ai/v1/chat/completions"


class AudioValidationError(Exception):
    """Raised when audio file validation fails."""


DEFAULT_DEVICE_PATTERN = r"bd.h200|00:05:30:00:05:4E"
SUMMARY_SYSTEM_PROMPT = (
    "You are a precise meeting summarizer. Produce a well-structured Markdown summary. "
    "Use the exact section headings provided. Be thorough but concise. "
    "If a section has no relevant content, write 'None identified.'"
)
SUMMARY_USER_PROMPT = """\
Summarize the following meeting transcript.

## Required Sections

### Overview
A 2-3 sentence high-level summary of what the meeting covered.

### Key Points
Bullet list of the most important topics discussed.

### Decisions
Bullet list of decisions that were made (who decided what).

### Action Items
Bullet list formatted as: **[Owner]** — task description (deadline if mentioned).

### Open Questions
Bullet list of unresolved questions or topics deferred to a future meeting.

### Participants
List of identifiable speakers/participants (if discernible from context).

---

## Transcript

{transcript}
"""


def notify(title, body=""):
    """Send a desktop notification via notify-send (silently ignored if unavailable)."""
    try:
        subprocess.Popen(
            ["notify-send", "--app-name=recmeet", title, body],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        pass


def _list_pactl_sources():
    """Return a list of all PipeWire/PulseAudio source names."""
    try:
        result = subprocess.run(
            ["pactl", "list", "short", "sources"],
            capture_output=True,
            text=True,
            check=True,
        )
    except FileNotFoundError:
        print("Error: 'pactl' not found. Install pipewire-pulse or pulseaudio-utils.", file=sys.stderr)
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(f"Error running pactl: {e.stderr.strip()}", file=sys.stderr)
        sys.exit(1)

    sources = []
    for line in result.stdout.strip().splitlines():
        fields = line.split("\t")
        if len(fields) >= 2:
            sources.append(fields[1])
    return sources


def detect_sources(pattern):
    """Auto-detect mic and monitor sources matching the given regex pattern.

    Returns {"mic": name|None, "monitor": name|None, "all_sources": [...]}.
    Sources whose names end in .monitor are classified as monitors; others as mic inputs.
    """
    all_sources = _list_pactl_sources()
    regex = re.compile(pattern, re.IGNORECASE)

    mic = None
    monitor = None
    for name in all_sources:
        if regex.search(name):
            if name.endswith(".monitor"):
                monitor = monitor or name
            else:
                mic = mic or name

    return {"mic": mic, "monitor": monitor, "all_sources": all_sources}


def find_recorder():
    """Find the best available recorder binary. Prefer pw-record over parecord."""
    for cmd in ["pw-record", "parecord"]:
        if subprocess.run(["which", cmd], capture_output=True).returncode == 0:
            return cmd
    print(
        "Error: No recorder found. Install pipewire (pw-record) or pulseaudio-utils (parecord).",
        file=sys.stderr,
    )
    sys.exit(1)


def display_elapsed(stop_event):
    """Background thread that shows a live elapsed-time counter on stderr."""
    start = time.monotonic()
    while not stop_event.is_set():
        elapsed = int(time.monotonic() - start)
        mins, secs = divmod(elapsed, 60)
        print(f"\rRecording... {mins:02d}:{secs:02d}", end="", flush=True, file=sys.stderr)
        stop_event.wait(1.0)
    # Clear the line
    print("\r" + " " * 30 + "\r", end="", flush=True, file=sys.stderr)


def _build_record_cmd(recorder, source, output_path):
    """Build a recorder command list for the given source and output path."""
    output_str = str(output_path)
    if recorder == "pw-record":
        return [
            "pw-record",
            "--target", source,
            "--format", "s16",
            "--rate", "16000",
            "--channels", "1",
            output_str,
        ]
    return [
        "parecord",
        "--device", source,
        "--file-format=wav",
        "--format=s16le",
        "--rate=16000",
        "--channels=1",
        output_str,
    ]


def record_audio(source, output_path):
    """Record audio from the given PipeWire/PulseAudio source until Ctrl+C."""
    recorder = find_recorder()
    cmd = _build_record_cmd(recorder, source, output_path)

    print(f"Recording from: {source}")
    print(f"Using: {recorder}")
    print("Press Ctrl+C to stop.\n")

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    stop_event = threading.Event()
    timer_thread = threading.Thread(target=display_elapsed, args=(stop_event,), daemon=True)
    timer_thread.start()

    try:
        proc.wait()
    except KeyboardInterrupt:
        proc.send_signal(signal.SIGINT)
        proc.wait()
    finally:
        stop_event.set()
        timer_thread.join(timeout=2)

    print("Recording stopped.")
    return output_path


def record_dual_audio(mic_source, monitor_source, mic_path, monitor_path):
    """Record from mic and monitor sources in parallel until Ctrl+C.

    Uses pw-record for the mic but always parecord for the monitor, because
    .monitor sources are a PulseAudio abstraction that pw-record doesn't capture.

    Sends SIGINT to both processes on Ctrl+C, with SIGKILL fallback after 5s.
    """
    recorder = find_recorder()
    mic_cmd = _build_record_cmd(recorder, mic_source, mic_path)
    # Monitor sources require parecord — pw-record records silence from .monitor names
    if subprocess.run(["which", "parecord"], capture_output=True).returncode != 0:
        print("Error: parecord required for monitor capture. Install pipewire-pulse or pulseaudio-utils.", file=sys.stderr)
        sys.exit(1)
    monitor_cmd = _build_record_cmd("parecord", monitor_source, monitor_path)

    print(f"Recording mic:     {mic_source} (via {recorder})")
    print(f"Recording monitor: {monitor_source} (via parecord)")
    print("Press Ctrl+C to stop.\n")

    mic_proc = subprocess.Popen(mic_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    monitor_proc = subprocess.Popen(monitor_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    stop_event = threading.Event()
    timer_thread = threading.Thread(target=display_elapsed, args=(stop_event,), daemon=True)
    timer_thread.start()

    def _stop_proc(proc):
        """Send SIGINT, wait up to 5s, then SIGKILL if still alive."""
        try:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    try:
        # Wait for either process to exit (shouldn't happen before Ctrl+C)
        while mic_proc.poll() is None and monitor_proc.poll() is None:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        _stop_proc(mic_proc)
        _stop_proc(monitor_proc)
        stop_event.set()
        timer_thread.join(timeout=2)

    print("Recording stopped.")


def mix_audio(mic_path, monitor_path, output_path):
    """Mix mic and monitor WAV files into a single file using ffmpeg.

    Falls back to copying the mic recording if ffmpeg fails.
    """
    import shutil

    cmd = [
        "ffmpeg", "-y",
        "-i", str(mic_path),
        "-i", str(monitor_path),
        "-filter_complex", "amix=inputs=2:duration=longest:normalize=0",
        "-ar", "16000",
        "-ac", "1",
        str(output_path),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            print(f"Mixed audio saved: {output_path}")
            return
        print(f"Warning: ffmpeg mix failed (exit {result.returncode}): {result.stderr.strip()}", file=sys.stderr)
    except FileNotFoundError:
        print("Warning: ffmpeg not found, cannot mix audio streams.", file=sys.stderr)
    except subprocess.TimeoutExpired:
        print("Warning: ffmpeg timed out while mixing audio.", file=sys.stderr)

    # Fallback: use mic recording as the combined audio
    print("Falling back to mic-only audio for transcription.", file=sys.stderr)
    shutil.copy2(str(mic_path), str(output_path))


def validate_audio(path, min_duration=1.0, label="Audio"):
    """Validate that the audio file exists and has a minimum duration.

    Tries Python's wave module first. If the WAV header is truncated (common when
    pw-record is interrupted), falls back to ffprobe, then to a raw file-size estimate.

    Raises AudioValidationError on failure.
    """
    path = Path(path)
    if not path.exists() or path.stat().st_size == 0:
        raise AudioValidationError(f"{label} file is missing or empty.")

    # Try standard wave module
    try:
        with wave.open(str(path), "rb") as wf:
            frames = wf.getnframes()
            rate = wf.getframerate()
            if rate > 0 and frames > 0:
                duration = frames / rate
                if duration < min_duration:
                    raise AudioValidationError(f"{label} too short ({duration:.1f}s).")
                print(f"{label} validated: {duration:.1f}s, {rate}Hz")
                return duration
    except wave.Error:
        pass

    # Fallback: try ffprobe
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", str(path)],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode == 0 and result.stdout.strip():
            duration = float(result.stdout.strip())
            if duration < min_duration:
                raise AudioValidationError(f"{label} too short ({duration:.1f}s).")
            print(f"{label} validated (ffprobe): {duration:.1f}s")
            return duration
    except (FileNotFoundError, subprocess.TimeoutExpired, ValueError):
        pass

    # Last resort: estimate from file size (16kHz, 16-bit mono = 32000 bytes/sec)
    # WAV header is 44 bytes
    data_size = path.stat().st_size - 44
    if data_size <= 0:
        raise AudioValidationError(f"{label} file contains no data.")
    duration = data_size / 32000
    if duration < min_duration:
        raise AudioValidationError(f"{label} too short (~{duration:.1f}s).")
    print(f"{label} validated (estimated): ~{duration:.1f}s")
    return duration


def format_timestamp(seconds):
    """Format seconds as MM:SS."""
    mins, secs = divmod(int(seconds), 60)
    return f"{mins:02d}:{secs:02d}"


def transcribe_audio(audio_path, model_name):
    """Transcribe audio using faster-whisper and return timestamped text."""
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        print(
            "Error: faster-whisper not installed. Run: pip install faster-whisper",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Loading Whisper model '{model_name}'...")
    model = WhisperModel(model_name, compute_type="int8")

    print("Transcribing...")
    segments, info = model.transcribe(str(audio_path), beam_size=5)

    lines = []
    for segment in segments:
        start = format_timestamp(segment.start)
        end = format_timestamp(segment.end)
        lines.append(f"[{start} - {end}] {segment.text.strip()}")

    transcript = "\n".join(lines)
    if not transcript.strip():
        print("Error: Transcription produced no text.", file=sys.stderr)
        sys.exit(1)

    print(f"Transcribed {len(lines)} segments (language: {info.language}, prob: {info.language_probability:.2f})")
    return transcript


def summarize_transcript(transcript, api_key):
    """Summarize the transcript using xAI Grok API."""
    headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
    data = {
        "model": "grok-3",
        "messages": [
            {"role": "system", "content": SUMMARY_SYSTEM_PROMPT},
            {"role": "user", "content": SUMMARY_USER_PROMPT.format(transcript=transcript)},
        ],
        "temperature": 0.3,
        "max_tokens": 4096,
    }

    print("Requesting Grok summary...")
    response = requests.post(API_URL, headers=headers, json=data, timeout=120)

    if response.status_code != 200:
        raise RuntimeError(f"Grok API error ({response.status_code}): {response.text}")

    return response.json()["choices"][0]["message"]["content"]


def create_output_dir(base_dir):
    """Create a timestamped output directory, handling collisions."""
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M")
    out_dir = Path(base_dir) / timestamp

    if out_dir.exists():
        # Append a suffix to avoid collision
        for i in range(2, 100):
            candidate = Path(base_dir) / f"{timestamp}_{i}"
            if not candidate.exists():
                out_dir = candidate
                break
        else:
            print("Error: Too many sessions in the same minute.", file=sys.stderr)
            sys.exit(1)

    out_dir.mkdir(parents=True)
    return out_dir


def parse_args():
    parser = argparse.ArgumentParser(
        prog="recmeet",
        description="Record, transcribe, and summarize meetings.",
    )
    parser.add_argument("--source", help="PipeWire/PulseAudio mic source (auto-detected if omitted)")
    parser.add_argument("--monitor", nargs="?", const="auto", default=None,
                        help="Monitor/speaker source (auto-detected if omitted; 'default' for system default output)")
    parser.add_argument("--mic-only", action="store_true", help="Record mic only (skip monitor capture)")
    parser.add_argument("--model", default="base", help="Whisper model: tiny/base/small/medium/large-v3 (default: base)")
    parser.add_argument("--output-dir", default="./meetings", help="Base directory for outputs (default: ./meetings)")
    parser.add_argument("--api-key", help="xAI API key (default: from env/dotenv)")
    parser.add_argument("--no-summary", action="store_true", help="Skip Grok summary (record + transcribe only)")
    parser.add_argument("--device-pattern", default=DEFAULT_DEVICE_PATTERN, help=f"Regex for device auto-detection (default: {DEFAULT_DEVICE_PATTERN})")
    return parser.parse_args()


def _resolve_monitor_source(args, detected=None):
    """Determine the monitor source based on CLI flags and auto-detection.

    If `detected` is provided (from a prior detect_sources call), reuses it
    to avoid a redundant pactl query.

    Returns the monitor source name, or None for mic-only mode.
    """
    if args.mic_only:
        return None

    # Explicit --monitor with a specific source name
    if args.monitor and args.monitor not in ("auto", "default"):
        return args.monitor

    # --monitor default: use the system default output's monitor
    if args.monitor == "default":
        all_sources = detected["all_sources"] if detected else _list_pactl_sources()
        for name in all_sources:
            if name.endswith(".monitor"):
                return name
        print("Warning: No monitor source found. Falling back to mic-only.", file=sys.stderr)
        return None

    # Auto-detect (default behavior, or --monitor with no value)
    if not detected:
        detected = detect_sources(args.device_pattern)
    return detected["monitor"]


def main():
    load_dotenv()
    args = parse_args()

    # Resolve API key
    api_key = args.api_key or os.getenv("XAI_API_KEY")
    if not args.no_summary and not api_key:
        print("Error: No API key. Set XAI_API_KEY in .env, environment, or use --api-key.", file=sys.stderr)
        print("Use --no-summary to skip summarization.", file=sys.stderr)
        sys.exit(1)

    # Detect audio sources
    detected = None
    if args.source:
        mic_source = args.source
    else:
        detected = detect_sources(args.device_pattern)
        mic_source = detected["mic"]
        if not mic_source:
            print(f"Error: No mic source matching pattern '{args.device_pattern}' found.", file=sys.stderr)
            print("\nAvailable sources:", file=sys.stderr)
            for s in detected["all_sources"]:
                print(f"  {s}", file=sys.stderr)
            print(f"\nUse --source <name> to specify one explicitly.", file=sys.stderr)
            sys.exit(1)

    monitor_source = _resolve_monitor_source(args, detected)
    dual_mode = monitor_source is not None

    if dual_mode:
        print(f"Mic source:     {mic_source}")
        print(f"Monitor source: {monitor_source}")
    else:
        print(f"Audio source: {mic_source}")
        if not args.mic_only:
            print("No monitor source found — recording mic only.")

    # Create output directory
    out_dir = create_output_dir(args.output_dir)
    audio_path = out_dir / "audio.wav"
    transcript_path = out_dir / "transcript.txt"
    summary_path = out_dir / "summary.md"
    print(f"Output directory: {out_dir}")

    # Record
    if dual_mode:
        mic_path = out_dir / "mic.wav"
        monitor_path = out_dir / "monitor.wav"
        notify("Recording started", f"Mic: {mic_source}\nMonitor: {monitor_source}")
        record_dual_audio(mic_source, monitor_source, mic_path, monitor_path)

        # Validate mic (fatal)
        try:
            validate_audio(mic_path, label="Mic audio")
        except AudioValidationError as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

        # Validate monitor (non-fatal — monitor may be silent)
        try:
            validate_audio(monitor_path, label="Monitor audio")
        except AudioValidationError as e:
            print(f"Warning: Monitor audio unusable ({e}). Using mic only.", file=sys.stderr)
            import shutil
            shutil.copy2(str(mic_path), str(audio_path))
            dual_mode = False

        if dual_mode:
            mix_audio(mic_path, monitor_path, audio_path)
    else:
        notify("Recording started", f"Source: {mic_source}")
        record_audio(mic_source, audio_path)

        try:
            validate_audio(audio_path)
        except AudioValidationError as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

    # Transcribe
    notify("Transcribing...", f"Model: {args.model}")
    transcript = transcribe_audio(audio_path, args.model)
    transcript_path.write_text(transcript)
    print(f"Transcript saved: {transcript_path}")

    # Summarize
    if not args.no_summary:
        notify("Summarizing...", "Sending to Grok")
        try:
            summary = summarize_transcript(transcript, api_key)
            summary_path.write_text(summary)
            print(f"Summary saved:    {summary_path}")
        except Exception as e:
            print(f"\nWarning: Summary failed — {e}", file=sys.stderr)
            print("Transcript is still available.", file=sys.stderr)
    else:
        print("Summary skipped (--no-summary).")

    # Done
    notify("Meeting complete", str(out_dir))
    print(f"\nDone! Files in: {out_dir}")


if __name__ == "__main__":
    main()
