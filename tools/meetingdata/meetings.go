// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

type MeetingInfo struct {
	DirName   string
	DirPath   string
	Date      string
	Time      string
	HasAudio  bool
	AudioPath string
	NotePath  string
}

var meetingDirRe = regexp.MustCompile(`^(\d{4}-\d{2}-\d{2})_(\d{2}-\d{2})`)

func DiscoverMeetings(outputDir string) ([]MeetingInfo, error) {
	entries, err := os.ReadDir(outputDir)
	if err != nil {
		return nil, fmt.Errorf("read output dir: %w", err)
	}

	var meetings []MeetingInfo
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		m := meetingDirRe.FindStringSubmatch(e.Name())
		if m == nil {
			continue
		}

		dirPath := filepath.Join(outputDir, e.Name())
		info := MeetingInfo{
			DirName: e.Name(),
			DirPath: dirPath,
			Date:    m[1],
			Time:    strings.ReplaceAll(m[2], "-", ":"),
		}

		info.AudioPath, info.HasAudio = findAudioFile(dirPath, m[1], m[2])
		meetings = append(meetings, info)
	}
	return meetings, nil
}

func findAudioFile(dirPath, date, time string) (string, bool) {
	// Prefer timestamped: audio_YYYY-MM-DD_HH-MM.wav
	timestamped := filepath.Join(dirPath, fmt.Sprintf("audio_%s_%s.wav", date, time))
	if _, err := os.Stat(timestamped); err == nil {
		return timestamped, true
	}

	// Fallback: audio.wav
	legacy := filepath.Join(dirPath, "audio.wav")
	if _, err := os.Stat(legacy); err == nil {
		return legacy, true
	}

	return "", false
}

func FindNoteForMeeting(info MeetingInfo, noteDir string) (string, error) {
	timestamp := info.Date + "_" + strings.ReplaceAll(info.Time, ":", "-")
	prefix := "Meeting_" + timestamp

	searchDirs := []string{info.DirPath}
	if noteDir != "" {
		// Search in YYYY/MM/ subdirectory
		year := info.Date[:4]
		month := info.Date[5:7]
		searchDirs = append(searchDirs, filepath.Join(noteDir, year, month))
		searchDirs = append(searchDirs, noteDir)
	}

	var bestPath string
	var bestMod int64

	for _, dir := range searchDirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasPrefix(e.Name(), prefix) || !strings.HasSuffix(e.Name(), ".md") {
				continue
			}
			fi, err := e.Info()
			if err != nil {
				continue
			}
			mod := fi.ModTime().UnixNano()
			if bestPath == "" || mod > bestMod {
				bestPath = filepath.Join(dir, e.Name())
				bestMod = mod
			}
		}
	}

	if bestPath == "" {
		return "", fmt.Errorf("no note found for meeting %s", info.DirName)
	}
	return bestPath, nil
}

func MatchNotesToMeetings(meetings []MeetingInfo, noteDir string) []MeetingInfo {
	for i := range meetings {
		p, err := FindNoteForMeeting(meetings[i], noteDir)
		if err == nil {
			meetings[i].NotePath = p
		}
	}
	return meetings
}
