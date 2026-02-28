#!/usr/bin/env bash
# claude-init — Bootstrap a project for AI-assisted pair programming with Claude Code
#
# Creates the scaffolding for agentic AI sessions:
#   CLAUDE.md              — Thin entry point with workflow rules (never committed)
#   RESUME.md              — Living project context document (never committed)
#   .claude/commands/      — Session management commands (restart, wrap)
#   tasks/                 — Work ticket tracking directory
#   .gitignore entries     — Ensures AI artifacts stay out of source control
#
# Usage:
#   claude-init                  # scaffold current directory
#   claude-init "Project Name"   # scaffold with a project name pre-filled
#   claude-init --force          # overwrite existing files
#
# Install:
#   cp claude-init ~/.local/bin/ && chmod +x ~/.local/bin/claude-init

set -euo pipefail

# --- Configuration -----------------------------------------------------------

FORCE=false
PROJECT_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --force | -f)
        FORCE=true
        shift
        ;;
    --help | -h)
        sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
        exit 0
        ;;
    *)
        PROJECT_NAME="$1"
        shift
        ;;
    esac
done

# --- Helpers -----------------------------------------------------------------

# Write a file only if it doesn't exist (or --force is set)
safe_write() {
    local path="$1"
    local content="$2"
    local dir
    dir="$(dirname "$path")"

    [[ -d "$dir" ]] || mkdir -p "$dir"

    if [[ -f "$path" ]] && ! $FORCE; then
        echo "  SKIP  $path (exists, use --force to overwrite)"
        return 0
    fi

    printf '%s' "$content" >"$path"
    if [[ -f "$path" ]] && ! $FORCE; then
        echo "  NEW   $path"
    else
        echo "  WRITE $path"
    fi
}

# Append a line to .gitignore if it's not already present
gitignore_ensure() {
    local entry="$1"
    local gitignore=".gitignore"

    # Create .gitignore if it doesn't exist
    [[ -f "$gitignore" ]] || touch "$gitignore"

    if ! grep -qxF "$entry" "$gitignore"; then
        # Add a newline before appending if file doesn't end with one
        if [[ -s "$gitignore" ]] && [[ "$(tail -c1 "$gitignore")" != "" ]]; then
            echo "" >>"$gitignore"
        fi
        echo "$entry" >>"$gitignore"
        echo "  IGNORE $entry"
    fi
}

# --- Preamble ----------------------------------------------------------------

echo "claude-init: Setting up AI pair programming scaffolding..."
echo ""

# Warn if not in a git repo
if ! git rev-parse --is-inside-work-tree &>/dev/null; then
    echo "  WARN  Not inside a git repository. Run 'git init' first for .gitignore management."
    echo ""
fi

# --- Create directory structure ----------------------------------------------

[[ -d ".claude/commands" ]] || mkdir -p ".claude/commands"
[[ -d "tasks" ]] || mkdir -p "tasks"
[[ -d "tasks/done" ]] || mkdir -p "tasks/done"

# --- CLAUDE.md ---------------------------------------------------------------

safe_write "CLAUDE.md" "$(
    cat <<'HEREDOC'
# CLAUDE.md

Read `RESUME.md` for full project context, architecture, design decisions,
test results, and task management procedures.

## Workflow Rules

- **Never commit without explicit human permission.** You may stage files
  (`git add`) and update `commit.msg` freely, but the actual `git commit`
  requires the human to say so. Normally the human reviews staged changes
  and commits themselves.
- **Never commit AI-generated artifacts or context files.** `CLAUDE.md`,
  `RESUME.md`, `HISTORY.md`, `commit.msg`, and `tasks/` are local-only session
  context — they must never appear in git history. Do not stage or commit them
  unless the human explicitly directs otherwise.
- **Git commit messages are the project's history.** The commit messages
  themselves are the sole documentation of how and why this project evolved
  over time and the objectives it was trying to achieve. Write them
  accordingly — they should be clear, detailed, and self-sufficient.
- On `/wrap` (session end): stage files, update `commit.msg`, manage tasks
  per `RESUME.md` procedures, but do not commit.
HEREDOC
)"

# --- RESUME.md ---------------------------------------------------------------

# Use project name if provided, otherwise placeholder
if [[ -n "$PROJECT_NAME" ]]; then
    RESUME_TITLE="# ${PROJECT_NAME} — Session Resume Context"
