---
id: cli.execution
type: spec
status: current
domain: presentation
summary: Defines CLI runtime composition, selection, output, stream, dry-run, mutation, scan, failure, and exit behavior.
---
# CLI execution specification

## Scope

This specification owns behavior shared across the Aobus CLI command tree: runtime construction, target selection, output encoding, stream discipline, dry-run guarantees, command failure, and exit status.
Exact command and DTO fields belong to the [CLI command reference](../../reference/cli/command.md).

## Code boundary

The CLI is a non-interactive application adapter.
`CliRuntime` composes `CoreRuntime` and exposes runtime `LibraryReader`/`LibraryWriter`, scan, transfer, and low-level administrative surfaces needed by commands.
It bypasses UIModel because it owns no reusable interactive state.
It also owns the synchronous boundary for asynchronous runtime tasks: `LoopExecutor` keeps callback delivery on the invocation thread, and `runTask()` drives that loop to terminal completion.

Command files under `app/cli/` parse CLI11 values, invoke runtime/core owners, and encode output.
They do not reimplement list membership, scan planning, query evaluation, mutation atomicity, or YAML transfer.

## Terminology

- **Payload output** is command data written to stdout.
- **Diagnostic output** is error or verbose progress written to stderr.
- A **domain failure** is a parsed command that runtime/core cannot satisfy.
- A **preview mutation** executes the normal writer operation in an uncommitted transaction and returns its change report.
- An **explicit target** is an id supplied on the command line rather than selected by an expression.

## Invariants

- stdout contains payload only; domain errors and verbose progress use stderr.
- Exit `0` includes legitimate empty results and no-op mutations.
- Domain/internal failures exit `1`; CLI11 parse failures retain CLI11's nonzero status.
- YAML and JSON are emitted from the same aggregate DTO and therefore expose the same semantic fields.
- Empty optionals are omitted; empty vectors/maps remain `[]`/`{}`.
- Explicit ids are resolved by direct current-library lookup and missing ids fail before a batch mutation starts.
- A command accepting ids or `--filter` rejects supplying both and rejects supplying neither when a target is required.
- Dry-run and committed mutation reports use the same classification and field-diff logic.
- A dry-run does not commit library content, update restore metadata, or publish library changes.

## State model

Global CLI options select one normalized music root and one output format for the command invocation.
`CliRuntime` owns one `CoreRuntime`/library environment for that invocation.
The invocation thread owns its callback executor and remains the only CLI application-control thread.
There is no workspace, interactive playback session, or persistent frontend state.

Mutation reports carry `dryRun`, affected identities when committed/known, and operation-specific diffs.
Create previews omit ids allocated only by the aborted transaction.

## Commands and transitions

### Selection and query

Track show/update and tag add/remove may select explicit ids or a predicate expression.
Predicate selection uses the shared query compiler/evaluator; formatted track output uses the scalar format-expression compiler.
Format expressions are plain-output only and cannot be combined with YAML/JSON output.

List detail uses the runtime source path, so manual-list parent membership and nested Smart List filtering match interactive frontends.

### Preview and commit

Writer-backed dry-run commands invoke the corresponding `preview*` runtime method.
The writer performs normal validation and mutation logic inside the write transaction, constructs the ordinary reply, suppresses change publication, and aborts instead of committing.

Scan dry-run uses the scan plan without apply.
Library import dry-run decodes and applies through the import transaction, then aborts and does not update restore metadata.
`lib fingerprint --pending` is bounded maintenance with no dry-run mode because completed identity batches are its unit of progress.
It runs worker work through `CliRuntime::runTask()` so callback-executor continuations and terminal completion return through the invocation-thread executor without deadlocking a future wait.
Its current progress and item-failure callbacks remain worker-produced and are serialized by the indexer.

### Scan and verify

Scan builds a runtime `ScanPlan`, emits summary/optional item output, then applies unless dry-run.
`--defer-fingerprint` imports new metadata and leaves new manifest identity pending for later fingerprinting.
Per-item apply failures are diagnostic rows; a transaction-level failure rejects the command.

Verify builds but does not apply a scan plan.
Changed and moved rows are reported; missing or error rows make verification fail.

### Structured output

Summary, mutation, detail, scan, verify, dump, and transfer commands emit one complete document.
Commands whose contract is an independent record stream may emit record-shaped plain output; structured commands use the aggregate shapes in the reference.
Callers parse JSON rather than relying on byte-exact whitespace from the ryml emitter.

## Failure and cancellation

Query/format compilation, unknown ids, invalid list ancestry, unsupported operation, storage, import/export, resource IO, scan, and verification failures become `CommandError`, print one diagnostic, and exit `1`.
Unexpected Aobus invariant exceptions are labeled internal errors; other exceptions use the generic CLI error leaf.

The CLI command invocation is synchronous at its adapter boundary.
Runtime operations that internally use bounded workers follow their own cancellation/lifetime contracts; the CLI does not invent a second partial-commit policy.
An asynchronous command is not complete until its terminal marker has run on the callback executor.
Its result or escaping exception is then returned or rethrown on the invocation thread.
If a callback throws during pumping, `runTask()` retains the first callback exception and continues until the terminal marker before consuming the spawned future.
A task failure remains primary and the callback failure is reported; if the task succeeds, the callback failure is rethrown on the invocation thread.

During teardown, CLI stops and joins runtime workers before draining already-ready callback turns while their `CoreRuntime` targets remain alive.

## Persistence and versioning

CLI output is a source-level automation contract but currently has no explicit schema version.
Changing a structured field requires updating the [CLI command reference](../../reference/cli/command.md) and smoke tests.
Library and transfer formats retain their own independent versioning.
No negotiated protocol envelope or compatibility layer is planned for the current CLI surface.

## Frontend observations

Plain output is optimized for humans and shell inspection.
YAML/JSON are intended for parsed automation.
`--help-all` emits the recursive command tree including command teaching footers; `--version` exits successfully without constructing command work.

## Implementation map

- [`Run.cpp`](../../../app/cli/Run.cpp) owns global parsing, help, exception leaves, and exit status.
- [`CliRuntime.cpp`](../../../app/cli/CliRuntime.cpp) owns non-interactive composition, loop driving, terminal task handoff, and producer-first teardown.
- [`LoopExecutor`](../../../include/ao/async/LoopExecutor.h) supplies the owner-thread queue wake and turn operations.
- [`Output.h`](../../../app/cli/Output.h) owns reflected YAML/JSON emission.
- Command implementations under [`app/cli/`](../../../app/cli/) own adapter-specific parsing and DTOs.

## Test map

- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects end-to-end parsing, commands, output, dry-run, and failures.
- [`CliRuntimeTest.cpp`](../../../test/unit/cli/CliRuntimeTest.cpp) protects worker-to-owner completion, callback-failure task completion, exception propagation, and teardown draining.
- [`OutputTest.cpp`](../../../test/unit/cli/OutputTest.cpp) protects structured scalar/container/optional encoding.
- [`CommandErrorTest.cpp`](../../../test/unit/cli/CommandErrorTest.cpp) protects domain error adaptation.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Track expression architecture](../../architecture/track-expression.md)
- [CLI command reference](../../reference/cli/command.md)
- [Library YAML transfer](../library/runtime/yaml-transfer.md)
