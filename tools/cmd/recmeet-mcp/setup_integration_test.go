// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

// Package main integration tests for recmeet-mcp.
//
// All test files in this package carry the `integration` build tag so the
// default `go test ./...` invocation skips them (binary build + subprocess
// exec are expensive). Run with `go test -tags=integration ./cmd/recmeet-mcp`.
package main

import (
	"fmt"
	"os"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// recmeetMcpBin is the absolute path to the built `recmeet-mcp` binary,
// set up once in TestMain and shared across all tests in this package.
var recmeetMcpBin string

func TestMain(m *testing.M) {
	bin, cleanup, err := testutil.BuildBinaryOnce("recmeet-mcp")
	if err != nil {
		fmt.Fprintf(os.Stderr, "TestMain: BuildBinaryOnce(recmeet-mcp): %v\n", err)
		os.Exit(2)
	}
	recmeetMcpBin = bin
	code := m.Run()
	cleanup()
	os.Exit(code)
}
