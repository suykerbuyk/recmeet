// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestToolRegistry(t *testing.T) {
	reg := NewToolRegistry()

	tool := &WriteFileTool{}
	reg.Register(tool)

	if got := reg.Get("write_file"); got == nil {
		t.Error("expected to find write_file tool")
	}
	if got := reg.Get("nonexistent"); got != nil {
		t.Error("expected nil for nonexistent tool")
	}

	all := reg.All()
	if len(all) != 1 {
		t.Errorf("expected 1 tool, got %d", len(all))
	}
}

func TestToolRegistryMultiple(t *testing.T) {
	reg := NewToolRegistry()
	reg.Register(&WriteFileTool{})
	reg.Register(&WebFetchTool{})

	all := reg.All()
	if len(all) != 2 {
		t.Errorf("expected 2 tools, got %d", len(all))
	}
}

func TestSearchMeetingsTool(t *testing.T) {
	// Set up fixture data
	noteDir := setupTestNotes(t)

	tool := &SearchMeetingsTool{NoteDir: noteDir, OutputDir: ""}
	def := tool.Definition()

	if def.Name != "search_meetings" {
		t.Errorf("expected name search_meetings, got %s", def.Name)
	}

	input, _ := json.Marshal(map[string]interface{}{
		"query": "planning",
		"limit": 10,
	})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "Q1 Planning") {
		t.Errorf("expected result to contain Q1 Planning, got: %s", result)
	}
}

func TestSearchMeetingsTool_NoResults(t *testing.T) {
	noteDir := setupTestNotes(t)

	tool := &SearchMeetingsTool{NoteDir: noteDir, OutputDir: ""}
	input, _ := json.Marshal(map[string]interface{}{
		"query": "xyznonexistent",
	})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Error("expected no error flag for empty results")
	}
	if !strings.Contains(result, "No meetings found") {
		t.Errorf("expected no results message, got: %s", result)
	}
}

func TestListActionItemsTool(t *testing.T) {
	noteDir := setupTestNotes(t)

	tool := &ListActionItemsTool{NoteDir: noteDir, OutputDir: ""}
	input, _ := json.Marshal(map[string]interface{}{
		"status": "open",
	})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "Alice") {
		t.Errorf("expected result to mention Alice, got: %s", result)
	}
}

func TestGetSpeakerProfilesTool_NoDir(t *testing.T) {
	tool := &GetSpeakerProfilesTool{SpeakerDB: "/nonexistent/speakers"}
	input, _ := json.Marshal(map[string]interface{}{})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "No speaker profiles") {
		t.Errorf("expected no profiles message, got: %s", result)
	}
}

// setupTestNotes creates a temp dir with test meeting notes and returns the path.
func setupTestNotes(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()

	// Copy fixture note into the temp dir with the right filename
	src := filepath.Join("..", "meetingdata", "testdata", "meeting_full.md")
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}

	dst := filepath.Join(dir, "Meeting_2026-03-15_14-30_Q1_Planning.md")
	if err := os.WriteFile(dst, data, 0o644); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	return dir
}
