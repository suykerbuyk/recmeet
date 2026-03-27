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

	cfg, err := meetingdata.LoadConfig("")
	if err != nil {
		errLog.Fatalf("load config: %v", err)
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
