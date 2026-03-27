// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/syketech/recmeet-tools/meetingdata"
)

// PrepWorkflow prepares context for an upcoming meeting.
// It researches participants, reviews past meetings, and fetches agenda content.
func PrepWorkflow(ctx context.Context, cfg AgentConfig, description string, participants []string, agendaURL string, outputPath string, verbose bool, dryRun bool) (string, error) {
	systemPrompt := buildPrepSystemPrompt(description, participants, agendaURL)

	if dryRun {
		return fmt.Sprintf("DRY RUN - System prompt:\n%s\n\nUser message:\n%s",
			systemPrompt, buildPrepUserMessage(description, participants, agendaURL)), nil
	}

	if cfg.AnthropicKey == "" {
		return "", fmt.Errorf("ANTHROPIC_API_KEY not set and not found in config")
	}

	registry := buildPrepRegistry(cfg)
	client := NewSDKClient(cfg.AnthropicKey)
	loop := NewLoop(client, cfg.Model, registry, cfg.MaxIterations, verbose)

	userMsg := buildPrepUserMessage(description, participants, agendaURL)
	result, err := loop.Run(ctx, systemPrompt, userMsg)
	if err != nil {
		return "", fmt.Errorf("agent loop failed: %w", err)
	}

	if outputPath == "" {
		outputPath = filepath.Join(cfg.ContextDir, fmt.Sprintf("prep_%s.md", time.Now().Format("2006-01-02_15-04")))
	}

	if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
		return "", fmt.Errorf("create output dir: %w", err)
	}
	if err := os.WriteFile(outputPath, []byte(result), 0o644); err != nil {
		return "", fmt.Errorf("write output: %w", err)
	}

	return outputPath, nil
}

// FollowUpWorkflow processes meeting notes and drafts follow-up communications.
func FollowUpWorkflow(ctx context.Context, cfg AgentConfig, notePath string, outputDir string, myName string, verbose bool, dryRun bool) (string, error) {
	note, err := meetingdata.ParseNote(notePath)
	if err != nil {
		return "", fmt.Errorf("parse note: %w", err)
	}

	items := meetingdata.ExtractActionItems(note)
	systemPrompt := buildFollowUpSystemPrompt(note, items, myName)
	userMsg := buildFollowUpUserMessage(note, items)

	if dryRun {
		return fmt.Sprintf("DRY RUN - System prompt:\n%s\n\nUser message:\n%s",
			systemPrompt, userMsg), nil
	}

	if cfg.AnthropicKey == "" {
		return "", fmt.Errorf("ANTHROPIC_API_KEY not set and not found in config")
	}

	registry := buildFollowUpRegistry(cfg, outputDir)
	client := NewSDKClient(cfg.AnthropicKey)
	loop := NewLoop(client, cfg.Model, registry, cfg.MaxIterations, verbose)

	result, err := loop.Run(ctx, systemPrompt, userMsg)
	if err != nil {
		return "", fmt.Errorf("agent loop failed: %w", err)
	}

	return result, nil
}

func buildPrepRegistry(cfg AgentConfig) *ToolRegistry {
	reg := NewToolRegistry()
	reg.Register(&SearchMeetingsTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&GetMeetingTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&ListActionItemsTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&GetSpeakerProfilesTool{SpeakerDB: cfg.SpeakerDB})
	if cfg.BraveAPIKey != "" {
		reg.Register(&WebSearchTool{APIKey: cfg.BraveAPIKey})
	}
	reg.Register(&WebFetchTool{})
	reg.Register(&WriteFileTool{})
	return reg
}

func buildFollowUpRegistry(cfg AgentConfig, outputDir string) *ToolRegistry {
	reg := NewToolRegistry()
	reg.Register(&SearchMeetingsTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&GetMeetingTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&ListActionItemsTool{NoteDir: cfg.NoteDir, OutputDir: cfg.OutputDir})
	reg.Register(&GetSpeakerProfilesTool{SpeakerDB: cfg.SpeakerDB})
	if cfg.BraveAPIKey != "" {
		reg.Register(&WebSearchTool{APIKey: cfg.BraveAPIKey})
	}
	reg.Register(&WebFetchTool{})
	reg.Register(&WriteFileTool{})
	return reg
}

