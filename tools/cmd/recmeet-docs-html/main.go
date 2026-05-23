// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// recmeet-docs-html renders a Markdown document containing embedded
// ```mermaid blocks into a self-contained dark-theme HTML page with
// client-side Mermaid.js rendering. Used by `make docs-html` to keep
// docs/COMPONENT-DIAGRAMS.html aligned with its .md source.
//
// Usage:
//
//	recmeet-docs-html <input.md> <output.html>
package main

import (
	"bytes"
	"fmt"
	"html"
	"os"
	"regexp"
	"strings"

	"github.com/yuin/goldmark"
	"github.com/yuin/goldmark/ast"
	"github.com/yuin/goldmark/extension"
	"github.com/yuin/goldmark/parser"
	"github.com/yuin/goldmark/renderer"
	gmhtml "github.com/yuin/goldmark/renderer/html"
	"github.com/yuin/goldmark/text"
	"github.com/yuin/goldmark/util"
)

const htmlTemplate = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>__TITLE__</title>
<style>
  :root {
    --bg: #1e1e2e;
    --surface: #282840;
    --text: #cdd6f4;
    --heading: #89b4fa;
    --link: #74c7ec;
    --border: #45475a;
    --code-bg: #313244;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html { scroll-behavior: smooth; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
    background: var(--bg);
    color: var(--text);
    line-height: 1.6;
    padding: 0;
  }
  .container {
    max-width: 1400px;
    margin: 0 auto;
    padding: 2rem 3rem;
  }
  h1 {
    font-size: 2.2rem;
    color: var(--heading);
    margin-bottom: 0.5rem;
    border-bottom: 2px solid var(--border);
    padding-bottom: 0.8rem;
  }
  h2 {
    font-size: 1.6rem;
    color: var(--heading);
    margin-top: 3rem;
    margin-bottom: 0.5rem;
    border-bottom: 1px solid var(--border);
    padding-bottom: 0.5rem;
  }
  h3 {
    font-size: 1.2rem;
    color: #a6adc8;
    margin-top: 2rem;
    margin-bottom: 0.5rem;
  }
  p, li {
    margin-bottom: 0.5rem;
    max-width: 80ch;
  }
  a { color: var(--link); text-decoration: none; }
  a:hover { text-decoration: underline; }
  hr {
    border: none;
    border-top: 1px solid var(--border);
    margin: 2.5rem 0;
  }
  code {
    background: var(--code-bg);
    padding: 0.15em 0.4em;
    border-radius: 4px;
    font-size: 0.9em;
    font-family: 'JetBrains Mono', 'Fira Code', 'Consolas', monospace;
  }
  ol, ul { padding-left: 1.5rem; margin-bottom: 1rem; }
  .diagram-wrapper {
    background: #ffffff;
    border-radius: 8px;
    padding: 1.5rem;
    margin: 1.5rem 0;
    overflow-x: auto;
    border: 1px solid var(--border);
  }
  .diagram-wrapper .mermaid {
    display: flex;
    justify-content: center;
  }
  .toc {
    background: var(--surface);
    border-radius: 8px;
    padding: 1.5rem 2rem;
    margin: 1.5rem 0 2rem;
    border: 1px solid var(--border);
  }
  .toc ol { columns: 2; column-gap: 2rem; }
  @media (max-width: 800px) {
    .container { padding: 1rem; }
    .toc ol { columns: 1; }
    h1 { font-size: 1.6rem; }
    h2 { font-size: 1.3rem; }
  }
  /* Make mermaid SVGs responsive */
  .mermaid svg {
    max-width: 100%;
    height: auto;
  }
</style>
</head>
<body>
<div class="container">

__BODY__

<hr>
<p style="text-align:center;color:#585b70;margin-top:3rem;font-size:0.85rem;">
  Generated from <code>__SOURCE_PATH__</code> &mdash; recmeet project
</p>

</div>

<script type="module">
  import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
  mermaid.initialize({
    startOnLoad: true,
    theme: 'default',
    securityLevel: 'loose',
    flowchart: {
      useMaxWidth: true,
      htmlLabels: true,
      curve: 'basis'
    },
    stateDiagram: {
      useMaxWidth: true
    }
  });
