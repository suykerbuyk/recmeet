// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/meetingdata"
)

func TestPrepWorkflow_DryRun(t *testing.T) {
	cfg := AgentConfig{
		Model:         "test-model",
		MaxIterations: 5,
		ContextDir:    t.TempDir(),
	}

	result, err := PrepWorkflow(
		context.Background(),
		cfg,
		"Q2 Planning Session",
		[]string{"Alice", "Bob"},
		"https://example.com/agenda",
		"",
		false,
		true, // dry-run
	)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if !strings.Contains(result, "DRY RUN") {
		t.Error("expected DRY RUN in output")
	}
	if !strings.Contains(result, "Q2 Planning Session") {
		t.Error("expected meeting description in output")
	}
	if !strings.Contains(result, "Alice") {
		t.Error("expected participant Alice in output")
	}
	if !strings.Contains(result, "https://example.com/agenda") {
		t.Error("expected agenda URL in output")
	}
}

func TestPrepWorkflow_NoAPIKey(t *testing.T) {
	cfg := AgentConfig{
		Model:         "test-model",
		MaxIterations: 5,
		AnthropicKey:  "",
	}

	_, err := PrepWorkflow(
		context.Background(),
		cfg,
		"Test meeting",
		nil,
		"",
		"",
		false,
		false, // not dry-run
	)
	if err == nil {
		t.Error("expected error for missing API key")
	}
	if !strings.Contains(err.Error(), "ANTHROPIC_API_KEY") {
		t.Errorf("expected API key error, got: %v", err)
	}
}

func TestFollowUpWorkflow_DryRun(t *testing.T) {
	notePath := setupFixtureNote(t)

	cfg := AgentConfig{
		Model:         "test-model",
		MaxIterations: 5,
	}

	result, err := FollowUpWorkflow(
		context.Background(),
		cfg,
		notePath,
		t.TempDir(),
		"John",
		false,
		true, // dry-run
	)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if !strings.Contains(result, "DRY RUN") {
		t.Error("expected DRY RUN in output")
	}
	if !strings.Contains(result, "Q1 Planning Session") {
		t.Error("expected meeting title in output")
	}
	if !strings.Contains(result, "Alice") {
		t.Error("expected action item assignee in output")
	}
	if !strings.Contains(result, "John") {
		t.Error("expected my-name in output")
	}
}

func TestFollowUpWorkflow_BadNotePath(t *testing.T) {
	cfg := AgentConfig{
		Model:         "test-model",
		MaxIterations: 5,
	}

	_, err := FollowUpWorkflow(
		context.Background(),
		cfg,
		"/nonexistent/note.md",
		"",
		"",
		false,
		true,
	)
	if err == nil {
		t.Error("expected error for bad note path")
	}
}

func TestBuildPrepSystemPrompt(t *testing.T) {
	prompt := buildPrepSystemPrompt("Test meeting", []string{"Alice", "Bob"}, "https://example.com")

	if !strings.Contains(prompt, "meeting preparation") {
		t.Error("expected 'meeting preparation' in system prompt")
	}
	if !strings.Contains(prompt, "Alice, Bob") {
		t.Error("expected participants in system prompt")
	}
	if !strings.Contains(prompt, "https://example.com") {
		t.Error("expected agenda URL in system prompt")
	}
}

func TestBuildFollowUpSystemPrompt(t *testing.T) {
	notePath := setupFixtureNote(t)
	note, err := meetingdata.ParseNote(notePath)
	if err != nil {
		t.Fatalf("parse note: %v", err)
	}
	items := meetingdata.ExtractActionItems(note)

	prompt := buildFollowUpSystemPrompt(note, items, "John")
	if !strings.Contains(prompt, "follow-up") {
		t.Error("expected 'follow-up' in system prompt")
	}
	if !strings.Contains(prompt, "John") {
		t.Error("expected my-name in system prompt")
	}
}

func TestBuildFollowUpUserMessage(t *testing.T) {
	notePath := setupFixtureNote(t)
	note, err := meetingdata.ParseNote(notePath)
	if err != nil {
		t.Fatalf("parse note: %v", err)
	}
	items := meetingdata.ExtractActionItems(note)

	msg := buildFollowUpUserMessage(note, items)
	if !strings.Contains(msg, "Q1 Planning Session") {
		t.Error("expected meeting title in user message")
	}
	if !strings.Contains(msg, "Alice") {
		t.Error("expected Alice in action items")
	}
	if !strings.Contains(msg, "Bob") {
		t.Error("expected Bob in action items")
	}
}

func TestBuildPrepRegistry(t *testing.T) {
	cfg := AgentConfig{}
	cfg.NoteDir = "/tmp/notes"
	cfg.OutputDir = "/tmp/meetings"
	cfg.SpeakerDB = "/tmp/speakers"
	cfg.BraveAPIKey = "test-brave-key"

	reg := buildPrepRegistry(cfg)

	// Should have all tools registered
	expectedTools := []string{"search_meetings", "get_meeting", "list_action_items", "get_speaker_profiles", "web_search", "web_fetch", "write_file"}
	for _, name := range expectedTools {
		if reg.Get(name) == nil {
			t.Errorf("expected tool %q to be registered", name)
		}
	}
}

func TestBuildPrepRegistry_NoBraveKey(t *testing.T) {
	cfg := AgentConfig{}
	reg := buildPrepRegistry(cfg)

	if reg.Get("web_search") != nil {
		t.Error("expected web_search to NOT be registered without Brave API key")
	}
}

// setupFixtureNote creates a temp copy of the full meeting fixture.
func setupFixtureNote(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	src := filepath.Join("..", "meetingdata", "testdata", "meeting_full.md")
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	notePath := filepath.Join(dir, "Meeting_2026-03-15_14-30_test.md")
	if err := os.WriteFile(notePath, data, 0o644); err != nil {
		t.Fatalf("write fixture: %v", err)
	}
	return notePath
}
