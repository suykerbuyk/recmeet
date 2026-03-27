// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
)

// WebSearchTool performs web searches via the Brave Search API.
type WebSearchTool struct {
	APIKey  string
	BaseURL string // override for testing; empty = production
}

func (t *WebSearchTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "web_search",
		Description: "Search the web using the Brave Search API. Returns top 5 results with title, URL, and description.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"query": map[string]interface{}{"type": "string", "description": "Search query"},
			},
			"required": []string{"query"},
		},
	}
}

func (t *WebSearchTool) Execute(ctx context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		Query string `json:"query"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}
	if params.Query == "" {
		return "Error: query is required", true, nil
	}

	baseURL := t.BaseURL
	if baseURL == "" {
		baseURL = "https://api.search.brave.com/res/v1/web/search"
	}

	reqURL := fmt.Sprintf("%s?q=%s&count=5", baseURL, url.QueryEscape(params.Query))
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, reqURL, nil)
	if err != nil {
		return "", true, fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Subscription-Token", t.APIKey)

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Sprintf("Error: %s", err), true, nil
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Sprintf("Error reading response: %s", err), true, nil
	}

	if resp.StatusCode != http.StatusOK {
		return fmt.Sprintf("Brave API error (HTTP %d): %s", resp.StatusCode, string(body)), true, nil
	}

	return formatBraveResults(body)
}

type braveResponse struct {
	Web struct {
		Results []struct {
			Title       string `json:"title"`
			URL         string `json:"url"`
			Description string `json:"description"`
		} `json:"results"`
	} `json:"web"`
}

func formatBraveResults(body []byte) (string, bool, error) {
	var resp braveResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		return fmt.Sprintf("Error parsing response: %s", err), true, nil
	}

	if len(resp.Web.Results) == 0 {
		return "No results found.", false, nil
	}

	var sb strings.Builder
	for i, r := range resp.Web.Results {
		if i >= 5 {
			break
		}
		fmt.Fprintf(&sb, "%d. %s\n   %s\n   %s\n\n", i+1, r.Title, r.URL, r.Description)
	}
	return sb.String(), false, nil
}
