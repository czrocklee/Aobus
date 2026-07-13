---
id: development.commit-message
type: development
status: current
domain: development
summary: Defines the commit structure and message rules for Aobus changes.
---
# Commit message guidelines

Commit messages are part of the project history. Write them for the next person
debugging, reviewing, reverting, or bisecting the change.

## Format

Use Conventional Commits:

```text
type(scope): imperative summary
```

Examples:

```text
docs(dev): add commit message guidelines
test(runtime): cover playlist reload cancellation
fix(gtk): keep tag editor focus after validation error
refactor(library): simplify metadata import flow
```

Use the subject alone when the reason for the change is clear. Add a body when
the diff does not explain the motivation, tradeoff, risk, or validation.

## Types

- `feat`: user-visible capability or supported behavior.
- `fix`: bug fix or behavior correction.
- `docs`: documentation-only change.
- `test`: test-only change or test infrastructure change.
- `refactor`: restructuring that preserves behavior.
- `build`: build system, dependency, or packaging change.
- `ci`: continuous-integration automation.
- `chore`: repository maintenance that does not fit the other types.

Prefer the most specific accurate type. For example, a documentation
reorganization is `docs`, not `refactor`.

## Scope

Use a scope when it helps locate the change. Prefer existing project boundaries:

- product layers: `core`, `runtime`, `uimodel`, `gtk`, `cli`
- repository tools: `ao`, `cmake`, `nix`, `lint`
- documentation areas: `dev`, `design`, `agents`
- test areas: `core`, `gtk`, `tooling`

Omit the scope only when the change is genuinely cross-cutting and no short
scope would make the subject clearer.

## Subject

- Use imperative mood: `add`, `fix`, `rename`, `remove`, `document`.
- Keep it specific to the primary technical contribution.
- Keep it concise; aim for one line that stays readable in `git log --oneline`.
- Do not end with a period.
- Do not describe the workflow: avoid `update docs after review`,
  `address feedback`, or `misc cleanup`.

Good:

```text
docs(dev): consolidate testing guidance under dev docs
fix(runtime): stop playback before replacing the active sink
test(gtk): cover detail field outside-click commits
```

Weak:

```text
docs: update
fix bug
refactor stuff
chore: address comments
```

## Body

Use a body when it adds information the diff cannot carry well:

- why the change is needed
- what tradeoff was chosen
- how compatibility, migration, or risk was handled
- what validation was run when that matters to future readers

Keep the body factual and project-focused. Avoid AI/tool attribution, internal
plans, generated-by trailers, and co-author signatures unless the user
explicitly requires a real human co-author line.

Example:

```text
refactor(runtime): isolate sink reload cancellation

Move cancellation ownership into the reload coordinator so sink replacement and
shutdown use the same stop path. This keeps the public runtime contract stable
while making timeout handling testable without a GTK loop.
```

## Review checklist

- The type matches the change.
- The scope helps the reader locate the affected area.
- The subject is imperative and describes the result, not the process.
- The body explains non-obvious motivation, tradeoffs, or validation.
- The message contains no AI/tool attribution or internal implementation notes.
