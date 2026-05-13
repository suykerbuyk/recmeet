// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/syketech/recmeet-tools/testutil"
)

// TestMCPServer_UnknownTool sends a tools/call for a tool that does not
// exist. The MCP protocol's tools/call dispatcher should surface this as
// either a transport-level error or a CallToolResult with IsError=true.
// In either case, the server MUST remain alive afterward; we verify
// liveness by issuing a follow-up ListTools that succeeds.
func TestMCPServer_UnknownTool(t *testing.T) {
	cfg, _ := buildSearchableFixture(t)
	c := newInProcessClient(t, cfg)

	req := mcp.CallToolRequest{}
	req.Params.Name = "definitely_not_a_real_tool"
	req.Params.Arguments = map[string]any{}

	res, err := c.CallTool(context.Background(), req)
	// Accept either an MCP-level error (err != nil) or a tool-level
	// IsError. The contract is "calling an unknown tool does not
	// crash or kill the server"; either signal satisfies it.
	if err == nil {
		if res == nil {
			t.Fatalf("unknown_tool: nil result and nil err — server failed to surface error")
		}
		if !res.IsError {
			t.Errorf("unknown_tool: expected IsError=true, got success")
		}
	}

	// Liveness check: server still answers ListTools.
	if _, err := c.ListTools(context.Background(), mcp.ListToolsRequest{}); err != nil {
		t.Fatalf("server died after unknown_tool call: %v", err)
	}
}

// TestMCPServer_MalformedJSON spawns the binary, writes garbage bytes
// to its stdin, and verifies the process does not crash via signal
// (SIGSEGV/SIGABRT). Exit code may be 0 (graceful EOF) or nonzero
// (decoder error); both are acceptable. What is NOT acceptable: a
// signal-terminated process.
func TestMCPServer_MalformedJSON(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, recmeetMcpBin)
	cmd.Stdin = strings.NewReader("not valid json\n\nstill not json\n")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	// Discard stdout — we don't care about the JSON-RPC stream
	// here, only that the process doesn't crash.
	cmd.Stdout = io.Discard

	if err := cmd.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// Give the server a moment to process the garbage, then EOF.
	time.Sleep(500 * time.Millisecond)

	// Stdin was a strings.Reader (already EOF after read); just wait.
	err := cmd.Wait()

	// Exit code 0 is fine (graceful EOF). Nonzero is fine. Killed by
	// signal is NOT fine.
	if cmd.ProcessState == nil {
		t.Fatalf("ProcessState nil after wait")
	}
	if !cmd.ProcessState.Exited() {
		t.Fatalf("process was killed by signal (not a clean exit): %v\nstderr:\n%s",
			cmd.ProcessState, stderr.String())
	}
	// `err` may be non-nil for a nonzero exit; that's expected.
	_ = err
}

// TestMCPServer_MissingConfig spawns the binary with RECMEET_CONFIG
// pointed at a non-existent path. Phase 0.2 guarantees this is a
// fast-fail: nonzero exit + stderr naming the bad path.
func TestMCPServer_MissingConfig(t *testing.T) {
	missingPath := "/nonexistent/recmeet-integration-test/config.yaml"
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, recmeetMcpBin)
	cmd.Env = append(os.Environ(), "RECMEET_CONFIG="+missingPath)
	cmd.Stdin = strings.NewReader("") // EOF immediately
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	cmd.Stdout = io.Discard

	err := cmd.Run()
	if err == nil {
		t.Fatalf("expected nonzero exit, got success; stderr:\n%s", stderr.String())
	}
	if cmd.ProcessState == nil || !cmd.ProcessState.Exited() {
		t.Fatalf("process was killed by signal: %v\nstderr:\n%s",
			cmd.ProcessState, stderr.String())
	}
	if code := cmd.ProcessState.ExitCode(); code == 0 {
		t.Errorf("expected nonzero exit, got %d", code)
	}

	testutil.AssertActionableStderr(t, stderr.String(), testutil.ExpectMissingConfig, missingPath)
}
