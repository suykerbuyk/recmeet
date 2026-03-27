// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadAgentConfig_Defaults(t *testing.T) {
	// Load with nonexistent path to get defaults
	cfg, err := LoadAgentConfig("/nonexistent/config.yaml")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.Model != "claude-sonnet-4-6" {
		t.Errorf("expected model claude-sonnet-4-6, got %s", cfg.Model)
	}
	if cfg.MaxIterations != 20 {
		t.Errorf("expected max_iterations 20, got %d", cfg.MaxIterations)
	}
}

func TestLoadAgentConfig_FromConfigFile(t *testing.T) {
	t.Setenv("ANTHROPIC_API_KEY", "")
	os.Unsetenv("ANTHROPIC_API_KEY")

	configPath := filepath.Join("..", "meetingdata", "testdata", "config.yaml")
	cfg, err := LoadAgentConfig(configPath)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.AnthropicKey != "sk-ant-test123" {
		t.Errorf("expected anthropic key from config, got %q", cfg.AnthropicKey)
	}
	if cfg.OutputDir != "/home/test/meetings" {
		t.Errorf("expected output dir /home/test/meetings, got %s", cfg.OutputDir)
	}
}

func TestLoadAgentConfig_EnvOverridesConfig(t *testing.T) {
	t.Setenv("ANTHROPIC_API_KEY", "env-key-123")

	configPath := filepath.Join("..", "meetingdata", "testdata", "config.yaml")
	cfg, err := LoadAgentConfig(configPath)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.AnthropicKey != "env-key-123" {
		t.Errorf("expected env key to override config, got %q", cfg.AnthropicKey)
	}
}

func TestLoadAgentConfig_BraveAPIKey(t *testing.T) {
	t.Setenv("BRAVE_API_KEY", "brave-key-456")

	cfg, err := LoadAgentConfig("/nonexistent/config.yaml")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.BraveAPIKey != "brave-key-456" {
		t.Errorf("expected brave key from env, got %q", cfg.BraveAPIKey)
	}
}

func TestLoadAgentConfig_NoBraveKeyWithoutEnv(t *testing.T) {
	os.Unsetenv("BRAVE_API_KEY")

	cfg, err := LoadAgentConfig("/nonexistent/config.yaml")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.BraveAPIKey != "" {
		t.Errorf("expected empty brave key, got %q", cfg.BraveAPIKey)
	}
}
