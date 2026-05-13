// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	anthropic "github.com/anthropics/anthropic-sdk-go"
)

// MockAnthropic is an httptest-backed Anthropic Messages API stub.
//
// Tests wire it into the agent by setting the ANTHROPIC_BASE_URL env
// var on the spawned binary (or the in-process SDK client) to the
// server's URL(). Each POST /v1/messages pops the next queued response
// (or returns 500 if the queue is empty) and records the parsed
// request body for assertion.
//
// Lifecycle is managed via t.Cleanup; callers do not need to call
// Close explicitly.
type MockAnthropic struct {
	t      *testing.T
	server *httptest.Server

	mu        sync.Mutex
	responses []queuedResponse
	requests  []RecordedRequest
}

type queuedResponse struct {
	status int
	body   []byte
}

// RecordedRequest is a parsed snapshot of one inbound request to the
// mock Anthropic server. The fields here are the ones useful for
// assertions; the full body is also retained for ad-hoc inspection.
type RecordedRequest struct {
	HTTPMethod    string
	Path          string
	ParsedModel   string
	ParsedSystem  string
	ParsedTools   []string // tool names only
	ParsedMessages []ParsedMessage
	RawBody       []byte
}

// ParsedMessage captures the role + last text segment of one message
// in the inbound request's `messages` array. For assertions we only
// need the most-recent user/assistant text — full content fidelity is
// preserved in RawBody if a test needs it.
type ParsedMessage struct {
	Role string
	Text string
}

// NewMockAnthropic creates an unstarted mock server and registers a
// t.Cleanup to shut it down. Test callers use URL() to get the
// ANTHROPIC_BASE_URL value.
func NewMockAnthropic(t *testing.T) *MockAnthropic {
	t.Helper()
	m := &MockAnthropic{t: t}
	m.server = httptest.NewServer(http.HandlerFunc(m.handle))
	t.Cleanup(m.server.Close)
	return m
}

// URL returns the base URL of the mock server, suitable as
// ANTHROPIC_BASE_URL.
func (m *MockAnthropic) URL() string {
	return m.server.URL
}

// QueueResponse pushes a scripted response onto the FIFO queue. The
// `resp` argument may be:
//
//   - a *anthropic.Message-shaped struct (typically built via
//     BuildTextBlockResponse / BuildToolUseResponse / BuildEndTurnResponse)
//   - a []byte containing the raw response body to emit verbatim
//   - a string treated the same as []byte
//
// In all cases the server emits HTTP 200 with the JSON body.
func (m *MockAnthropic) QueueResponse(resp any) {
	m.t.Helper()
	var body []byte
	switch v := resp.(type) {
	case []byte:
		body = v
	case string:
		body = []byte(v)
	default:
		b, err := json.Marshal(resp)
		if err != nil {
			m.t.Fatalf("MockAnthropic.QueueResponse: marshal: %v", err)
		}
		body = b
	}
	m.mu.Lock()
	m.responses = append(m.responses, queuedResponse{status: 200, body: body})
	m.mu.Unlock()
}

// QueueStatus pushes a scripted error response. The body is sent
// verbatim with the given HTTP status code; callers wanting a JSON
// error body should hand-craft it (e.g. `{"type":"error","error":{...}}`).
func (m *MockAnthropic) QueueStatus(code int, body string) {
	m.mu.Lock()
	m.responses = append(m.responses, queuedResponse{status: code, body: []byte(body)})
	m.mu.Unlock()
}

// Requests returns a snapshot copy of all inbound requests observed
// so far. Safe to call from test code without locking.
func (m *MockAnthropic) Requests() []RecordedRequest {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]RecordedRequest, len(m.requests))
	copy(out, m.requests)
	return out
}

