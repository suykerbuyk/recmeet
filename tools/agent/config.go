// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"os"

	"github.com/syketech/recmeet-tools/meetingdata"
)

// AgentConfig extends the base meetingdata config with agent-specific settings.
type AgentConfig struct {
	meetingdata.Config
	Model         string // LLM model to use (default: "claude-sonnet-4-6")
	MaxIterations int    // max agentic loop iterations (default: 20)
	ContextDir    string // staging dir for context files
	BraveAPIKey   string // Brave Search API key
	AnthropicKey  string // Anthropic API key
}

// LoadAgentConfig loads base config from the given path and overlays
// agent-specific settings from environment variables.
func LoadAgentConfig(configPath string) (AgentConfig, error) {
	base, err := meetingdata.LoadConfig(configPath)
	if err != nil {
		return AgentConfig{}, err
	}

	ac := AgentConfig{
		Config:        base,
		Model:         "claude-sonnet-4-6",
		MaxIterations: 20,
	}

	// Anthropic key: env var takes precedence, then config file
	if key := os.Getenv("ANTHROPIC_API_KEY"); key != "" {
		ac.AnthropicKey = key
	} else if key, ok := base.APIKeys["anthropic"]; ok {
		ac.AnthropicKey = key
	}

	if key := os.Getenv("BRAVE_API_KEY"); key != "" {
		ac.BraveAPIKey = key
	}

	return ac, nil
}
