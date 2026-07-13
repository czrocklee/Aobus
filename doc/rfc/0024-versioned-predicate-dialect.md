---
id: rfc.0024.versioned-predicate-dialect
type: rfc
status: draft
domain: query
summary: Proposes an explicitly versioned predicate-text dialect across Smart Lists, workspace filters, CLI input, parsing, and semantic compilation.
depends-on: none
---
# RFC 0024: Versioned predicate dialect

## Problem

Predicate expressions are durable product data without a durable language identity.
Smart Lists store expression text in the library, workspace/session state can retain filters, and CLI scripts submit the same text surface.
At materialization time Aobus reparses and recompiles that text against the currently installed parser, field catalog, unit rules, dictionary binding, and evaluator semantics.

The current reference explicitly records that there is no language version.
Consequently, all of these changes can silently reinterpret existing data:

- changing operator precedence, tokenization, escaping, aliases, or accepted literals;
- renaming or removing a field, unit, codec constant, or shorthand;
- changing missing-value, comparison, list, range, or dictionary truth semantics; and
- fixing a parser/evaluator bug on which a stored expression happened to rely.

Syntax rejection is visible, but semantic drift is more dangerous: a Smart List can continue compiling and select a different set of tracks with no storage migration or diagnostic.
Workspace filters can restore with changed meaning, while a CLI script cannot state which grammar/semantics it expects.

Persisting only text is otherwise a strong boundary.
AST nodes, opcodes, access profiles, and dictionary ids are implementation details and should remain regenerable.
The missing piece is not persisted bytecode; it is a versioned dialect contract around the text.

There is also no single dispatch point.
Consumers call the current parser/compiler directly and own local empty-expression and error policies.
Adding compatibility branches independently in Smart Lists, workspace restore, and CLI would create several partial language authorities.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0006](0006-coherent-derived-track-views.md), [RFC 0008](0008-declarative-track-capability-bridge.md), [RFC 0009](0009-pure-expression-binding.md).

RFC 0006 should carry one dialect-bound filter request through asynchronous view replacement.
RFC 0008 must version capability/catalog changes that alter language fields or aliases.
RFC 0009 must bind and evaluate a selected dialect without mutating the library dictionary or confusing dialect version with dictionary generation.

## Goals

- Give every persisted or automation-relevant predicate text an explicit dialect id and semantic version.
- Freeze the current accepted grammar and truth behavior as a testable initial dialect.
- Dispatch parse, semantic compilation, diagnostics, and serialization through one core language boundary.
- Preserve text as the durable representation and keep AST/bytecode/runtime plans transient.
- Load existing unversioned Smart Lists and managed filters deterministically.
- Reject or preserve unsupported future dialects without silently evaluating them as current.
- Define when a migration may rewrite expression text and how semantic equivalence is demonstrated.
- Keep quick-search authoring policy and presentation outside the core predicate language owner.

## Non-goals

- Persist AST nodes, execution plans, dictionary ids, or access-profile opcodes.
- Version scalar format expressions in this RFC; they share parser primitives but have a different executable surface and compatibility risk.
- Add new predicate syntax or change existing truth semantics merely to exercise versioning.
- Make a dialect version equal to the application, database, YAML transfer, workspace, or CLI protocol version.
- Guarantee that arbitrary future dialects can be down-converted to old clients.
- Move Smart List membership or view lifecycle into the query library.

## Proposed design

### Predicate text value

Introduce a core value at persistence and application boundaries:

```text
PredicateText {
  dialect: PredicateDialectId
  version: PredicateDialectVersion
  text: UTF-8 string
}
```

The initial id is a stable product identifier such as `aobus-predicate`; its initial version freezes the current documented grammar and semantics.
The exact serialized spellings belong to the relevant library/workspace/CLI reference documents.

The type is not optional metadata attached after parsing.
Every API that can retain an expression or promise automation semantics accepts or returns the complete value.
Convenience APIs for current interactive input may fill the current dialect explicitly at the boundary.

### Central dialect registry and compiler

Provide one core registry/dispatch boundary:

```text
compilePredicate(PredicateText, PredicateBindingContext)
  -> ExecutionPlan
  -> UnsupportedDialect
  -> FormatRejected
  -> BindingFailure
```

Each registered dialect owns:

- lexical and grammar acceptance;
- canonical field/alias/unit/constant catalog for that version;
- semantic compilation and truth rules;
- diagnostics required for rejected text; and
- an optional canonical serializer used only by proven migrations or authoring tools.

Consumers no longer select the unversioned global parser and then infer semantics from the current build.
They pass the retained `PredicateText` to the registry and keep their existing ownership of source/view/result publication.

Parser implementation can be shared across versions when behavior is identical.
A version is a semantic contract, not a demand to copy all code.
Compatibility adapters and immutable tables may parameterize one parser/compiler while golden tests prove each registered version.

### Initial version freeze

Define the current predicate reference and evaluation specification at the implementation commit that accepts this RFC as the initial dialect baseline.
Freeze at least:

- token forms, escaping, keyword boundaries, and precedence;
- system fields, aliases, tag/custom variable syntax, units, and codec constants;
- existence, missing-value, string, numeric, list, and range behavior;
- case-sensitivity rules;
- empty-expression policy at each consuming boundary; and
- failure classification for syntax, semantic, unsupported-field, and unit errors.

Tests use durable golden inputs and expected compile/evaluation outcomes rather than only round-tripping the current implementation.

### Storage surfaces

Smart List records store dialect id, version, and text as one logical value.
Library YAML transfer includes the same identity when it transfers a Smart List.

