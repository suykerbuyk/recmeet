// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"os"
	"path/filepath"
	"testing"
)

func setupMeetingDir(t *testing.T) string {
	t.Helper()
	tmp := t.TempDir()

	// Create meeting directories
	dir1 := filepath.Join(tmp, "2026-03-15_14-30")
	os.MkdirAll(dir1, 0755)
	os.WriteFile(filepath.Join(dir1, "audio_2026-03-15_14-30.wav"), []byte("audio"), 0644)

	dir2 := filepath.Join(tmp, "2026-03-20_09-00")
	os.MkdirAll(dir2, 0755)
	os.WriteFile(filepath.Join(dir2, "audio.wav"), []byte("audio"), 0644) // legacy name

	// Non-meeting dir (should be ignored)
	os.MkdirAll(filepath.Join(tmp, "not-a-meeting"), 0755)

	return tmp
}

func TestDiscoverMeetings(t *testing.T) {
	tmp := setupMeetingDir(t)

	meetings, err := DiscoverMeetings(tmp)
	if err != nil {
		t.Fatalf("DiscoverMeetings: %v", err)
	}

	if len(meetings) != 2 {
		t.Fatalf("expected 2 meetings, got %d", len(meetings))
	}

	// Check first meeting (timestamped audio)
	m1 := findMeeting(meetings, "2026-03-15_14-30")
	if m1 == nil {
		t.Fatal("meeting 2026-03-15_14-30 not found")
	}
	if !m1.HasAudio {
		t.Error("expected HasAudio for timestamped meeting")
	}
	if m1.Date != "2026-03-15" {
		t.Errorf("Date = %q", m1.Date)
	}
	if m1.Time != "14:30" {
		t.Errorf("Time = %q", m1.Time)
	}

	// Check second meeting (legacy audio)
	m2 := findMeeting(meetings, "2026-03-20_09-00")
	if m2 == nil {
		t.Fatal("meeting 2026-03-20_09-00 not found")
	}
	if !m2.HasAudio {
		t.Error("expected HasAudio for legacy audio meeting")
	}
}

func TestDiscoverMeetings_Empty(t *testing.T) {
	tmp := t.TempDir()
	meetings, err := DiscoverMeetings(tmp)
	if err != nil {
		t.Fatalf("DiscoverMeetings: %v", err)
	}
	if len(meetings) != 0 {
		t.Errorf("expected 0 meetings, got %d", len(meetings))
	}
}

func TestDiscoverMeetings_NoAudio(t *testing.T) {
	tmp := t.TempDir()
	os.MkdirAll(filepath.Join(tmp, "2026-03-15_14-30"), 0755)

	meetings, err := DiscoverMeetings(tmp)
	if err != nil {
		t.Fatalf("DiscoverMeetings: %v", err)
	}
	if len(meetings) != 1 {
		t.Fatalf("expected 1 meeting, got %d", len(meetings))
	}
	if meetings[0].HasAudio {
		t.Error("expected no audio")
	}
}

func TestFindNoteForMeeting(t *testing.T) {
	tmp := t.TempDir()
	dir := filepath.Join(tmp, "2026-03-15_14-30")
	os.MkdirAll(dir, 0755)
	notePath := filepath.Join(dir, "Meeting_2026-03-15_14-30_Q1_Planning.md")
	os.WriteFile(notePath, []byte("test"), 0644)

	info := MeetingInfo{
		DirName: "2026-03-15_14-30",
		DirPath: dir,
		Date:    "2026-03-15",
		Time:    "14:30",
	}

	path, err := FindNoteForMeeting(info, "")
	if err != nil {
		t.Fatalf("FindNoteForMeeting: %v", err)
	}
	if path != notePath {
		t.Errorf("path = %q, want %q", path, notePath)
	}
}

func TestFindNoteForMeeting_NoteDir(t *testing.T) {
	tmp := t.TempDir()
	dir := filepath.Join(tmp, "meetings", "2026-03-15_14-30")
	os.MkdirAll(dir, 0755)

	noteDir := filepath.Join(tmp, "notes", "2026", "03")
	os.MkdirAll(noteDir, 0755)
	notePath := filepath.Join(noteDir, "Meeting_2026-03-15_14-30_Title.md")
	os.WriteFile(notePath, []byte("test"), 0644)

	info := MeetingInfo{
		DirName: "2026-03-15_14-30",
		DirPath: dir,
		Date:    "2026-03-15",
		Time:    "14:30",
	}

	path, err := FindNoteForMeeting(info, filepath.Join(tmp, "notes"))
	if err != nil {
		t.Fatalf("FindNoteForMeeting: %v", err)
	}
	if path != notePath {
		t.Errorf("path = %q, want %q", path, notePath)
	}
}

func TestFindNoteForMeeting_NotFound(t *testing.T) {
	tmp := t.TempDir()
	dir := filepath.Join(tmp, "2026-03-15_14-30")
	os.MkdirAll(dir, 0755)

	info := MeetingInfo{
		DirName: "2026-03-15_14-30",
		DirPath: dir,
		Date:    "2026-03-15",
		Time:    "14:30",
	}

	_, err := FindNoteForMeeting(info, "")
	if err == nil {
		t.Error("expected error for missing note")
	}
}

func findMeeting(meetings []MeetingInfo, dirname string) *MeetingInfo {
	for i := range meetings {
		if meetings[i].DirName == dirname {
			return &meetings[i]
		}
	}
	return nil
}
