// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package mcpserver

import (
	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"
	"github.com/syketech/recmeet-tools/meetingdata"
)

// NewServer creates an MCP server with all recmeet tools registered.
// The returned server is ready to be wrapped in a StdioServer for transport.
func NewServer(cfg meetingdata.Config) *server.MCPServer {
	s := server.NewMCPServer(
		"recmeet",
		"0.1.0",
		server.WithToolCapabilities(true),
	)

	registerTools(s, cfg)
	return s
}

// registerTools adds all recmeet MCP tools to the server.
func registerTools(s *server.MCPServer, cfg meetingdata.Config) {
	s.AddTool(
		mcp.NewTool("search_meetings",
			mcp.WithDescription("Search meeting notes by keyword, date range, and participants"),
			mcp.WithReadOnlyHintAnnotation(true),
			mcp.WithDestructiveHintAnnotation(false),
			mcp.WithString("query",
				mcp.Description("Search keyword to match against title, summary, tags, participants"),
			),
			mcp.WithString("date_from",
				mcp.Description("Start date filter (YYYY-MM-DD)"),
			),
			mcp.WithString("date_to",
				mcp.Description("End date filter (YYYY-MM-DD)"),
			),
			mcp.WithArray("participants",
				mcp.Description("Filter to meetings with any of these participants"),
				mcp.WithStringItems(),
			),
			mcp.WithNumber("limit",
				mcp.Description("Maximum number of results to return"),
			),
		),
		searchMeetingsHandler(cfg),
	)

	s.AddTool(
		mcp.NewTool("get_meeting",
			mcp.WithDescription("Get full details for a specific meeting by directory name (e.g. 2026-03-15_14-30)"),
			mcp.WithReadOnlyHintAnnotation(true),
			mcp.WithDestructiveHintAnnotation(false),
			mcp.WithString("meeting_dir",
				mcp.Required(),
				mcp.Description("Meeting directory name, e.g. 2026-03-15_14-30"),
			),
		),
		getMeetingHandler(cfg),
	)

	s.AddTool(
		mcp.NewTool("list_action_items",
			mcp.WithDescription("List action items from all meeting notes, optionally filtered by status or assignee"),
			mcp.WithReadOnlyHintAnnotation(true),
			mcp.WithDestructiveHintAnnotation(false),
			mcp.WithString("status",
				mcp.Description("Filter by status: open, done, or all"),
				mcp.Enum("open", "done", "all"),
			),
			mcp.WithString("assignee",
				mcp.Description("Filter to action items assigned to this person"),
			),
			mcp.WithNumber("limit",
				mcp.Description("Maximum number of results to return"),
			),
		),
		listActionItemsHandler(cfg),
	)

	s.AddTool(
		mcp.NewTool("get_speaker_profiles",
			mcp.WithDescription("List all enrolled speaker profiles from the speaker identification database"),
			mcp.WithReadOnlyHintAnnotation(true),
			mcp.WithDestructiveHintAnnotation(false),
		),
		getSpeakerProfilesHandler(cfg),
	)

	s.AddTool(
		mcp.NewTool("write_context_file",
			mcp.WithDescription("Write a context file to the recmeet staging directory for use in future meetings"),
			mcp.WithReadOnlyHintAnnotation(false),
			mcp.WithDestructiveHintAnnotation(false),
			mcp.WithString("filename",
				mcp.Required(),
				mcp.Description("Filename for the context file (e.g. project-status.md)"),
			),
			mcp.WithString("content",
				mcp.Required(),
				mcp.Description("Content to write to the file"),
			),
		),
		writeContextFileHandler(cfg),
	)
}