func (m *MockAnthropic) handle(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	_ = r.Body.Close()

	rec := RecordedRequest{
		HTTPMethod: r.Method,
		Path:       r.URL.Path,
		RawBody:    body,
	}

	// Parse the request loosely — we don't care about every field,
	// only the ones useful for assertions.
	if len(body) > 0 {
		var parsed struct {
			Model    string `json:"model"`
			System   any    `json:"system"`
			Messages []struct {
				Role    string `json:"role"`
				Content any    `json:"content"`
			} `json:"messages"`
			Tools []struct {
				Name string `json:"name"`
			} `json:"tools"`
		}
		if err := json.Unmarshal(body, &parsed); err == nil {
			rec.ParsedModel = parsed.Model
			rec.ParsedSystem = flattenSystemField(parsed.System)
			for _, t := range parsed.Tools {
				rec.ParsedTools = append(rec.ParsedTools, t.Name)
			}
			for _, msg := range parsed.Messages {
				rec.ParsedMessages = append(rec.ParsedMessages, ParsedMessage{
					Role: msg.Role,
					Text: flattenContentField(msg.Content),
				})
			}
		}
	}

	m.mu.Lock()
	m.requests = append(m.requests, rec)
	var resp queuedResponse
	var have bool
	if len(m.responses) > 0 {
		resp = m.responses[0]
		m.responses = m.responses[1:]
		have = true
	}
	m.mu.Unlock()

	if !have {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte(`{"type":"error","error":{"type":"mock_empty_queue","message":"MockAnthropic: no scripted response queued"}}`))
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(resp.status)
	_, _ = w.Write(resp.body)
}

// flattenSystemField extracts text from the Anthropic API's "system"
// field, which can be either a plain string or an array of
// {type:"text", text:"..."} blocks.
func flattenSystemField(v any) string {
	switch s := v.(type) {
	case string:
		return s
	case []any:
		var parts []string
		for _, item := range s {
			if obj, ok := item.(map[string]any); ok {
				if t, _ := obj["text"].(string); t != "" {
					parts = append(parts, t)
				}
			}
		}
		return strings.Join(parts, "\n")
	}
	return ""
}

// flattenContentField extracts text from a message's "content" field,
// which can be a plain string or an array of content blocks. Tool-use
// and tool-result blocks are summarized rather than dropped so tests
// can grep for tool names.
func flattenContentField(v any) string {
	switch s := v.(type) {
	case string:
		return s
	case []any:
		var parts []string
		for _, item := range s {
			obj, ok := item.(map[string]any)
			if !ok {
				continue
			}
			typ, _ := obj["type"].(string)
			switch typ {
			case "text":
				if t, _ := obj["text"].(string); t != "" {
					parts = append(parts, t)
				}
			case "tool_use":
				name, _ := obj["name"].(string)
				parts = append(parts, fmt.Sprintf("[tool_use:%s]", name))
			case "tool_result":
				id, _ := obj["tool_use_id"].(string)
				parts = append(parts, fmt.Sprintf("[tool_result:%s]", id))
			}
		}
		return strings.Join(parts, "\n")
	}
	return ""
}

// ---------------------------------------------------------------------
// Response constructors — shared by in-process tests in `agent/`
// (loop_test.go) and binary-driven tests in `cmd/recmeet-agent/`.
// ---------------------------------------------------------------------

// MessageResponse is the JSON-marshallable shape of an Anthropic
// Messages API response. It is a structural subset of
// anthropic.Message — sufficient for tests since the SDK is lenient
// about unknown fields. Tests that need the SDK's exact struct can
// build *anthropic.Message values directly.
type MessageResponse struct {
	ID         string          `json:"id"`
	Type       string          `json:"type"`
	Role       string          `json:"role"`
	Model      string          `json:"model"`
	StopReason string          `json:"stop_reason"`
	Content    []ContentBlock  `json:"content"`
	Usage      UsageInfo       `json:"usage"`
}

