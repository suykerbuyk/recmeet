// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package testutil

import (
	"os"
	"testing"
)

func TestBuildBinaryOnce_BuildsExecutable(t *testing.T) {
	bin, cleanup, err := BuildBinaryOnce("recmeet-mcp")
	if err != nil {
		t.Fatalf("BuildBinaryOnce: %v", err)
	}
	defer cleanup()

	info, err := os.Stat(bin)
	if err != nil {
		t.Fatalf("stat %s: %v", bin, err)
	}
	if info.Mode()&0o100 == 0 {
		t.Errorf("expected %s to be executable, mode=%v", bin, info.Mode())
	}
	if info.Size() == 0 {
		t.Errorf("expected %s to have nonzero size", bin)
	}
}

func TestBuildBinaryOnce_CleanupRemovesDir(t *testing.T) {
	bin, cleanup, err := BuildBinaryOnce("recmeet-agent")
	if err != nil {
		t.Fatalf("BuildBinaryOnce: %v", err)
	}
	cleanup()

	if _, err := os.Stat(bin); !os.IsNotExist(err) {
		t.Errorf("expected %s to be gone after cleanup, got err=%v", bin, err)
	}
}

func TestBuildBinaryOnce_EmptyName(t *testing.T) {
	_, _, err := BuildBinaryOnce("")
	if err == nil {
		t.Error("expected error on empty name")
	}
}

func TestBuildBinaryOnce_UnknownCmd(t *testing.T) {
	_, cleanup, err := BuildBinaryOnce("definitely-not-a-real-cmd")
	if err == nil {
		cleanup()
		t.Fatal("expected error building a nonexistent cmd")
	}
}
