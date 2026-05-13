// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/mark3labs/mcp-go/client/transport"
	"github.com/mark3labs/mcp-go/mcp"
)

// TestTeeStdioTransport_RoundTrip drives the transport against a
// shell-based echo server that loops back JSON-RPC requests as
// responses with a fixed shape. This exercises Start, SendRequest, the
// stdout tee, and Close in a single hermetic test that doesn't need
// the recmeet-mcp binary.
//
// The echo script reads one line per request and emits one line per
// response. The response body is a synthetic
// {"jsonrpc":"2.0","id":<id>,"result":{"echo":"ack"}} — enough for
// transport.JSONRPCResponse to parse.
func TestTeeStdioTransport_RoundTrip(t *testing.T) {
	// Use `sh` with awk-like loop: read JSON line, extract id field
	// crudely with sed, emit response.
	script := `while IFS= read -r line; do
  id=$(echo "$line" | sed -nE 's/.*"id"[ ]*:[ ]*([0-9]+).*/\1/p')
  if [ -z "$id" ]; then id=0; fi
  printf '{"jsonrpc":"2.0","id":%s,"result":{"echo":"ack"}}\n' "$id"
done
`
	cmd := exec.Command("sh", "-c", script)
	var sink bytes.Buffer
	cmd.Stderr = io.Discard

	tt, err := NewTeeStdioTransport(cmd, &sink)
	if err != nil {
		t.Fatalf("NewTeeStdioTransport: %v", err)
	}

	if err := tt.Start(context.Background()); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer tt.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	req := transport.JSONRPCRequest{
		JSONRPC: "2.0",
		ID:      mcp.NewRequestId(int64(1)),
		Method:  "ping",
	}
	resp, err := tt.SendRequest(ctx, req)
	if err != nil {
		t.Fatalf("SendRequest: %v", err)
	}
	if resp == nil {
		t.Fatal("nil response")
	}
	var result map[string]string
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		t.Fatalf("unmarshal result: %v", err)
	}
	if result["echo"] != "ack" {
		t.Errorf("result: %v", result)
	}

	if !strings.Contains(sink.String(), `"echo":"ack"`) {
		t.Errorf("tee sink did not capture the response line:\n%s", sink.String())
	}

	if tt.GetSessionId() != "" {
		t.Errorf("GetSessionId: expected empty, got %q", tt.GetSessionId())
	}
}

func TestTeeStdioTransport_NilCmd(t *testing.T) {
	if _, err := NewTeeStdioTransport(nil, io.Discard); err == nil {
		t.Error("expected error on nil cmd")
	}
}

func TestTeeStdioTransport_NotStartedRejects(t *testing.T) {
	cmd := exec.Command("true")
	tt, err := NewTeeStdioTransport(cmd, nil)
	if err != nil {
		t.Fatalf("NewTeeStdioTransport: %v", err)
	}
	_, err = tt.SendRequest(context.Background(), transport.JSONRPCRequest{
		JSONRPC: "2.0",
		ID:      mcp.NewRequestId(int64(1)),
		Method:  "ping",
	})
	if err == nil {
		t.Error("expected error from SendRequest before Start")
	}
	if err := tt.SendNotification(context.Background(), mcp.JSONRPCNotification{}); err == nil {
		t.Error("expected error from SendNotification before Start")
	}
}

func TestTeeStdioTransport_SetNotificationHandler(t *testing.T) {
	cmd := exec.Command("true")
	tt, _ := NewTeeStdioTransport(cmd, nil)
	tt.SetNotificationHandler(func(_ mcp.JSONRPCNotification) {})
	tt.SetNotificationHandler(nil)
}
