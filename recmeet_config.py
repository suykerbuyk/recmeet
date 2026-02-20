"""recmeet_config — YAML configuration management for recmeet."""

import os
from pathlib import Path

import yaml

CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "recmeet"
CONFIG_PATH = CONFIG_DIR / "config.yaml"

DEFAULTS = {
    "model": "base",
    "no_summary": False,
    "mic_only": False,
    "output_dir": "./meetings",
    "device_pattern": r"bd.h200|00:05:30:00:05:4E",
    "mic_source": "",
    "monitor_source": "",
    # api_key deliberately omitted from defaults — loaded from .env or env var
}


def load_config():
    """Load config from YAML, merged with defaults. Missing keys get defaults."""
    config = dict(DEFAULTS)
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH) as f:
                on_disk = yaml.safe_load(f)
            if isinstance(on_disk, dict):
                for key, value in on_disk.items():
                    if value is not None:
                        config[key] = value
        except (yaml.YAMLError, OSError) as e:
            print(f"Warning: Could not read {CONFIG_PATH}: {e}")
    return config


def save_config(config):
    """Write config dict to YAML, preserving only non-default and explicitly set values."""
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)

    # Build a clean dict: include everything the caller passes, skip empty strings
    # that match defaults (mic_source/monitor_source empty = auto-detect)
    clean = {}
    for key, value in config.items():
        if key.startswith("_"):
            continue
        clean[key] = value

    with open(CONFIG_PATH, "w") as f:
        f.write("# recmeet configuration\n")
        f.write("# Edit this file or use the tray menu to change settings.\n\n")
        yaml.dump(clean, f, default_flow_style=False, sort_keys=False)


def generate_initial_config(all_sources=None):
    """Generate an initial config file with detected devices as comments.

    Does nothing if the config file already exists.
    Returns the config dict.
    """
    if CONFIG_PATH.exists():
        return load_config()

    CONFIG_DIR.mkdir(parents=True, exist_ok=True)

    mic_sources = []
    monitor_sources = []
    if all_sources:
        for name in all_sources:
            if name.endswith(".monitor"):
                monitor_sources.append(name)
            else:
                mic_sources.append(name)

    lines = [
        "# recmeet configuration",
        "# Edit this file or use the tray menu to change settings.",
        "",
        f"model: {DEFAULTS['model']}",
        f"no_summary: {str(DEFAULTS['no_summary']).lower()}",
        f"mic_only: {str(DEFAULTS['mic_only']).lower()}",
        "# api_key: xai-your-key-here",
        f"output_dir: {DEFAULTS['output_dir']}",
        f'device_pattern: "{DEFAULTS["device_pattern"]}"',
        "",
        "# Mic source (leave empty for auto-detection)",
    ]

    if mic_sources:
        lines.append("# Detected mic sources:")
        for s in mic_sources:
            lines.append(f"#   - {s}")

    lines += [
        'mic_source: ""',
        "",
        "# Monitor source (leave empty for auto-detection)",
    ]

    if monitor_sources:
        lines.append("# Detected monitor sources:")
        for s in monitor_sources:
            lines.append(f"#   - {s}")

    lines += [
        'monitor_source: ""',
        "",
    ]

    with open(CONFIG_PATH, "w") as f:
        f.write("\n".join(lines))

    return load_config()


def apply_cli_overrides(config, args):
    """Apply CLI argument overrides onto a config dict. CLI args take precedence.

    Args is expected to be an argparse.Namespace with the standard recmeet flags.
    Returns the modified config dict.
    """
    if getattr(args, "source", None):
        config["mic_source"] = args.source
    if getattr(args, "monitor", None) and args.monitor not in ("auto",):
        config["monitor_source"] = args.monitor
    if getattr(args, "model", None):
        config["model"] = args.model
    if getattr(args, "output_dir", None):
        config["output_dir"] = args.output_dir
    if getattr(args, "api_key", None):
        config["api_key"] = args.api_key
    if getattr(args, "no_summary", False):
        config["no_summary"] = True
    if getattr(args, "mic_only", False):
        config["mic_only"] = True
    if getattr(args, "device_pattern", None):
        config["device_pattern"] = args.device_pattern
    return config
