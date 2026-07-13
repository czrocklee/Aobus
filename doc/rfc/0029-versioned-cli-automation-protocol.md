---
id: rfc.0029.versioned-cli-automation-protocol
type: rfc
status: draft
domain: presentation
summary: Proposes an explicit versioned CLI automation envelope with stable command kinds, result and error schemas, stream discipline, and compatibility policy.
depends-on: none
---
# RFC 0029: Versioned CLI automation protocol

## Problem

The CLI deliberately treats YAML and JSON as parsed automation output.
Both formats are emitted from shared aggregate DTOs, stdout is payload-only, stderr is diagnostic-only, and exit codes distinguish success, domain/internal failure, and CLI11 parse failure.
These are good protocol foundations, but the structured surface has no protocol identity or schema version.

Current output uses command-specific top-level fields.
Adding, renaming, moving, retyping, or changing omission rules for a field can break automation even when the application and library formats remain compatible.
The command reference says such changes require documentation and smoke-test updates, but a caller cannot negotiate, pin, or reject a schema it understands.

Error automation is weaker than success automation.
Domain failures print human-readable text to stderr and exit `1`; there is no stable machine error code, outcome kind, or structured error body shared across commands.
A script must parse prose or combine incomplete stdout expectations with process status.

Several independent versions can affect a CLI invocation:

- the CLI automation schema;
- the library database and YAML transfer formats;
- a predicate language dialect;
- command behavior/mutation receipts; and
- the application build version.

Without explicit identities, callers can confuse one for another.
For example, adding a predicate dialect must not require a new CLI output major version unless the automation schema itself changes.

The command/DTO inventory is also maintained manually in code, reference tables, and representative smoke assertions.
There is no executable schema catalog from which complete compatibility fixtures or reference fragments can be checked.

## Dependencies

- Hard: None.
- Conditional: [RFC 0024](0024-versioned-predicate-dialect.md).
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0013](0013-coherent-application-reporting-policy.md).

Predicate-bearing automation conditionally depends on RFC 0024 to accept or report an explicit language dialect independently of the CLI protocol version.
RFC 0003 should supply stable mutation receipt data rather than CLI-reconstructed commit evidence.
RFC 0013 should align domain dispositions and stable error categories across CLI and interactive frontends.

## Goals

- Give structured CLI automation an explicit protocol name and requested version.
- Define one stable success/error envelope for YAML and JSON.
- Give every command/result/error a stable machine kind independent of display text.
- Preserve stdout/stderr discipline and deterministic exit classification.
- Define additive versus breaking schema changes, unknown-field behavior, deprecation, and supported-version discovery.
- Keep plain output human-oriented and outside the structured compatibility guarantee.
- Keep CLI protocol, predicate dialect, library transfer, database, and application versions independent.
- Generate or mechanically validate command/result schema inventory from code-owned descriptors.
- Provide complete golden compatibility fixtures rather than representative field assertions only.

## Non-goals

- Turn the CLI into a long-running RPC server or streaming daemon.
- Stabilize human-readable plain text, help prose, whitespace, or log messages as a machine protocol.
- Embed library YAML documents directly as the CLI protocol schema.
- Guarantee support for every historical protocol version forever.
- Replace CLI11 as the syntax/parser authority.
- Expose internal C++ exception types, source locations, or stack traces as protocol fields.

## Proposed design

### Explicit protocol selection

Add a global structured-output option such as:

```text
--protocol-version <major[.minor]>
```

It is valid only with YAML or JSON output.
A numeric version pins automation behavior; an optional discovery command/flag lists supported versions without opening a library.
Automation should not use an implicit `latest` value because it defeats reproducibility.

The first rollout keeps the current unversioned `-O yaml|json` surface as a clearly named legacy mode for a bounded deprecation window.
The versioned envelope is opt-in until release policy deliberately changes the default.
Unsupported requested versions fail before command work or library mutation begins.

The exact flag spelling and legacy timeline become CLI reference facts after acceptance.

### Common envelope

Every versioned structured invocation writes exactly one document to stdout:

