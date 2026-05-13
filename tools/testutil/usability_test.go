// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"testing"
)

// We exercise AssertActionableStderr indirectly via checkActionableStderr —
// the substring-check core — so positive and negative cases can be
// asserted from a single *testing.T without subtest-failure plumbing.

func TestAssertActionableStderr_MissingAnthropicKey(t *testing.T) {
	good := "Error: ANTHROPIC_API_KEY env var not set; set it or add api_keys.anthropic to your config.yaml"
	bad := "Error: no API key"

	if missing, _ := checkActionableStderr(good, ExpectMissingAnthropicKey); len(missing) != 0 {
		t.Errorf("positive: unexpected missing parts: %v", missing)
	}
	if missing, _ := checkActionableStderr(bad, ExpectMissingAnthropicKey); len(missing) != 2 {
		t.Errorf("negative: expected 2 missing parts, got %v", missing)
	}

	// And the production path against the positive case must pass:
	AssertActionableStderr(t, good, ExpectMissingAnthropicKey)
}

func TestAssertActionableStderr_BadNotePath(t *testing.T) {
	path := "/tmp/nonexistent/note.md"
	good := "Error: cannot read " + path + "; please use an absolute path to the note file"

	if missing, _ := checkActionableStderr(good, ExpectBadNotePath, path); len(missing) != 0 {
		t.Errorf("positive: missing=%v", missing)
	}
	if missing, _ := checkActionableStderr("use absolute path", ExpectBadNotePath, path); len(missing) != 1 {
		t.Errorf("negative (missing path): expected 1 missing, got %v", missing)
	}
	if missing, _ := checkActionableStderr(path, ExpectBadNotePath, path); len(missing) != 1 {
		t.Errorf("negative (missing hint): expected 1 missing, got %v", missing)
	}

	AssertActionableStderr(t, good, ExpectBadNotePath, path)
}

func TestAssertActionableStderr_UnparseableNote(t *testing.T) {
	path := "/tmp/bad.md"
	good := "Error: failed to parse " + path + ": frontmatter parse error at line 1"
	if missing, _ := checkActionableStderr(good, ExpectUnparseableNote, path); len(missing) != 0 {
		t.Errorf("positive: missing=%v", missing)
	}
	AssertActionableStderr(t, good, ExpectUnparseableNote, path)
}

func TestAssertActionableStderr_UnknownFlag(t *testing.T) {
	flag := "--bogus"
	good := "Error: unknown flag: " + flag + "\nRun 'recmeet-agent --help' for usage."
	if missing, _ := checkActionableStderr(good, ExpectUnknownFlag, flag); len(missing) != 0 {
		t.Errorf("positive: missing=%v", missing)
	}
	if missing, _ := checkActionableStderr("unknown flag "+flag, ExpectUnknownFlag, flag); len(missing) != 1 {
		t.Errorf("negative (no --help hint): expected 1 missing, got %v", missing)
	}
	AssertActionableStderr(t, good, ExpectUnknownFlag, flag)
}

func TestAssertActionableStderr_MissingConfig(t *testing.T) {
	path := "/tmp/explicit-config.yaml"
	good := "Error: config not found: " + path + " (default location is ~/.config/recmeet/config.yaml)"
	if missing, _ := checkActionableStderr(good, ExpectMissingConfig, path); len(missing) != 0 {
		t.Errorf("positive: missing=%v", missing)
	}
	AssertActionableStderr(t, good, ExpectMissingConfig, path)
}

func TestAssertActionableStderr_MockAnthropic401(t *testing.T) {
	good := "Error: Anthropic API returned 401; please refresh your API key"
	if missing, _ := checkActionableStderr(good, ExpectMockAnthropic401); len(missing) != 0 {
		t.Errorf("positive: missing=%v", missing)
	}
	if missing, _ := checkActionableStderr("auth failure", ExpectMockAnthropic401); len(missing) != 2 {
		t.Errorf("negative: expected 2 missing, got %v", missing)
	}
	AssertActionableStderr(t, good, ExpectMockAnthropic401)
}

func TestAssertActionableStderr_UnknownExpectation(t *testing.T) {
	if _, ok := checkActionableStderr("anything", Expectation("not_a_real_expectation")); ok {
		t.Error("expected unknown expectation to report ok=false")
	}
}
