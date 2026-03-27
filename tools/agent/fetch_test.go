// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestWebFetchTool_Definition(t *testing.T) {
	tool := &WebFetchTool{}
	def := tool.Definition()

	if def.Name != "web_fetch" {
		t.Errorf("expected name web_fetch, got %s", def.Name)
	}
}

func TestWebFetchTool_Execute(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		w.Write([]byte(`<!DOCTYPE html>
<html>
<head><title>Test Page</title></head>
<body>
<h1>Hello World</h1>
<p>This is a test page with some content.</p>
<script>var x = 1;</script>
</body>
</html>`))
	}))
	defer server.Close()

	tool := &WebFetchTool{}
	input, _ := json.Marshal(map[string]string{"url": server.URL})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "Hello World") {
		t.Errorf("expected 'Hello World' in result, got: %s", result)
	}
	if !strings.Contains(result, "test page") {
		t.Errorf("expected 'test page' in result, got: %s", result)
	}
	// Script content should be stripped
	if strings.Contains(result, "var x") {
		t.Errorf("expected script content to be stripped, got: %s", result)
	}
}

func TestWebFetchTool_EmptyURL(t *testing.T) {
	tool := &WebFetchTool{}
	input, _ := json.Marshal(map[string]string{"url": ""})

	result, isError, _ := tool.Execute(context.Background(), input)
	if !isError {
		t.Error("expected error for empty URL")
	}
	if !strings.Contains(result, "required") {
		t.Errorf("expected 'required' in error, got: %s", result)
	}
}

func TestWebFetchTool_HTTPError(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	}))
	defer server.Close()

	tool := &WebFetchTool{}
	input, _ := json.Marshal(map[string]string{"url": server.URL})

	result, isError, _ := tool.Execute(context.Background(), input)
	if !isError {
		t.Error("expected error flag for 404")
	}
	if !strings.Contains(result, "404") {
		t.Errorf("expected '404' in error, got: %s", result)
	}
}

func TestWebFetchTool_Truncation(t *testing.T) {
	// Create a page with more than 10000 chars of body text
	bigContent := strings.Repeat("word ", 5000) // 25000 chars
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		w.Write([]byte("<html><body><p>" + bigContent + "</p></body></html>"))
	}))
	defer server.Close()

	tool := &WebFetchTool{}
	input, _ := json.Marshal(map[string]string{"url": server.URL})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if len(result) > maxFetchLen {
		t.Errorf("expected result to be truncated to %d chars, got %d", maxFetchLen, len(result))
	}
}

func TestExtractText(t *testing.T) {
	html := `<html><body><h1>Title</h1><p>Paragraph</p></body></html>`
	text := extractText(html)
	if !strings.Contains(text, "Title") || !strings.Contains(text, "Paragraph") {
		t.Errorf("expected extracted text to contain Title and Paragraph, got: %s", text)
	}
}

func TestExtractText_NoBody(t *testing.T) {
	html := `<html><head><title>No Body</title></head></html>`
	text := extractText(html)
	// No body content, should be empty
	if text != "" {
		t.Errorf("expected empty text for no-body HTML, got: %q", text)
	}
}

func TestStripTags(t *testing.T) {
	result := stripTags("<b>bold</b> and <i>italic</i>")
	if !strings.Contains(result, "bold") || !strings.Contains(result, "italic") {
		t.Errorf("expected stripped text, got: %s", result)
	}
	if strings.Contains(result, "<") {
		t.Errorf("expected no tags, got: %s", result)
	}
}