```text
protocol:
  name: aobus-cli
  major: 1
  minor: 0
command: track.update
outcome: success
data: ... command-specific result ...
```

Failure uses the same envelope:

```text
protocol: { name, major, minor }
command: track.update
outcome: rejected | failed
error:
  code: stable.machine.code
  category: usage | validation | conflict | notFound | storage | io | internal
  message: human-readable text
  details: ... optional bounded typed evidence ...
```

Fields absent by schema are omitted; required envelope fields never depend on the command.
YAML and JSON represent the same logical model and field names.

`message` is for humans and is not stable for branching.
Automation branches on `outcome`, `error.code`, typed details, and exit status.
Internal errors expose a stable category/code and correlation id but not private exception or filesystem detail unless the command contract explicitly permits it.

### Command and result kinds

Assign a stable dotted command kind to every leaf command, such as `track.show`, `list.create`, and `library.resource.export`.
Aliases or future syntax changes map to the same kind when semantics remain compatible.

Each protocol major owns a schema descriptor for:

- accepted command kind;
- result data shape;
- dry-run/preview shape;
- stable error codes and detail shapes;
- omission/default rules; and
- applicable exit classification.

Mutation DTOs consume runtime/core receipts where available.
The CLI adapter may rename fields for protocol stability through explicit mapping, but it does not infer whether a commit occurred from mutable state.

### Version evolution

Within one major version, a minor update may:

- add optional envelope or command data fields;
- add a new command kind;
- add a new error code within a documented category; or
- extend an enum only when clients are required to handle unknown values.

Clients must ignore unknown fields within a supported major while validating required fields and known value types.

A new major is required to remove/rename a field, change its type or meaning, make an optional field required, change command/result identity, or alter outcome/exit semantics incompatibly.
The executable can support more than one major through explicit encoder adapters over the same current domain results.

Protocol support has a documented lifecycle: introduced, current, deprecated, and removed no earlier than the repository/release policy permits.
Removal produces a clear unsupported-version failure before side effects.

### Exit and stream contract

Keep stdout as exactly one versioned payload document for every parsed versioned command, including domain failure.
Human progress and diagnostics remain on stderr.

Define exit classes independently of prose:

- `0` for `outcome: success`, including valid empty/no-op results;
- a documented nonzero domain exit for `rejected`/ordinary `failed` outcomes;
- a distinct internal failure exit if compatibility permits; and
- CLI11 usage exits for failures that occur before a command/protocol envelope can be established.

If a protocol version was successfully selected and command identity is known, validation and domain failures should still emit the structured error envelope.
Fatal inability to encode stdout is the exceptional leaf and writes only bounded stderr diagnostics.

Exact exit values are frozen in the protocol reference rather than inherited accidentally from current implementation branches.

### Independent embedded versions

When a command accepts predicate text, its input options and any echoed typed details carry a `PredicateDialect` only if RFC 0024 is implemented.
That dialect is not derived from protocol major.

Library database version, library YAML format version, and application version appear only in command data whose semantics call for them.
They never replace the envelope protocol version.

Dry-run is command/result data and retains the same protocol schema as commit mode.
A mutation result also carries the domain receipt/revision defined by its runtime owner when available.

### Schema catalog and reference generation

Create a code-owned protocol catalog or descriptor set used by encoders and tests.
It enumerates every command kind and top-level DTO field for each supported major without requiring runtime reflection over arbitrary C++ aggregates.

Use the catalog to:

- assert that every registered CLI leaf has a protocol mapping;
- generate machine-readable schema fixtures or a stable schema report;
- check that YAML and JSON encode the same model;
- verify required/optional fields and error codes; and
- generate or mechanically compare the command/result portion of the CLI reference.

CLI11 remains syntax authority.
The catalog records automation semantics, so the two inventories are cross-checked rather than pretending one descriptor owns both parsing and DTO meaning.

### Security and bounded output

Structured errors bound repeated item details and include summary counts when a command can fail for many inputs.
User file contents, raw exceptions, and unbounded backend messages do not become schema fields by default.

