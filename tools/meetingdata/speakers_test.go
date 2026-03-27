// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadSpeakerProfiles(t *testing.T) {
	tmp := t.TempDir()
	src, _ := os.ReadFile("testdata/speaker_profile.json")
	os.WriteFile(filepath.Join(tmp, "Alice_Smith.json"), src, 0644)

	profiles, err := LoadSpeakerProfiles(tmp)
	if err != nil {
		t.Fatalf("LoadSpeakerProfiles: %v", err)
	}

	if len(profiles) != 1 {
		t.Fatalf("expected 1 profile, got %d", len(profiles))
	}

	p := profiles[0]
	if p.Name != "Alice_Smith" {
		t.Errorf("Name = %q", p.Name)
	}
	if p.EmbeddingCount != 2 {
		t.Errorf("EmbeddingCount = %d, want 2", p.EmbeddingCount)
	}
	if p.Embeddings != nil {
		t.Error("Embeddings should be nil (stripped)")
	}
	if p.Created != "2026-03-01T10:00:00Z" {
		t.Errorf("Created = %q", p.Created)
	}
}

func TestLoadSpeakerProfiles_Empty(t *testing.T) {
	tmp := t.TempDir()
	profiles, err := LoadSpeakerProfiles(tmp)
	if err != nil {
		t.Fatalf("LoadSpeakerProfiles: %v", err)
	}
	if len(profiles) != 0 {
		t.Errorf("expected 0 profiles, got %d", len(profiles))
	}
}

func TestLoadSpeakerProfiles_Missing(t *testing.T) {
	profiles, err := LoadSpeakerProfiles("/nonexistent/path")
	if err != nil {
		t.Fatalf("expected no error for missing dir, got: %v", err)
	}
	if profiles != nil {
		t.Error("expected nil profiles")
	}
}

func TestLoadSpeakerProfiles_CorruptJSON(t *testing.T) {
	tmp := t.TempDir()
	os.WriteFile(filepath.Join(tmp, "bad.json"), []byte("{invalid}"), 0644)

	profiles, err := LoadSpeakerProfiles(tmp)
	if err != nil {
		t.Fatalf("LoadSpeakerProfiles: %v", err)
	}
	if len(profiles) != 0 {
		t.Errorf("expected 0 profiles for corrupt JSON, got %d", len(profiles))
	}
}

func TestLoadMeetingSpeakers(t *testing.T) {
	tmp := t.TempDir()
	src, _ := os.ReadFile("testdata/speakers.json")
	os.WriteFile(filepath.Join(tmp, "speakers.json"), src, 0644)

	speakers, err := LoadMeetingSpeakers(tmp)
	if err != nil {
		t.Fatalf("LoadMeetingSpeakers: %v", err)
	}

	if len(speakers) != 2 {
		t.Fatalf("expected 2 speakers, got %d", len(speakers))
	}

	s0 := speakers[0]
	if s0.Label != "Alice_Smith" {
		t.Errorf("speaker[0].Label = %q", s0.Label)
	}
	if !s0.Identified {
		t.Error("speaker[0] should be identified")
	}
	if s0.Confidence != 0.95 {
		t.Errorf("speaker[0].Confidence = %f", s0.Confidence)
	}
	if s0.Embedding != nil {
		t.Error("Embedding should be nil (stripped)")
	}

	s1 := speakers[1]
	if s1.Label != "Speaker_01" {
		t.Errorf("speaker[1].Label = %q", s1.Label)
	}
	if s1.Identified {
		t.Error("speaker[1] should not be identified")
	}
}

func TestLoadMeetingSpeakers_Missing(t *testing.T) {
	tmp := t.TempDir()
	speakers, err := LoadMeetingSpeakers(tmp)
	if err != nil {
		t.Fatalf("expected no error for missing speakers.json, got: %v", err)
	}
	if speakers != nil {
		t.Error("expected nil speakers")
	}
}

func TestLoadMeetingSpeakers_CorruptJSON(t *testing.T) {
	tmp := t.TempDir()
	os.WriteFile(filepath.Join(tmp, "speakers.json"), []byte("{bad}"), 0644)

	_, err := LoadMeetingSpeakers(tmp)
	if err == nil {
		t.Error("expected error for corrupt speakers.json")
	}
}
