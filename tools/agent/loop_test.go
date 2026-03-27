// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"testing"

	anthropic "github.com/anthropics/anthropic-sdk-go"
)

// MockAnthropicClient returns scripted responses.
type MockAnthropicClient struct {
	responses []*anthropic.Message
	calls     int
}

func (m *MockAnthropicClient) CreateMessage(_ context.Context, _ anthropic.MessageNewParams) (*anthropic.Message, error) {
	if m.calls >= len(m.responses) {
		return nil, fmt.Errorf("no more scripted responses (call %d)", m.calls)
	}
	resp := m.responses[m.calls]
	m.calls++
	return resp, nil
}

func TestLoop_SimpleResponse(t *testing.T) {
	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{
			{
				StopReason: anthropic.StopReasonEndTurn,
				Content: []anthropic.ContentBlockUnion{
					{Type: "text", Text: "Hello, world!"},
				},
			},
		},
	}

	reg := NewToolRegistry()
	loop := NewLoop(mock, "test-model", reg, 5, false)

	result, err := loop.Run(context.Background(), "system", "user")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != "Hello, world!" {
		t.Errorf("expected 'Hello, world!', got %q", result)
	}
	if mock.calls != 1 {
		t.Errorf("expected 1 API call, got %d", mock.calls)
	}
}

func TestLoop_ToolUse(t *testing.T) {
	toolInput, _ := json.Marshal(map[string]string{"path": "/tmp/test.txt", "content": "hello"})

	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{
			{
				StopReason: anthropic.StopReasonToolUse,
				Content: []anthropic.ContentBlockUnion{
					{Type: "text", Text: "Let me write a file."},
					{
						Type:  "tool_use",
						ID:    "tool_1",
						Name:  "echo_tool",
						Input: json.RawMessage(toolInput),
					},
				},
			},
			{
				StopReason: anthropic.StopReasonEndTurn,
				Content: []anthropic.ContentBlockUnion{
					{Type: "text", Text: "Done!"},
				},
			},
		},
	}

	reg := NewToolRegistry()
	reg.Register(&echoTool{})

	loop := NewLoop(mock, "test-model", reg, 5, false)
	result, err := loop.Run(context.Background(), "", "do something")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != "Done!" {
		t.Errorf("expected 'Done!', got %q", result)
	}
	if mock.calls != 2 {
		t.Errorf("expected 2 API calls, got %d", mock.calls)
	}
}

func TestLoop_UnknownTool(t *testing.T) {
	toolInput, _ := json.Marshal(map[string]string{"query": "test"})

	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{
			{
				StopReason: anthropic.StopReasonToolUse,
				Content: []anthropic.ContentBlockUnion{
					{
						Type:  "tool_use",
						ID:    "tool_1",
						Name:  "nonexistent_tool",
						Input: json.RawMessage(toolInput),
					},
				},
			},
			{
				StopReason: anthropic.StopReasonEndTurn,
				Content: []anthropic.ContentBlockUnion{
					{Type: "text", Text: "I see the tool was not found."},
				},
			},
		},
	}

	reg := NewToolRegistry()
	loop := NewLoop(mock, "test-model", reg, 5, false)

	result, err := loop.Run(context.Background(), "", "test")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if result != "I see the tool was not found." {
		t.Errorf("unexpected result: %q", result)
	}
}

func TestLoop_MaxIterations(t *testing.T) {
	toolInput, _ := json.Marshal(map[string]string{})

	// Always returns tool_use to exhaust iterations
	infiniteToolUse := &anthropic.Message{
		StopReason: anthropic.StopReasonToolUse,
		Content: []anthropic.ContentBlockUnion{
			{
				Type:  "tool_use",
				ID:    "tool_1",
				Name:  "echo_tool",
				Input: json.RawMessage(toolInput),
			},
		},
	}

	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{infiniteToolUse, infiniteToolUse, infiniteToolUse},
	}

	reg := NewToolRegistry()
	reg.Register(&echoTool{})

	loop := NewLoop(mock, "test-model", reg, 3, false)
	_, err := loop.Run(context.Background(), "", "test")
	if err == nil {
		t.Error("expected max iterations error")
	}
}

// echoTool is a simple test tool that returns its input as a string.
type echoTool struct{}

func (e *echoTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "echo_tool",
		Description: "Echoes input back",
		InputSchema: map[string]interface{}{
			"type":       "object",
			"properties": map[string]interface{}{},
		},
	}
}

func (e *echoTool) Execute(_ context.Context, inputJSON []byte) (string, bool, error) {
	return string(inputJSON), false, nil
}
