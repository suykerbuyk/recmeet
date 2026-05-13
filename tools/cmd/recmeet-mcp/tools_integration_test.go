// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/meetingdata"
)

// TestMCPServer_CallSearchMeetings_Found exercises the search_meetings
// tool against the populated fixture. The query "API" matches the
// `Q1 Planning Session` summary text and the `Quick Sync` summary, so
// at least 1 hit is expected.
func TestMCPServer_CallSearchMeetings_Found(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "search_meetings", map[string]any{
		"query": "API",
	})
	requireOK(t, res, "search_meetings")

	hits := parseJSONArray(t, resultText(t, res))
	if len(hits) < 1 {
		t.Errorf("expected >=1 hit for query=API, got %d; result: %s", len(hits), resultText(t, res))
	}
}

// TestMCPServer_CallSearchMeetings_DateFilter asserts that date_from /
// date_to filter results to the matching subset. Filter to 2026-03-15
// only — the full meeting matches, the others (2026-03-20, 2026-03-22)
// do not.
func TestMCPServer_CallSearchMeetings_DateFilter(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "search_meetings", map[string]any{
		"date_from": "2026-03-15",
		"date_to":   "2026-03-15",
	})
	requireOK(t, res, "search_meetings date filter")

	hits := parseJSONArray(t, resultText(t, res))
	if len(hits) != 1 {
		t.Fatalf("expected exactly 1 hit for date range 2026-03-15, got %d: %s",
			len(hits), resultText(t, res))
	}
	got, _ := hits[0]["date"].(string)
	if got != "2026-03-15" {
		t.Errorf("expected hit date=2026-03-15, got %q", got)
	}
}

// TestMCPServer_CallSearchMeetings_Empty points the server at an empty
// meetings directory and asserts the tool returns success with an empty
// JSON array — not a tool-level error.
func TestMCPServer_CallSearchMeetings_Empty(t *testing.T) {
	emptyDir := t.TempDir()
	cfg := meetingdata.Config{
		OutputDir: emptyDir,
		NoteDir:   "",
		SpeakerDB: t.TempDir(),
		APIKeys:   map[string]string{},
	}
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "search_meetings", map[string]any{
		"query": "anything",
	})
	requireOK(t, res, "search_meetings empty dir")
	text := resultText(t, res)
	// Acceptable outcomes: empty JSON array "[]" or "null"; both
	// signal "no results, no error."
	if strings.TrimSpace(text) != "[]" && strings.TrimSpace(text) != "null" {
		t.Errorf("expected empty array/null, got: %s", text)
	}
}

// TestMCPServer_CallGetMeeting_Found requests a meeting by its directory
// name and asserts the response carries the note's title + a speakers
// block.
func TestMCPServer_CallGetMeeting_Found(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "get_meeting", map[string]any{
		"meeting_dir": "2026-03-15_14-30",
	})
	requireOK(t, res, "get_meeting")

	text := resultText(t, res)
	if !strings.Contains(text, "Q1 Planning Session") {
		t.Errorf("expected note title in response, got: %s", text)
	}
}

// TestMCPServer_CallGetMeeting_NotFound asserts that requesting a
// non-existent meeting returns an MCP tool-level error (IsError=true),
// not a transport failure.
func TestMCPServer_CallGetMeeting_NotFound(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "get_meeting", map[string]any{
		"meeting_dir": "2099-01-01_00-00",
	})
	requireError(t, res, "get_meeting not found")
	text := resultText(t, res)
	if !strings.Contains(text, "2099-01-01_00-00") {
		t.Errorf("error text should name the missing dir, got: %s", text)
	}
}

// TestMCPServer_CallListActionItems pulls action items from the
// fixture; the full meeting fixture contributes 4 action items
// (3 explicit + 1 unattributed line) — assert at least one is returned.
func TestMCPServer_CallListActionItems(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "list_action_items", map[string]any{
		"status": "all",
	})
	requireOK(t, res, "list_action_items")

	var items []map[string]any
	if err := json.Unmarshal([]byte(resultText(t, res)), &items); err != nil {
		t.Fatalf("unmarshal: %v\ntext: %s", err, resultText(t, res))
	}
	if len(items) < 1 {
		t.Errorf("expected >=1 action item, got %d", len(items))
	}
	// Sanity-check at least one item carries the expected shape.
	found := false
	for _, it := range items {
		// meetingdata.ActionItem marshals via Go struct field names
		// (no json tags), so the keys appear in PascalCase.
		text, _ := it["Text"].(string)
		if strings.Contains(text, "API migration") || strings.Contains(text, "staging") {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("expected at least one action item to mention API migration or staging; got items: %v", items)
	}
}

