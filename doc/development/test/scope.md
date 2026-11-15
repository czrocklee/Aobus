---
id: development.test.scope
---
# Deciding what to test and check

This page decides whether a test, a runtime check, or a gate or CI step should
exist, and when one should be removed. The [testing policy](../test.md)
explains how to write a test once it is warranted. The criteria apply equally
to C++ product code and repository tooling.

## The admission rule

A test or check must meet all four conditions:

1. **It names a concrete failure**: a realistic change in this repository, or a
   defect that already happened.
2. **Nothing cheaper catches it**: the type system, a failing build, a
   lower-layer test, a tool's exit status, an existing gate, or the feature's
   first ordinary use. Ask: if this broke, would something already fail? If it
   would, the real run covers it. If it would not, as with a false green, where
   the gates pass while something went unchecked, or a wrong artifact that
   nobody notices, the test is warranted.
3. **It discriminates** against an oracle independent of the code under test.
   Restated configuration, such as the flags an action carries because Starlark
   declares them, or a reimplemented algorithm is not an oracle.
4. **Its cost is proportional**, counting code, runtime, fixtures that mirror
   external formats, and false failures on upgrades.

The rule applies to gate and CI steps as well: a real build of a configuration
supersedes an analysis-only check of it.

## Trust boundary

Pinned tools and dependencies are trusted: Bazel and its rule sets, Nix and its
store, compilers and linkers, admitted host SDKs, and the Python standard
library. So is data that project code produced earlier in the same run. User
input, files a user edits, persisted library data, network responses, and
processes with a loosely specified output are untrusted.

Tests and checks stay on the project's side of this boundary. They cover what
the project passes to a trusted tool and how it uses the tool's documented
output, never whether the tool honors its own contract. Only two cases cross
the boundary:

- **An observed defect** in the pinned version: test the project's workaround,
  name the upstream defect, and remove the test with the workaround.
- **A local patch**, such as the
  [rules_cc coverage patch](../../../bazel/rules_cc_coverage_private_headers.patch):
  test the patched behavior, and retire the test with the patch.

A tool or dependency upgrade is accepted by running the native gates on the new
pin, not by a standing suite that re-tests the tool.

## What to test, and what not

Test behavior the project owns and a caller observes, persisted and exchanged
formats, failure, cancellation and teardown paths,
[concurrency contracts](concurrency-and-sanitizer.md), parsers of untrusted
input, and real defects as [regression tests](validation-and-review.md#regression-tests).
In tooling, the tests worth keeping guard against a false green or a silently
wrong artifact:

- selection of targets, tests, files and CI routes, where a mistake leaves
  something unchecked;
- translation of command-line intent into tool invocations;
- environment isolation and the invalidation of stale results;
- generated interfaces that another consumer reads;
- propagation of failure exit codes, and cleanup after a failure.

Do not test:

- inputs that no producer emits, unless they cross the trust boundary;
- platform or mode matrices whose cells expect the same outcome;
- diagnostic wording and receipt fields that no program reads;
- test harnesses and probes, whose own defects already make their gate fail.

## Runtime checks

A runtime check is a test that runs in production. Validate untrusted input
where it enters and fail there with a clear diagnostic. Do not re-check trusted
data, such as the run's own output or a tool's output after a successful exit,
and in particular:

- do not hash or snapshot inputs to detect edits during a repeatable run;
- do not keep a standing reconciliation of two pipelines: fix the discrepancy
  it finds and keep at most a cheap regression check;
- do not depend on a tool's diagnostic wording or undocumented formats;
- do not record provenance, checksums or "not proven" notes that nothing reads.

Fail-closed handling belongs to security and data-integrity boundaries. In
tooling, an unexpected shape may surface as an ordinary exception. C++ code
chooses its channel from the [error-handling rules](../coding-style.md#error-handling).

## Cheapest effective form

Prefer, in order: making the failure impossible through types, construction or
one source of truth; a compile-time assertion under the real toolchain, as the
[sanitizer profile canary](../../../test/core/SanitizerProfileTest.cpp) does for
compiler facts; the failing build or test of a real target; a small test of
owned logic; and a runtime check at a trust boundary. Re-deriving behavior from
query or action-graph output is the last resort, kept in one place.

## Removal

Tests and checks are not a ratchet: remove one that fails the admission rule,
stating what is lost, and update any reader-facing guarantee that the removal
weakens. Test names built on "cannot", "never" or "fails closed" for trusted
input, and docstrings that say more about what is not proven than about what
is, are good places to start looking.
