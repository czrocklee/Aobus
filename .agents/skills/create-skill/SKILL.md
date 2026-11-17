---
name: create-skill
description: Create, audit, or refactor Agent Skills and AGENTS.md repository instructions. Do not load for ordinary product or contributor documentation.
---

# Maintain agent instructions and skills

## Admission and ownership

- Extend or merge skills with the same user intent and success criteria; retire
  one when canonical guidance already supplies its only contribution.
- Root instructions own repository-wide agent defaults; skills own task-specific
  authorization and review protocols. Product, contributor, and `./ao` contracts
  belong in `doc/`; add missing contracts there before routing a skill to them.
- Keep repository instructions machine-independent
  ([file boundaries](../../../doc/development/documentation.md#file-boundaries));
  hosts, accounts, SSH/VM paths, and sync recipes belong in `~/.agents/skills/`.

## Root instruction files

- Admit essential commands, private cross-task constraints, and authorization
  gates that agents would otherwise miss. Omit discoverable project overviews,
  generic engineering advice, and subsystem details already owned elsewhere.
- Keep a few contributor, product, and validation entry points rather than
  copying task indexes or the host's skill catalog. Markdown links are on-demand
  routes, not automatically loaded instructions; keep essential defaults inline.
- State useful actions positively. Preserve real safety boundaries, not catalogs
  of nonexistent commands or defensive reminders. When a summary needs more
  exceptions, prefer the owning reference over another layer of qualifications.
- Cut redundant information, not meaningful layout; no universal line quota
  establishes quality. Before removing a rule, name the repo-specific mistake it
  prevents and preserve explicit user constraints.

## Skill bodies

- Assume frontier models know routine coding, Git, debugging, and `--help`.
  Avoid lectures, rigid step-by-step pipelines, and all-caps pressure words.
- Retain private context: authorization boundaries, precise `#anchor` routes,
  counter-intuitive traps, and command syntax where flags or ordering are fragile.
  Route to contracts rather than summarizing them; an inline silent-trap reminder
  accompanies its canonical link.

## Frontmatter and Aobus validation

- Keep `<verb>-<noun>` directory/`name` parity and closed `---` delimiters.
  `./ao check docs` strips skill frontmatter without parsing YAML. Omit client UI
  sidecars such as `agents/openai.yaml`. Use `disable-model-invocation: true` only
  when explicit invocation or a mandatory root route preserves discoverability.
- Front-load concrete user vocabulary in `description`, with near-miss exclusions
  where adjacent skills could false-match; aim for roughly 120–240 characters.
- Use relative Markdown links, not inline-code paths, for repo references
  (`../../../doc/...#anchor` from `SKILL.md`; `../../../../` from `references/*.md`).
  Non-`.md` skill changes exit CI's `docs-only` path and take the tooling route in
  [validation and review](../../../doc/development/test/validation-and-review.md#validation).
- Keep a temporary fact/link ledger outside the checkout during consolidations.
  Validate links with `./ao check docs`, parse skill YAML, verify live claims,
  and check representative task routing, including near misses and retired routes.
