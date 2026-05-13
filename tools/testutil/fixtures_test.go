// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syketech/recmeet-tools/meetingdata"
)

func TestBuildMeetingsFixture(t *testing.T) {
	root := BuildMeetingsFixture(t)

	entries, err := os.ReadDir(root)
	if err != nil {
		t.Fatalf("ReadDir(%s): %v", root, err)
	}
	if len(entries) != 3 {
		t.Errorf("expected 3 meeting subdirs, got %d", len(entries))
	}
	for _, e := range entries {
		if !e.IsDir() {
			t.Errorf("expected dir, got %s", e.Name())
			continue
		}
		dir := filepath.Join(root, e.Name())
		mdPath := filepath.Join(dir, e.Name()+".md")
		if _, err := os.Stat(mdPath); err != nil {
			t.Errorf("missing note %s: %v", mdPath, err)
		}
		spkPath := filepath.Join(dir, "speakers.json")
		if _, err := os.Stat(spkPath); err != nil {
			t.Errorf("missing speakers.json: %v", err)
		}
		matches, err := filepath.Glob(filepath.Join(dir, "audio_*.wav.json"))
		if err != nil || len(matches) != 1 {
			t.Errorf("expected 1 audio sidecar in %s, got %v err=%v", dir, matches, err)
		}
	}
}

func TestCopyNoteFixture(t *testing.T) {
	for _, name := range []string{"meeting_full", "meeting_simple", "meeting_no_actions"} {
		dst := CopyNoteFixture(t, name)
		if !strings.HasSuffix(dst, name+".md") {
			t.Errorf("dst path %s missing %s.md", dst, name)
		}
		b, err := os.ReadFile(dst)
		if err != nil {
			t.Errorf("read %s: %v", dst, err)
		}
		if len(b) == 0 {
			t.Errorf("%s is empty", dst)
		}
	}
}

func TestWriteTempConfig_RoundTrip(t *testing.T) {
	cfg := map[string]any{
		"output": map[string]any{
			"directory": "/tmp/recmeet-out",
		},
		"api_keys": map[string]any{
			"anthropic": "sk-ant-test",
		},
		"web": map[string]any{
			"port": 9090,
			"bind": "0.0.0.0",
		},
	}
	path := WriteTempConfig(t, cfg)
	loaded, err := meetingdata.LoadConfig(path)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if loaded.OutputDir != "/tmp/recmeet-out" {
		t.Errorf("OutputDir round-trip: %q", loaded.OutputDir)
	}
	if loaded.APIKeys["anthropic"] != "sk-ant-test" {
		t.Errorf("anthropic key round-trip: %q", loaded.APIKeys["anthropic"])
	}
	if loaded.WebPort != 9090 {
		t.Errorf("WebPort round-trip: %d", loaded.WebPort)
	}
	if loaded.WebBind != "0.0.0.0" {
		t.Errorf("WebBind round-trip: %q", loaded.WebBind)
	}
}

func TestAgendaHTMLFixture(t *testing.T) {
	path := AgendaHTMLFixture(t)
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile(%s): %v", path, err)
	}
	body := string(b)
	if !strings.Contains(body, "<html") || !strings.Contains(body, "Q2 Planning") {
		t.Errorf("agenda missing expected content: %s", body[:min(200, len(body))])
	}
	// ~1 KB ballpark
	if len(b) < 400 || len(b) > 4096 {
		t.Errorf("agenda size unexpected: %d bytes", len(b))
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
