// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strings"
	"sync"

	"github.com/mark3labs/mcp-go/client/transport"
	"github.com/mark3labs/mcp-go/mcp"
)

// TeeStdioTransport is a transport.Interface implementation that
// spawns the supplied *exec.Cmd, reads its stdout via an io.TeeReader
// so callers can inspect the full byte stream (e.g. for the
// stdio-hygiene gate), and routes JSON-RPC frames to the client like
// the upstream `Stdio` transport. ~80 LoC end-to-end; intentionally
// minimal — only the surface area the protocol tests need.
//
// Tests use it like:
//
//	cmd := exec.Command(recmeetMcpBin)
//	var sink bytes.Buffer
//	tt, err := testutil.NewTeeStdioTransport(cmd, &sink)
//	if err != nil { t.Fatal(err) }
//	c := client.NewClient(tt)
//	defer c.Close()
//	// ... exercise the client ...
//	// Now scan sink line by line and json.Unmarshal each into json.RawMessage
//	// to assert stdout hygiene.
type TeeStdioTransport struct {
	cmd       *exec.Cmd
	stdin     io.WriteCloser
	stdoutTee io.Writer

	mu             sync.Mutex
	responses      map[string]chan *transport.JSONRPCResponse
	done           chan struct{}
	closeOnce      sync.Once
	onNotification func(notification mcp.JSONRPCNotification)
	notifyMu       sync.RWMutex
	started        bool
}

// NewTeeStdioTransport prepares (but does not start) a tee'd stdio
// transport. The caller is responsible for setting cmd.Env / cmd.Dir
// before passing it in. Stdout is teed through stdoutTee; pass an
// in-memory buffer (e.g. *bytes.Buffer) for hygiene inspection or
// io.Discard if you don't need the byte stream.
//
// Stderr is left to the caller — wire cmd.Stderr to a buffer or
// os.Stderr before calling this if you need it.
func NewTeeStdioTransport(cmd *exec.Cmd, stdoutTee io.Writer) (*TeeStdioTransport, error) {
	if cmd == nil {
		return nil, errors.New("NewTeeStdioTransport: nil cmd")
	}
	if stdoutTee == nil {
		stdoutTee = io.Discard
	}
	return &TeeStdioTransport{
		cmd:       cmd,
		stdoutTee: stdoutTee,
		responses: make(map[string]chan *transport.JSONRPCResponse),
		done:      make(chan struct{}),
	}, nil
}

// Start spawns the subprocess and begins the response-reader goroutine.
func (t *TeeStdioTransport) Start(_ context.Context) error {
	t.mu.Lock()
	if t.started {
		t.mu.Unlock()
		return nil
	}
	t.started = true
	t.mu.Unlock()

	stdin, err := t.cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("TeeStdioTransport.Start: stdin pipe: %w", err)
	}
	stdout, err := t.cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("TeeStdioTransport.Start: stdout pipe: %w", err)
	}
	if err := t.cmd.Start(); err != nil {
		return fmt.Errorf("TeeStdioTransport.Start: cmd.Start: %w", err)
	}
	t.stdin = stdin

	go t.readLoop(bufio.NewReader(io.TeeReader(stdout, t.stdoutTee)))
	return nil
}

func (t *TeeStdioTransport) readLoop(r *bufio.Reader) {
	for {
		select {
		case <-t.done:
			return
		default:
		}
		line, err := r.ReadString('\n')
		if err != nil {
			t.closeDone()
			return
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}
		var base struct {
			ID     *mcp.RequestId `json:"id,omitempty"`
			Method string         `json:"method,omitempty"`
		}
		if err := json.Unmarshal([]byte(line), &base); err != nil {
			continue
		}
		// Notifications (method + no id)
		if base.Method != "" && base.ID == nil {
			var note mcp.JSONRPCNotification
			if err := json.Unmarshal([]byte(line), &note); err != nil {
				continue
			}
			t.notifyMu.RLock()
			h := t.onNotification
			t.notifyMu.RUnlock()
			if h != nil {
				h(note)
			}
			continue
		}
		// Otherwise: response to one of our requests.
		var resp transport.JSONRPCResponse
		if err := json.Unmarshal([]byte(line), &resp); err != nil {
			continue
		}
		idKey := resp.ID.String()
		t.mu.Lock()
		ch, ok := t.responses[idKey]
		if ok {
			delete(t.responses, idKey)
		}
		t.mu.Unlock()
		if ok {
			ch <- &resp
		}
	}
}

func (t *TeeStdioTransport) closeDone() {
	t.closeOnce.Do(func() { close(t.done) })
}

// SendRequest writes a JSON-RPC request to stdin and blocks for the
// matching response.
func (t *TeeStdioTransport) SendRequest(ctx context.Context, req transport.JSONRPCRequest) (*transport.JSONRPCResponse, error) {
	if t.stdin == nil {
		return nil, errors.New("TeeStdioTransport.SendRequest: not started")
	}
	body, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("marshal request: %w", err)
	}
	body = append(body, '\n')

	idKey := req.ID.String()
	ch := make(chan *transport.JSONRPCResponse, 1)
	t.mu.Lock()
	t.responses[idKey] = ch
	t.mu.Unlock()

	if _, err := t.stdin.Write(body); err != nil {
		t.mu.Lock()
		delete(t.responses, idKey)
		t.mu.Unlock()
		return nil, fmt.Errorf("write stdin: %w", err)
	}

	select {
	case <-t.done:
		return nil, errors.New("transport closed")
	case <-ctx.Done():
		t.mu.Lock()
		delete(t.responses, idKey)
		t.mu.Unlock()
		return nil, ctx.Err()
	case resp := <-ch:
		return resp, nil
	}
}

// SendNotification writes a JSON-RPC notification to stdin (no response
// is expected).
func (t *TeeStdioTransport) SendNotification(_ context.Context, n mcp.JSONRPCNotification) error {
	if t.stdin == nil {
		return errors.New("TeeStdioTransport.SendNotification: not started")
	}
	body, err := json.Marshal(n)
	if err != nil {
		return fmt.Errorf("marshal notification: %w", err)
	}
	body = append(body, '\n')
	_, err = t.stdin.Write(body)
	return err
}

// SetNotificationHandler registers a handler for server-emitted
// notifications. Setting nil clears the handler.
func (t *TeeStdioTransport) SetNotificationHandler(h func(mcp.JSONRPCNotification)) {
	t.notifyMu.Lock()
	t.onNotification = h
	t.notifyMu.Unlock()
}

// Close terminates the subprocess. stdin is closed so the server sees
// EOF; cmd.Wait() reaps the process. Safe to call multiple times.
func (t *TeeStdioTransport) Close() error {
	t.closeDone()
	if t.stdin != nil {
		_ = t.stdin.Close()
	}
	if t.cmd != nil && t.cmd.Process != nil {
		_ = t.cmd.Wait()
	}
	return nil
}

// GetSessionId is required by transport.Interface; stdio has no
// session ID concept.
func (t *TeeStdioTransport) GetSessionId() string { return "" }

// compile-time check
var _ transport.Interface = (*TeeStdioTransport)(nil)
