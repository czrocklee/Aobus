---
id: rfc.0032.explicit-managed-state-codecs
type: rfc
status: draft
domain: persistence
summary: Proposes retiring reflected application configuration traits in favor of domain-neutral YAML node helpers and explicit owner-local managed-state codecs.
depends-on: none
---
# RFC 0032: Explicit managed-state codecs

## Problem

The grouped configuration store provides useful fail-closed candidate writes, but its serialization boundary is not owned coherently.
[`ConfigStore`](../../app/include/ao/rt/ConfigStore.h) transitively exports [`ConfigTraits`](../../app/include/ao/yaml/ConfigTraits.h), a large application-level header declared in the generic-looking `ao::yaml` namespace.
That header includes application logging and core ids, then offers broad template rules for arithmetic values, enums, `raw()` wrappers, containers, and reflected aggregates.

The reflected aggregate path uses Boost.PFR member names as YAML keys.
Renaming a C++ field can therefore change durable data without an explicit format edit, and reordering or renumbering an enum can change its serialized ordinal.
The generic `HasRawMethod` rule similarly treats an implementation convention as a persistence contract even when no format owner selected that representation.

The permissive readers add another ambiguity.
Some container paths skip invalid children or retain caller defaults, while `readExact` requires recursively complete aggregate/vector input.
Strict candidate loading is valuable, but it does not make generated member names or enum ordinals stable format vocabulary.
The caller still cannot see from its type whether it is reading a versioned document, a best-effort preference group, or an implementation-shaped snapshot.

Recent workspace-session and track-presentation work already uses explicit semantic-state/DTO codecs for the most consequential persisted payloads.
Those codecs validate stable textual ids, reject malformed candidates, and let the semantic owner decide compatibility, but generic traits still turn the DTO member names into YAML keys.
Meanwhile shell layout and component-state documents still directly consume generic traits, and GTK application preferences, window state, session values, and key bindings retain several aggregate-shaped groups.

Moving the whole traits header into the core YAML library would fix only the file location.
It would promote application logging, broad reflection, and accidental schema derivation into a public mechanism.
Deleting all helpers at once would move repetitive scalar/node mechanics into every codec.
The required boundary is narrower: the core utility layer may own domain-neutral YAML node operations, while every durable application shape has an explicit semantic owner and codec.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0025](0025-bounded-shell-layout-documents.md).

RFC 0025 proposes strict, bounded shell-layout candidate decoding.
Its document and component-state codecs should use the explicit owner-local pattern from this RFC if both proposals are implemented, while the shell RFC continues to own layout-specific limits, versions, fallback, and unknown-key policy.
Neither proposal must wait for the other to establish its independent contract.

## Goals

- Remove the application-owned `ao/yaml/ConfigTraits.h` catch-all and stop `ConfigStore` from exporting serialization policy transitively.
- Retain only domain-neutral RapidYAML scalar/node mechanics in the core utility YAML surface, with no application ids, logging, or durable-schema policy.
- Give each persisted payload family an explicit encode/decode boundary beside its semantic owner.
- Require durable field names, enum/id encodings, version handling, defaults, and unknown-field policy to be deliberate code rather than consequences of C++ layout.
- Preserve `ConfigStore` as the grouped document and observable replacement owner without making it the schema owner.
- Keep failed decoding candidate-based: an invalid group never partially mutates live state.
- Make codec errors structured enough for the calling workflow to classify absence, format rejection, unsupported version, and persistence failure without logging inside the codec mechanism.
- Migrate source APIs coherently without retaining a second reflected compatibility path.

## Non-goals

- Replace RapidYAML, redesign the atomic replacement primitive, or introduce a general object-relational mapper.
- Redesign the existing [write-only YAML reflection formatter](../../include/ao/yaml/Reflect.h) used for CLI YAML/JSON output; [RFC 0029](0029-versioned-cli-automation-protocol.md) owns any structured CLI schema migration.
- Require every private best-effort preference group to become a separately versioned top-level file.
- Put application DTOs, stable ids, logging, recovery, or user-visible reporting in the core YAML target.
- Infer migrations automatically from C++ member names or historical aggregate layouts.
- Preserve arbitrary malformed or future data unless the owning format explicitly requires round-trip extensions.
- Make `ConfigStore` responsible for defaults, compatibility, resource limits, or domain validation.

## Proposed design

### Two explicit layers

The YAML boundary is split into two layers.

The core mechanism layer owns only operations equivalent to:

```text
scalar read/write for bool, bounded integers, floating point, and strings
node-kind and child/key inspection
safe conversion between string_view and RapidYAML spans
small helpers for explicitly requested sequence/map traversal
parser and emitter ownership
```

These helpers live under `include/ao/yaml`, are provided by `ao_utility`, and depend only on core error/value utilities and RapidYAML.
They do not know `TrackId`, `ListId`, application logging, Boost.PFR, `raw()` conventions, or a generic enum representation.

