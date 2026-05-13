// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"strings"
	"testing"
)

// Expectation is a named operator-mistake → required-stderr-content
// contract. Tests that exercise an error path call
// AssertActionableStderr with the matching expectation rather than
// duplicating substring assertions inline.
type Expectation string

const (
	// ExpectMissingAnthropicKey — stderr must name both the env var
	// (ANTHROPIC_API_KEY) and the config-file alternative
	// (api_keys.anthropic).
	ExpectMissingAnthropicKey Expectation = "missing_anthropic_key"

	// ExpectBadNotePath — stderr must contain the failing path AND a
	// hint that absolute paths are expected.
	ExpectBadNotePath Expectation = "bad_note_path"

	// ExpectUnparseableNote — stderr must name the path AND the first
	// parse-error string surfaced from meetingdata.
	ExpectUnparseableNote Expectation = "unparseable_note"

	// ExpectUnknownFlag — stderr must name the offending flag AND
	// reference `--help` for discovery.
	ExpectUnknownFlag Expectation = "unknown_flag"

	// ExpectMissingConfig — stderr must name the path the operator set
	// (e.g. via RECMEET_CONFIG) AND the binary's default location.
	ExpectMissingConfig Expectation = "missing_config"

	// ExpectMockAnthropic401 — stderr must name the auth failure AND
	// hint at refreshing the API key.
	ExpectMockAnthropic401 Expectation = "mock_anthropic_401"
)

// expectation describes the substrings required for one named contract.
// `details` is an optional context value (e.g. the path that failed, the
// flag name) — callers thread it through AssertActionableStderr's
// optional varargs.
type expectationSpec struct {
	// staticParts must appear in stderr verbatim.
	staticParts []string
	// detailLabel describes what `details` represents in failure
	// messages; "" means details are not used.
	detailLabel string
	// detailRequired, if true, means the test must provide one detail
	// string and it must appear in stderr.
	detailRequired bool
}

var expectationSpecs = map[Expectation]expectationSpec{
	ExpectMissingAnthropicKey: {
		staticParts: []string{"ANTHROPIC_API_KEY", "api_keys.anthropic"},
	},
	ExpectBadNotePath: {
		staticParts:    []string{"absolute"},
		detailLabel:    "failing path",
		detailRequired: true,
	},
	ExpectUnparseableNote: {
		staticParts:    []string{"parse"},
		detailLabel:    "failing path",
		detailRequired: true,
	},
	ExpectUnknownFlag: {
		staticParts:    []string{"--help"},
		detailLabel:    "flag name",
		detailRequired: true,
	},
	ExpectMissingConfig: {
		staticParts:    []string{".config/recmeet/config.yaml"},
		detailLabel:    "configured path",
		detailRequired: true,
	},
	ExpectMockAnthropic401: {
		staticParts: []string{"401", "refresh"},
	},
}

// AssertActionableStderr verifies that `stderr` contains the substrings
// required by `expectation`. When `details` is supplied, it is treated
// as the variable component (failing path, flag name, …) and must also
// appear in stderr.
//
// On failure, the full stderr is logged via t.Logf to aid debugging,
// then the missing substring is named in the t.Errorf call. The helper
// uses t.Helper() so failures point at the test's call site, not this
// function.
func AssertActionableStderr(t *testing.T, stderr string, expectation Expectation, details ...string) {
	t.Helper()
	missing, ok := checkActionableStderr(stderr, expectation, details...)
	if !ok {
		// Unknown expectation: surface via Fatalf so the test stops.
		t.Fatalf("AssertActionableStderr: unknown expectation %q", expectation)
		return
	}
	spec := expectationSpecs[expectation]
	if spec.detailRequired && len(details) == 0 {
		t.Fatalf("AssertActionableStderr(%s): expectation requires a %s detail; pass it as a vararg",
			expectation, spec.detailLabel)
		return
	}
	if len(missing) > 0 {
		t.Logf("AssertActionableStderr(%s): full stderr was:\n%s", expectation, stderr)
		t.Errorf("AssertActionableStderr(%s): stderr is missing required substrings: %v",
			expectation, missing)
	}
}

// checkActionableStderr is the substring-check core extracted from
// AssertActionableStderr. Returns the list of missing substrings and a
// boolean indicating whether the expectation is known. Exposed at
// package scope for the unit tests; not part of the public API.
func checkActionableStderr(stderr string, expectation Expectation, details ...string) (missing []string, knownExpectation bool) {
	spec, ok := expectationSpecs[expectation]
	if !ok {
		return nil, false
	}
	for _, part := range spec.staticParts {
		if !strings.Contains(stderr, part) {
			missing = append(missing, part)
		}
	}
	if spec.detailRequired {
		for _, d := range details {
			if !strings.Contains(stderr, d) {
				missing = append(missing, d)
			}
		}
	}
	return missing, true
}