Workspace/session filters store the complete predicate value rather than assuming the application current dialect on restore.
Transient in-memory filters may use the current version but retain it once captured in a view request.

CLI query options default to a documented protocol-selected dialect for human convenience.
Versioned automation under RFC 0029 can request or include a predicate dialect explicitly.

No surface stores the AST or execution plan.
Every process rebuilds those artifacts through the selected registered dialect.

### Legacy and future input

Existing unversioned persisted expressions are decoded as one explicitly named legacy baseline, which is the frozen initial dialect when compatibility is verified.
The decoder marks that provenance so a successful later save can add explicit version fields without changing text.

An unsupported future dialect is not reparsed as the current dialect.
The owning loader preserves its raw id/version/text when the persistence format supports round trip and exposes a typed unsupported outcome.
Smart List membership does not silently become `true`; source behavior for an invalid/unsupported expression remains explicitly specified and reported.

Older applications must preserve unknown version fields when they promise lossless round trip, or fail closed before rewriting the containing object.
That rule must align with the library and managed-state store contracts.

### Language evolution and migration

A change requires a new dialect version when it can alter whether accepted persisted text parses, what it binds to, or which tracks it matches.
Additive implementation optimizations that preserve all observable outcomes remain within a version.

A migration from version A to B may occur only when one of these is true:

- the text is unchanged and B formally retains A behavior for that input class; or
- a version-A parser and serializer produce version-B text whose evaluation is proven equivalent over the relevant typed domain.

The migration retains the original text/version until the containing library or managed-state transaction commits.
Failure leaves the old value readable by its registered dialect.

Do not use the current parser to rewrite unknown-version text.

### Diagnostics and authoring

Diagnostics include the dialect id/version that produced them.
Completion and editor capability surfaces select the same versioned catalog as the expression being edited.
They may offer an explicit "upgrade expression" action, but cannot silently substitute the current catalog while editing a legacy dialect.

Quick-search remains UIModel authoring policy.
It emits a complete current `PredicateText`; it does not become an alternate predicate dialect unless its generated syntax itself is persisted as a separately governed language.

## Alternatives

### Treat the application version as the language version

Most releases do not change predicate semantics, and a language change should not require retaining every application build's parser.
An independent dialect evolves only when its contract changes.

### Version only the library database

Predicates also live in workspace/session state and CLI automation, and two dialects can coexist during migration inside one database version.
Storage version does not identify language semantics precisely enough.

### Persist AST or bytecode

Internal shapes couple data to implementation layout, compiler optimizations, endianness, and opcode evolution.
Text plus dialect identity is smaller, inspectable, and recompilable.

### Promise that the grammar will never change

This forbids necessary corrections and still does not mechanically detect accidental semantic drift.
A frozen registered version makes compatibility executable.

### Migrate all expressions eagerly on startup

Startup-wide rewriting increases failure and durability risk and can make a library unreadable by an older client in one step.
Version dispatch permits deliberate transactional migration.

## Compatibility and migration

The first implementation preserves current expression text and behavior.
Existing unversioned Smart Lists and filters decode to the frozen initial version and gain explicit identity only on a governed write.

Adding fields to the library record or YAML transfer may require their existing version/migration mechanisms.
The storage proposal must specify how an older client handles the added identity before acceptance.

Public C++ call sites migrate from `std::string` plus implicit current parser to `PredicateText` at retention boundaries.
Low-level parser tests may continue to exercise one selected dialect directly.

CLI compatibility is staged with RFC 0029: existing unversioned invocations keep the documented default, while versioned automation can pin the language.

## Validation

- A golden corpus freezes parse success/failure, AST meaning, compile outcomes, diagnostics, access profiles, and evaluation truth for the initial dialect.
- Legacy unversioned Smart Lists and workspace filters load with the same membership as before the change.
- Unsupported future ids/versions are never evaluated through the current compiler and survive any promised lossless round trip.
- Mixed-version Smart Lists can materialize in one library through their registered dialects.
- Version dispatch covers empty expressions, aliases, quoted names, units, lists, ranges, missing fields, tags, custom metadata, and dictionary-backed comparisons.
- Migration tests prove commit/abort behavior and preserve the old value on parse, validation, or storage failure.
- Completion/editor tests use the catalog belonging to the edited dialect.
- CLI and library YAML fixtures prove that their own protocol/format versions remain independent from predicate dialect.
- Performance tests prevent version dispatch from materially regressing plan compilation or evaluation.
- A full `./ao check` passes after migration.

## Open questions

- What exact stable dialect id and numeric/string version representation should serialized surfaces use?
- Does the initial version freeze today's implementation exactly, or should known defects be separated into a legacy version and corrected current version during rollout?
- Which persisted workspace/filter surfaces require lossless preservation of unsupported future dialects?
- Should format expressions adopt a sibling versioned dialect in a follow-up RFC or one broader expression envelope with distinct language ids?
- What equivalence evidence is sufficient before an automatic text-rewriting migration is allowed?

## Promotion plan

If accepted and implemented:

- update the [track expression architecture](../architecture/track-expression.md) with dialect ownership, dispatch, and consumer boundaries;
- update the [predicate language reference](../reference/query/predicate-language.md) and [predicate evaluation specification](../spec/query/predicate-evaluation.md) with the initial stable version;
- update Smart List, track-source, workspace-session, and track-filter specifications with complete predicate values and unsupported-version behavior;
- update library YAML, library model, workspace state, and CLI references with exact serialized dialect fields where applicable;
- update contributor guidance for classifying compatible versus version-requiring language changes; and
- record the initial dialect baseline and migration policy in a decision when accepted.