The application codec layer lives beside each semantic payload owner.
It names every persisted key and explicitly converts stable ids and enums.
It may use a private DTO when that separates the wire shape from live state, but aggregate reflection never defines the durable format.

The existing `ao::yaml` reflection facility remains a separate one-way output formatter.
It is not a replacement for `ConfigTraits`, is never selected by `ConfigStore`, and cannot define a managed-state format that Aobus later reads.
Its use of explicit enum-name tables and optional key overrides is governed with the CLI output contract; RFC 0029 decides whether a versioned automation protocol requires fully explicit output schemas.

### Codec contract

Each payload family exposes a narrow contract equivalent to:

```text
encodeFoo(Foo const&) -> Result<YamlDocument or YamlNodeValue>
decodeFoo(YamlDocument or ConstNodeRef) -> Result<Foo>
```

An owner may instead provide a stateless codec object when `ConfigStore` needs a generic call shape.
The type must still be selected explicitly at the call site; argument-dependent lookup or a catch-all template must not silently opt a new type into persistence.

Decode constructs a fresh candidate and returns it only after complete validation.
Encode completes a candidate representation before `ConfigStore` changes the grouped document.
Neither path logs or reports to users.
Errors carry bounded format context, while the owning workflow selects diagnostic and reporting disposition.

Closed enums use explicit textual ids or an exhaustive named mapping owned by the format.
Open identifiers preserve a validated stable string form.
Numeric ordinals are allowed only when the format owner deliberately documents and tests them as stable protocol values.

### `ConfigStore` after the split

`ConfigStore` remains responsible for:

- loading and parsing the grouped YAML document;
- finding, replacing, or removing named groups;
- building a complete write candidate;
- committing through the current observable atomic-replacement contract; and
- preserving the live caller value when a load fails.

It does not include or export a global traits header.
Its typed convenience functions either receive an explicit codec or accept an already encoded node/document value.
A representative API shape is:

```text
load(group, codec) -> Result<Decoded>
save(group, value, codec, ...)
loadNode(group) -> Result<ConstNodeCandidate>
saveNode(group, EncodedNode, ...)
```

The exact shape is selected during implementation based on ownership and multi-group save ergonomics.
The important constraint is that `ConfigStore` cannot derive a durable schema solely from `T`.

Multi-group save retains its all-or-nothing file candidate.
Each group is encoded independently first; if any codec rejects its value, no group is replaced.
Product workflows that intentionally want independent persistence use separate save operations and own the partial-success policy explicitly.

### Payload inventory and ownership

Migration starts from a complete inventory, grouped by semantic owner rather than template instantiation:

| Payload family | Codec owner | Required treatment |
|---|---|---|
| Workspace session | runtime workspace | Preserve the implemented versioned explicit codec |
| Track column layout and list presentation preference | UIModel presentation | Preserve the implemented explicit codecs and stable ids |
| Shell layout document and component state | UIModel application shell | Add explicit version/schema codecs; coordinate limits with RFC 0025 |
| GTK window, application preference, and session groups | GTK application composition | Add named private codecs and deliberate default/unknown-key policy |
| Key bindings and other frontend-authored groups | Owning frontend or UIModel capsule | Encode stable command/action ids explicitly |
| Generic scalar/container mechanics | core YAML | Keep only when free of application semantics and implicit schema |

The inventory records current YAML keys and representations before code changes.
That record is evidence for explicit codecs, not a promise to support obsolete layouts indefinitely.

### Compatibility policy belongs to the owner

This RFC does not impose one unknown-field policy on every configuration group.

- A versioned authored document may reject unknown or future structure and preserve the original file.
- A private preference group may apply documented defaults for absent optional fields.
- An extension-bearing format may retain unknown fields explicitly.
- A transient implementation snapshot may deliberately have no migration reader.

Whichever policy is selected appears in the owner codec and tests.
Generic helpers never silently skip a malformed sequence element or unknown key on the owner's behalf.

The current `loadExact` name disappears with reflected aggregate decoding.
Strictness becomes a property of the selected codec, where required fields, optional fields, allowed extras, and semantic validation are visible together.

### Logging and error ownership

The core YAML mechanism returns typed parse/conversion failures and contains no `rt::Log` dependency.
Application codecs add bounded field/path evidence without formatting user-facing copy.
`ConfigStore` adds group and file operation context.
The workflow that initiated load/save decides whether the result is inline, diagnostic-only, a fallback, or a shared report under the failure/reporting architecture.

This layering prevents a reusable decoder from silently logging and continuing, and prevents a failure-path log dependency from making `ao::yaml` application-owned.

### Migration sequence

