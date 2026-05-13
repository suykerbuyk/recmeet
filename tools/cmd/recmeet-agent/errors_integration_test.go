// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// TestAgentCLI_PrepNoAPIKey verifies the operator-visible error when
// running `prep "topic"` without an API key (and not in dry-run). The
// stderr must name BOTH the env var (ANTHROPIC_API_KEY) AND the
// config-file alternative (api_keys.anthropic) per the
// usability.ExpectMissingAnthropicKey contract.
func TestAgentCLI_PrepNoAPIKey(t *testing.T) {
	res := runAgent(t, nil, "prep", "Topic")
	if res.ExitCode == 0 {
		t.Fatalf("expected nonzero exit, got 0\nstdout=%s\nstderr=%s", res.Stdout, res.Stderr)
	}
	testutil.AssertActionableStderr(t, res.Stderr, testutil.ExpectMissingAnthropicKey)
}

// TestAgentCLI_FollowUpBadPath verifies that pointing follow-up at a
// nonexistent path exits nonzero with stderr naming the missing file
// AND hinting at absolute paths.
func TestAgentCLI_FollowUpBadPath(t *testing.T) {
	missing := "/nonexistent/recmeet-agent-test-path-does-not-exist.md"
	res := runAgent(t, nil, "follow-up", missing)
	if res.ExitCode == 0 {
		t.Fatalf("expected nonzero exit for missing path, got 0\nstdout=%s\nstderr=%s",
			res.Stdout, res.Stderr)
	}
	testutil.AssertActionableStderr(t, res.Stderr, testutil.ExpectBadNotePath, missing)
}

// TestAgentCLI_FollowUpBadNote verifies follow-up surfaces a parse
// error when the input file isn't a meeting note. We point follow-up
// at a directory; meetingdata.ParseNote then fails at os.ReadFile and
// the workflow wraps it as "parse note: ...", satisfying the
// usability.ExpectUnparseableNote contract (path + "parse" substrings).
func TestAgentCLI_FollowUpBadNote(t *testing.T) {
	// Use a directory so os.ReadFile returns "is a directory", which
	// the workflow wraps as a parse failure. A plain markdown file
	// without frontmatter does NOT currently produce a parse error
	// from meetingdata (it returns an empty MeetingNote), so we
	// exercise the path-shape failure mode instead.
	badPath := filepath.Join(t.TempDir(), "not-a-note")
	if err := os.MkdirAll(badPath, 0o755); err != nil {
		t.Fatalf("mkdir bad path: %v", err)
	}
	res := runAgent(t, nil, "follow-up", badPath)
	if res.ExitCode == 0 {
		t.Fatalf("expected nonzero exit for unparseable note, got 0\nstdout=%s\nstderr=%s",
			res.Stdout, res.Stderr)
	}
	testutil.AssertActionableStderr(t, res.Stderr, testutil.ExpectUnparseableNote, badPath)
}
