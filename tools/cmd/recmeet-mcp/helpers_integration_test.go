// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

//go:build integration

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	mcpclient "github.com/mark3labs/mcp-go/client"
	"github.com/mark3labs/mcp-go/mcp"
	"github.com/syketech/recmeet-tools/mcpserver"
	"github.com/syketech/recmeet-tools/meetingdata"
	"github.com/syketech/recmeet-tools/testutil"
)

// fixtureRoot represents a populated meetings directory tree with notes
// whose filenames match the `Meeting_*.md` pattern that
// meetingdata.findMDFiles requires for discovery. The shared
// testutil.BuildMeetingsFixture helper writes notes as `<dirName>.md`,
// which is the on-disk convention recmeet itself uses but is not
// directly discoverable through SearchNotes. We adapt it here once for
// the Phase A protocol tests rather than duplicating the fixture body.
type fixtureRoot struct {
	OutputDir string // points at .../meetings (3 meeting subdirs inside)
	NoteDir   string // empty: SearchNotes will walk OutputDir
	SpeakerDB string // populated speaker DB with one profile
}

// buildSearchableFixture constructs a fixture set under t.TempDir() that
// is fully discoverable by SearchNotes / ListAllActionItems /
// DiscoverMeetings / FindNoteForMeeting. Returns the populated paths
// alongside the meetingdata.Config the in-process server should use.
func buildSearchableFixture(t *testing.T) (meetingdata.Config, fixtureRoot) {
	t.Helper()

	// Start with the shared fixture (3 meetings, audio sidecars,
	// speakers.json) under a fresh t.TempDir().
	meetingsRoot := testutil.BuildMeetingsFixture(t)

	// Rename each note from `<dirName>.md` to `Meeting_<dirName>.md`
	// so findMDFiles + FindNoteForMeeting can pick them up.
	entries, err := os.ReadDir(meetingsRoot)
	if err != nil {
		t.Fatalf("buildSearchableFixture: read %s: %v", meetingsRoot, err)
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		dir := filepath.Join(meetingsRoot, e.Name())
		oldPath := filepath.Join(dir, e.Name()+".md")
		newPath := filepath.Join(dir, "Meeting_"+e.Name()+".md")
		if _, err := os.Stat(oldPath); err == nil {
			if err := os.Rename(oldPath, newPath); err != nil {
				t.Fatalf("buildSearchableFixture: rename %s: %v", oldPath, err)
			}
		}
	}

	// Populate a speaker DB with one profile for get_speaker_profiles.
	speakerDB := filepath.Join(filepath.Dir(meetingsRoot), "speakers-db")
	if err := os.MkdirAll(speakerDB, 0o755); err != nil {
		t.Fatalf("buildSearchableFixture: mkdir %s: %v", speakerDB, err)
	}
	profile := `{
  "name": "Alice_Smith",
  "created": "2026-03-01T10:00:00Z",
  "updated": "2026-03-15T14:30:00Z",
  "embeddings": [[0.1, 0.2, 0.3, 0.4]]
}
`
	if err := os.WriteFile(filepath.Join(speakerDB, "alice.json"), []byte(profile), 0o644); err != nil {
		t.Fatalf("buildSearchableFixture: write profile: %v", err)
	}

	cfg := meetingdata.Config{
		OutputDir: meetingsRoot,
		NoteDir:   "",
		SpeakerDB: speakerDB,
		APIKeys:   map[string]string{},
	}

	return cfg, fixtureRoot{
		OutputDir: meetingsRoot,
		NoteDir:   "",
		SpeakerDB: speakerDB,
	}
}