</script>
</body>
</html>
`

// codeBlockRenderer overrides goldmark's default fenced-code renderer
// so that `mermaid`-tagged fences emit the diagram-wrapper div the
// embedded Mermaid.js script expects, while other fences fall back to
// a standard <pre><code> with HTML escaping.
type codeBlockRenderer struct{}

func (r *codeBlockRenderer) RegisterFuncs(reg renderer.NodeRendererFuncRegisterer) {
	reg.Register(ast.KindFencedCodeBlock, r.renderFenced)
	reg.Register(ast.KindCodeBlock, r.renderIndented)
}

func (r *codeBlockRenderer) renderFenced(w util.BufWriter, source []byte, node ast.Node, entering bool) (ast.WalkStatus, error) {
	if !entering {
		return ast.WalkContinue, nil
	}
	n := node.(*ast.FencedCodeBlock)
	lang := string(n.Language(source))

	if lang == "mermaid" {
		w.WriteString(`<div class="diagram-wrapper"><pre class="mermaid">` + "\n")
		writeLines(w, n, source)
		w.WriteString(`</pre></div>` + "\n")
		return ast.WalkSkipChildren, nil
	}

	w.WriteString(`<pre><code`)
	if lang != "" {
		fmt.Fprintf(w, ` class="language-%s"`, html.EscapeString(lang))
	}
	w.WriteString(`>`)
	writeLines(w, n, source)
	w.WriteString(`</code></pre>` + "\n")
	return ast.WalkSkipChildren, nil
}

func (r *codeBlockRenderer) renderIndented(w util.BufWriter, source []byte, node ast.Node, entering bool) (ast.WalkStatus, error) {
	if !entering {
		return ast.WalkContinue, nil
	}
	n := node.(*ast.CodeBlock)
	w.WriteString(`<pre><code>`)
	writeLines(w, n, source)
	w.WriteString(`</code></pre>` + "\n")
	return ast.WalkSkipChildren, nil
}

// writeLines emits the lines of a code-block node with the minimal
// element-content HTML escape (`<`, `>`, `&`). Escaping these matters
// inside <pre class="mermaid"> because mermaid comments occasionally
// include angle-bracketed placeholders (e.g. `context_<ts>.json`) that
// a browser's HTML parser would otherwise strip from the textContent
// Mermaid.js reads back. We deliberately do NOT escape `"` or `'`
// (the html.EscapeString defaults) — those don't require escaping in
// element content and their absence preserves the readable mermaid
// node-label syntax (e.g. `CLI["recmeet"]`) in the generated HTML.
func writeLines(w util.BufWriter, node interface {
	Lines() *text.Segments
}, source []byte) {
	lines := node.Lines()
	for i := 0; i < lines.Len(); i++ {
		seg := lines.At(i)
		w.WriteString(escapeElementText(string(seg.Value(source))))
	}
}

func escapeElementText(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	return s
}

var sectionHeadingRE = regexp.MustCompile(`^(\d+)\.\s+(.+)$`)

type tocEntry struct {
	id    string
	label string
}

func extractText(node ast.Node, source []byte) string {
	var buf bytes.Buffer
	ast.Walk(node, func(n ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering {
			return ast.WalkContinue, nil
		}
		if t, ok := n.(*ast.Text); ok {
			buf.Write(t.Segment.Value(source))
		}
		return ast.WalkContinue, nil
	})
	return buf.String()
}

func findFirstH1(doc ast.Node, source []byte) string {
	var found string
	ast.Walk(doc, func(n ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering || found != "" {
			return ast.WalkContinue, nil
		}
		if h, ok := n.(*ast.Heading); ok && h.Level == 1 {
			found = extractText(h, source)
			return ast.WalkStop, nil
		}
		return ast.WalkContinue, nil
	})
	return found
}

func collectTOC(doc ast.Node, source []byte) []tocEntry {
	var entries []tocEntry
	ast.Walk(doc, func(n ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering {
			return ast.WalkContinue, nil
		}
		h, ok := n.(*ast.Heading)
		if !ok || h.Level != 2 {
			return ast.WalkContinue, nil
		}
		txt := extractText(h, source)
		if m := sectionHeadingRE.FindStringSubmatch(txt); m != nil {
			entries = append(entries, tocEntry{id: "s" + m[1], label: m[2]})
		}
		return ast.WalkContinue, nil
	})
	return entries
}

