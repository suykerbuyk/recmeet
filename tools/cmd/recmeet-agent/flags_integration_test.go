// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/testutil"
)

// TestAgentCLI_UnknownFlag verifies that `prep --bogus` exits nonzero
// with stderr naming the offending flag plus a usage block (which
// includes `--help` so the operator can discover the right flag).
func TestAgentCLI_UnknownFlag(t *testing.T) {
	res := runAgent(t, nil, "prep", "--bogus")
	if res.ExitCode == 0 {
		t.Fatalf("prep --bogus exit=0, want nonzero\nstdout=%s\nstderr=%s", res.Stdout, res.Stderr)
	}
	if !strings.Contains(res.Stderr, "unknown flag") {
		t.Errorf("stderr missing 'unknown flag': %s", res.Stderr)
	}
	testutil.AssertActionableStderr(t, res.Stderr, testutil.ExpectUnknownFlag, "--bogus")
}

// TestAgentCLI_UnknownSubcommand verifies `recmeet-agent bogus` exits
// nonzero with stderr identifying the unknown subcommand and pointing
// the operator at --help for the valid command list.
//
// Cobra's default behavior is to emit `Error: unknown command "bogus"
// for "recmeet-agent"` plus a `Run 'recmeet-agent --help' for usage.`
// hint; we verify both substrings rather than enumerating the valid
// subcommands inline (those are discoverable via the recommended
// --help follow-up).
func TestAgentCLI_UnknownSubcommand(t *testing.T) {
	res := runAgent(t, nil, "bogus")
	if res.ExitCode == 0 {
		t.Fatalf("bogus exit=0, want nonzero\nstdout=%s\nstderr=%s", res.Stdout, res.Stderr)
	}
	for _, want := range []string{"unknown command", "--help"} {
		if !strings.Contains(res.Stderr, want) {
			t.Errorf("subcommand error stderr missing %q\nstderr=%s", want, res.Stderr)
		}
	}
}
