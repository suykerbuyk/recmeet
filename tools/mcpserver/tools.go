// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package mcpserver

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"
	"github.com/syketech/recmeet-tools/meetingdata"
)

// textResult returns a successful CallToolResult with a single text content block.
func textResult(text string) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{mcp.NewTextContent(text)},
	}
}

// errorResult returns a CallToolResult marked as an error.
func errorResult(msg string) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{mcp.NewTextContent(msg)},
		IsError: true,
	}
}

// jsonText marshals v to indented JSON and returns it as a string.
func jsonText(v any) (string, error) {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return "", err
	}
	return string(data), nil
}

func searchMeetingsHandler(cfg meetingdata.Config) server.ToolHandlerFunc {
	return func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		query := req.GetString("query", "")
		filters := meetingdata.SearchFilters{
			DateFrom:     req.GetString("date_from", ""),
			DateTo:       req.GetString("date_to", ""),
			Participants: req.GetStringSlice("participants", nil),
			Limit:        req.GetInt("limit", 20),
		}

		notes, err := meetingdata.SearchNotes(cfg.NoteDir, cfg.OutputDir, query, filters)
		if err != nil {
			return errorResult(fmt.Sprintf("search failed: %v", err)), nil
		}

		// Build a summary for each match (omit full transcript for brevity).
		type noteSummary struct {
			Path         string   `json:"path"`
			Title        string   `json:"title"`
			Date         string   `json:"date"`
			Time         string   `json:"time"`
			Participants []string `json:"participants,omitempty"`
			Summary      string   `json:"summary,omitempty"`
			ActionCount  int      `json:"action_items"`
		}

		results := make([]noteSummary, 0, len(notes))
		for _, n := range notes {
			results = append(results, noteSummary{
				Path:         n.Path,
				Title:        n.Frontmatter.Title,
				Date:         n.Frontmatter.Date,
				Time:         n.Frontmatter.Time,
				Participants: n.Frontmatter.Participants,
				Summary:      n.SummaryText,
				ActionCount:  len(n.ActionItems),
			})
		}

		text, err := jsonText(results)
		if err != nil {
			return errorResult(fmt.Sprintf("marshal results: %v", err)), nil
		}
		return textResult(text), nil
	}
}

func getMeetingHandler(cfg meetingdata.Config) server.ToolHandlerFunc {
	return func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		dirName, err := req.RequireString("meeting_dir")
		if err != nil {
			return errorResult(err.Error()), nil
		}

		meetings, err := meetingdata.DiscoverMeetings(cfg.OutputDir)
		if err != nil {
			return errorResult(fmt.Sprintf("discover meetings: %v", err)), nil
		}

		var info *meetingdata.MeetingInfo
		for i := range meetings {
			if meetings[i].DirName == dirName {
				info = &meetings[i]
				break
			}
		}
		if info == nil {
			return errorResult(fmt.Sprintf("meeting %q not found in %s", dirName, cfg.OutputDir)), nil
		}

		type meetingDetail struct {
			DirName  string                     `json:"dir_name"`
			DirPath  string                     `json:"dir_path"`
			Date     string                     `json:"date"`
			Time     string                     `json:"time"`
			HasAudio bool                       `json:"has_audio"`
			Note     *meetingdata.MeetingNote    `json:"note,omitempty"`
			Speakers []meetingdata.MeetingSpeaker `json:"speakers,omitempty"`
		}

		detail := meetingDetail{
			DirName:  info.DirName,
			DirPath:  info.DirPath,
			Date:     info.Date,
			Time:     info.Time,
			HasAudio: info.HasAudio,
		}

		notePath, err := meetingdata.FindNoteForMeeting(*info, cfg.NoteDir)
		if err == nil {
			note, parseErr := meetingdata.ParseNote(notePath)
			if parseErr == nil {
				detail.Note = note
			}
		}

		speakers, _ := meetingdata.LoadMeetingSpeakers(info.DirPath)
		detail.Speakers = speakers

		text, err := jsonText(detail)
		if err != nil {
			return errorResult(fmt.Sprintf("marshal meeting: %v", err)), nil
		}
		return textResult(text), nil
	}
}

func listActionItemsHandler(cfg meetingdata.Config) server.ToolHandlerFunc {
	return func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		filters := meetingdata.ActionItemFilters{
			Status:   req.GetString("status", "all"),
			Assignee: req.GetString("assignee", ""),
			Limit:    req.GetInt("limit", 50),
		}

		items, err := meetingdata.ListAllActionItems(cfg.NoteDir, cfg.OutputDir, filters)
		if err != nil {
			return errorResult(fmt.Sprintf("list action items: %v", err)), nil
		}

		text, err := jsonText(items)
		if err != nil {
			return errorResult(fmt.Sprintf("marshal items: %v", err)), nil
		}
		return textResult(text), nil
	}
}

func getSpeakerProfilesHandler(cfg meetingdata.Config) server.ToolHandlerFunc {
	return func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		dbDir := cfg.SpeakerDB
		if dbDir == "" {
			dbDir = meetingdata.DefaultSpeakerDB()
		}

		profiles, err := meetingdata.LoadSpeakerProfiles(dbDir)
		if err != nil {
			return errorResult(fmt.Sprintf("load speakers: %v", err)), nil
		}

		if profiles == nil {
			profiles = []meetingdata.SpeakerProfile{}
		}

		text, err := jsonText(profiles)
		if err != nil {
			return errorResult(fmt.Sprintf("marshal profiles: %v", err)), nil
		}
		return textResult(text), nil
	}
}

func writeContextFileHandler(cfg meetingdata.Config) server.ToolHandlerFunc {
	return func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		filename, err := req.RequireString("filename")
		if err != nil {
			return errorResult(err.Error()), nil
		}
		content, err := req.RequireString("content")
		if err != nil {
			return errorResult(err.Error()), nil
		}

		// Sanitize filename: strip path separators to prevent directory traversal.
		filename = filepath.Base(filename)
		if filename == "." || filename == "/" {
			return errorResult("invalid filename"), nil
		}

		// Reject filenames starting with dot (hidden files).
		if strings.HasPrefix(filename, ".") {
			return errorResult("filename must not start with '.'"), nil
		}

		contextDir := contextStagingDir()
		if err := os.MkdirAll(contextDir, 0o755); err != nil {
			return errorResult(fmt.Sprintf("create context dir: %v", err)), nil
		}

		fullPath := filepath.Join(contextDir, filename)
		if err := os.WriteFile(fullPath, []byte(content), 0o644); err != nil {
			return errorResult(fmt.Sprintf("write file: %v", err)), nil
		}

		return textResult(fmt.Sprintf("Wrote %d bytes to %s", len(content), fullPath)), nil
	}
}

// contextStagingDir returns the path to the context staging directory.
func contextStagingDir() string {
	if dir := os.Getenv("XDG_DATA_HOME"); dir != "" {
		return filepath.Join(dir, "recmeet", "context")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "recmeet", "context")
}
