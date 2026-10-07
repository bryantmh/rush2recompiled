Agents are expensive and a waste of my money. NEVER call more than 2 agents and even then that should be a rare occurence. Use subagents at Sonnet Medium

We aren't british. no "colour" and the like

Ensure you document code and functions that you have taken the time to disassemble so that other sessions can easily reference and add to the knowledge base instead of working everything out again from scratch

If porting from SF Rush or Rush 2049 default to porting the original games' code and just patching the Rush 2 differences rather than fully reimplementing from scratch

If your change will alter base Rush 2 behavior, it needs to go behind a toggle in the recomp UI

This is now a live app. Any behavior that would invalidate a config or save needs to be migrated at startup to the new system

Scratch scripts, logs and test output go in `tmp/` in the project root (gitignored) or the session scratchpad. Never in `build/`, the project root or anywhere else in the tree

Prefer reusable tools over one-off scripts. If a script would be useful again, put it in `tools/` with arguments and a comment on what it does, and extend an existing tool before writing a new one
