// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/mark3labs/mcp-go/server"
	"github.com/syketech/recmeet-tools/mcpserver"
	"github.com/syketech/recmeet-tools/meetingdata"
)

func main() {
	// CRITICAL: Redirect os.Stdout to os.Stderr so that any stray fmt.Print
	// or log output from dependencies does not corrupt the MCP JSON-RPC
	// stream on stdout. The MCP stdio transport writes directly to the
	// original stdout via the writer passed to Listen().
	realStdout := os.Stdout
	os.Stdout = os.Stderr

	errLog := log.New(os.Stderr, "recmeet-mcp: ", log.LstdFlags)

	// Honor RECMEET_CONFIG if set: pass the explicit path through so
	// LoadConfig surfaces a "not found" error instead of silently falling
	// back to defaults. Empty env var preserves the default-path lookup,
	// which silently returns defaults when the file is absent.
	configPath := os.Getenv("RECMEET_CONFIG")
	cfg, err := meetingdata.LoadConfig(configPath)
	if err != nil {
		// On a missing explicit-config error, surface both the path
		// the operator pointed us at AND the default location so the
		// fix is obvious: either drop a file at the default path or
		// fix the RECMEET_CONFIG env var.
		errLog.Printf("load config: %v", err)
		errLog.Printf("hint: set RECMEET_CONFIG to a valid config path, or place a config at %s",
			meetingdata.DefaultConfigPath())
		os.Exit(1)
	}

	mcpSrv := mcpserver.NewServer(cfg)
	stdio := server.NewStdioServer(mcpSrv)
	stdio.SetErrorLogger(errLog)

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	errLog.Println("starting recmeet MCP server on stdio")
	if err := stdio.Listen(ctx, os.Stdin, realStdout); err != nil {
		fmt.Fprintf(os.Stderr, "recmeet-mcp: %v\n", err)
		os.Exit(1)
	}
}
