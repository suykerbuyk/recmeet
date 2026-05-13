// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"fmt"
	"testing"

	anthropic "github.com/anthropics/anthropic-sdk-go"
	"github.com/syketech/recmeet-tools/testutil"
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
			testutil.BuildAnthropicTextMessage("Hello, world!"),
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
	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{
			testutil.BuildAnthropicToolUseMessage(
				"echo_tool",
				map[string]any{"path": "/tmp/test.txt", "content": "hello"},
				"Let me write a file.",
			),
			testutil.BuildAnthropicTextMessage("Done!"),
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
	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{
			testutil.BuildAnthropicToolUseMessage(
				"nonexistent_tool",
				map[string]any{"query": "test"},
				"",
			),
			testutil.BuildAnthropicTextMessage("I see the tool was not found."),
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
	// Always returns tool_use to exhaust iterations
	infiniteToolUse := func() *anthropic.Message {
		return testutil.BuildAnthropicToolUseMessage("echo_tool", map[string]any{}, "")
	}

	mock := &MockAnthropicClient{
		responses: []*anthropic.Message{infiniteToolUse(), infiniteToolUse(), infiniteToolUse()},
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
