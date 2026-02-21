Update RESUME.md and its dependent documents (CLAUDE.md) to fully reflect the
current state of the project. These files serve as the single source of truth
for restoring AI thread context and resuming work on this codebase.

Specifically:
- Read the current RESUME.md and CLAUDE.md
- Compare against the actual codebase state (files, tests, architecture)
- Update all sections that are stale: file inventory, test counts, module
  descriptions, architecture diagrams, design decisions, and test results
- Rewrite commit.msg to document all code changes made in this session
- Stage all modified and newly added project files (use git add with explicit
  file paths — never use git add -A or git add .)

Do not add "Co-Authored-By" lines to commit messages or source files.

Do not ask for confirmation — just do the updates, stage the files, show what
changed, and note that the user should review before committing.
