// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package main

import (
	"context"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"
	"github.com/syketech/recmeet-tools/agent"
)

func main() {
	if err := rootCmd().Execute(); err != nil {
		os.Exit(1)
	}
}

func rootCmd() *cobra.Command {
	root := &cobra.Command{
		Use:   "recmeet-agent",
		Short: "AI agent for meeting preparation and follow-up",
	}

	root.AddCommand(prepCmd())
	root.AddCommand(followUpCmd())

	return root
}

func prepCmd() *cobra.Command {
	var (
		participants string
		agendaURL    string
		output       string
		model        string
		verbose      bool
		dryRun       bool
		configPath   string
	)

	cmd := &cobra.Command{
		Use:   "prep [description]",
		Short: "Prepare context for an upcoming meeting",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			description := args[0]

			cfg, err := agent.LoadAgentConfig(configPath)
			if err != nil {
				return fmt.Errorf("load config: %w", err)
			}

			if model != "" {
				cfg.Model = model
			}
			if cfg.ContextDir == "" {
				cfg.ContextDir = "."
			}

			var parts []string
			if participants != "" {
				for _, p := range strings.Split(participants, ",") {
					parts = append(parts, strings.TrimSpace(p))
				}
			}

			ctx := context.Background()
			result, err := agent.PrepWorkflow(ctx, cfg, description, parts, agendaURL, output, verbose, dryRun)
			if err != nil {
				return err
			}

			if dryRun {
				fmt.Println(result)
			} else {
				fmt.Printf("Context written to: %s\n", result)
			}
			return nil
		},
	}

	cmd.Flags().StringVar(&participants, "participants", "", "Comma-separated participant names")
	cmd.Flags().StringVar(&agendaURL, "agenda-url", "", "URL to meeting agenda")
	cmd.Flags().StringVar(&output, "output", "", "Output file path")
	cmd.Flags().StringVar(&model, "model", "", "LLM model to use (default: claude-sonnet-4-6)")
	cmd.Flags().BoolVar(&verbose, "verbose", false, "Verbose output")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Print prompts without calling API")
	cmd.Flags().StringVar(&configPath, "config", "", "Config file path")

	return cmd
}

func followUpCmd() *cobra.Command {
	var (
		outputDir  string
		myName     string
		model      string
		verbose    bool
		dryRun     bool
		configPath string
	)

	cmd := &cobra.Command{
		Use:   "follow-up [note-path]",
		Short: "Process meeting notes and draft follow-up communications",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			notePath := args[0]

			cfg, err := agent.LoadAgentConfig(configPath)
			if err != nil {
				return fmt.Errorf("load config: %w", err)
			}

			if model != "" {
				cfg.Model = model
			}
			if outputDir == "" {
				outputDir = "."
			}

			ctx := context.Background()
			result, err := agent.FollowUpWorkflow(ctx, cfg, notePath, outputDir, myName, verbose, dryRun)
			if err != nil {
				return err
			}

			fmt.Println(result)
			return nil
		},
	}

	cmd.Flags().StringVar(&outputDir, "output-dir", "", "Directory for output drafts")
	cmd.Flags().StringVar(&myName, "my-name", "", "Your name for drafting messages")
	cmd.Flags().StringVar(&model, "model", "", "LLM model to use")
	cmd.Flags().BoolVar(&verbose, "verbose", false, "Verbose output")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Print prompts without calling API")
	cmd.Flags().StringVar(&configPath, "config", "", "Config file path")

	return cmd
}
