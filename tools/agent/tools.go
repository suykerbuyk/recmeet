// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"github.com/syketech/recmeet-tools/meetingdata"
)

// Tool is the interface each agent tool must implement.
type Tool interface {
	Definition() ToolDefinition
	Execute(ctx context.Context, inputJSON []byte) (result string, isError bool, err error)
}

// ToolDefinition describes a tool for the Anthropic API.
type ToolDefinition struct {
	Name        string
	Description string
	InputSchema map[string]interface{}
}

// ToolRegistry manages available tools.
type ToolRegistry struct {
	tools map[string]Tool
}

// NewToolRegistry creates an empty registry.
func NewToolRegistry() *ToolRegistry {
	return &ToolRegistry{tools: make(map[string]Tool)}
}

// Register adds a tool to the registry.
func (r *ToolRegistry) Register(t Tool) {
	r.tools[t.Definition().Name] = t
}

// Get returns the tool with the given name, or nil.
func (r *ToolRegistry) Get(name string) Tool {
	return r.tools[name]
}

// All returns all registered tools.
func (r *ToolRegistry) All() []Tool {
	out := make([]Tool, 0, len(r.tools))
	for _, t := range r.tools {
		out = append(out, t)
	}
	return out
}

// --- Meeting data tool wrappers ---

// SearchMeetingsTool searches meeting notes by query and filters.
type SearchMeetingsTool struct {
	NoteDir   string
	OutputDir string
}

func (t *SearchMeetingsTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "search_meetings",
		Description: "Search meeting notes by keyword, date range, and participants.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"query":        map[string]interface{}{"type": "string", "description": "Search query text"},
				"date_from":    map[string]interface{}{"type": "string", "description": "Start date (YYYY-MM-DD)"},
				"date_to":      map[string]interface{}{"type": "string", "description": "End date (YYYY-MM-DD)"},
				"participants": map[string]interface{}{"type": "string", "description": "Comma-separated participant names"},
				"limit":        map[string]interface{}{"type": "integer", "description": "Max results to return"},
			},
		},
	}
}

func (t *SearchMeetingsTool) Execute(_ context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		Query        string `json:"query"`
		DateFrom     string `json:"date_from"`
		DateTo       string `json:"date_to"`
		Participants string `json:"participants"`
		Limit        int    `json:"limit"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}

	var parts []string
	if params.Participants != "" {
		for _, p := range strings.Split(params.Participants, ",") {
			parts = append(parts, strings.TrimSpace(p))
		}
	}

	filters := meetingdata.SearchFilters{
		DateFrom:     params.DateFrom,
		DateTo:       params.DateTo,
		Participants: parts,
		Limit:        params.Limit,
	}
	if filters.Limit == 0 {
		filters.Limit = 10
	}

	notes, err := meetingdata.SearchNotes(t.NoteDir, t.OutputDir, params.Query, filters)
	if err != nil {
		return fmt.Sprintf("Error searching: %s", err), true, nil
	}

	if len(notes) == 0 {
		return "No meetings found matching the query.", false, nil
	}

	var sb strings.Builder
	for i, n := range notes {
		fmt.Fprintf(&sb, "## %d. %s (%s)\n", i+1, n.Frontmatter.Title, n.Frontmatter.Date)
		fmt.Fprintf(&sb, "Participants: %s\n", strings.Join(n.Frontmatter.Participants, ", "))
		if n.SummaryText != "" {
			fmt.Fprintf(&sb, "Summary: %s\n", truncate(n.SummaryText, 300))
		}
		sb.WriteString("\n")
	}
	return sb.String(), false, nil
}

// GetMeetingTool retrieves full details of a specific meeting note.
type GetMeetingTool struct {
	NoteDir   string
	OutputDir string
}

func (t *GetMeetingTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "get_meeting",
		Description: "Get the full contents of a meeting note by date and time.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"date": map[string]interface{}{"type": "string", "description": "Meeting date (YYYY-MM-DD)"},
				"time": map[string]interface{}{"type": "string", "description": "Meeting time (HH:MM)"},
			},
			"required": []string{"date"},
		},
	}
}

func (t *GetMeetingTool) Execute(_ context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		Date string `json:"date"`
		Time string `json:"time"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}

	meetings, err := meetingdata.DiscoverMeetings(t.OutputDir)
	if err != nil {
		return fmt.Sprintf("Error discovering meetings: %s", err), true, nil
	}

	for _, m := range meetings {
		if m.Date == params.Date && (params.Time == "" || m.Time == params.Time) {
			notePath, err := meetingdata.FindNoteForMeeting(m, t.NoteDir)
			if err != nil {
				return fmt.Sprintf("Meeting found but no note: %s", err), true, nil
			}
			note, err := meetingdata.ParseNote(notePath)
			if err != nil {
				return fmt.Sprintf("Error parsing note: %s", err), true, nil
			}
			data, _ := json.MarshalIndent(note, "", "  ")
			return string(data), false, nil
		}
	}
	return "No meeting found for the given date/time.", false, nil
}

// ListActionItemsTool lists action items across all meetings.
type ListActionItemsTool struct {
	NoteDir   string
	OutputDir string
}

func (t *ListActionItemsTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "list_action_items",
		Description: "List action items from meetings, filterable by status and assignee.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"status":   map[string]interface{}{"type": "string", "description": "Filter: open, done, or all", "enum": []string{"open", "done", "all"}},
				"assignee": map[string]interface{}{"type": "string", "description": "Filter by assignee name"},
				"limit":    map[string]interface{}{"type": "integer", "description": "Max results"},
			},
		},
	}
}

func (t *ListActionItemsTool) Execute(_ context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		Status   string `json:"status"`
		Assignee string `json:"assignee"`
		Limit    int    `json:"limit"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}

	if params.Status == "" {
		params.Status = "all"
	}
	if params.Limit == 0 {
		params.Limit = 50
	}

	items, err := meetingdata.ListAllActionItems(t.NoteDir, t.OutputDir, meetingdata.ActionItemFilters{
		Status:   params.Status,
		Assignee: params.Assignee,
		Limit:    params.Limit,
	})
	if err != nil {
		return fmt.Sprintf("Error listing action items: %s", err), true, nil
	}

	if len(items) == 0 {
		return "No action items found.", false, nil
	}

	var sb strings.Builder
	for _, item := range items {
		check := "[ ]"
		if item.Done {
			check = "[x]"
		}
		assignee := ""
		if item.Assignee != "" {
			assignee = fmt.Sprintf(" (%s)", item.Assignee)
		}
		fmt.Fprintf(&sb, "- %s %s%s [%s]\n", check, item.Text, assignee, item.MeetingDate)
	}
	return sb.String(), false, nil
}

// GetSpeakerProfilesTool returns known speaker profiles.
type GetSpeakerProfilesTool struct {
	SpeakerDB string
}

func (t *GetSpeakerProfilesTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "get_speaker_profiles",
		Description: "List all known speaker profiles from the speaker database.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{},
		},
	}
}

func (t *GetSpeakerProfilesTool) Execute(_ context.Context, _ []byte) (string, bool, error) {
	profiles, err := meetingdata.LoadSpeakerProfiles(t.SpeakerDB)
	if err != nil {
		return fmt.Sprintf("Error loading profiles: %s", err), true, nil
	}

	if len(profiles) == 0 {
		return "No speaker profiles found.", false, nil
	}

	data, _ := json.MarshalIndent(profiles, "", "  ")
	return string(data), false, nil
}

func truncate(s string, maxLen int) string {
	if len(s) <= maxLen {
		return s
	}
	return s[:maxLen] + "..."
}