// ContentBlock matches the JSON shape of one entry in an Anthropic
// message's `content` array. Field omission via `omitempty` keeps the
// JSON tight for whichever block type the test is scripting.
type ContentBlock struct {
	Type  string          `json:"type"`
	Text  string          `json:"text,omitempty"`
	ID    string          `json:"id,omitempty"`
	Name  string          `json:"name,omitempty"`
	Input json.RawMessage `json:"input,omitempty"`
}

// UsageInfo is the minimal `usage` object the SDK expects in
// every response.
type UsageInfo struct {
	InputTokens  int `json:"input_tokens"`
	OutputTokens int `json:"output_tokens"`
}

func defaultUsage() UsageInfo { return UsageInfo{InputTokens: 1, OutputTokens: 1} }

// BuildTextBlockResponse builds a stop_reason="end_turn" response with
// a single text block. Use this for the simplest "agent ran, model
// returned text, done" path.
func BuildTextBlockResponse(text string) MessageResponse {
	return MessageResponse{
		ID:         "msg_test_text",
		Type:       "message",
		Role:       "assistant",
		Model:      "claude-test",
		StopReason: "end_turn",
		Content:    []ContentBlock{{Type: "text", Text: text}},
		Usage:      defaultUsage(),
	}
}

// BuildEndTurnResponse is an alias for BuildTextBlockResponse for
// readability at call sites that emphasize the stop-reason semantics.
func BuildEndTurnResponse(text string) MessageResponse {
	return BuildTextBlockResponse(text)
}

// BuildAnthropicTextMessage builds an *anthropic.Message with a single
// text block and stop_reason=end_turn — the in-process equivalent of
// BuildEndTurnResponse, used by tests that drive the AnthropicClient
// interface directly (e.g. agent/loop_test.go's MockAnthropicClient).
func BuildAnthropicTextMessage(text string) *anthropic.Message {
	return &anthropic.Message{
		StopReason: anthropic.StopReasonEndTurn,
		Content: []anthropic.ContentBlockUnion{
			{Type: "text", Text: text},
		},
	}
}

// BuildAnthropicToolUseMessage builds an *anthropic.Message with a
// stop_reason=tool_use and one tool_use block (plus an optional text
// preamble). `input` is JSON-marshalled into the block's Input field.
// Used by tests driving the in-process AnthropicClient interface.
func BuildAnthropicToolUseMessage(toolName string, input map[string]any, preamble string) *anthropic.Message {
	raw, _ := json.Marshal(input)
	if raw == nil {
		raw = json.RawMessage("{}")
	}
	blocks := []anthropic.ContentBlockUnion{}
	if preamble != "" {
		blocks = append(blocks, anthropic.ContentBlockUnion{Type: "text", Text: preamble})
	}
	blocks = append(blocks, anthropic.ContentBlockUnion{
		Type:  "tool_use",
		ID:    "tool_" + toolName,
		Name:  toolName,
		Input: raw,
	})
	return &anthropic.Message{
		StopReason: anthropic.StopReasonToolUse,
		Content:    blocks,
	}
}

// BuildToolUseResponse builds a stop_reason="tool_use" response that
// invokes a single named tool with the given input. The optional
// `preamble` text appears as a separate text block before the
// tool_use block — pass "" to omit.
func BuildToolUseResponse(toolName string, input map[string]any, preamble string) MessageResponse {
	raw, _ := json.Marshal(input)
	if raw == nil {
		raw = json.RawMessage("{}")
	}
	blocks := []ContentBlock{}
	if preamble != "" {
		blocks = append(blocks, ContentBlock{Type: "text", Text: preamble})
	}
	blocks = append(blocks, ContentBlock{
		Type:  "tool_use",
		ID:    "tool_" + toolName,
		Name:  toolName,
		Input: raw,
	})
	return MessageResponse{
		ID:         "msg_test_tool",
		Type:       "message",
		Role:       "assistant",
		Model:      "claude-test",
		StopReason: "tool_use",
		Content:    blocks,
		Usage:      defaultUsage(),
	}
}
