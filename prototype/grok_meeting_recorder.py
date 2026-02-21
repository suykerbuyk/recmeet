import argparse
import os
import signal
import subprocess
import sys
import time

import requests
import whisper

# Default configurations
DEFAULT_SOURCE = (
    "alsa_input.usb-BD_H200-00.mono-fallback"  # Replace with your actual source name
)
DEFAULT_MODEL = (
    "base"  # Whisper model: tiny/base/small/medium/large (trade speed vs. accuracy)
)
DEFAULT_OUTPUT_FILE = "meeting_audio.wav"
DEFAULT_TRANSCRIPT_FILE = "transcript.txt"
DEFAULT_SUMMARY_FILE = "meeting_summary.md"
API_URL = (
    "https://api.x.ai/v1/chat/completions"  # xAI Grok API endpoint (OpenAI-compatible)
)


def record_audio(source_name, output_file):
    """Start recording from PulseAudio source."""
    print(f"Starting recording from {source_name}. Press Ctrl+C to stop.")
    proc = subprocess.Popen(
        ["parecord", "--device", source_name, "--file-format=wav", output_file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc


def stop_recording(proc):
    """Gracefully stop the recording process."""
    proc.send_signal(signal.SIGINT)
    proc.wait()
    print("Recording stopped.")


def transcribe_audio(audio_file, model_name):
    """Transcribe audio using Whisper."""
    print("Loading Whisper model...")
    model = whisper.load_model(model_name)
    print("Transcribing audio...")
    result = model.transcribe(audio_file)
    return result["text"]


def summarize_transcript(transcript, api_key):
    """Summarize transcript using xAI Grok API."""
    prompt = f"Summarize this meeting transcript in Markdown format. Include key points, action items, decisions, and participants if identifiable. Be thorough, exploring implications and nuances:\n\n{transcript}"

    headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
    data = {
        "model": "grok-4",  # Or "grok-4.1-fast" for speed
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.7,  # Adjust for creativity (0-1)
        "max_tokens": 1024,  # Limit output length
    }

    response = requests.post(API_URL, headers=headers, json=data)
    if response.status_code == 200:
        return response.json()["choices"][0]["message"]["content"]
    else:
        raise Exception(f"API error: {response.text}")


def main():
    parser = argparse.ArgumentParser(description="Meeting Recorder and Summarizer")
    parser.add_argument(
        "--source", default=DEFAULT_SOURCE, help="PulseAudio source name"
    )
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Whisper model size")
    parser.add_argument(
        "--output", default=DEFAULT_OUTPUT_FILE, help="Audio output file"
    )
    parser.add_argument(
        "--api-key", default=os.getenv("XAI_API_KEY"), help="xAI API key"
    )
    args = parser.parse_args()

    if not args.api_key:
        sys.exit("Error: Set XAI_API_KEY environment variable or use --api-key.")

    # Record
    proc = record_audio(args.source, args.output)
    try:
        while proc.poll() is None:
            time.sleep(0.1)
    except KeyboardInterrupt:
        stop_recording(proc)

    # Transcribe
    transcript = transcribe_audio(args.output, args.model)
    with open(DEFAULT_TRANSCRIPT_FILE, "w") as f:
        f.write(transcript)
    print(f"Transcript saved to {DEFAULT_TRANSCRIPT_FILE}")

    # Summarize
    try:
        summary = summarize_transcript(transcript, args.api_key)
        with open(DEFAULT_SUMMARY_FILE, "w") as f:
            f.write(summary)
        print(f"Summary saved to {DEFAULT_SUMMARY_FILE}")
    except Exception as e:
        print(f"Summary failed: {e}")


if __name__ == "__main__":
    main()
