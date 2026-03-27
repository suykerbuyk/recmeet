// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"testing"
)

func TestExtractActionItems_Full(t *testing.T) {
	note, err := ParseNote("testdata/meeting_full.md")
	if err != nil {
		t.Fatalf("ParseNote: %v", err)
	}

	items := note.ActionItems
	if len(items) != 4 {
		t.Fatalf("expected 4 items, got %d", len(items))
	}

	// Check assignee extraction
	if items[0].Assignee != "Alice" {
		t.Errorf("item[0].Assignee = %q, want Alice", items[0].Assignee)
	}
	if items[0].Done {
		t.Error("item[0] should not be done")
	}

	if items[1].Assignee != "Bob" {
		t.Errorf("item[1].Assignee = %q, want Bob", items[1].Assignee)
	}

	// Check done status
	if !items[2].Done {
		t.Error("item[2] should be done")
	}

	// Unassigned item
	if items[3].Assignee != "" {
		t.Errorf("item[3].Assignee = %q, want empty", items[3].Assignee)
	}
}

func TestExtractActionItems_NoneIdentified(t *testing.T) {
	note, err := ParseNote("testdata/meeting_no_actions.md")
	if err != nil {
		t.Fatalf("ParseNote: %v", err)
	}

	if len(note.ActionItems) != 0 {
		t.Errorf("expected 0 items (none filtered), got %d", len(note.ActionItems))
	}
}

func TestExtractActionItems_CalloutIgnored(t *testing.T) {
	body := `
> [!summary] Summary
> ## Action Items
> - [ ] This should be ignored
>

## Action Items

- [ ] This should be included
`
	items := extractActionItems(body)
	if len(items) != 1 {
		t.Fatalf("expected 1 item, got %d", len(items))
	}
	if items[0].Text != "This should be included" {
		t.Errorf("unexpected text: %q", items[0].Text)
	}
}

func TestListAllActionItems(t *testing.T) {
	tmp := t.TempDir()
	src, _ := os.ReadFile("testdata/meeting_full.md")
	os.WriteFile(filepath.Join(tmp, "Meeting_2026-03-15_14-30.md"), src, 0644)
	src2, _ := os.ReadFile("testdata/meeting_simple.md")
	os.WriteFile(filepath.Join(tmp, "Meeting_2026-03-20_09-00.md"), src2, 0644)

	// All items
	items, err := ListAllActionItems(tmp, "", ActionItemFilters{Status: "all"})
	if err != nil {
		t.Fatalf("ListAllActionItems: %v", err)
	}
	if len(items) != 5 {
		t.Errorf("expected 5 total items, got %d", len(items))
	}

	// Open only
	items, err = ListAllActionItems(tmp, "", ActionItemFilters{Status: "open"})
	if err != nil {
		t.Fatalf("ListAllActionItems: %v", err)
	}
	if len(items) != 4 {
		t.Errorf("expected 4 open items, got %d", len(items))
	}

	// Done only
	items, err = ListAllActionItems(tmp, "", ActionItemFilters{Status: "done"})
	if err != nil {
		t.Fatalf("ListAllActionItems: %v", err)
	}
	if len(items) != 1 {
		t.Errorf("expected 1 done item, got %d", len(items))
	}

	// By assignee
	items, err = ListAllActionItems(tmp, "", ActionItemFilters{Assignee: "Alice"})
	if err != nil {
		t.Fatalf("ListAllActionItems: %v", err)
	}
	if len(items) != 1 {
		t.Errorf("expected 1 item for Alice, got %d", len(items))
	}

	// With limit
	items, err = ListAllActionItems(tmp, "", ActionItemFilters{Limit: 2})
	if err != nil {
		t.Fatalf("ListAllActionItems: %v", err)
	}
	if len(items) != 2 {
		t.Errorf("expected 2 items with limit, got %d", len(items))
	}
}
