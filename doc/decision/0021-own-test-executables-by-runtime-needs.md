---
id: decision.0021.own-test-executables-by-runtime-needs
status: accepted
---
# Decision 0021: own test executables by runtime and link needs

## Context

Aobus verifies one functional contract with many kinds of cases: portable data
and fake collaborators, device-free SDK and refusal controls, cases that must
succeed against a live audio endpoint or service, and cases that rely on
binary-specific link interposition. A test directory groups the contract; it
cannot also decide which program runs a case, because runtime and link needs
differ within one directory.

## Decision

Test directories are functional. A directory such as `test/audio/backend`
groups cases by the contract they verify, not by executable or platform, and
may feed more than one executable.

A case joins the backend executable only when it requires success against a
true live endpoint or service, or when it needs the link interposition that is
specific to that binary. Device-free audio-backend cases — including portable
SDK, null, fake, refusal, and data contracts — stay in core even from the same
directory. Spawning an owned child probe alone does not justify another binary.
Platform branches in the CMake test-source declarations assign each file to its
owning executable with an explicit disabled reason where a platform does not
provide it.

Validation gates select programs by purpose: the default group is the fast
development loop, the full gate owns completion, and the sanitizer and
concurrency groups own their instrumented questions. Automatic group selections
exclude hidden `[.manual]` cases; running one requires an explicit filter.

Only two case markers exist, `[concurrency]` and `[.manual]`, and the test
audit rejects any other tag.

Case existence and form come from the bounded content scan of the test
sources. Executable ownership is authoritative only in the fresh CMake
configured manifest, reconciled against that scan by `./ao check` after a
build; the standalone audit and hygiene tools do not assign ownership.

Current rules live in [test suites](../development/test/test-suite.md) and
[validation and review](../development/test/validation-and-review.md).

## Alternatives considered

- One executable per directory or layer: rejected, because directories would
  have to mirror executables and platform branches, splitting functional
  families just to keep selection simple.
- One large test binary: rejected, because every run would carry the most
  demanding case's needs, from endpoint availability to binary-wide link
  options.
- A richer tag taxonomy: rejected, because layer and component labels drift
  away from what a gate actually selects; programs and explicit filters express
  what the gates need.

## Consequences

A functional directory can be split across executables, so which program runs
a case is answered by the configured manifest, not by the path alone.
Adding or removing a marker is a policy change to this decision, because the
audit enforces exactly the two markers.
