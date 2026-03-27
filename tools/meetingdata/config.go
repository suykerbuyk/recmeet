// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	OutputDir  string
	NoteDir    string
	SpeakerDB  string
	WebPort    int
	WebBind    string
	APIKeys    map[string]string
	Provider   string
	APIModel   string
	Domain     string
}

func DefaultConfig() Config {
	return Config{
		OutputDir: "./meetings",
		WebPort:   8384,
		WebBind:   "127.0.0.1",
		Provider:  "xai",
		APIModel:  "grok-3",
		Domain:    "general",
		APIKeys:   make(map[string]string),
	}
}

func DefaultConfigPath() string {
	if dir := os.Getenv("XDG_CONFIG_HOME"); dir != "" {
		return filepath.Join(dir, "recmeet", "config.yaml")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".config", "recmeet", "config.yaml")
}

func DefaultSpeakerDB() string {
	if dir := os.Getenv("XDG_DATA_HOME"); dir != "" {
		return filepath.Join(dir, "recmeet", "speakers")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "recmeet", "speakers")
}

func LoadConfig(path string) (Config, error) {
	if path == "" {
		path = DefaultConfigPath()
	}

	cfg := DefaultConfig()

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return cfg, err
	}

	entries := parseYAML(string(data))

	if v := getVal(entries, "output", "directory"); v != "" {
		cfg.OutputDir = v
	}
	if v := getVal(entries, "notes", "directory"); v != "" {
		cfg.NoteDir = v
	}
	if v := getVal(entries, "notes", "domain"); v != "" {
		cfg.Domain = v
	}
	if v := getVal(entries, "speaker_id", "database"); v != "" {
		cfg.SpeakerDB = v
	}
	if v := getVal(entries, "web", "port"); v != "" {
		if port, err := strconv.Atoi(v); err == nil {
			cfg.WebPort = port
		}
	}
	if v := getVal(entries, "web", "bind"); v != "" {
		cfg.WebBind = v
	}
	if v := getVal(entries, "summary", "provider"); v != "" {
		cfg.Provider = v
	}
	if v := getVal(entries, "summary", "model"); v != "" {
		cfg.APIModel = v
	}

	for _, provider := range []string{"xai", "openai", "anthropic"} {
		if v := getVal(entries, "api_keys", provider); v != "" {
			cfg.APIKeys[provider] = v
		}
	}

	if cfg.SpeakerDB == "" {
		cfg.SpeakerDB = DefaultSpeakerDB()
	}

	return cfg, nil
}

type yamlEntry struct {
	key    string
	value  string
	indent int
}

func parseYAML(text string) []yamlEntry {
	var entries []yamlEntry
	for _, line := range strings.Split(text, "\n") {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || trimmed[0] == '#' {
			continue
		}

		indent := 0
		for indent < len(line) && line[indent] == ' ' {
			indent++
		}

		colon := strings.Index(trimmed, ":")
		if colon < 0 {
			continue
		}

		key := strings.TrimSpace(trimmed[:colon])
		val := strings.TrimSpace(trimmed[colon+1:])
		val = unquote(val)
		entries = append(entries, yamlEntry{key: key, value: val, indent: indent})
	}
	return entries
}

func unquote(s string) string {
	if len(s) >= 2 && ((s[0] == '"' && s[len(s)-1] == '"') ||
		(s[0] == '\'' && s[len(s)-1] == '\'')) {
		return s[1 : len(s)-1]
	}
	return s
}

func getVal(entries []yamlEntry, section, key string) string {
	inSection := section == ""
	for _, e := range entries {
		if section != "" {
			if e.indent == 0 && e.key == section && e.value == "" {
				inSection = true
			} else if e.indent == 0 && e.key != section {
				inSection = false
			}
		}
		if inSection && e.key == key {
			return e.value
		}
	}
	return ""
}
