// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestMockAnthropic_QueueAndPop(t *testing.T) {
	mock := NewMockAnthropic(t)
	mock.QueueResponse(BuildTextBlockResponse("Hi there"))

	resp, err := http.Post(mock.URL()+"/v1/messages", "application/json",
		strings.NewReader(`{"model":"claude-test","messages":[{"role":"user","content":"hi"}]}`))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != 200 {
		t.Errorf("expected 200, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	var parsed MessageResponse
	if err := json.Unmarshal(body, &parsed); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if parsed.StopReason != "end_turn" {
		t.Errorf("stop_reason: %q", parsed.StopReason)
	}
	if len(parsed.Content) != 1 || parsed.Content[0].Text != "Hi there" {
		t.Errorf("content: %#v", parsed.Content)
	}
}

func TestMockAnthropic_QueueStatus_401(t *testing.T) {
	mock := NewMockAnthropic(t)
	mock.QueueStatus(401, `{"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}}`)

	resp, err := http.Post(mock.URL()+"/v1/messages", "application/json",
		strings.NewReader(`{"model":"claude-test","messages":[]}`))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != 401 {
		t.Errorf("expected 401, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !bytes.Contains(body, []byte("authentication_error")) {
		t.Errorf("body did not contain scripted error: %s", body)
	}
}

func TestMockAnthropic_EmptyQueueReturns500(t *testing.T) {
	mock := NewMockAnthropic(t)
	resp, err := http.Post(mock.URL()+"/v1/messages", "application/json",
		strings.NewReader(`{}`))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != 500 {
		t.Errorf("expected 500 when queue empty, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !bytes.Contains(body, []byte("mock_empty_queue")) {
		t.Errorf("missing diagnostic body: %s", body)
	}
}

func TestMockAnthropic_RequestsRecorded(t *testing.T) {
	mock := NewMockAnthropic(t)
	mock.QueueResponse(BuildTextBlockResponse("ok"))

	reqBody := `{
  "model": "claude-haiku-4-5",
  "system": "You are a test.",
  "messages": [{"role":"user","content":"hello"}],
  "tools": [{"name":"search_meetings"},{"name":"get_meeting"}]
}`
	resp, err := http.Post(mock.URL()+"/v1/messages", "application/json", strings.NewReader(reqBody))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	_ = resp.Body.Close()

	reqs := mock.Requests()
	if len(reqs) != 1 {
		t.Fatalf("expected 1 request, got %d", len(reqs))
	}
	rr := reqs[0]
	if rr.HTTPMethod != "POST" {
		t.Errorf("method: %s", rr.HTTPMethod)
	}
	if rr.Path != "/v1/messages" {
		t.Errorf("path: %s", rr.Path)
	}
	if rr.ParsedModel != "claude-haiku-4-5" {
		t.Errorf("model: %q", rr.ParsedModel)
	}
	if rr.ParsedSystem != "You are a test." {
		t.Errorf("system: %q", rr.ParsedSystem)
	}
	if len(rr.ParsedTools) != 2 || rr.ParsedTools[0] != "search_meetings" {
		t.Errorf("tools: %#v", rr.ParsedTools)
	}
	if len(rr.ParsedMessages) != 1 || rr.ParsedMessages[0].Role != "user" {
		t.Errorf("messages: %#v", rr.ParsedMessages)
	}
	if rr.ParsedMessages[0].Text != "hello" {
		t.Errorf("user text: %q", rr.ParsedMessages[0].Text)
	}
}

func TestMockAnthropic_BuildToolUseResponse(t *testing.T) {
	r := BuildToolUseResponse("search_meetings", map[string]any{"query": "Q2"}, "Searching now")
	if r.StopReason != "tool_use" {
		t.Errorf("stop_reason: %q", r.StopReason)
	}
	if len(r.Content) != 2 {
		t.Fatalf("expected 2 content blocks, got %d", len(r.Content))
	}
	if r.Content[0].Type != "text" || r.Content[0].Text != "Searching now" {
		t.Errorf("preamble block: %#v", r.Content[0])
	}
	if r.Content[1].Type != "tool_use" || r.Content[1].Name != "search_meetings" {
		t.Errorf("tool_use block: %#v", r.Content[1])
	}
	if !bytes.Contains([]byte(r.Content[1].Input), []byte(`"Q2"`)) {
		t.Errorf("tool input missing query: %s", r.Content[1].Input)
	}
}

func TestMockAnthropic_BuildAnthropicHelpers(t *testing.T) {
	text := BuildAnthropicTextMessage("done")
	if text.StopReason != "end_turn" {
		t.Errorf("StopReason: %v", text.StopReason)
	}
	if len(text.Content) != 1 || text.Content[0].Text != "done" {
		t.Errorf("content: %#v", text.Content)
	}

	tu := BuildAnthropicToolUseMessage("foo", map[string]any{"k": "v"}, "")
	if tu.StopReason != "tool_use" {
		t.Errorf("StopReason: %v", tu.StopReason)
	}
	if len(tu.Content) != 1 || tu.Content[0].Name != "foo" {
		t.Errorf("content: %#v", tu.Content)
	}
}
