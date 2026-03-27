// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"regexp"
	"strings"
)

type Frontmatter struct {
	Title        string
	Date         string
	Created      string
	Time         string
	Type         string
	Domain       string
	Status       string
	Description  string
	Tags         []string
	Participants []string
	Duration     string
	Source       string
	WhisperModel string
}

type MeetingNote struct {
	Path        string
	Frontmatter Frontmatter
	ContextText string
	SummaryText string
	ActionItems []ActionItem
	Transcript  string
}

func ParseNote(path string) (*MeetingNote, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parseNoteContent(path, string(data))
}

func parseNoteContent(path, content string) (*MeetingNote, error) {
	note := &MeetingNote{Path: path}

	fm, body := splitFrontmatter(content)
	note.Frontmatter = parseFrontmatter(fm)

	note.ContextText = extractCallout(body, "note")
	note.SummaryText = extractCallout(body, "summary")
	note.Transcript = extractCallout(body, "abstract")
	note.ActionItems = extractActionItems(body)

	return note, nil
}

func splitFrontmatter(content string) (string, string) {
	if !strings.HasPrefix(content, "---") {
		return "", content
	}
	rest := content[3:]
	idx := strings.Index(rest, "\n---")
	if idx < 0 {
		return "", content
	}
	return strings.TrimSpace(rest[:idx]), rest[idx+4:]
}

func parseFrontmatter(fm string) Frontmatter {
	var f Frontmatter
	if fm == "" {
		return f
	}

	lines := strings.Split(fm, "\n")
	var currentList *[]string

	for _, line := range lines {
		trimmed := strings.TrimSpace(line)

		// Handle list items
		if strings.HasPrefix(trimmed, "- ") {
			if currentList != nil {
				val := strings.TrimPrefix(trimmed, "- ")
				val = unquote(strings.TrimSpace(val))
				val = stripWikilinks(val)
				*currentList = append(*currentList, val)
			}
			continue
		}

		currentList = nil

		colon := strings.Index(trimmed, ":")
		if colon < 0 {
			continue
		}

		key := strings.TrimSpace(trimmed[:colon])
		val := strings.TrimSpace(trimmed[colon+1:])
		val = unquote(val)

		switch key {
		case "title":
			f.Title = val
		case "date":
			f.Date = val
		case "created":
			f.Created = val
		case "time":
			f.Time = val
		case "type":
			f.Type = val
		case "domain":
			f.Domain = val
		case "status":
			f.Status = val
		case "description":
			f.Description = val
		case "duration":
			f.Duration = val
		case "source":
			f.Source = val
		case "whisper_model":
			f.WhisperModel = val
		case "tags":
			if val == "" {
				currentList = &f.Tags
			}
		case "participants":
			if val == "" {
				currentList = &f.Participants
			}
		}
	}
	return f
}

func stripWikilinks(s string) string {
	s = strings.ReplaceAll(s, "[[", "")
	s = strings.ReplaceAll(s, "]]", "")
	return s
}

var calloutHeaderRe = regexp.MustCompile(`^>\s*\[!(\w+)\][-]?\s*(.*)$`)

func extractCallout(body, calloutType string) string {
	lines := strings.Split(body, "\n")
	var result []string
	inCallout := false

	for _, line := range lines {
		if !inCallout {
			if m := calloutHeaderRe.FindStringSubmatch(line); m != nil && m[1] == calloutType {
				inCallout = true
				continue
			}
		} else {
			if strings.HasPrefix(line, "> ") {
				result = append(result, strings.TrimPrefix(line, "> "))
			} else if strings.TrimSpace(line) == ">" {
				result = append(result, "")
			} else {
				break
			}
		}
	}
	return strings.TrimSpace(strings.Join(result, "\n"))
}

type SearchFilters struct {
	DateFrom     string
	DateTo       string
	Participants []string
	Limit        int
}

func SearchNotes(noteDir, outputDir, query string, filters SearchFilters) ([]MeetingNote, error) {
	paths, err := findNotePaths(noteDir, outputDir)
	if err != nil {
		return nil, err
	}

	queryLower := strings.ToLower(query)
	var results []MeetingNote

	for _, p := range paths {
		note, err := ParseNote(p)
		if err != nil {
			continue
		}

		if !matchesFilters(note, queryLower, filters) {
			continue
		}

		results = append(results, *note)
		if filters.Limit > 0 && len(results) >= filters.Limit {
			break
		}
	}
	return results, nil
}

func matchesFilters(note *MeetingNote, query string, filters SearchFilters) bool {
	if filters.DateFrom != "" && note.Frontmatter.Date < filters.DateFrom {
		return false
	}
	if filters.DateTo != "" && note.Frontmatter.Date > filters.DateTo {
		return false
	}

	if len(filters.Participants) > 0 {
		found := false
		for _, fp := range filters.Participants {
			fpLower := strings.ToLower(fp)
			for _, np := range note.Frontmatter.Participants {
				if strings.Contains(strings.ToLower(np), fpLower) {
					found = true
					break
				}
			}
			if found {
				break
			}
		}
		if !found {
			return false
		}
	}

	if query == "" {
		return true
	}

	searchable := strings.ToLower(strings.Join([]string{
		note.Frontmatter.Title,
		note.SummaryText,
		strings.Join(note.Frontmatter.Tags, " "),
		strings.Join(note.Frontmatter.Participants, " "),
	}, " "))

	return strings.Contains(searchable, query)
}

func findNotePaths(noteDir, outputDir string) ([]string, error) {
	var paths []string

	if noteDir != "" {
		p, err := findMDFiles(noteDir)
		if err == nil {
			paths = append(paths, p...)
		}
	}

	if outputDir != "" && outputDir != noteDir {
		p, err := findMDFiles(outputDir)
		if err == nil {
			paths = append(paths, p...)
		}
	}

	return paths, nil
}

func findMDFiles(dir string) ([]string, error) {
	var files []string
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	for _, e := range entries {
		if e.IsDir() {
			// Recurse into subdirectories (YYYY/MM/ or meeting dirs)
			sub, err := findMDFiles(strings.Join([]string{dir, e.Name()}, "/"))
			if err == nil {
				files = append(files, sub...)
			}
		} else if strings.HasSuffix(e.Name(), ".md") && strings.HasPrefix(e.Name(), "Meeting_") {
			files = append(files, strings.Join([]string{dir, e.Name()}, "/"))
		}
	}
	return files, nil
}
