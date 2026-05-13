// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"path/filepath"
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// TestAgentCLI_Verbose verifies --verbose causes additional stderr
// output that identifies tool calls. The Loop emits
// `[agent] tool_use: <name>` via log.Printf when verbose is true.
func TestAgentCLI_Verbose(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	mock.QueueResponse(testutil.BuildToolUseResponse(
		"search_meetings",
		map[string]any{"query": "x"},
		"",
	))
	mock.QueueResponse(testutil.BuildEndTurnResponse("done."))

	outFile := filepath.Join(t.TempDir(), "brief.md")
	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Verbose",
		"--output", outFile,
		"--verbose",
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep --verbose exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stderr, "tool_use") &&
		!strings.Contains(res.Stderr, "search_meetings") {
		t.Errorf("--verbose stderr does not mention a tool call\nstderr=%s", res.Stderr)
	}
}

// TestAgentCLI_ConfigPath verifies that --config <path> is honored:
// the binary loads the config file without error and the dry-run
// proceeds. We use dry-run to keep the test deterministic without
// engaging mock-Anthropic plumbing; the goal is to prove --config is
// parsed and applied (a malformed/missing config would fail this).
//
// Plan-deviation note: the original plan asserted "model from the
// config shows up in dry-run output", but the dry-run printer emits
// the system prompt + user message — neither of which contains the
// model (model is an API-request parameter, not prompt content).
// Asserting on a value the printer doesn't emit would be a false
// signal; we instead assert the dry-run runs cleanly with the
// config file present.
func TestAgentCLI_ConfigPath(t *testing.T) {
	cfgPath := testutil.WriteTempConfig(t, map[string]any{
		"output": map[string]any{
			"directory": filepath.Join(t.TempDir(), "out"),
		},
		"summary": map[string]any{
			"model": "claude-haiku-4-5",
		},
	})
	res := runAgent(t, nil,
		"prep", "ConfigTest",
		"--config", cfgPath,
		"--dry-run",
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep --config exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stdout, "DRY RUN") {
		t.Errorf("stdout missing 'DRY RUN'\nstdout=%s", res.Stdout)
	}
	if !strings.Contains(res.Stdout, "ConfigTest") {
		t.Errorf("stdout missing topic 'ConfigTest'\nstdout=%s", res.Stdout)
	}
}
