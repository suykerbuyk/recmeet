// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package meetingdata

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type SpeakerProfile struct {
	Name           string      `json:"name"`
	Created        string      `json:"created"`
	Updated        string      `json:"updated"`
	EmbeddingCount int         `json:"-"`
	Embeddings     [][]float32 `json:"embeddings"`
}

type MeetingSpeaker struct {
	ClusterID   int       `json:"cluster_id"`
	Label       string    `json:"label"`
	Identified  bool      `json:"identified"`
	DurationSec float32   `json:"duration_sec"`
	Confidence  float32   `json:"confidence"`
	Embedding   []float32 `json:"embedding"`
}

type meetingSpeakersWrapper struct {
	Speakers []MeetingSpeaker `json:"speakers"`
}

func LoadSpeakerProfiles(dbDir string) ([]SpeakerProfile, error) {
	entries, err := os.ReadDir(dbDir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, fmt.Errorf("read speaker db: %w", err)
	}

	var profiles []SpeakerProfile
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".json") {
			continue
		}

		data, err := os.ReadFile(filepath.Join(dbDir, e.Name()))
		if err != nil {
			continue
		}

		var p SpeakerProfile
		if err := json.Unmarshal(data, &p); err != nil {
			continue
		}
		p.EmbeddingCount = len(p.Embeddings)
		p.Embeddings = nil // Don't expose raw embeddings via MCP
		profiles = append(profiles, p)
	}
	return profiles, nil
}

func LoadMeetingSpeakers(meetingDir string) ([]MeetingSpeaker, error) {
	path := filepath.Join(meetingDir, "speakers.json")
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, fmt.Errorf("read speakers.json: %w", err)
	}

	var wrapper meetingSpeakersWrapper
	if err := json.Unmarshal(data, &wrapper); err != nil {
		return nil, fmt.Errorf("parse speakers.json: %w", err)
	}

	// Strip embeddings from response (large, not useful via MCP)
	for i := range wrapper.Speakers {
		wrapper.Speakers[i].Embedding = nil
	}
	return wrapper.Speakers, nil
}