func renderTOC(entries []tocEntry) string {
	if len(entries) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString(`<nav class="toc">` + "\n")
	b.WriteString(`<h3>Table of Contents</h3>` + "\n")
	b.WriteString(`<ol>` + "\n")
	for _, e := range entries {
		fmt.Fprintf(&b, "  <li><a href=\"#%s\">%s</a></li>\n", e.id, html.EscapeString(e.label))
	}
	b.WriteString(`</ol>` + "\n")
	b.WriteString(`</nav>` + "\n")
	return b.String()
}

// h2IDRewriteRE matches a goldmark-auto-assigned <h2 id="..."> opening
// tag whose visible text begins with `<digits>. ` so the id can be
// rewritten to the s<N> anchor scheme the TOC links to.
var h2IDRewriteRE = regexp.MustCompile(`<h2 id="[^"]*">(\d+)\.\s`)

func rewriteH2IDs(body string) string {
	return h2IDRewriteRE.ReplaceAllStringFunc(body, func(match string) string {
		m := h2IDRewriteRE.FindStringSubmatch(match)
		return fmt.Sprintf(`<h2 id="s%s">%s. `, m[1], m[1])
	})
}

// injectTOC inserts the TOC + a horizontal rule immediately before the
// first numbered <h2 in body so the TOC sits between the intro paragraph
// and the first numbered section, matching the hand-rolled layout.
func injectTOC(body, tocHTML string) string {
	if tocHTML == "" {
		return body
	}
	m := firstNumberedH2RE.FindStringIndex(body)
	if m == nil {
		return body
	}
	return body[:m[0]] + tocHTML + "\n<hr>\n\n" + body[m[0]:]
}

var firstNumberedH2RE = regexp.MustCompile(`<h2 id="s\d+">`)

// manualTOCRE matches a hand-written `## Table of Contents` section
// (the h2 plus the immediately following <ol>) so it can be stripped
// from the rendered body — the auto-generated <nav class="toc"> at the
// top of the page is the canonical TOC. The manual section stays in the
// .md for GitHub readers who view the raw source.
var manualTOCRE = regexp.MustCompile(`(?s)<h2 id="table-of-contents">Table of Contents</h2>\s*<ol>.*?</ol>\s*`)

func stripManualTOC(body string) string {
	return manualTOCRE.ReplaceAllString(body, "")
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <input.md> <output.html>\n", os.Args[0])
		os.Exit(2)
	}
	inputPath := os.Args[1]
	outputPath := os.Args[2]

	source, err := os.ReadFile(inputPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read %s: %v\n", inputPath, err)
		os.Exit(1)
	}

	md := goldmark.New(
		goldmark.WithExtensions(extension.GFM),
		goldmark.WithParserOptions(parser.WithAutoHeadingID()),
		goldmark.WithRendererOptions(
			gmhtml.WithUnsafe(),
			renderer.WithNodeRenderers(util.Prioritized(&codeBlockRenderer{}, 50)),
		),
	)

	doc := md.Parser().Parse(text.NewReader(source))

	title := "Component Interaction Diagrams"
	if h1 := findFirstH1(doc, source); h1 != "" {
		title = h1
	}

	tocEntries := collectTOC(doc, source)

	var bodyBuf bytes.Buffer
	if err := md.Renderer().Render(&bodyBuf, source, doc); err != nil {
		fmt.Fprintf(os.Stderr, "render: %v\n", err)
		os.Exit(1)
	}
	body := bodyBuf.String()
	body = rewriteH2IDs(body)
	body = stripManualTOC(body)
	body = injectTOC(body, renderTOC(tocEntries))

	out := htmlTemplate
	out = strings.ReplaceAll(out, "__TITLE__", html.EscapeString("recmeet — "+title))
	out = strings.ReplaceAll(out, "__SOURCE_PATH__", html.EscapeString(inputPath))
	out = strings.ReplaceAll(out, "__BODY__", body)

	if err := os.WriteFile(outputPath, []byte(out), 0644); err != nil {
		fmt.Fprintf(os.Stderr, "write %s: %v\n", outputPath, err)
		os.Exit(1)
	}
}
