// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadConfig(t *testing.T) {
	cfg, err := LoadConfig("testdata/config.yaml")
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}

	if cfg.OutputDir != "/home/test/meetings" {
		t.Errorf("OutputDir = %q, want /home/test/meetings", cfg.OutputDir)
	}
	if cfg.NoteDir != "/home/test/notes" {
		t.Errorf("NoteDir = %q, want /home/test/notes", cfg.NoteDir)
	}
	if cfg.SpeakerDB != "/tmp/speakers" {
		t.Errorf("SpeakerDB = %q, want /tmp/speakers", cfg.SpeakerDB)
	}
	if cfg.WebPort != 9090 {
		t.Errorf("WebPort = %d, want 9090", cfg.WebPort)
	}
	if cfg.WebBind != "0.0.0.0" {
		t.Errorf("WebBind = %q, want 0.0.0.0", cfg.WebBind)
	}
	if cfg.Provider != "anthropic" {
		t.Errorf("Provider = %q, want anthropic", cfg.Provider)
	}
	if cfg.APIModel != "claude-sonnet-4-6" {
		t.Errorf("APIModel = %q, want claude-sonnet-4-6", cfg.APIModel)
	}
	if cfg.Domain != "engineering" {
		t.Errorf("Domain = %q, want engineering", cfg.Domain)
	}
	if cfg.APIKeys["anthropic"] != "sk-ant-test123" {
		t.Errorf("APIKeys[anthropic] = %q, want sk-ant-test123", cfg.APIKeys["anthropic"])
	}
	if cfg.APIKeys["openai"] != "sk-test456" {
		t.Errorf("APIKeys[openai] = %q, want sk-test456", cfg.APIKeys["openai"])
	}
}

func TestLoadConfig_Missing(t *testing.T) {
	cfg, err := LoadConfig("/nonexistent/config.yaml")
	if err != nil {
		t.Fatalf("expected no error for missing config, got: %v", err)
	}
	if cfg.OutputDir != "./meetings" {
		t.Errorf("expected default OutputDir, got %q", cfg.OutputDir)
	}
	if cfg.WebPort != 8384 {
		t.Errorf("expected default WebPort 8384, got %d", cfg.WebPort)
	}
}

func TestLoadConfig_Empty(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "config.yaml")
	os.WriteFile(path, []byte("# empty config\n"), 0644)

	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if cfg.OutputDir != "./meetings" {
		t.Errorf("expected default OutputDir, got %q", cfg.OutputDir)
	}
}

func TestParseYAML(t *testing.T) {
	input := `
section:
  key1: value1
  key2: "quoted value"
  key3: 'single quoted'
# comment
other:
  num: 42
`
	entries := parseYAML(input)

	if v := getVal(entries, "section", "key1"); v != "value1" {
		t.Errorf("key1 = %q, want value1", v)
	}
	if v := getVal(entries, "section", "key2"); v != "quoted value" {
		t.Errorf("key2 = %q, want 'quoted value'", v)
	}
	if v := getVal(entries, "section", "key3"); v != "single quoted" {
		t.Errorf("key3 = %q, want 'single quoted'", v)
	}
	if v := getVal(entries, "other", "num"); v != "42" {
		t.Errorf("num = %q, want 42", v)
	}
}

func TestDefaultConfig(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.OutputDir != "./meetings" {
		t.Errorf("default OutputDir = %q", cfg.OutputDir)
	}
	if cfg.WebPort != 8384 {
		t.Errorf("default WebPort = %d", cfg.WebPort)
	}
	if cfg.Provider != "xai" {
		t.Errorf("default Provider = %q", cfg.Provider)
	}
}
