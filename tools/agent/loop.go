// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"

	anthropic "github.com/anthropics/anthropic-sdk-go"
	"github.com/anthropics/anthropic-sdk-go/option"
	"github.com/anthropics/anthropic-sdk-go/packages/param"
)

// AnthropicClient is an interface for testability, matching the
// anthropic SDK's Messages.New method.
type AnthropicClient interface {
	CreateMessage(ctx context.Context, params anthropic.MessageNewParams) (*anthropic.Message, error)
}

// sdkClient wraps the real anthropic.Client.
type sdkClient struct {
	client *anthropic.Client
}

func (c *sdkClient) CreateMessage(ctx context.Context, params anthropic.MessageNewParams) (*anthropic.Message, error) {
	return c.client.Messages.New(ctx, params)
}

// NewSDKClient creates an AnthropicClient backed by the real Anthropic SDK.
func NewSDKClient(apiKey string) AnthropicClient {
	client := anthropic.NewClient(option.WithAPIKey(apiKey))
	return &sdkClient{client: &client}
}

// Loop runs the agentic tool-use loop against the Anthropic API.
type Loop struct {
	client   AnthropicClient
	model    string
	registry *ToolRegistry
	maxIter  int
	verbose  bool
}

// NewLoop creates a new agentic loop.
func NewLoop(client AnthropicClient, model string, registry *ToolRegistry, maxIter int, verbose bool) *Loop {
	return &Loop{
		client:   client,
		model:    model,
		registry: registry,
		maxIter:  maxIter,
		verbose:  verbose,
	}
}

// Run executes the agentic loop: call Claude, handle tool use, repeat.
// Returns the final text response.
func (l *Loop) Run(ctx context.Context, systemPrompt string, userMessage string) (string, error) {
	messages := []anthropic.MessageParam{
		anthropic.NewUserMessage(anthropic.NewTextBlock(userMessage)),
	}

	tools := l.buildToolDefs()

	for iter := range l.maxIter {
		if l.verbose {
			log.Printf("[agent] iteration %d/%d", iter+1, l.maxIter)
		}

		params := anthropic.MessageNewParams{
			Model:     l.model,
			MaxTokens: 4096,
			Messages:  messages,
			Tools:     tools,
		}
		if systemPrompt != "" {
			params.System = []anthropic.TextBlockParam{
				{Text: systemPrompt},
			}
		}

		resp, err := l.client.CreateMessage(ctx, params)
		if err != nil {
			return "", fmt.Errorf("API call failed: %w", err)
		}

		if l.verbose {
			log.Printf("[agent] stop_reason=%s, content_blocks=%d", resp.StopReason, len(resp.Content))
		}

		// Check for end_turn: extract text and return
		if resp.StopReason == anthropic.StopReasonEndTurn {
			return extractTextFromContent(resp.Content), nil
		}

		// Handle tool_use: execute tools and build result messages
		if resp.StopReason == anthropic.StopReasonToolUse {
			// Add assistant response to conversation
			assistantBlocks := contentToParamBlocks(resp.Content)
			messages = append(messages, anthropic.NewAssistantMessage(assistantBlocks...))

			// Execute each tool call and collect results
			var toolResults []anthropic.ContentBlockParamUnion
			for _, block := range resp.Content {
				if block.Type != "tool_use" {
					continue
				}

				toolName := block.Name
				toolID := block.ID
				inputJSON := []byte(block.Input)

				if l.verbose {
					log.Printf("[agent] tool_use: %s (id=%s)", toolName, toolID)
				}

				tool := l.registry.Get(toolName)
				if tool == nil {
					toolResults = append(toolResults,
						anthropic.NewToolResultBlock(toolID, fmt.Sprintf("Unknown tool: %s", toolName), true))
					continue
				}

				result, isError, err := tool.Execute(ctx, inputJSON)
				if err != nil {
					toolResults = append(toolResults,
						anthropic.NewToolResultBlock(toolID, fmt.Sprintf("Tool error: %s", err), true))
					continue
				}

				toolResults = append(toolResults,
					anthropic.NewToolResultBlock(toolID, result, isError))
			}

			messages = append(messages, anthropic.NewUserMessage(toolResults...))
			continue
		}

		// Other stop reasons (max_tokens, etc.): return what we have
		return extractTextFromContent(resp.Content), nil
	}

	return "", fmt.Errorf("max iterations (%d) reached without completion", l.maxIter)
}

// buildToolDefs converts registered tools to Anthropic API tool definitions.
func (l *Loop) buildToolDefs() []anthropic.ToolUnionParam {
	allTools := l.registry.All()
	defs := make([]anthropic.ToolUnionParam, 0, len(allTools))
	for _, t := range allTools {
		def := t.Definition()

		// Extract required fields from the schema if present
		var required []string
		if req, ok := def.InputSchema["required"]; ok {
			if arr, ok := req.([]string); ok {
				required = arr
			}
		}

		// Extract properties
		properties := def.InputSchema["properties"]

		tp := anthropic.ToolParam{
			Name: def.Name,
			Description: param.NewOpt(def.Description),
			InputSchema: anthropic.ToolInputSchemaParam{
				Properties: properties,
				Required:   required,
			},
		}
		defs = append(defs, anthropic.ToolUnionParam{OfTool: &tp})
	}
	return defs
}

// extractTextFromContent extracts concatenated text from content blocks.
func extractTextFromContent(content []anthropic.ContentBlockUnion) string {
	var parts []string
	for _, block := range content {
		if block.Type == "text" && block.Text != "" {
			parts = append(parts, block.Text)
		}
	}
	return strings.Join(parts, "\n")
}

// contentToParamBlocks converts response content blocks to param blocks
// for including in the next request.
func contentToParamBlocks(content []anthropic.ContentBlockUnion) []anthropic.ContentBlockParamUnion {
	blocks := make([]anthropic.ContentBlockParamUnion, 0, len(content))
	for _, block := range content {
		switch block.Type {
		case "text":
			blocks = append(blocks, anthropic.NewTextBlock(block.Text))
		case "tool_use":
			// Re-serialize the input as-is
			var input any
			if len(block.Input) > 0 {
				_ = json.Unmarshal(block.Input, &input)
			}
			blocks = append(blocks, anthropic.NewToolUseBlock(block.ID, input, block.Name))
		case "thinking":
			blocks = append(blocks, anthropic.NewThinkingBlock(block.Signature, block.Thinking))
		case "redacted_thinking":
			blocks = append(blocks, anthropic.NewRedactedThinkingBlock(block.Data))
		}
	}
	return blocks
}
