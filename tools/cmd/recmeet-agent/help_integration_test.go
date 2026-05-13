// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"strings"
	"testing"
)

// TestAgentCLI_Help_Root verifies `recmeet-agent --help` lists both
// subcommands and exits 0.
func TestAgentCLI_Help_Root(t *testing.T) {
	res := runAgent(t, nil, "--help")
	if res.ExitCode != 0 {
		t.Fatalf("--help exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	for _, want := range []string{"prep", "follow-up"} {
		if !strings.Contains(res.Stdout, want) {
			t.Errorf("--help stdout missing %q\nstdout=%s", want, res.Stdout)
		}
	}
}

// TestAgentCLI_Help_Prep verifies `recmeet-agent prep --help` lists
// every prep-subcommand flag from the Phase B plan table.
func TestAgentCLI_Help_Prep(t *testing.T) {
	res := runAgent(t, nil, "prep", "--help")
	if res.ExitCode != 0 {
		t.Fatalf("prep --help exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	wantFlags := []string{
		"--participants",
		"--agenda-url",
		"--output",
		"--model",
		"--verbose",
		"--dry-run",
		"--config",
		"--max-iterations",
	}
	for _, f := range wantFlags {
		if !strings.Contains(res.Stdout, f) {
			t.Errorf("prep --help stdout missing %q\nstdout=%s", f, res.Stdout)
		}
	}
}

// TestAgentCLI_Help_FollowUp verifies `recmeet-agent follow-up --help`
// lists every follow-up-subcommand flag. Note: follow-up uses
// `--output-dir` (directory), distinct from prep's `--output` (file).
func TestAgentCLI_Help_FollowUp(t *testing.T) {
	res := runAgent(t, nil, "follow-up", "--help")
	if res.ExitCode != 0 {
		t.Fatalf("follow-up --help exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	wantFlags := []string{
		"--output-dir",
		"--my-name",
		"--model",
		"--verbose",
		"--dry-run",
		"--config",
		"--max-iterations",
	}
	for _, f := range wantFlags {
		if !strings.Contains(res.Stdout, f) {
			t.Errorf("follow-up --help stdout missing %q\nstdout=%s", f, res.Stdout)
		}
	}
}
