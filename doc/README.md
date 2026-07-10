# Aobus Documentation Guide

This directory holds project knowledge that should survive beyond a single
change. Put new documents where readers will expect to find the source of truth,
and prefer linking to an existing document over repeating the same policy in
multiple places.

## Directory Roles

| Directory | Use for |
|---|---|
| `doc/dev/` | Contributor practices: coding style, testing, commit messages, local workflow, and review rules. |
| `doc/design/` | Current product behavior, architecture, contracts, data models, UI behavior, and subsystem design decisions. |
| `doc/plan/` | Planned or in-progress implementation work, audits, migration plans, and phased execution notes. |
| `doc/brainstorm/` | Exploratory ideas that are not yet scoped as a plan or accepted as design. |

## Choosing a Location

- If the document tells contributors how to work in this repository, put it in
  `doc/dev/`.
- If it describes how Aobus behaves or how a subsystem is designed today, put it
  in `doc/design/`.
- If it describes work that still needs to be done, put it in `doc/plan/`.
- If it captures early thinking without commitment, put it in `doc/brainstorm/`.

When a plan becomes implemented behavior, update or create the relevant
`doc/design/` document. Do not leave implemented behavior documented only in a
plan.

## Status and Freshness

Design documents should read as current unless they explicitly say otherwise.
If a design is proposed, historical, or partially implemented, say that near the
top of the file.

Plans may become stale as the implementation changes. Treat them as execution
records or proposals, not as higher authority than current code and current
design docs.

## Cross-References

- Link from broad entry points to focused docs.
- Avoid copying long guidance between files; keep one owner and point other docs
  to it.
- Keep agent-facing instructions as routing layers when possible. Human docs
  should hold the project policy; agent docs should say which human doc to read.

Current cross-platform dependency policy is documented in
`design/dependency-version-governance.md`; the contributor procedure for
changing pins is `dev/dependency-upgrades.md`.
