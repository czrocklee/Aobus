---
id: decision.0023.full-input-native-lint-reports
status: accepted
---

# Decision 0023: keep full declared inputs for native lint reports

## Decision

Keep Bazel's ordinary [lint report cache](../development/bazel.md#lint-reports). On a miss, run stock clang-tidy with all declared inputs. Accept redundant local reruns rather than risk returning a stale clean report from `./ao check`.

## Rejected shortcuts

A shared target declared `Base64.h` for consumers that never included it. An early trial let Bazel mark either of two headers unused. Editing `Base64.h` then triggered 2 lint executions instead of 60 in the same output base. All reports were clean: this demonstrated reuse, not correctness. With a fresh output base and a primed disk cache, an unseen edit still ran all 60.

**Predicting includes.** Removing files absent from the include list preserved a warning about including `Fragment.cpp` but lost the note suggesting `Fragment.h`. Removing a directory changed `__has_include` on a path containing `..` and hid a failing assertion during header replay.

**Replacing the driver.** When we deliberately omitted the Aobus clang-tidy plugin (`libAobusLintPlugin.so`), the replacement reported clean. An independent verifier detected the missing checks and a stock rerun recovered the warning. This tested the fallback, not the replacement; the verifier still needed all inputs.

**Tracing stock.** We created an optional file outside the declared inputs and edited a comment in a declared header the trace had marked unused. The declared edit made stock rerun and report a forbidden raw `throw`; the prototype reused its clean report. Rechecking the optional file's existence fixed that case. It did not account for a separate fixture that used `__BASE_FILE__` to observe its sandbox path: successive unrelated header edits made stock alternate between clean and a compiler error.

## Cost and consequences

In a final Linux trial on seven reports, stock reran seven after a `Base64.h` edit and six after an `AtomicFileTransaction.h` edit. The proposed lists could spare zero and at most one, respectively. Neither edit justified a skip, and learning the lists required a second traced invocation per live report. No whole-command speedup was established.

Look first at why the header is declared. Giving `Base64.h` to its four consuming targets instead of a shared header glob cut prototype `AobusLintScan` actions listing it from 1,975 to 116 and `CppCompile` actions from 1,396 to 75. These are declaration counts, not executions or timings. The split was reverted; future splits should follow API boundaries and preserve public labels and header coverage.

The 60-report and target-split trials used baseline `90270690d`; the seven-report trial used `f5b22e759`. The split counts came from `bazel aquery --include_pruned_inputs --include_aspects` on `//test/lint:production_lint`. The baselines do not include the experimental rules.

Before timing another optimization, reproduce a known warning, note, or compiler error with stock, then compare report bytes, logs, and gate outcomes through failure and repair. Use the real main and selected consumer, not a second implementation sharing the replay path. Measure the whole command on a new local edit, including learning and validation; a disk hit or a warm no-edit build answers a different question.
