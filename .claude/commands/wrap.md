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