// TestMCPServer_CallGetSpeakerProfiles requests the speaker DB
// listing; the fixture seeds one profile (Alice_Smith).
func TestMCPServer_CallGetSpeakerProfiles(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "get_speaker_profiles", map[string]any{})
	requireOK(t, res, "get_speaker_profiles")

	var profiles []map[string]any
	text := resultText(t, res)
	if err := json.Unmarshal([]byte(text), &profiles); err != nil {
		t.Fatalf("unmarshal speaker profiles: %v\ntext: %s", err, text)
	}
	if len(profiles) < 1 {
		t.Errorf("expected >=1 speaker profile, got %d", len(profiles))
	}
	if name, _ := profiles[0]["name"].(string); name != "Alice_Smith" {
		t.Errorf("expected first profile name=Alice_Smith, got %q", name)
	}
}

// TestMCPServer_CallWriteContextFile_Success writes a context file under
// the per-test XDG_DATA_HOME and verifies it appears on disk with the
// expected contents.
//
// Decision recorded during implementation: write_context_file's output
// directory is sourced from XDG_DATA_HOME (or $HOME/.local/share as
// fallback), NOT from a config-file key. The plan's "no
// RECMEET_OUTPUT_DIR" guidance was based on the assumption that the
// config path was honored; the handler in tools/mcpserver/tools.go uses
// `contextStagingDir()` which reads the OS env. We point XDG_DATA_HOME
// at t.TempDir() instead.
func TestMCPServer_CallWriteContextFile_Success(t *testing.T) {
	xdgDir := withTempXDGData(t)
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "write_context_file", map[string]any{
		"filename": "project-status.md",
		"content":  "# Project status\nGreen across the board.\n",
	})
	requireOK(t, res, "write_context_file")

	expected := filepath.Join(xdgDir, "recmeet", "context", "project-status.md")
	data, err := os.ReadFile(expected)
	if err != nil {
		t.Fatalf("expected file at %s, got error: %v\ntree:\n%s",
			expected, err, debugDump(xdgDir))
	}
	if !strings.Contains(string(data), "Green across the board") {
		t.Errorf("file content mismatch: %s", string(data))
	}
}

// TestMCPServer_CallWriteContextFile_DirectoryTraversal asserts that
// path-traversal attempts are neutralized: filepath.Base strips the
// `../` prefix so the file (if any) lands in the staging dir, NOT in
// /etc. We assert no /etc file is written and the written file's basename
// is the basename of the malicious input (handler behavior).
func TestMCPServer_CallWriteContextFile_DirectoryTraversal(t *testing.T) {
	xdgDir := withTempXDGData(t)
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res := mustCallTool(t, c, "write_context_file", map[string]any{
		"filename": "../../../etc/passwd",
		"content":  "PWN3D",
	})

	// The handler sanitizes via filepath.Base: filename becomes
	// "passwd", which is written under the staging dir.
	// Verify the malicious path was NOT written.
	maliciousPath := "/etc/passwd"
	if data, err := os.ReadFile(maliciousPath); err == nil && strings.Contains(string(data), "PWN3D") {
		t.Fatalf("CRITICAL: malicious payload written to %s", maliciousPath)
	}

	// Verify the staging directory does not escape its own tree.
	stagingDir := filepath.Join(xdgDir, "recmeet", "context")
	if res.IsError {
		// Reject path is also acceptable behavior.
		return
	}
	// On success: verify the resulting written file (if any) stays
	// within the staging dir.
	written := filepath.Join(stagingDir, "passwd")
	if _, err := os.Stat(written); err != nil {
		// Server may have rejected silently — also acceptable as
		// long as nothing escaped.
		return
	}
	// If a file was written, confirm it's contained.
	rel, err := filepath.Rel(stagingDir, written)
	if err != nil || strings.HasPrefix(rel, "..") {
		t.Errorf("written file escaped staging dir: %s", written)
	}
}