1. Inventory every `ConfigStore` payload and direct `yaml::read`/`write` use, recording current keys and owner policy.
2. Extract the minimal domain-neutral node/scalar helpers into core YAML with focused tests.
3. Adapt the already-explicit workspace and presentation codecs to the new helper surface.
4. Implement explicit shell layout/component codecs together with, or compatible with, RFC 0025.
5. Replace GTK and remaining runtime aggregate persistence one payload family at a time.
6. Change `ConfigStore` to require explicit codecs or encoded nodes and remove its traits export.
7. Delete `ConfigTraits.h`, Boost.PFR persistence use, generic enum/raw-object encoding, and temporary adapters.
8. Add guardrails against application headers under the core YAML namespace and reflected durable schemas.

No compatibility alias or generic fallback lands after the final migration.
During each slice, one payload has exactly one writable codec.

## Alternatives

### Move all current traits into core YAML

This corrects the directory but codifies reflection, application ids, logging, and implicit enum/raw representations as reusable public policy.
It increases rather than removes the accidental-schema surface.

### Keep reflection for private preferences only

The boundary between private and durable data changes over time, and even private preferences must survive field refactors within a released application.
Small explicit codecs are cheaper than auditing whether each reflected aggregate has silently become user state.

### Specialize global traits for every application type

Explicit specializations would avoid some reflection, but they still centralize unrelated schemas in one namespace and remain transitively selected by type.
Owner-local named codecs make dependency direction and compatibility policy visible.

### Replace `ConfigStore` with one file per value

File granularity does not solve schema ownership and would discard the useful complete-document candidate/commit behavior.
Separate files remain an owner decision for payloads with independent lifecycle or durability needs.

### Adopt a general serialization framework

A framework may reduce boilerplate but cannot choose stable ids, unknown-key behavior, resource bounds, or migration policy.
The repository already has sufficient YAML mechanics; the missing piece is explicit ownership.

## Compatibility and migration

This proposal changes internal C++ APIs and may deliberately change undocumented aggregate-shaped configuration formats as their owners define explicit codecs.
Aobus has no source-compatibility obligation between application layers.

Implemented versioned formats retain their documented stable ids and version contracts.
For remaining groups, implementation first records whether existing user data is supported, rejected with fallback, or intentionally reset.
No migration code is added merely because a reflected representation once existed; compatibility must be justified by the owning product state.

At no point may a failed candidate decode overwrite a live value or cause an unreadable/future file to be rewritten implicitly.
Existing observable atomic replacement and fail-closed save behavior remain in force.

## Validation

- An inventory test or review artifact accounts for every persisted type and direct YAML read/write call before `ConfigTraits.h` is deleted.
- Core utility YAML tests cover scalar conversion, node-kind rejection, bounded error context, and parser/emitter ownership without including application headers.
- Every codec has round-trip tests plus malformed-kind, missing-required-field, invalid-id/enum, unknown-field, and semantic-rejection cases appropriate to its owner policy.
- Versioned codecs reject unsupported future versions before interpreting payload fields and do not trigger an implicit rewrite.
- Candidate tests prove a failed decode leaves the supplied/live object unchanged.
- Multi-group tests prove one encode/decode failure cannot partially commit the grouped document.
- Format fixtures pin deliberate key names and stable textual ids independently of C++ member names and declaration order.
- Repository searches find no Boost.PFR managed-state persistence, generic enum ordinal codec, `HasRawMethod` codec, application logging in the core utility YAML surface, or transitive traits export from `ConfigStore`.
- Compile-boundary checks prove the `ao_utility` YAML surface does not include application/runtime ids and application payload codecs live with their semantic owners.
- RFC 0025 validation remains authoritative for shell byte, depth, collection, expansion, and widget-construction limits.
- The implementation passes `./ao check` and `./ao docs check`.

## Open questions

- Should `ConfigStore` accept explicit codec objects directly, or only transport encoded/decoded YAML node values between the store and owner codecs?
- Which small sequence/map traversal helpers remain sufficiently policy-free to belong in core YAML?
- Which current GTK preference groups merit explicit version fields, and which require only stable named optional fields?
- Should format fixtures preserve emitted key order for review readability even when decode treats map order as irrelevant?

## Promotion plan

If accepted, update the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with the mechanism/codec/store ownership split.
Update the [grouped configuration store specification](../spec/persistence/config-store.md) with the explicit-codec API, candidate guarantees, and multi-group behavior.

Update each affected workspace, presentation, shell, and frontend state specification/reference with its implemented fields, versions, defaults, and unknown-key policy.
Coordinate the shell layout document reference with RFC 0025 rather than duplicating its limits and fallback authority.
Add development guidance and build guardrails for owner-local codecs, stable ids, no reflected durable schemas, and no application dependencies in core YAML.
Record a decision if the selected codec/store API or compatibility policy has durable alternatives worth preserving beyond this RFC.