// newInProcessClient constructs an in-process MCP client backed by a
// freshly-built mcpserver.NewServer(cfg). It mirrors the server-setup
// code path used by `recmeet-mcp/main.go` minus the stdio transport, so
// the in-process tests exercise the same handler graph as the as-built
// binary.
func newInProcessClient(t *testing.T, cfg meetingdata.Config) *mcpclient.Client {
	t.Helper()
	srv := mcpserver.NewServer(cfg)
	c, err := mcpclient.NewInProcessClient(srv)
	if err != nil {
		t.Fatalf("NewInProcessClient: %v", err)
	}
	t.Cleanup(func() { _ = c.Close() })
	if err := c.Start(context.Background()); err != nil {
		t.Fatalf("client.Start: %v", err)
	}

	initReq := mcp.InitializeRequest{}
	initReq.Params.ProtocolVersion = mcp.LATEST_PROTOCOL_VERSION
	initReq.Params.ClientInfo = mcp.Implementation{Name: "recmeet-mcp-integration", Version: "0.0.0"}
	if _, err := c.Initialize(context.Background(), initReq); err != nil {
		t.Fatalf("client.Initialize: %v", err)
	}
	return c
}

// mustCallTool calls the named tool and fails the test on transport
// failure. It returns the raw CallToolResult so individual tests can
// assert on .IsError / .Content as they see fit.
func mustCallTool(t *testing.T, c *mcpclient.Client, name string, args map[string]any) *mcp.CallToolResult {
	t.Helper()
	req := mcp.CallToolRequest{}
	req.Params.Name = name
	req.Params.Arguments = args
	res, err := c.CallTool(context.Background(), req)
	if err != nil {
		t.Fatalf("CallTool(%s): transport error: %v", name, err)
	}
	if res == nil {
		t.Fatalf("CallTool(%s): nil result", name)
	}
	return res
}

// resultText returns the concatenated text content from a CallToolResult.
// Fails the test if any content block is not text.
func resultText(t *testing.T, res *mcp.CallToolResult) string {
	t.Helper()
	if res == nil {
		t.Fatalf("resultText: nil result")
	}
	var sb strings.Builder
	for _, c := range res.Content {
		tc, ok := c.(mcp.TextContent)
		if !ok {
			t.Fatalf("resultText: non-text content block %T", c)
		}
		sb.WriteString(tc.Text)
	}
	return sb.String()
}

// requireOK asserts the call returned without IsError set; failure
// reports the surfaced text for diagnosis.
func requireOK(t *testing.T, res *mcp.CallToolResult, label string) {
	t.Helper()
	if res.IsError {
		t.Fatalf("%s: result.IsError=true: %s", label, resultText(t, res))
	}
}

// requireError asserts the call returned with IsError set.
func requireError(t *testing.T, res *mcp.CallToolResult, label string) {
	t.Helper()
	if !res.IsError {
		t.Fatalf("%s: expected IsError=true, got success: %s", label, resultText(t, res))
	}
}

// parseJSONArray decodes JSON text into a slice of generic maps.
func parseJSONArray(t *testing.T, text string) []map[string]any {
	t.Helper()
	var out []map[string]any
	if err := json.Unmarshal([]byte(text), &out); err != nil {
		t.Fatalf("parseJSONArray: %v\ninput: %s", err, text)
	}
	return out
}

// expectedToolNames is the canonical set of tools the recmeet-mcp
// server registers. Used by both TestMCPServer_ListTools and the
// in-process callers in TestMCPServer_StdoutHygiene.
func expectedToolNames() []string {
	return []string{
		"search_meetings",
		"get_meeting",
		"list_action_items",
		"get_speaker_profiles",
		"write_context_file",
	}
}

// withTempXDGData isolates XDG_DATA_HOME to a per-test temp dir. The
// write_context_file handler writes under XDG_DATA_HOME/recmeet/context;
// pinning the env var here keeps the test hermetic and lets the test
// assert on the resulting file path.
func withTempXDGData(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	t.Setenv("XDG_DATA_HOME", dir)
	return dir
}

// debugDump returns a one-line summary of the directory tree under
// root, suitable for embedding in test failure messages.
func debugDump(root string) string {
	var sb strings.Builder
	_ = filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		fmt.Fprintf(&sb, "  %s\n", path)
		return nil
	})
	return sb.String()
}
