// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"

	"golang.org/x/net/html"
)

const maxFetchLen = 10000

// WebFetchTool fetches a URL and extracts text content from the HTML.
type WebFetchTool struct{}

func (t *WebFetchTool) Definition() ToolDefinition {
	return ToolDefinition{
		Name:        "web_fetch",
		Description: "Fetch a URL and extract its text content. Returns up to 10000 characters of body text.",
		InputSchema: map[string]interface{}{
			"type": "object",
			"properties": map[string]interface{}{
				"url": map[string]interface{}{"type": "string", "description": "URL to fetch"},
			},
			"required": []string{"url"},
		},
	}
}

func (t *WebFetchTool) Execute(ctx context.Context, inputJSON []byte) (string, bool, error) {
	var params struct {
		URL string `json:"url"`
	}
	if err := json.Unmarshal(inputJSON, &params); err != nil {
		return "", true, fmt.Errorf("invalid input: %w", err)
	}
	if params.URL == "" {
		return "Error: url is required", true, nil
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, params.URL, nil)
	if err != nil {
		return "", true, fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("User-Agent", "recmeet-agent/1.0")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Sprintf("Error fetching URL: %s", err), true, nil
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Sprintf("HTTP %d fetching %s", resp.StatusCode, params.URL), true, nil
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Sprintf("Error reading response: %s", err), true, nil
	}

	text := extractText(string(body))
	if len(text) > maxFetchLen {
		text = text[:maxFetchLen]
	}
	if text == "" {
		return "(no text content found)", false, nil
	}
	return text, false, nil
}

// extractText strips HTML tags and returns body text content.
func extractText(rawHTML string) string {
	doc, err := html.Parse(strings.NewReader(rawHTML))
	if err != nil {
		// Fallback: return raw with tags stripped
		return stripTags(rawHTML)
	}

	var sb strings.Builder
	var inBody bool
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if n.Type == html.ElementNode {
			switch n.Data {
			case "script", "style", "noscript":
				return
			case "body":
				inBody = true
			}
		}
		if inBody && n.Type == html.TextNode {
			text := strings.TrimSpace(n.Data)
			if text != "" {
				sb.WriteString(text)
				sb.WriteString(" ")
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(doc)
	return strings.TrimSpace(sb.String())
}

// stripTags is a simple fallback tag stripper.
func stripTags(s string) string {
	var sb strings.Builder
	inTag := false
	for _, r := range s {
		switch {
		case r == '<':
			inTag = true
		case r == '>':
			inTag = false
			sb.WriteRune(' ')
		case !inTag:
			sb.WriteRune(r)
		}
	}
	return strings.TrimSpace(sb.String())
}