String encoding remains UTF-8 and locale-independent.
Numeric units and timestamp representations are explicit per field.
No command emits NaN/Infinity or platform-dependent path encoding without a documented representation.

## Alternatives

### Use the application version

Application releases change more often than the automation schema and cannot express several supported protocol majors in one executable.
Protocol compatibility needs its own identity.

### Version each command independently

Per-command versions permit local evolution but multiply negotiation, envelope, error, and support policy.
One protocol major with command-specific schemas is easier for clients to reason about.

### Keep top-level command-specific DTOs and add only `version`

That identifies the schema but leaves success/error shape, command kind, and generic client handling inconsistent.
A common envelope supplies the missing protocol boundary.

### Publish JSON Schema only

An external schema is valuable but does not make the executable negotiate versions or emit stable error outcomes.
Generate schema/report artifacts from the code-owned protocol catalog after the runtime contract exists.

### Send errors only to stderr

That preserves human diagnostics but forces automation to parse prose and cannot carry typed conflict/missing evidence.
Versioned mode should encode domain outcomes on stdout while retaining stderr for operators.

## Compatibility and migration

Legacy plain output is unchanged.
Current unversioned YAML/JSON remains available during the documented deprecation window and preserves its existing top-level shapes.

Versioned protocol 1 uses the new envelope and maps current command DTO semantics deliberately.
It is not assumed to be byte- or shape-compatible with the unversioned legacy surface.
Migration documentation shows how each legacy top-level shape moves under `data` and how failures become structured.

Scripts opt in with an explicit version, validate `protocol.name/major`, branch on stable kinds/codes, and ignore unknown optional fields within the major.
No protocol-default switch occurs until all commands have complete catalog coverage and golden fixtures.

Supporting an old major does not require preserving old domain internals.
Encoder adapters translate current typed results to supported old shapes or reject an operation that cannot be represented safely before side effects.

## Validation

- Every CLI leaf command has one stable protocol command kind and success schema in the catalog.
- Every expected domain failure has a stable code/category and structured envelope when command identity is known.
- Golden YAML and JSON fixtures cover every command for success, empty/no-op, dry-run, validation, not-found, conflict where applicable, storage/I/O, and internal-error redaction.
- YAML and JSON round-trip to the same logical envelope and remain locale-independent.
- Unsupported versions fail before runtime/library construction or mutation.
- Same-major additive-field fixtures remain readable by a reference client that ignores unknown fields.
- Breaking-change tests require an explicit new major rather than silently updating a version-1 fixture.
- stdout contains exactly one payload document; progress/logs remain on stderr; exit codes match outcome classes.
- Protocol, predicate dialect, application, library database, and YAML transfer versions vary independently in fixtures.
- Schema/catalog tooling detects an unregistered command, missing DTO mapping, undocumented field, or undocumented error code.
- Legacy structured and plain compatibility tests remain during the deprecation window.
- A full `./ao check` passes after implementation.

## Open questions

- What exact option and discovery surface should select/list protocol versions?
- Should versioned domain failures always write their envelope to stdout, or should a separate machine-error stream mode be considered?
- Which exact nonzero exit classes can be introduced without harming existing shell usage?
- How long is the unversioned YAML/JSON deprecation window, and will it ever become an alias for a fixed protocol major?
- Should the catalog generate JSON Schema, a simpler repository-owned schema report, or both?
- How many old major versions should one executable support concurrently?

## Promotion plan

If accepted and implemented:

- update the [CLI execution specification](../spec/cli/execution.md) with negotiation, envelope, stream, error, and exit behavior;
- update the [CLI command reference](../reference/cli/command.md) with exact protocol versions, command kinds, schemas, codes, fields, and legacy migration;
- update the [presentation architecture](../architecture/presentation.md) with the CLI automation adapter boundary without treating CLI as interactive UIModel;
- update predicate, library mutation, and reporting documents where their typed versions/receipts/dispositions enter the protocol;
- add generated or mechanically checked protocol schema/reference tooling and complete compatibility fixtures; and
- record the protocol-1 envelope, compatibility rules, and support lifecycle in a decision when accepted.
