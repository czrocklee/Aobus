# Aobus Agent Council

> Status: implemented baseline. `aobus-council` and `config/agent-council.yaml` are the production
> agent mechanism.

## Purpose

The council runs a roster of model agents in copied workspaces and returns advisory review text.
It does not edit the real repository, extract patches, certify changes, or maintain route history.
The chair reads the artifacts, checks claims against the tree, and owns the final plan or review.

## Commands

```bash
./ao council validate-config --registry config/agent-council.yaml
./ao council run --registry config/agent-council.yaml --repo "$PWD" --out <fresh-out> <intent>...
```

Exit `0` means every phase reached quorum. Exit `2` means a policy failure such as missed quorum,
dependency failure, or real-tree conflict. Exit `3` is infrastructure failure, `5` configuration
failure, and `64` CLI or intent input failure.

## Schemas

The registry schema is `aobus-council-registry/v1`:

```yaml
schema: aobus-council-registry/v1
harnesses: {}
agents: {}
councils: {}
```

An intent schema is `aobus-council-intent/v1`:

```yaml
schema: aobus-council-intent/v1
id: phase-a
task-kind: council-review
invariant: Preserve behavior.
focus:
  - path: lib/audio/
depends-on: []
overrides:
  depth: panel
  quorum: 2
body: |
  Review the supplied change.
```

`focus` is advisory prompt context. It is not an enforcement boundary. Omit it when there is no useful
path hint.

`depth` selects how many council rounds run:

- `panel` runs one independent-review round.
- `challenge` runs independent review, then asks usable members to challenge peer drafts.
- `full` runs independent review, peer challenge, then asks usable members to revise their own
  review after considering the challenge round.

## Execution

For each run, the runner requires a fresh output directory, then creates an immutable baseline copy
of the repository under it. Each council member receives its own copied workspace plus a prompt
containing the phase body, invariant, council parameters, and focus hints. The baseline is internal
scratch: it is only the rsync source for member workspaces, so it is reclaimed when the run finishes,
whether it succeeds or fails. Snapshotting copies the repository's `.git` verbatim, so any stale lock
files (such as `index.lock`) left by a concurrent parent git operation are stripped from the copy
before the baseline records its own review-base ref.

Member sandboxes bind the copied workspace over the real repository path. They also bind the host
`HOME` at `/tmp/aobus-home` so authenticated agent CLIs can reuse local config, and expose common
review tools such as `git` and `rg` under `/tmp/aobus-tools` at the front of `PATH`. The `HOME` bind
is writable by design; council agents are trusted local tools, not an untrusted-code isolation
boundary.

Artifacts are written under `<out>/<phase-id>/`:

- `intent.yaml`, `resolved.yaml`, `manifest.yaml`, `evidence.yaml`, and `trace.yaml`.
- `dossier.md`, the combined advisory surface for the chair.
- `members/<agent>/<round>/prompt.md`, `stdout.txt`, `stderr.txt`, `response.md`, and `workspace/`.
  Current round ids are `r1` for independent review, `r2` for peer challenge, and `r3` for
  self-revision.

`manifest.yaml` records the phase id, failure reason, and summary. Failure reasons are `none`,
`dependency-failed`, `quorum-failed`, `infrastructure-failed`, and `real-tree-changed`.
`evidence.yaml` records whether each member response was usable for quorum, grouped by round. It also
records whether the review text came from stdout or stderr, because some CLIs write their final
answer to stderr. A response is usable when the member exits successfully, returns non-empty review
text, and does not return an authentication or login challenge. A phase succeeds when the final
executed round reaches quorum. If an earlier round misses quorum, dependent rounds do not run and the
phase fails. All intents are resolved before output setup or snapshot creation, so invalid task
kinds or override rosters do not leave partial run artifacts. Member-level infrastructure failures
are recorded as unusable member evidence; they only make the phase an infrastructure failure when the
round cannot still reach quorum. Infrastructure phase failures still write terminal evidence,
dossier, manifest, and trace artifacts, then the scheduler continues so dependent phases can be
written as `dependency-failed` and independent phases can still run. The CLI exits with the
infrastructure exit code when any phase manifest is `infrastructure-failed`. If the real repository
changes during a run, completed manifests are rewritten to `real-tree-changed`. Dependent phases are
skipped when any dependency fails.

## Registry Rules

The registry declares shared harnesses, concrete agent identities, and council task kinds. A council
roster must:

- reference known agents;
- contain no duplicate agent;
- use distinct vendors;
- satisfy `1 <= quorum <= roster size`.

Intent overrides may replace `roster`, `depth`, and `quorum`. No other override exists.

## Boundary

Council output is advisory. It may improve review quality by forcing independent model perspectives,
but it is not acceptance authority and cannot land code. The normal development workflow remains:
read the dossier, verify claims, make real-tree edits manually, and run the relevant validation.
