// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWriteFileTool_Definition(t *testing.T) {
	tool := &WriteFileTool{}
	def := tool.Definition()

	if def.Name != "write_file" {
		t.Errorf("expected name write_file, got %s", def.Name)
	}
}

func TestWriteFileTool_Execute(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "output.txt")

	tool := &WriteFileTool{}
	input, _ := json.Marshal(map[string]string{
		"path":    path,
		"content": "hello world",
	})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "11 bytes") {
		t.Errorf("expected byte count in result, got: %s", result)
	}

	// Verify file was written
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read file: %v", err)
	}
	if string(data) != "hello world" {
		t.Errorf("expected 'hello world', got %q", string(data))
	}
}

func TestWriteFileTool_CreatesParentDirs(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "sub", "deep", "file.txt")

	tool := &WriteFileTool{}
	input, _ := json.Marshal(map[string]string{
		"path":    path,
		"content": "nested",
	})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read file: %v", err)
	}
	if string(data) != "nested" {
		t.Errorf("expected 'nested', got %q", string(data))
	}
}

func TestWriteFileTool_EmptyPath(t *testing.T) {
	tool := &WriteFileTool{}
	input, _ := json.Marshal(map[string]string{
		"path":    "",
		"content": "test",
	})

	result, isError, _ := tool.Execute(context.Background(), input)
	if !isError {
		t.Error("expected error for empty path")
	}
	if !strings.Contains(result, "required") {
		t.Errorf("expected 'required' in error, got: %s", result)
	}
}
