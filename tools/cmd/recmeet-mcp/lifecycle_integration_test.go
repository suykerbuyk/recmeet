// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math/rand"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	mcpclient "github.com/mark3labs/mcp-go/client"
	"github.com/mark3labs/mcp-go/mcp"
	"github.com/syketech/recmeet-tools/testutil"
)

// runGracefulShutdown drives a minimal initialize + tools/list round
// trip against the as-built binary over stdio without going through the
// mcp-go client library (the library owns the subprocess lifecycle and
// doesn't expose the signal/EOF moment). Frames are written and read
// inline, then the caller's `terminate` func applies the chosen
// shutdown signal.
//
// Asserts the process exits within 2 seconds of the signal.
type shutdownTerminator func(t *testing.T, cmd *exec.Cmd, stdin io.WriteCloser)

func runGracefulShutdown(t *testing.T, name string, terminate shutdownTerminator) {
	t.Helper()

	cmd := exec.Command(recmeetMcpBin)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatalf("StdinPipe: %v", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("StdoutPipe: %v", err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	if err := cmd.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	reader := bufio.NewReader(stdout)

	// Send initialize.
	initReq := map[string]any{
		"jsonrpc": "2.0",
		"id":      1,
		"method":  "initialize",
		"params": map[string]any{
			"protocolVersion": mcp.LATEST_PROTOCOL_VERSION,
			"clientInfo":      map[string]any{"name": "shutdown-test", "version": "0.0.0"},
			"capabilities":    map[string]any{},
		},
	}
	if err := writeJSONLine(stdin, initReq); err != nil {
		t.Fatalf("write initialize: %v", err)
	}
	if _, err := readJSONLine(reader, 3*time.Second); err != nil {
		t.Fatalf("read initialize response: %v\nstderr:\n%s", err, stderr.String())
	}

	// Send initialized notification (required for some servers).
	noteFrame := map[string]any{
		"jsonrpc": "2.0",
		"method":  "notifications/initialized",
		"params":  map[string]any{},
	}
	if err := writeJSONLine(stdin, noteFrame); err != nil {
		t.Fatalf("write notifications/initialized: %v", err)
	}

	// Send tools/list.
	listReq := map[string]any{
		"jsonrpc": "2.0",
		"id":      2,
		"method":  "tools/list",
		"params":  map[string]any{},
	}
	if err := writeJSONLine(stdin, listReq); err != nil {
		t.Fatalf("write tools/list: %v", err)
	}
	if _, err := readJSONLine(reader, 3*time.Second); err != nil {
		t.Fatalf("read tools/list response: %v\nstderr:\n%s", err, stderr.String())
	}

	// Apply the variant-specific termination.
	terminate(t, cmd, stdin)

	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()

	select {
	case <-done:
		// Process exited within budget — pass. We accept any exit
		// code: EOF results in 0, signals may map to a nonzero code
		// depending on whether the signal context handler ran. The
		// contract under test is "does not hang."
	case <-time.After(2 * time.Second):
		_ = cmd.Process.Kill()
		<-done
		t.Fatalf("variant=%s: process did not exit within 2s after termination\nstderr:\n%s",
			name, stderr.String())
	}
}

// writeJSONLine marshals v and writes "<json>\n" to w.
func writeJSONLine(w io.Writer, v any) error {
	body, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}
	body = append(body, '\n')
	_, err = w.Write(body)
	return err
}

// readJSONLine reads one newline-terminated line from r with a timeout.
// Returns the line bytes (without trailing newline) or an error.
func readJSONLine(r *bufio.Reader, timeout time.Duration) ([]byte, error) {
	type result struct {
		line []byte
		err  error
	}
	ch := make(chan result, 1)
	go func() {
		line, err := r.ReadBytes('\n')
		ch <- result{line: line, err: err}
	}()
	select {
	case res := <-ch:
		return bytes.TrimRight(res.line, "\r\n"), res.err
	case <-time.After(timeout):
		return nil, fmt.Errorf("readJSONLine: timeout after %v", timeout)
	}
}

// TestMCPServer_GracefulShutdown_EOF closes stdin after one round trip;
// the server should observe EOF and exit cleanly within 2 seconds.
func TestMCPServer_GracefulShutdown_EOF(t *testing.T) {
	runGracefulShutdown(t, "eof", func(_ *testing.T, _ *exec.Cmd, stdin io.WriteCloser) {
		_ = stdin.Close()
	})
}

// TestMCPServer_GracefulShutdown_SIGTERM sends SIGTERM after one round
// trip.
func TestMCPServer_GracefulShutdown_SIGTERM(t *testing.T) {
	runGracefulShutdown(t, "sigterm", func(t *testing.T, cmd *exec.Cmd, _ io.WriteCloser) {
		if err := cmd.Process.Signal(syscall.SIGTERM); err != nil {
			t.Fatalf("signal SIGTERM: %v", err)
		}
	})
}

// TestMCPServer_GracefulShutdown_SIGINT sends SIGINT (ctrl-C) after one
// round trip.
func TestMCPServer_GracefulShutdown_SIGINT(t *testing.T) {
	runGracefulShutdown(t, "sigint", func(t *testing.T, cmd *exec.Cmd, _ io.WriteCloser) {
		if err := cmd.Process.Signal(syscall.SIGINT); err != nil {
			t.Fatalf("signal SIGINT: %v", err)
		}
	})
}

// TestMCPServer_StdoutHygiene drives 50 randomized MCP operations over
// stdio with the subprocess's stdout teed into a buffer. After the run,
// every `\n`-terminated line in the teed buffer must parse as a valid
// JSON-RPC frame (object, jsonrpc=2.0). Any deviation indicates a stray
// log/print escaped onto fd 1.
//
// The 50-op script is randomized; the seed is logged at test start
// (and surfaced in failure output via t.Logf) so a failing run can be
// reproduced bit-identically with `RECMEET_HYGIENE_SEED=<n>`.
func TestMCPServer_StdoutHygiene(t *testing.T) {
	seed := time.Now().UnixNano()
	if s := os.Getenv("RECMEET_HYGIENE_SEED"); s != "" {
		if parsed, err := strconv.ParseInt(s, 10, 64); err == nil {
			seed = parsed
		}
	}
	t.Logf("stdio-hygiene seed: %d (reproduce with RECMEET_HYGIENE_SEED=%d)", seed, seed)
	rng := rand.New(rand.NewSource(seed))

	// Build a config file pointing at a populated meetings fixture so
	// the operations have meaningful data to exercise.
	cfg, _ := buildSearchableFixture(t)
	cfgPath := testutil.WriteTempConfig(t, map[string]any{
		"output": map[string]any{
			"directory": cfg.OutputDir,
		},
		"speaker_id": map[string]any{
			"database": cfg.SpeakerDB,
		},
	})

	cmd := exec.Command(recmeetMcpBin)
	cmd.Env = append(os.Environ(), "RECMEET_CONFIG="+cfgPath)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	var stdoutBuf bytes.Buffer
	tt, err := testutil.NewTeeStdioTransport(cmd, &stdoutBuf)
	if err != nil {
		t.Fatalf("NewTeeStdioTransport: %v", err)
	}
	c := mcpclient.NewClient(tt)

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	if err := c.Start(ctx); err != nil {
		t.Fatalf("client.Start: %v", err)
	}

	initReq := mcp.InitializeRequest{}
	initReq.Params.ProtocolVersion = mcp.LATEST_PROTOCOL_VERSION
	initReq.Params.ClientInfo = mcp.Implementation{Name: "stdio-hygiene", Version: "0.0.0"}
	if _, err := c.Initialize(ctx, initReq); err != nil {
		t.Fatalf("Initialize: %v\nseed=%d\nstderr:\n%s", err, seed, stderr.String())
	}

	type opSpec struct {
		name string
		fn   func() error
	}
	queries := []string{"API", "planning", "sprint", "migration", "standup", "Alice", "Bob", "Charlie"}
	meetings := []string{"2026-03-15_14-30", "2026-03-20_09-00", "2026-03-22_10-00", "2099-01-01_00-00"}
	statuses := []string{"open", "done", "all"}
	ops := []opSpec{
		{"list_tools", func() error {
			_, err := c.ListTools(ctx, mcp.ListToolsRequest{})
			return err
		}},
		{"search_meetings", func() error {
			req := mcp.CallToolRequest{}
			req.Params.Name = "search_meetings"
			req.Params.Arguments = map[string]any{"query": queries[rng.Intn(len(queries))]}
			_, err := c.CallTool(ctx, req)
			return err
		}},
		{"get_meeting", func() error {
			req := mcp.CallToolRequest{}
			req.Params.Name = "get_meeting"
			req.Params.Arguments = map[string]any{"meeting_dir": meetings[rng.Intn(len(meetings))]}
			_, err := c.CallTool(ctx, req)
			return err
		}},
		{"list_action_items", func() error {
			req := mcp.CallToolRequest{}
			req.Params.Name = "list_action_items"
			req.Params.Arguments = map[string]any{"status": statuses[rng.Intn(len(statuses))]}
			_, err := c.CallTool(ctx, req)
			return err
		}},
		{"get_speaker_profiles", func() error {
			req := mcp.CallToolRequest{}
			req.Params.Name = "get_speaker_profiles"
			req.Params.Arguments = map[string]any{}
			_, err := c.CallTool(ctx, req)
			return err
		}},
		{"failing_call", func() error {
			// Intentionally exercise error paths — request a
			// meeting that definitely doesn't exist.
			req := mcp.CallToolRequest{}
			req.Params.Name = "get_meeting"
			req.Params.Arguments = map[string]any{"meeting_dir": "no-such-meeting"}
			_, err := c.CallTool(ctx, req)
			return err
		}},
	}

	const N = 50
	for i := 0; i < N; i++ {
		op := ops[rng.Intn(len(ops))]
		if err := op.fn(); err != nil {
			// Transport errors are unexpected; tool-level errors
			// (IsError=true on the result) are fine and don't surface
			// here because CallTool returns nil err for those.
			t.Fatalf("op[%d]=%s: transport error: %v\nseed=%d\nstderr:\n%s",
				i, op.name, err, seed, stderr.String())
		}
	}

	// Close the client; this closes stdin so the server exits.
	_ = c.Close()

	// Every \n-terminated line in stdoutBuf must be valid JSON-RPC.
	scan := strings.Split(stdoutBuf.String(), "\n")
	for i, line := range scan {
		line = strings.TrimRight(line, "\r")
		if line == "" {
			continue
		}
		var raw json.RawMessage
		if err := json.Unmarshal([]byte(line), &raw); err != nil {
			t.Fatalf("STDIO HYGIENE VIOLATION at line %d (seed=%d): not valid JSON: %q\nerr: %v\nfull stdout:\n%s",
				i, seed, line, err, stdoutBuf.String())
		}
		var probe map[string]any
		if err := json.Unmarshal(raw, &probe); err != nil {
			t.Fatalf("STDIO HYGIENE VIOLATION at line %d (seed=%d): not a JSON object: %q",
				i, seed, line)
		}
		// JSON-RPC frames must carry jsonrpc=2.0.
		if jr, _ := probe["jsonrpc"].(string); jr != "2.0" {
			t.Errorf("STDIO HYGIENE: line %d (seed=%d): missing jsonrpc=2.0 field: %s", i, seed, line)
		}
	}
}
