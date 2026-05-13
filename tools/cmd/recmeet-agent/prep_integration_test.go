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

// TestAgentCLI_PrepMockAPI_TextOnly verifies the happy text-only path:
// mock returns one end_turn text block, the agent writes its result
// to the --output file, and exits 0.
func TestAgentCLI_PrepMockAPI_TextOnly(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	mock.QueueResponse(testutil.BuildEndTurnResponse("Meeting brief: prepared for Q2 sync."))

	outDir := t.TempDir()
	outFile := filepath.Join(outDir, "brief.md")

	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Q2 Sync",
		"--output", outFile,
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep exit=%d, want 0\nstdout=%s\nstderr=%s", res.ExitCode, res.Stdout, res.Stderr)
	}

	b, err := os.ReadFile(outFile)
	if err != nil {
		t.Fatalf("read output: %v", err)
	}
	got := string(b)
	if !strings.Contains(got, "Meeting brief: prepared for Q2 sync.") {
		t.Errorf("output file missing mock response text\ngot=%s", got)
	}

	reqs := mock.Requests()
	if len(reqs) != 1 {
		t.Errorf("mock observed %d requests, want 1", len(reqs))
	}
}

// TestAgentCLI_PrepMockAPI_ToolUse verifies the tool-use loop: mock
// turn 1 returns a tool_use block invoking search_meetings; the agent
// dispatches the tool and feeds the result back. Turn 2 returns
// end_turn with a final brief. Assert the file is written and the
// mock observed two requests with the tool_result echoed in turn 2.
func TestAgentCLI_PrepMockAPI_ToolUse(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	mock.QueueResponse(testutil.BuildToolUseResponse(
		"search_meetings",
		map[string]any{"query": "Q2"},
		"Let me search for prior Q2 meetings...",
	))
	mock.QueueResponse(testutil.BuildEndTurnResponse("Based on prior meetings, here is the brief."))

	outDir := t.TempDir()
	outFile := filepath.Join(outDir, "brief.md")

	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Q2 Planning",
		"--output", outFile,
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep tool-use exit=%d, want 0\nstdout=%s\nstderr=%s",
			res.ExitCode, res.Stdout, res.Stderr)
	}

	b, err := os.ReadFile(outFile)
	if err != nil {
		t.Fatalf("read output: %v", err)
	}
	got := string(b)
	if !strings.Contains(got, "Based on prior meetings") {
		t.Errorf("output file missing final mock text\ngot=%s", got)
	}

	reqs := mock.Requests()
	if len(reqs) != 2 {
		t.Fatalf("mock observed %d requests, want 2", len(reqs))
	}
	// Turn 2's last message should carry the tool_result for search_meetings.
	turn2 := reqs[1]
	foundToolResult := false
	for _, m := range turn2.ParsedMessages {
		if strings.Contains(m.Text, "[tool_result:") {
			foundToolResult = true
			break
		}
	}
	if !foundToolResult {
		t.Errorf("turn 2 did not carry a tool_result message\nrequests=%+v", reqs)
	}
}

// TestAgentCLI_PrepMockAPI_MaxIterations verifies that `--max-iterations 3`
// is honored: mock queues 5 tool-use responses (never end_turn). The
// agent must bail at 3 with a nonzero exit and an iteration-limit
// message on stderr.
func TestAgentCLI_PrepMockAPI_MaxIterations(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	for range 5 {
		mock.QueueResponse(testutil.BuildToolUseResponse(
			"search_meetings",
			map[string]any{"query": "x"},
			"",
		))
	}

	outDir := t.TempDir()
	outFile := filepath.Join(outDir, "brief.md")

	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Endless",
		"--output", outFile,
		"--max-iterations", "3",
	)
	if res.ExitCode == 0 {
		t.Fatalf("prep --max-iterations 3 exit=0, want nonzero\nstdout=%s\nstderr=%s",
			res.Stdout, res.Stderr)
	}
	combined := res.Stdout + res.Stderr
	if !strings.Contains(combined, "max iterations") &&
		!strings.Contains(combined, "iteration") {
		t.Errorf("stderr does not mention iteration limit\nstdout=%s\nstderr=%s",
			res.Stdout, res.Stderr)
	}
	reqs := mock.Requests()
	if len(reqs) != 3 {
		t.Errorf("mock observed %d requests, want exactly 3 (the cap)", len(reqs))
	}
}

// TestAgentCLI_PrepMockAPI_OutputFile verifies --output writes to the
// exact path given.
func TestAgentCLI_PrepMockAPI_OutputFile(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	const want = "Final briefing text."
	mock.QueueResponse(testutil.BuildEndTurnResponse(want))

	outFile := filepath.Join(t.TempDir(), "brief.md")
	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Topic",
		"--output", outFile,
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep --output exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	b, err := os.ReadFile(outFile)
	if err != nil {
		t.Fatalf("read output: %v", err)
	}
	if !strings.Contains(string(b), want) {
		t.Errorf("output file content mismatch\nwant substring=%q\ngot=%s", want, string(b))
	}
}

// TestAgentCLI_PrepMockAPI_OutputDirAutoCreate verifies that --output
// with multiple non-existent parent dirs auto-creates them. The
// PrepWorkflow already calls os.MkdirAll on filepath.Dir(outputPath),
// so this asserts that contract holds end-to-end.
func TestAgentCLI_PrepMockAPI_OutputDirAutoCreate(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	mock.QueueResponse(testutil.BuildEndTurnResponse("Auto-created dir content."))

	root := t.TempDir()
	deep := filepath.Join(root, "sub", "dir", "brief.md")

	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "Deep",
		"--output", deep,
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep auto-create-dir exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	b, err := os.ReadFile(deep)
	if err != nil {
		t.Fatalf("read deep output: %v", err)
	}
	if !strings.Contains(string(b), "Auto-created dir content") {
		t.Errorf("output content mismatch\ngot=%s", string(b))
	}
}

// TestAgentCLI_PrepMockAPI_ModelOverride verifies --model is reflected
// in the request body the SDK sends to Anthropic; the mock records
// the parsed model name from each inbound request.
func TestAgentCLI_PrepMockAPI_ModelOverride(t *testing.T) {
	mock := testutil.NewMockAnthropic(t)
	mock.QueueResponse(testutil.BuildEndTurnResponse("ok"))

	outFile := filepath.Join(t.TempDir(), "brief.md")
	res := runAgent(t, map[string]string{
		"ANTHROPIC_BASE_URL": mock.URL(),
		"ANTHROPIC_API_KEY":  "test-key-not-real",
	},
		"prep", "ModelTest",
		"--output", outFile,
		"--model", "claude-haiku-4-5",
	)
	if res.ExitCode != 0 {
		t.Fatalf("prep --model exit=%d, want 0\nstderr=%s", res.ExitCode, res.Stderr)
	}
	reqs := mock.Requests()
	if len(reqs) != 1 {
		t.Fatalf("mock observed %d requests, want 1", len(reqs))
	}
	if reqs[0].ParsedModel != "claude-haiku-4-5" {
		t.Errorf("mock saw model=%q, want claude-haiku-4-5", reqs[0].ParsedModel)
	}
}
