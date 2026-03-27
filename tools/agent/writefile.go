// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// WriteFileTool writes content to a file, creating parent directories as needed.
type WriteFileTool struct{}

func (t *WriteFileTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "write_file",
		Description: "Write content to a file on disk. Creates parent directories if they don't exist.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"path":    map[string]interface{}{"type": "string", "description": "File path to write to"},
				"content": map[string]interface{}{"type": "string", "description": "Content to write"},
			},
			"required": []string{"path", "content"},
		},
	}
}

func (t *WriteFileTool) Execute(_ context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		Path    string `json:"path"`
		Content string `json:"content"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}
	if params.Path == "" {
		return "Error: path is required", true, nil
	}

	dir := filepath.Dir(params.Path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Sprintf("Error creating directory %s: %s", dir, err), true, nil
	}

	if err := os.WriteFile(params.Path, []byte(params.Content), 0o644); err != nil {
		return fmt.Sprintf("Error writing file: %s", err), true, nil
	}

	return fmt.Sprintf("Wrote %d bytes to %s", len(params.Content), params.Path), false, nil
}
