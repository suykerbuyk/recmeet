// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package mcpserver

import (
	"testing"

	"github.com/syketech/recmeet-tools/meetingdata"
)

func TestNewServer_RegistersTools(t *testing.T) {
	cfg := meetingdata.Config{
		OutputDir: t.TempDir(),
		NoteDir:   t.TempDir(),
		SpeakerDB: t.TempDir(),
	}

	s := NewServer(cfg)
	if s == nil {
		t.Fatal("NewServer returned nil")
	}
}

func TestNewServer_DefaultConfig(t *testing.T) {
	cfg := meetingdata.DefaultConfig()
	cfg.SpeakerDB = t.TempDir()

	s := NewServer(cfg)
	if s == nil {
		t.Fatal("NewServer returned nil")
	}
}
