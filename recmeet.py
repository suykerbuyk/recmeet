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


def detect_device(pattern):
    """Auto-detect a PipeWire/PulseAudio source matching the given regex pattern."""
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

    regex = re.compile(pattern, re.IGNORECASE)
    sources = []
    for line in result.stdout.strip().splitlines():
        fields = line.split("\t")
        if len(fields) >= 2:
            name = fields[1]
            sources.append(name)
            if regex.search(name):
                return name

    print(f"Error: No source matching pattern '{pattern}' found.", file=sys.stderr)
    print("\nAvailable sources:", file=sys.stderr)
    for s in sources:
        print(f"  {s}", file=sys.stderr)
    print(f"\nUse --source <name> to specify one explicitly.", file=sys.stderr)
    sys.exit(1)


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


def record_audio(source, output_path):
    """Record audio from the given PipeWire/PulseAudio source until Ctrl+C."""
    recorder = find_recorder()
    output_str = str(output_path)

    if recorder == "pw-record":
        cmd = [
            "pw-record",
            "--target", source,
            "--format", "s16",
            "--rate", "16000",
            "--channels", "1",
            output_str,
        ]
    else:
        cmd = [
            "parecord",
            "--device", source,
            "--file-format=wav",
            "--format=s16le",
            "--rate=16000",
            "--channels=1",
            output_str,
        ]

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


def validate_audio(path, min_duration=1.0):
    """Validate that the audio file exists and has a minimum duration.

    Tries Python's wave module first. If the WAV header is truncated (common when
    pw-record is interrupted), falls back to ffprobe, then to a raw file-size estimate.
    """
    path = Path(path)
    if not path.exists() or path.stat().st_size == 0:
        print("Error: Audio file is missing or empty.", file=sys.stderr)
        sys.exit(1)

    # Try standard wave module
    try:
        with wave.open(str(path), "rb") as wf:
            frames = wf.getnframes()
            rate = wf.getframerate()
            if rate > 0 and frames > 0:
                duration = frames / rate
                if duration < min_duration:
                    print(f"Error: Recording too short ({duration:.1f}s).", file=sys.stderr)
                    sys.exit(1)
                print(f"Audio validated: {duration:.1f}s, {rate}Hz")
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
                print(f"Error: Recording too short ({duration:.1f}s).", file=sys.stderr)
                sys.exit(1)
            print(f"Audio validated (ffprobe): {duration:.1f}s")
            return duration
    except (FileNotFoundError, subprocess.TimeoutExpired, ValueError):
        pass

    # Last resort: estimate from file size (16kHz, 16-bit mono = 32000 bytes/sec)
    # WAV header is 44 bytes
    data_size = path.stat().st_size - 44
    if data_size <= 0:
        print("Error: Audio file contains no data.", file=sys.stderr)
        sys.exit(1)
    duration = data_size / 32000
    if duration < min_duration:
        print(f"Error: Recording too short (~{duration:.1f}s).", file=sys.stderr)
        sys.exit(1)
    print(f"Audio validated (estimated): ~{duration:.1f}s")
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
    parser.add_argument("--source", help="PipeWire/PulseAudio source (auto-detected if omitted)")
    parser.add_argument("--model", default="base", help="Whisper model: tiny/base/small/medium/large-v3 (default: base)")
    parser.add_argument("--output-dir", default="./meetings", help="Base directory for outputs (default: ./meetings)")
    parser.add_argument("--api-key", help="xAI API key (default: from env/dotenv)")
    parser.add_argument("--no-summary", action="store_true", help="Skip Grok summary (record + transcribe only)")
    parser.add_argument("--device-pattern", default=DEFAULT_DEVICE_PATTERN, help=f"Regex for device auto-detection (default: {DEFAULT_DEVICE_PATTERN})")
    return parser.parse_args()


def main():
    load_dotenv()
    args = parse_args()

    # Resolve API key
    api_key = args.api_key or os.getenv("XAI_API_KEY")
    if not args.no_summary and not api_key:
        print("Error: No API key. Set XAI_API_KEY in .env, environment, or use --api-key.", file=sys.stderr)
        print("Use --no-summary to skip summarization.", file=sys.stderr)
        sys.exit(1)

    # Detect audio source
    source = args.source or detect_device(args.device_pattern)
    print(f"Audio source: {source}")

    # Create output directory
    out_dir = create_output_dir(args.output_dir)
    audio_path = out_dir / "audio.wav"
    transcript_path = out_dir / "transcript.txt"
    summary_path = out_dir / "summary.md"
    print(f"Output directory: {out_dir}")

    # Record
    notify("Recording started", f"Source: {source}")
    record_audio(source, audio_path)

    # Validate
    validate_audio(audio_path)

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
