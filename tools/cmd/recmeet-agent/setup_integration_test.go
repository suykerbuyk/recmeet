// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

// Package main integration tests for recmeet-agent.
//
// All test files in this package carry the `integration` build tag so the
// default `go test ./...` invocation skips them (binary build + subprocess
// exec are expensive). Run with `go test -tags=integration ./cmd/recmeet-agent`.
package main

import (
	"fmt"
	"os"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// recmeetAgentBin is the absolute path to the built `recmeet-agent` binary,
// set up once in TestMain and shared across all tests in this package.
var recmeetAgentBin string

func TestMain(m *testing.M) {
	bin, cleanup, err := testutil.BuildBinaryOnce("recmeet-agent")
	if err != nil {
		fmt.Fprintf(os.Stderr, "TestMain: BuildBinaryOnce(recmeet-agent): %v\n", err)
		os.Exit(2)
	}
	recmeetAgentBin = bin
	code := m.Run()
	cleanup()
	os.Exit(code)
}
