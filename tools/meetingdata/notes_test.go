// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseNote_Full(t *testing.T) {
	note, err := ParseNote("testdata/meeting_full.md")
	if err != nil {
		t.Fatalf("ParseNote: %v", err)
	}

	fm := note.Frontmatter
	if fm.Title != "Q1 Planning Session" {
		t.Errorf("Title = %q", fm.Title)
	}
	if fm.Date != "2026-03-15" {
		t.Errorf("Date = %q", fm.Date)
	}
	if fm.Time != "14:30" {
		t.Errorf("Time = %q", fm.Time)
	}
	if fm.Domain != "engineering" {
		t.Errorf("Domain = %q", fm.Domain)
	}
	if fm.Duration != "1:23:45" {
		t.Errorf("Duration = %q", fm.Duration)
	}
	if fm.WhisperModel != "small" {
		t.Errorf("WhisperModel = %q", fm.WhisperModel)
	}
	if len(fm.Tags) != 3 {
		t.Errorf("Tags count = %d, want 3", len(fm.Tags))
	}
	if len(fm.Participants) != 3 {
		t.Errorf("Participants count = %d, want 3", len(fm.Participants))
	}
	// Check wikilinks are stripped
	if fm.Participants[0] != "Alice_Smith" {
		t.Errorf("Participant[0] = %q, want Alice_Smith", fm.Participants[0])
	}

	if note.ContextText == "" {
		t.Error("ContextText is empty")
	}
	if note.SummaryText == "" {
		t.Error("SummaryText is empty")
	}
	if note.Transcript == "" {
		t.Error("Transcript is empty")
	}

	if len(note.ActionItems) != 4 {
		t.Fatalf("ActionItems count = %d, want 4", len(note.ActionItems))
	}
	if note.ActionItems[0].Assignee != "Alice" {
		t.Errorf("ActionItem[0].Assignee = %q, want Alice", note.ActionItems[0].Assignee)
	}
	if note.ActionItems[2].Done != true {
		t.Error("ActionItem[2] should be done")
	}
	if note.ActionItems[3].Assignee != "" {
		t.Errorf("ActionItem[3] should have no assignee, got %q", note.ActionItems[3].Assignee)
	}
}

func TestParseNote_Simple(t *testing.T) {
	note, err := ParseNote("testdata/meeting_simple.md")
	if err != nil {
		t.Fatalf("ParseNote: %v", err)
	}

	if note.Frontmatter.Title != "Quick Sync" {
		t.Errorf("Title = %q", note.Frontmatter.Title)
	}
	if note.ContextText != "" {
		t.Errorf("ContextText should be empty, got %q", note.ContextText)
	}
	if note.SummaryText == "" {
		t.Error("SummaryText is empty")
	}
	if len(note.ActionItems) != 1 {
		t.Errorf("ActionItems count = %d, want 1", len(note.ActionItems))
	}
}

func TestParseNote_NoActions(t *testing.T) {
	note, err := ParseNote("testdata/meeting_no_actions.md")
	if err != nil {
		t.Fatalf("ParseNote: %v", err)
	}

	if len(note.ActionItems) != 0 {
		t.Errorf("ActionItems count = %d, want 0 (none identified filtered)", len(note.ActionItems))
	}
}

func TestSplitFrontmatter(t *testing.T) {
	tests := []struct {
		name    string
		input   string
		wantFM  bool
		wantBody bool
	}{
		{"with frontmatter", "---\ntitle: test\n---\nbody", true, true},
		{"no frontmatter", "just body", false, true},
		{"empty", "", false, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			fm, body := splitFrontmatter(tt.input)
			if tt.wantFM && fm == "" {
				t.Error("expected frontmatter")
			}
			if tt.wantBody && body == "" && tt.input != "" {
				t.Error("expected body")
			}
		})
	}
}

func TestExtractCallout(t *testing.T) {
	body := `
> [!summary] Meeting Summary
> Line one of summary.
> Line two of summary.
>

## Action Items
`
	text := extractCallout(body, "summary")
	if text == "" {
		t.Fatal("extractCallout returned empty")
	}
	if text != "Line one of summary.\nLine two of summary." {
		t.Errorf("unexpected callout text: %q", text)
	}
}

func TestStripWikilinks(t *testing.T) {
	if got := stripWikilinks("[[Alice_Smith]]"); got != "Alice_Smith" {
		t.Errorf("stripWikilinks = %q", got)
	}
	if got := stripWikilinks("no links"); got != "no links" {
		t.Errorf("stripWikilinks = %q", got)
	}
}

func TestSearchNotes(t *testing.T) {
	// Set up a temp dir with meeting notes
	tmp := t.TempDir()
	src, _ := os.ReadFile("testdata/meeting_full.md")
	os.WriteFile(filepath.Join(tmp, "Meeting_2026-03-15_14-30_Q1_Planning.md"), src, 0644)
	src2, _ := os.ReadFile("testdata/meeting_simple.md")
	os.WriteFile(filepath.Join(tmp, "Meeting_2026-03-20_09-00_Quick_Sync.md"), src2, 0644)

	results, err := SearchNotes(tmp, "", "planning", SearchFilters{})
	if err != nil {
		t.Fatalf("SearchNotes: %v", err)
	}
	if len(results) != 1 {
		t.Errorf("expected 1 result for 'planning', got %d", len(results))
	}

	// Search with date filter
	results, err = SearchNotes(tmp, "", "", SearchFilters{DateFrom: "2026-03-18"})
	if err != nil {
		t.Fatalf("SearchNotes: %v", err)
	}
	if len(results) != 1 {
		t.Errorf("expected 1 result after 2026-03-18, got %d", len(results))
	}

	// Search with participant filter
	results, err = SearchNotes(tmp, "", "", SearchFilters{Participants: []string{"Bob"}})
	if err != nil {
		t.Fatalf("SearchNotes: %v", err)
	}
	if len(results) != 1 {
		t.Errorf("expected 1 result for participant Bob, got %d", len(results))
	}

	// Search with limit
	results, err = SearchNotes(tmp, "", "", SearchFilters{Limit: 1})
	if err != nil {
		t.Fatalf("SearchNotes: %v", err)
	}
	if len(results) != 1 {
		t.Errorf("expected 1 result with limit 1, got %d", len(results))
	}
}