func buildPrepSystemPrompt(description string, participants []string, agendaURL string) string {
	var sb strings.Builder
	sb.WriteString(`You are a meeting preparation assistant for recmeet, a meeting recorder system.
Your job is to prepare comprehensive context for an upcoming meeting.

You should:
1. Search past meetings involving the participants to find relevant history
2. Look up any open action items assigned to the participants
3. If an agenda URL is provided, fetch and analyze it
4. If web search is available, research the participants and relevant topics
5. Compile everything into a well-organized briefing document

The briefing should include:
- Participant background and roles (from past meetings and web research)
- Relevant past meeting summaries and decisions
- Open action items for each participant
- Agenda analysis (if URL provided)
- Key topics and talking points
- Potential questions to ask

Format the output as a clean Markdown document.
`)

	if description != "" {
		fmt.Fprintf(&sb, "\nMeeting description: %s\n", description)
	}
	if len(participants) > 0 {
		fmt.Fprintf(&sb, "Participants: %s\n", strings.Join(participants, ", "))
	}
	if agendaURL != "" {
		fmt.Fprintf(&sb, "Agenda URL: %s\n", agendaURL)
	}

	return sb.String()
}

func buildPrepUserMessage(description string, participants []string, agendaURL string) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "Please prepare a briefing for this upcoming meeting.\n\n")
	fmt.Fprintf(&sb, "Description: %s\n", description)
	if len(participants) > 0 {
		fmt.Fprintf(&sb, "Participants: %s\n", strings.Join(participants, ", "))
	}
	if agendaURL != "" {
		fmt.Fprintf(&sb, "Agenda URL: %s\n", agendaURL)
	}
	sb.WriteString("\nStart by searching past meetings and action items, then compile the briefing.")
	return sb.String()
}

func buildFollowUpSystemPrompt(note *meetingdata.MeetingNote, items []meetingdata.ActionItem, myName string) string {
	var sb strings.Builder
	sb.WriteString(`You are a follow-up assistant for recmeet, a meeting recorder system.
Your job is to process meeting notes and help draft follow-up communications.

You should:
1. Analyze the meeting summary and action items
2. Classify action items by assignee and urgency
3. Draft follow-up messages for each participant with their action items
4. Suggest any items that need clarification or escalation
5. Use the write_file tool to save draft communications

For each participant who has action items, write a professional but friendly
follow-up message summarizing what was discussed and their specific action items
with deadlines if mentioned.
`)

	if myName != "" {
		fmt.Fprintf(&sb, "\nYou are drafting messages on behalf of: %s\n", myName)
	}

	return sb.String()
}

func buildFollowUpUserMessage(note *meetingdata.MeetingNote, items []meetingdata.ActionItem) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "# Meeting: %s\n", note.Frontmatter.Title)
	fmt.Fprintf(&sb, "Date: %s\n", note.Frontmatter.Date)
	fmt.Fprintf(&sb, "Participants: %s\n\n", strings.Join(note.Frontmatter.Participants, ", "))

	if note.SummaryText != "" {
		fmt.Fprintf(&sb, "## Summary\n%s\n\n", note.SummaryText)
	}

	if len(items) > 0 {
		sb.WriteString("## Action Items\n")
		for _, item := range items {
			check := "[ ]"
			if item.Done {
				check = "[x]"
			}
			assignee := ""
			if item.Assignee != "" {
				assignee = fmt.Sprintf(" (%s)", item.Assignee)
			}
			fmt.Fprintf(&sb, "- %s %s%s\n", check, item.Text, assignee)
		}
		sb.WriteString("\n")
	}

	sb.WriteString("Please analyze the meeting and draft follow-up communications for each participant with action items.")
	return sb.String()
}
