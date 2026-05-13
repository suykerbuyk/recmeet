// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Package testutil contains shared helpers for integration-style tests
// across the recmeet Go tools (`recmeet-mcp`, `recmeet-agent`). The
// helpers are not themselves gated behind a build tag so that callers
// from both default test runs and integration test runs can import
// them; consumer test files apply `//go:build integration` as needed.
package testutil

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// BuildBinaryOnce builds `./cmd/<name>` from the `tools/` module root
// into a per-call temporary directory and returns the absolute path to
// the built binary plus a cleanup function that removes the temp dir.
//
// Intended usage from a test package:
//
//	var recmeetMcpBin string
//
//	func TestMain(m *testing.M) {
//	    bin, cleanup, err := testutil.BuildBinaryOnce("recmeet-mcp")
//	    if err != nil {
//	        fmt.Fprintln(os.Stderr, err)
//	        os.Exit(2)
//	    }
//	    recmeetMcpBin = bin
//	    code := m.Run()
//	    cleanup()
//	    os.Exit(code)
//	}
//
// The temporary directory's lifetime spans the whole `m.Run()`, so the
// returned path is safe to use from any test in the package.
func BuildBinaryOnce(name string) (path string, cleanup func(), err error) {
	if name == "" {
		return "", func() {}, fmt.Errorf("BuildBinaryOnce: empty binary name")
	}

	moduleRoot, err := findModuleRoot()
	if err != nil {
		return "", func() {}, fmt.Errorf("BuildBinaryOnce(%s): %w", name, err)
	}

	dir, err := os.MkdirTemp("", "recmeet-bin-*")
	if err != nil {
		return "", func() {}, fmt.Errorf("BuildBinaryOnce(%s): mkdir: %w", name, err)
	}

	cleanup = func() { _ = os.RemoveAll(dir) }

	binPath := filepath.Join(dir, name)
	if runtime.GOOS == "windows" {
		binPath += ".exe"
	}

	// When GOCOVERDIR is set in the test environment, build the
	// binary with `-cover` so subprocess invocations write coverage
	// counters that `go tool covdata` can fold back into the final
	// coverage report. Without this, integration tests that exercise
	// the binary as a subprocess report 0% coverage on its main.go
	// even when the code paths are fully exercised.
	args := []string{"build", "-o", binPath}
	if os.Getenv("GOCOVERDIR") != "" {
		args = append(args, "-cover", "-coverpkg=./...")
	}
	args = append(args, "./cmd/"+name)

	cmd := exec.Command("go", args...)
	cmd.Dir = moduleRoot
	cmd.Env = os.Environ()

	out, buildErr := cmd.CombinedOutput()
	if buildErr != nil {
		cleanup()
		return "", func() {}, fmt.Errorf(
			"BuildBinaryOnce(%s): go build failed in %s: %w\noutput:\n%s",
			name, moduleRoot, buildErr, string(out),
		)
	}

	// Sanity-check the artifact: the temp file must exist and be
	// executable. If the build silently produced nothing, surface the
	// failure here rather than letting a later test exec something that
	// isn't there.
	info, err := os.Stat(binPath)
	if err != nil {
		cleanup()
		return "", func() {}, fmt.Errorf("BuildBinaryOnce(%s): stat after build: %w", name, err)
	}
	if runtime.GOOS != "windows" && info.Mode()&0o100 == 0 {
		cleanup()
		return "", func() {}, fmt.Errorf("BuildBinaryOnce(%s): %s is not executable (mode=%v)", name, binPath, info.Mode())
	}

	return binPath, cleanup, nil
}

// findModuleRoot locates the `tools/` module root by walking up from
// the directory of the caller's source file until a `go.mod` file is
// found. This is robust against the test binary's working directory
// (which Go sets to the test's package dir) and avoids assumptions
// about repository layout beyond "this file lives somewhere under the
// module root."
func findModuleRoot() (string, error) {
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		return "", fmt.Errorf("runtime.Caller failed")
	}
	dir := filepath.Dir(thisFile)
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("findModuleRoot: walked up from %s without finding go.mod", filepath.Dir(thisFile))
		}
		dir = parent
	}
}
