// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"regexp"
	"strings"
)

type ActionItem struct {
	Text        string
	Done        bool
	Assignee    string
	MeetingDate string
	NotePath    string
}

type ActionItemFilters struct {
	Status   string // "open", "done", "all"
	Assignee string
	Limit    int
}

var (
	checkboxRe = regexp.MustCompile(`^-\s*\[([ xX])\]\s*(.*)$`)
	assigneeRe = regexp.MustCompile(`^\*\*\[([^\]]+)\]\*\*\s*[-:]\s*(.*)$`)
	noneRe     = regexp.MustCompile(`(?i)^none\b`)
)

func extractActionItems(body string) []ActionItem {
	lines := strings.Split(body, "\n")
	inSection := false
	var items []ActionItem

	for _, line := range lines {
		trimmed := strings.TrimSpace(line)

		// Only match "## Action Items" at top level (not inside callouts)
		if strings.HasPrefix(line, ">") {
			continue
		}
		if trimmed == "## Action Items" {
			inSection = true
			continue
		}
		if inSection && strings.HasPrefix(trimmed, "## ") {
			break
		}

		if !inSection {
			continue
		}

		m := checkboxRe.FindStringSubmatch(trimmed)
		if m == nil {
			continue
		}

		text := strings.TrimSpace(m[2])
		if noneRe.MatchString(text) {
			continue
		}

		item := ActionItem{
			Text: text,
			Done: m[1] != " ",
		}

		if am := assigneeRe.FindStringSubmatch(text); am != nil {
			item.Assignee = am[1]
			item.Text = am[2]
		}

		items = append(items, item)
	}
	return items
}

func ExtractActionItems(note *MeetingNote) []ActionItem {
	items := note.ActionItems
	for i := range items {
		items[i].MeetingDate = note.Frontmatter.Date
		items[i].NotePath = note.Path
	}
	return items
}

func ListAllActionItems(noteDir, outputDir string, filters ActionItemFilters) ([]ActionItem, error) {
	paths, err := findNotePaths(noteDir, outputDir)
	if err != nil {
		return nil, err
	}

	var allItems []ActionItem
	for _, p := range paths {
		note, err := ParseNote(p)
		if err != nil {
			continue
		}

		items := ExtractActionItems(note)
		for _, item := range items {
			if !matchesItemFilters(item, filters) {
				continue
			}
			allItems = append(allItems, item)
			if filters.Limit > 0 && len(allItems) >= filters.Limit {
				return allItems, nil
			}
		}
	}
	return allItems, nil
}

func matchesItemFilters(item ActionItem, filters ActionItemFilters) bool {
	switch filters.Status {
	case "open":
		if item.Done {
			return false
		}
	case "done":
		if !item.Done {
			return false
		}
	}

	if filters.Assignee != "" {
		if !strings.EqualFold(item.Assignee, filters.Assignee) {
			return false
		}
	}

	return true
}
