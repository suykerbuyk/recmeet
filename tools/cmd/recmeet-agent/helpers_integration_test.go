// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"testing"
	"time"
)

// runResult captures the outcome of a subprocess invocation. ExitCode
// is -1 when the binary failed to start (e.g. ENOENT).
type runResult struct {
	Stdout   string
	Stderr   string
	ExitCode int
}

// runAgent invokes the built recmeet-agent binary with the given args
// and environment overlay. Env keys present in `env` override the
// inherited os.Environ() set. A 30-second context timeout protects
// against hangs (e.g. agent loop with malformed mock responses).
func runAgent(t *testing.T, env map[string]string, args ...string) runResult {
	t.Helper()
	return runAgentWithStdin(t, env, "", args...)
}

// runAgentWithStdin is the same as runAgent but writes `stdin` to the
// subprocess's standard input. Used by tests that want to pipe a fake
// input stream; passing "" leaves stdin /dev/null-equivalent.
func runAgentWithStdin(t *testing.T, env map[string]string, stdin string, args ...string) runResult {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, recmeetAgentBin, args...)
	cmd.Env = mergedEnv(env)
	if stdin != "" {
		cmd.Stdin = bytes.NewBufferString(stdin)
	}
	var stdoutBuf, stderrBuf bytes.Buffer
	cmd.Stdout = &stdoutBuf
	cmd.Stderr = &stderrBuf

	err := cmd.Run()
	res := runResult{
		Stdout: stdoutBuf.String(),
		Stderr: stderrBuf.String(),
	}
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			res.ExitCode = exitErr.ExitCode()
		} else if ctx.Err() == context.DeadlineExceeded {
			t.Fatalf("runAgent: subprocess timeout after 30s\nargs=%v\nstdout=%s\nstderr=%s",
				args, res.Stdout, res.Stderr)
		} else {
			t.Fatalf("runAgent: failed to start: %v\nargs=%v", err, args)
		}
	}
	return res
}

// mergedEnv overlays `overrides` onto a sanitized copy of os.Environ().
// Sanitization clears any inherited ANTHROPIC_* / RECMEET_* variables so
// the test starts from a known-empty slate; tests that want them set
// must supply them explicitly via the overrides map.
func mergedEnv(overrides map[string]string) []string {
	out := make([]string, 0, len(os.Environ()))
	skip := map[string]bool{
		"ANTHROPIC_API_KEY":  true,
		"ANTHROPIC_BASE_URL": true,
		"BRAVE_API_KEY":      true,
		"RECMEET_CONFIG":     true,
	}
	have := make(map[string]bool, len(overrides))
	for _, kv := range os.Environ() {
		eq := -1
		for i, c := range kv {
			if c == '=' {
				eq = i
				break
			}
		}
		if eq < 0 {
			out = append(out, kv)
			continue
		}
		key := kv[:eq]
		if skip[key] {
			continue
		}
		out = append(out, kv)
	}
	for k, v := range overrides {
		have[k] = true
		out = append(out, k+"="+v)
		_ = have
	}
	return out
}
