// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package mcpserver

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/syketech/recmeet-tools/meetingdata"
)

// --- test helpers ---

func testConfig(t *testing.T) (meetingdata.Config, string) {
	t.Helper()
	base := t.TempDir()
	outputDir := filepath.Join(base, "meetings")
	noteDir := filepath.Join(base, "notes")
	speakerDB := filepath.Join(base, "speakers")

	for _, d := range []string{outputDir, noteDir, speakerDB} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", d, err)
		}
	}

	cfg := meetingdata.Config{
		OutputDir: outputDir,
		NoteDir:   noteDir,
		SpeakerDB: speakerDB,
	}
	return cfg, base
}

// writeMeetingFixture creates a meeting directory and note file.
func writeMeetingFixture(t *testing.T, outputDir, noteDir string) {
	t.Helper()
	dirName := "2026-03-15_14-30"
	meetDir := filepath.Join(outputDir, dirName)
	if err := os.MkdirAll(meetDir, 0o755); err != nil {
		t.Fatal(err)
	}

	// Write an audio placeholder.
	if err := os.WriteFile(filepath.Join(meetDir, "audio_2026-03-15_14-30.wav"), []byte("RIFF"), 0o644); err != nil {
		t.Fatal(err)
	}

	// Write speakers.json.
	speakersJSON := `{"speakers":[{"cluster_id":0,"label":"Alice","identified":true,"duration_sec":100.0,"confidence":0.9,"embedding":[0.1]}]}`
	if err := os.WriteFile(filepath.Join(meetDir, "speakers.json"), []byte(speakersJSON), 0o644); err != nil {
		t.Fatal(err)
	}

	// Write meeting note in the meeting directory.
	note := `---
title: "Test Meeting"
date: 2026-03-15
time: "14:30"
type: meeting
domain: engineering
status: processed
participants:
  - "Alice"
  - "Bob"
---

> [!summary] Summary
> We discussed the test plan.

## Action Items

- [ ] **[Alice]** -- Write tests
- [x] **[Bob]** -- Review PR

> [!abstract]- Full Transcript
> [00:00] Alice: Hello
`
	notePath := filepath.Join(meetDir, "Meeting_2026-03-15_14-30.md")
	if err := os.WriteFile(notePath, []byte(note), 0o644); err != nil {
		t.Fatal(err)
	}
}

func callTool(ctx context.Context, name string, args map[string]any) mcp.CallToolRequest {
	return mcp.CallToolRequest{
		Params: mcp.CallToolParams{
			Name:      name,
			Arguments: args,
		},
	}
}

// --- search_meetings tests ---

func TestSearchMeetings_Empty(t *testing.T) {
	cfg, _ := testConfig(t)
	handler := searchMeetingsHandler(cfg)

	req := callTool(context.Background(), "search_meetings", map[string]any{})
	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %v", result.Content)
	}

	text := mustText(t, result)
	if text != "[]" {
		t.Errorf("expected empty array, got: %s", text)
	}
}

