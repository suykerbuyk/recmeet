// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// TestAgentCLI_FollowUpMockAPI_StdoutDraft verifies the happy path
// without write_file: mock returns a text-only end_turn response;
// the CLI prints the text to stdout (FollowUpWorkflow returns the
// final text, main.go fmt.Println's it).
func TestAgentCLI_FollowUpMockAPI_StdoutDraft(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	const draft = "Hi Alice, here is the follow-up summary."
	mock.QueueResponse(testutil.BuildEndTurnResponse(draft))

	notePath := testutil.CopyNoteFixture(t, "meeting_full")
	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"follow-up", notePath,
		"--my-name", "Tester",
	)
	if res.ExitCode != 0 {
		t.Fatalf("follow-up exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stdout, draft) {
		t.Errorf("stdout missing draft text\nstdout=%s", res.Stdout)
	}
}

// TestAgentCLI_FollowUpMockAPI_WriteFile drives the agent through a
// write_file tool-use turn: mock scripts a write_file call targeting
// <output-dir>/draft.md, the agent dispatches the tool, the file is
// written, and the second mock response is an end_turn summary
// printed to stdout.
func TestAgentCLI_FollowUpMockAPI_WriteFile(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	outDir := t.TempDir()
	draftPath := filepath.Join(outDir, "draft.md")
	const draftBody = "Hi team,\n\nThanks for the Q1 sync.\n\n— Tester"
	mock.QueueResponse(testutil.BuildToolUseResponse(
		"write_file",
		map[string]any{"path": draftPath, "content": draftBody},
		"Drafting follow-up...",
	))
	mock.QueueResponse(testutil.BuildEndTurnResponse("Drafted email to Alice and Bob."))

	notePath := testutil.CopyNoteFixture(t, "meeting_full")
	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"follow-up", notePath,
		"--output-dir", outDir,
		"--my-name", "Tester",
	)
	if res.ExitCode != 0 {
		t.Fatalf("follow-up write_file exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	if !strings.Contains(res.Stdout, "Drafted email") {
		t.Errorf("stdout missing end-turn summary\nstdout=%s", res.Stdout)
	}
	b, err := os.ReadFile(draftPath)
	if err != nil {
		t.Fatalf("read draft file %s: %v", draftPath, err)
	}
	if !strings.Contains(string(b), draftBody) {
		t.Errorf("draft file body mismatch\nwant substring=%q\ngot=%s", draftBody, string(b))
	}
}
