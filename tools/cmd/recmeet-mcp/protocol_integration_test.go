// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"context"
	"encoding/json"
	"sort"
	"testing"
	"time"

	mcpclient "github.com/mark3labs/mcp-go/client"
	"github.com/mark3labs/mcp-go/mcp"
)

// TestMCPServer_Initialize drives the MCP `initialize` handshake against
// the as-built binary over stdio. It asserts the server returns a
// capabilities block advertising the `tools` capability — proof that the
// JSON-RPC handshake completes successfully on the real transport.
func TestMCPServer_Initialize(t *testing.T) {
	c, err := mcpclient.NewStdioMCPClient(recmeetMcpBin, []string{})
	if err != nil {
		t.Fatalf("NewStdioMCPClient: %v", err)
	}
	defer c.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	initReq := mcp.InitializeRequest{}
	initReq.Params.ProtocolVersion = mcp.LATEST_PROTOCOL_VERSION
	initReq.Params.ClientInfo = mcp.Implementation{Name: "test-init", Version: "0.0.0"}

	result, err := c.Initialize(ctx, initReq)
	if err != nil {
		t.Fatalf("Initialize: %v", err)
	}

	if result.Capabilities.Tools == nil {
		t.Errorf("server did not advertise `tools` capability; capabilities=%+v", result.Capabilities)
	}
	if result.ServerInfo.Name == "" {
		t.Errorf("ServerInfo.Name empty")
	}
}

// TestMCPServer_ListTools verifies the in-process handler graph
// registers exactly the 5 expected tools, each with a non-empty
// description and a valid JSON Schema (`type: "object"`).
func TestMCPServer_ListTools(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	res, err := c.ListTools(context.Background(), mcp.ListToolsRequest{})
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}
	if len(res.Tools) != 5 {
		t.Fatalf("expected 5 tools, got %d", len(res.Tools))
	}

	got := make([]string, 0, len(res.Tools))
	for _, tool := range res.Tools {
		got = append(got, tool.Name)
		if tool.Description == "" {
			t.Errorf("tool %s: empty description", tool.Name)
		}
		// Round-trip the schema through JSON to validate it's a
		// well-formed JSON Schema object with type=object.
		raw, err := json.Marshal(tool.InputSchema)
		if err != nil {
			t.Errorf("tool %s: marshal InputSchema: %v", tool.Name, err)
			continue
		}
		var schema map[string]any
		if err := json.Unmarshal(raw, &schema); err != nil {
			t.Errorf("tool %s: unmarshal InputSchema: %v", tool.Name, err)
			continue
		}
		if schema["type"] != "object" {
			t.Errorf("tool %s: InputSchema.type = %v, want \"object\"", tool.Name, schema["type"])
		}
	}

	sort.Strings(got)
	want := expectedToolNames()
	sort.Strings(want)
	for i, name := range want {
		if got[i] != name {
			t.Errorf("tool[%d]: got %s, want %s (full sorted list: %v)", i, got[i], name, got)
		}
	}
}