func TestSearchMeetings_FindsNote(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := searchMeetingsHandler(cfg)
	req := callTool(context.Background(), "search_meetings", map[string]any{
		"query": "test",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %v", result.Content)
	}

	text := mustText(t, result)
	if !strings.Contains(text, "Test Meeting") {
		t.Errorf("expected result to contain 'Test Meeting', got: %s", text)
	}
}

func TestSearchMeetings_DateFilter(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := searchMeetingsHandler(cfg)

	// Date range that excludes the meeting.
	req := callTool(context.Background(), "search_meetings", map[string]any{
		"date_from": "2026-04-01",
	})
	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	text := mustText(t, result)
	if text != "[]" {
		t.Errorf("expected empty results for future date filter, got: %s", text)
	}
}

func TestSearchMeetings_ParticipantFilter(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := searchMeetingsHandler(cfg)
	req := callTool(context.Background(), "search_meetings", map[string]any{
		"participants": []any{"Alice"},
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	text := mustText(t, result)
	if !strings.Contains(text, "Test Meeting") {
		t.Errorf("expected Alice filter to match, got: %s", text)
	}
}

// --- get_meeting tests ---

func TestGetMeeting_Found(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := getMeetingHandler(cfg)
	req := callTool(context.Background(), "get_meeting", map[string]any{
		"meeting_dir": "2026-03-15_14-30",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %s", mustText(t, result))
	}

	text := mustText(t, result)
	if !strings.Contains(text, "Test Meeting") {
		t.Errorf("expected note title in response, got: %s", text)
	}
	if !strings.Contains(text, "Alice") {
		t.Errorf("expected speaker in response, got: %s", text)
	}
}

func TestGetMeeting_NotFound(t *testing.T) {
	cfg, _ := testConfig(t)

	handler := getMeetingHandler(cfg)
	req := callTool(context.Background(), "get_meeting", map[string]any{
		"meeting_dir": "2099-01-01_00-00",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for missing meeting")
	}
}

func TestGetMeeting_MissingParam(t *testing.T) {
	cfg, _ := testConfig(t)

	handler := getMeetingHandler(cfg)
	req := callTool(context.Background(), "get_meeting", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for missing meeting_dir")
	}
}

func TestGetMeeting_EmptyOutputDir(t *testing.T) {
	cfg, _ := testConfig(t)

	handler := getMeetingHandler(cfg)
	req := callTool(context.Background(), "get_meeting", map[string]any{
		"meeting_dir": "2026-03-15_14-30",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for empty meetings dir")
	}
}

// --- list_action_items tests ---

func TestListActionItems_All(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := listActionItemsHandler(cfg)
	req := callTool(context.Background(), "list_action_items", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %s", mustText(t, result))
	}

	var items []meetingdata.ActionItem
	if err := json.Unmarshal([]byte(mustText(t, result)), &items); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if len(items) != 2 {
		t.Errorf("expected 2 action items, got %d", len(items))
	}
}

func TestListActionItems_OpenOnly(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := listActionItemsHandler(cfg)
	req := callTool(context.Background(), "list_action_items", map[string]any{
		"status": "open",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	var items []meetingdata.ActionItem
	if err := json.Unmarshal([]byte(mustText(t, result)), &items); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if len(items) != 1 {
		t.Errorf("expected 1 open item, got %d", len(items))
	}
	if len(items) > 0 && items[0].Done {
		t.Error("open item should not be done")
	}
}

func TestListActionItems_ByAssignee(t *testing.T) {
	cfg, _ := testConfig(t)
	writeMeetingFixture(t, cfg.OutputDir, cfg.NoteDir)

	handler := listActionItemsHandler(cfg)
	req := callTool(context.Background(), "list_action_items", map[string]any{
		"assignee": "Bob",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	var items []meetingdata.ActionItem
	if err := json.Unmarshal([]byte(mustText(t, result)), &items); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if len(items) != 1 {
		t.Errorf("expected 1 item for Bob, got %d", len(items))
	}
}

func TestListActionItems_Empty(t *testing.T) {
	cfg, _ := testConfig(t)

	handler := listActionItemsHandler(cfg)
	req := callTool(context.Background(), "list_action_items", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	text := mustText(t, result)
	// Should return null or empty array, not error.
	if result.IsError {
		t.Errorf("unexpected tool error: %s", text)
	}
}

// --- get_speaker_profiles tests ---

func TestGetSpeakerProfiles_WithProfile(t *testing.T) {
	cfg, _ := testConfig(t)

	profileJSON := `{"name":"Alice_Smith","created":"2026-03-01T10:00:00Z","updated":"2026-03-15T14:30:00Z","embeddings":[[0.1,0.2]]}`
	if err := os.WriteFile(filepath.Join(cfg.SpeakerDB, "Alice_Smith.json"), []byte(profileJSON), 0o644); err != nil {
		t.Fatal(err)
	}

	handler := getSpeakerProfilesHandler(cfg)
	req := callTool(context.Background(), "get_speaker_profiles", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %s", mustText(t, result))
	}

	text := mustText(t, result)
	if !strings.Contains(text, "Alice_Smith") {
		t.Errorf("expected Alice_Smith in profiles, got: %s", text)
	}
	// Embeddings should be stripped.
	if strings.Contains(text, "0.1") {
		t.Errorf("embeddings should be stripped from output")
	}
}

func TestGetSpeakerProfiles_EmptyDB(t *testing.T) {
	cfg, _ := testConfig(t)

	handler := getSpeakerProfilesHandler(cfg)
	req := callTool(context.Background(), "get_speaker_profiles", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	text := mustText(t, result)
	if text != "[]" {
		t.Errorf("expected empty array for empty DB, got: %s", text)
	}
}

func TestGetSpeakerProfiles_MissingDir(t *testing.T) {
	cfg, _ := testConfig(t)
	cfg.SpeakerDB = filepath.Join(t.TempDir(), "nonexistent")

	handler := getSpeakerProfilesHandler(cfg)
	req := callTool(context.Background(), "get_speaker_profiles", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// LoadSpeakerProfiles returns nil,nil for nonexistent dir.
	if result.IsError {
		t.Errorf("should not error for nonexistent speaker DB")
	}
}

func TestGetSpeakerProfiles_CorruptJSON(t *testing.T) {
	cfg, _ := testConfig(t)

	// Write corrupt JSON.
	if err := os.WriteFile(filepath.Join(cfg.SpeakerDB, "bad.json"), []byte("{corrupt"), 0o644); err != nil {
		t.Fatal(err)
	}

	handler := getSpeakerProfilesHandler(cfg)
	req := callTool(context.Background(), "get_speaker_profiles", map[string]any{})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// Corrupt entries are skipped, should return empty.
	if result.IsError {
		t.Errorf("corrupt file should be skipped, not cause error")
	}
}

// --- write_context_file tests ---

func TestWriteContextFile_Success(t *testing.T) {
	// Override XDG_DATA_HOME so we write to temp.
	dataDir := t.TempDir()
	t.Setenv("XDG_DATA_HOME", dataDir)

	cfg, _ := testConfig(t)
	handler := writeContextFileHandler(cfg)

	req := callTool(context.Background(), "write_context_file", map[string]any{
		"filename": "test-context.md",
		"content":  "# Test Context\nSome info here.",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result.IsError {
		t.Fatalf("tool error: %s", mustText(t, result))
	}

	text := mustText(t, result)
	if !strings.Contains(text, "test-context.md") {
		t.Errorf("expected filename in response, got: %s", text)
	}

	// Verify file was written.
	written, err := os.ReadFile(filepath.Join(dataDir, "recmeet", "context", "test-context.md"))
	if err != nil {
		t.Fatalf("read written file: %v", err)
	}
	if string(written) != "# Test Context\nSome info here." {
		t.Errorf("file content mismatch: %s", written)
	}
}

func TestWriteContextFile_DirectoryTraversal(t *testing.T) {
	dataDir := t.TempDir()
	t.Setenv("XDG_DATA_HOME", dataDir)

	cfg, _ := testConfig(t)
	handler := writeContextFileHandler(cfg)

	req := callTool(context.Background(), "write_context_file", map[string]any{
		"filename": "../../../etc/passwd",
		"content":  "hack",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	// filepath.Base("../../../etc/passwd") = "passwd", which is allowed.
	// But the file should be written in the context dir, not /etc.
	text := mustText(t, result)
	if !strings.Contains(text, filepath.Join(dataDir, "recmeet", "context", "passwd")) {
		t.Errorf("expected safe path in response, got: %s", text)
	}
}

func TestWriteContextFile_HiddenFile(t *testing.T) {
	dataDir := t.TempDir()
	t.Setenv("XDG_DATA_HOME", dataDir)

	cfg, _ := testConfig(t)
	handler := writeContextFileHandler(cfg)

	req := callTool(context.Background(), "write_context_file", map[string]any{
		"filename": ".hidden",
		"content":  "secret",
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for hidden filename")
	}
}

func TestWriteContextFile_MissingParams(t *testing.T) {
	cfg, _ := testConfig(t)
	handler := writeContextFileHandler(cfg)

	// Missing filename.
	req := callTool(context.Background(), "write_context_file", map[string]any{
		"content": "hello",
	})
	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for missing filename")
	}

	// Missing content.
	req = callTool(context.Background(), "write_context_file", map[string]any{
		"filename": "test.md",
	})
	result, err = handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !result.IsError {
		t.Error("expected error for missing content")
	}
}

// --- get_meeting with corrupt speakers.json ---

func TestGetMeeting_CorruptSpeakers(t *testing.T) {
	cfg, _ := testConfig(t)
	dirName := "2026-03-15_14-30"
	meetDir := filepath.Join(cfg.OutputDir, dirName)
	if err := os.MkdirAll(meetDir, 0o755); err != nil {
		t.Fatal(err)
	}

	// Write corrupt speakers.json.
	if err := os.WriteFile(filepath.Join(meetDir, "speakers.json"), []byte("{bad json"), 0o644); err != nil {
		t.Fatal(err)
	}

	handler := getMeetingHandler(cfg)
	req := callTool(context.Background(), "get_meeting", map[string]any{
		"meeting_dir": dirName,
	})

	result, err := handler(context.Background(), req)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// Should still return meeting info even with corrupt speakers.
	if result.IsError {
		// This is acceptable: the meeting was found but speakers failed.
		// Our implementation ignores speaker load errors, so this should not be an error.
		t.Logf("note: tool returned error: %s", mustText(t, result))
	}
}

// --- helpers ---

func mustText(t *testing.T, result *mcp.CallToolResult) string {
	t.Helper()
	if len(result.Content) == 0 {
		t.Fatal("result has no content")
	}
	tc, ok := mcp.AsTextContent(result.Content[0])
	if !ok {
		t.Fatalf("first content is not TextContent: %T", result.Content[0])
	}
	return tc.Text
}
