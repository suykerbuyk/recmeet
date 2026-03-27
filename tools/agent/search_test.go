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

func TestWebSearchTool_Definition(t *testing.T) {
	tool := &WebSearchTool{APIKey: "test"}
	def := tool.Definition()

	if def.Name != "web_search" {
		t.Errorf("expected name web_search, got %s", def.Name)
	}
}

func TestWebSearchTool_Execute(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Subscription-Token") != "test-key" {
			t.Errorf("expected API key header, got %q", r.Header.Get("X-Subscription-Token"))
		}
		if r.URL.Query().Get("q") != "golang testing" {
			t.Errorf("expected query 'golang testing', got %q", r.URL.Query().Get("q"))
		}

		resp := braveResponse{}
		resp.Web.Results = []struct {
			Title       string `json:"title"`
			URL         string `json:"url"`
			Description string `json:"description"`
		}{
			{Title: "Go Testing", URL: "https://go.dev/doc/test", Description: "How to test in Go"},
			{Title: "Go Blog", URL: "https://go.dev/blog", Description: "The Go Blog"},
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(resp)
	}))
	defer server.Close()

	tool := &WebSearchTool{APIKey: "test-key", BaseURL: server.URL}
	input, _ := json.Marshal(map[string]string{"query": "golang testing"})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Errorf("unexpected tool error: %s", result)
	}
	if !strings.Contains(result, "Go Testing") {
		t.Errorf("expected result to contain 'Go Testing', got: %s", result)
	}
	if !strings.Contains(result, "https://go.dev/doc/test") {
		t.Errorf("expected result to contain URL, got: %s", result)
	}
}

func TestWebSearchTool_EmptyQuery(t *testing.T) {
	tool := &WebSearchTool{APIKey: "test"}
	input, _ := json.Marshal(map[string]string{"query": ""})

	result, isError, _ := tool.Execute(context.Background(), input)
	if !isError {
		t.Error("expected error for empty query")
	}
	if !strings.Contains(result, "required") {
		t.Errorf("expected 'required' in error, got: %s", result)
	}
}

func TestWebSearchTool_APIError(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
		w.Write([]byte(`{"error": "invalid key"}`))
	}))
	defer server.Close()

	tool := &WebSearchTool{APIKey: "bad-key", BaseURL: server.URL}
	input, _ := json.Marshal(map[string]string{"query": "test"})

	result, isError, _ := tool.Execute(context.Background(), input)
	if !isError {
		t.Error("expected error flag for HTTP 401")
	}
	if !strings.Contains(result, "401") {
		t.Errorf("expected 401 in error, got: %s", result)
	}
}

func TestWebSearchTool_NoResults(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		resp := braveResponse{}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(resp)
	}))
	defer server.Close()

	tool := &WebSearchTool{APIKey: "test", BaseURL: server.URL}
	input, _ := json.Marshal(map[string]string{"query": "test"})

	result, isError, err := tool.Execute(context.Background(), input)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if isError {
		t.Error("expected no error flag for empty results")
	}
	if !strings.Contains(result, "No results") {
		t.Errorf("expected 'No results' message, got: %s", result)
	}
}
