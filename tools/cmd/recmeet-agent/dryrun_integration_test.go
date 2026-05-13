// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// TestAgentCLI_PrepDryRun_Minimal verifies `prep "Q2 Plan" --dry-run`
// completes with exit 0 and prints the topic plus a DRY RUN banner.
// No API key required — dry-run never calls the API.
func TestAgentCLI_PrepDryRun_Minimal(t *testing.T) {
	res := runAgent(t, nil, "prep", "Q2 Plan", "--dry-run")
	if res.ExitCode != 0 {
		t.Fatalf("prep --dry-run exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stdout, "DRY RUN") {
		t.Errorf("stdout missing 'DRY RUN'\nstdout=%s", res.Stdout)
	}
	if !strings.Contains(res.Stdout, "Q2 Plan") {
		t.Errorf("stdout missing topic 'Q2 Plan'\nstdout=%s", res.Stdout)
	}
}

// TestAgentCLI_PrepDryRun_FullFlags exercises all prep flags in
// dry-run mode and asserts each flag's value flows through to the
// assembled prompt. Note: the model name is *not* part of the dry-run
// output (it's a request parameter, not prompt content); we exercise
// --model here to confirm it parses cleanly but assert on values that
// the dry-run printer actually emits.
func TestAgentCLI_PrepDryRun_FullFlags(t *testing.T) {
	res := runAgent(t, nil,
		"prep", "Q2 Plan",
		"--participants", "Alice Smith,Bob Jones",
		"--agenda-url", "https://example.com/q2-agenda",
		"--model", "claude-haiku-4-5",
		"--dry-run",
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep --dry-run full flags exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	for _, want := range []string{
		"DRY RUN",
		"Q2 Plan",
		"Alice Smith",
		"Bob Jones",
		"https://example.com/q2-agenda",
	} {
		if !strings.Contains(res.Stdout, want) {
			t.Errorf("stdout missing %q\nstdout=%s", want, res.Stdout)
		}
	}
}

// TestAgentCLI_FollowUpDryRun verifies that `follow-up <note> --dry-run`
// completes successfully and the assembled prompt references
// participants extracted from the meeting note fixture.
func TestAgentCLI_FollowUpDryRun(t *testing.T) {
	notePath := testutil.CopyNoteFixture(t, "meeting_full")
	res := runAgent(t, nil, "follow-up", notePath, "--dry-run")
	if res.ExitCode != 0 {
		t.Fatalf("follow-up --dry-run exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stdout, "DRY RUN") {
		t.Errorf("stdout missing 'DRY RUN'\nstdout=%s", res.Stdout)
	}
	// meeting_full.md frontmatter participants include Alice_Smith,
	// Bob_Jones, Charlie_Brown — at least one should appear in the
	// dry-run prompt (the follow-up user-message includes the
	// participants list).
	foundParticipant := false
	for _, p := range []string{"Alice_Smith", "Bob_Jones", "Charlie_Brown"} {
		if strings.Contains(res.Stdout, p) {
			foundParticipant = true
			break
		}
	}
	if !foundParticipant {
		t.Errorf("stdout missing any expected participant\nstdout=%s", res.Stdout)
	}
}