else
    RESUME_TITLE="# Project Name — Session Resume Context"
fi

safe_write "RESUME.md" "$(
    cat <<HEREDOC
${RESUME_TITLE}

## Project Summary

<!-- One paragraph: what it is, what it does, key technologies. -->
<!-- Updated by the AI during /wrap to reflect current state. -->

## Quick Reference

<!-- Common commands — copy-paste ready. Keep this short. -->

- **Build**: \`<build command>\`
- **Test**: \`<test command>\`
- **Run**: \`<run command>\`
- **Lint**: \`<lint command>\`

## Project History

See \`HISTORY.md\` for full iteration narratives.

<!-- Compact summary table — one row per iteration. Full narratives live in
     HISTORY.md (append-only archive). On /wrap, add a new row here AND
     append the full narrative to HISTORY.md. -->

| # | Summary | Key Changes |
|---|---------|-------------|

## File Inventory

<!-- Tree-style listing of all project files with one-line descriptions.
     Updated during /wrap to reflect new/moved/deleted files.

     Format:
     \`\`\`
     project/
     ├── src/
     │   ├── main.cpp          # Entry point
     │   └── lib.cpp           # Core library
     ├── tests/
     │   └── test_lib.cpp      # Unit tests
     └── CMakeLists.txt        # Build system
     \`\`\`
-->

## Architecture

<!-- Describe the build system, core pipeline/flow, module responsibilities,
     and key abstractions. Use ASCII diagrams for data flow.

     Subsections to consider (adapt to your project):
     - Build System
     - Core Pipeline / Data Flow
     - Module Responsibilities (table format)
     - Exception/Error Handling Strategy
     - Threading / Concurrency Model
-->

## Key Design Decisions

<!-- Numbered list of architectural decisions with rationale.
     Format:
     1. **Decision title**: Explanation of what was chosen and why.
        Include alternatives considered and why they were rejected.

     This section is append-only — decisions are never removed, only
     superseded by later decisions that reference the original. -->

## Test Suite

<!-- Test count, framework, tag organization, run commands.
     Updated during /wrap when tests change.

     Include:
     - Total test count and assertion count
     - Table of test tags/groups with coverage description
     - Run commands for common scenarios
-->

## Document Structure

| Document | Purpose | Loaded |
|----------|---------|--------|
| \`RESUME.md\` | Active working context: summary, architecture, design decisions, test suite, known issues, task management | Always (on every \`/restart\`) |
| \`HISTORY.md\` | Append-only archive: full iteration narratives and completed implementation plans | On demand (when historical detail is needed) |
| \`CLAUDE.md\` | AI workflow rules, pointer to RESUME.md | Always (auto-loaded by Claude Code) |

**Standard operating procedure:**
- On \`/wrap\`: append new iteration narrative to \`HISTORY.md\`, add summary row to the Project History table in \`RESUME.md\`
- Completed plans move from \`RESUME.md\` to \`HISTORY.md\` once the plan is fully implemented and verified
- \`HISTORY.md\` is never truncated or rewritten — only appended to
HEREDOC
)"

# --- HISTORY.md --------------------------------------------------------------

# Use project name if provided, otherwise placeholder
if [[ -n "$PROJECT_NAME" ]]; then
    HISTORY_TITLE="# ${PROJECT_NAME} — Project History"
else
    HISTORY_TITLE="# Project Name — Project History"
fi

safe_write "HISTORY.md" "$(
    cat <<HEREDOC
${HISTORY_TITLE}

## Iteration Narratives

<!-- Full iteration paragraphs are appended here during /wrap.
     Each paragraph describes what changed and why, in past tense.
     Format: "A Nth iteration [did X]. [Why]. [Technical details]." -->

## Completed Plans

<!-- Completed implementation plans are moved here from RESUME.md
     once the plan is fully implemented and verified. -->
HEREDOC
)"

# --- .claude/commands/restart.md ---------------------------------------------

safe_write ".claude/commands/restart.md" "$(
    cat <<'HEREDOC'
## Restoring full AI thread context:

Read `CLAUDE.md` for AI-specific workflow rules, then read each document it
references in referenced order to restore full project context:

1. `RESUME.md` — project purpose, architecture, design decisions, test results
2. `BUILD.md` — build system tutorial (if it exists)
3. `tasks/*.md` — open bugs and planned improvements
4. `HISTORY.md` — iteration narratives and completed plans (read on demand, not required for routine work)

After reading, briefly confirm what you loaded and note the current state:
test count, open tasks, and what was last worked on based on recent git history.

## Workflow Orchestration

#### 1. Plan Mode Default
- Enter plan mode for ANY non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STOP and re-plan immediately - don't keep pushing
- Use plan mode for verification steps, not just building
- Write detailed specs upfront to reduce ambiguity

#### 2. Subagent Strategy
- Use subagents liberally to keep main context window clean
- Offload research, exploration, and parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One tack per subagent for focused execution

#### 3. Self-Improvement Loop
- After ANY correction from the user: update `tasks/lessons.md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

#### 4. Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

#### 5. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes - don't over-engineer
- Challenge your own work before presenting it

#### 6. Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests - then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management

1. **Plan First**: Write plan to `tasks/todo.md` with checkable items
2. **Verify Plan**: Check in before starting implementation
3. **Track Progress**: Mark items complete as you go
4. **Explain Changes**: High-level summary at each step
5. **Document Results**: Add review section to `tasks/todo.md`
6. **Capture Lessons**: Update `tasks/lessons.md` after corrections

## Core Principles

- **Simplicity First**: Make every change as simple as possible. Impact minimal code.
- **No Laziness**: Find root causes. No temporary fixes. Senior developer standards.
- **Minimal Impact**: Changes should only touch what's necessary. Avoid introducing bugs.
HEREDOC
)"

# --- .claude/commands/wrap.md ------------------------------------------------

safe_write ".claude/commands/wrap.md" "$(
    cat <<'HEREDOC'
Update RESUME.md and its dependent documents (CLAUDE.md) to fully reflect the
current state of the project. These files serve as the single source of truth
for restoring AI thread context and resuming work on this codebase.

Specifically:
- Read the current RESUME.md and CLAUDE.md
- Compare against the actual codebase state (files, tests, architecture)
- Update all sections that are stale: file inventory, test counts, module
  descriptions, architecture diagrams, design decisions, and test results
- Append a new iteration narrative to HISTORY.md describing what changed
  in this session and why (past tense, technical detail)
- Add a corresponding summary row to the Project History table in RESUME.md
- Move any completed plans from RESUME.md to the Completed Plans section
  in HISTORY.md, replacing them with a single-line pointer
- Retire completed tasks: check each file in tasks/ (not tasks/done/) against
  the session's work and RESUME.md history — if a task has been implemented and
  committed, update its status to "Done", move it to tasks/done/, and update
  the RESUME.md file inventory accordingly
- Rewrite commit.msg to document all code changes made in this session
- Stage all modified and newly added project files (use git add with explicit
  file paths — never use git add -A or git add .)

Do not add "Co-Authored-By" lines to commit messages or source files.

Do not ask for confirmation — just do the updates, stage the files, show what
changed, and note that the user should review before committing.
HEREDOC
)"

# --- .gitignore management ---------------------------------------------------

echo ""
echo "Updating .gitignore..."

gitignore_ensure "CLAUDE.md"
gitignore_ensure "RESUME.md"
gitignore_ensure "HISTORY.md"
gitignore_ensure "commit.msg"
gitignore_ensure "tasks/"

# --- Summary -----------------------------------------------------------------

echo ""
echo "Done. Directory structure:"
echo ""
echo "  CLAUDE.md                    # AI workflow rules (gitignored)"
echo "  RESUME.md                    # Living project context (gitignored)"
echo "  HISTORY.md                   # Append-only iteration archive (gitignored)"
echo "  .claude/commands/restart.md  # /restart — restore AI context"
echo "  .claude/commands/wrap.md     # /wrap — close out session"
echo "  tasks/                       # Work tickets (gitignored)"
echo "  tasks/done/                  # Completed tickets"
echo "  .gitignore                   # Updated with AI artifact entries"
echo ""
echo "Next steps:"
echo "  1. Open Claude Code in this directory"
echo "  2. Run /restart to begin your first session"
echo "  3. Describe your project — the AI will populate RESUME.md"
echo "  4. Run /wrap before ending a session to save context"
echo ""
