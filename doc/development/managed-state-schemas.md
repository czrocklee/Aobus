---
id: development.managed-state-schemas
type: development
status: current
domain: persistence
summary: Defines how contributors add and review explicit owner-local schemas for application-managed YAML state.
---
# Managed-state schemas

## Scope

This guide owns the contributor workflow for adding or changing an application-managed YAML payload.
It covers schema ownership, stable schema choices, `ConfigStore` integration, Core YAML helper boundaries, test evidence, and repository guardrails.

Exact groups, fields, versions, defaults, and product restore behavior belong to the [application managed-state reference](../reference/persistence/application-config.md) and the linked domain specifications rather than this guide.

## Policy

Every managed-state reader has one explicit schema beside the owner of the payload's semantics or document format.
The schema must make these decisions visible in source:

- stable serialized field names;
- textual or numeric representations for identifiers and enums;
- required fields and owner-seeded optional defaults;
- root and nested node kinds;
- duplicate-key and unknown-key policy;
- version dispatch before interpretation of version-specific payload;
- structural and semantic candidate validation;
- compatibility or migration behavior, when deliberately supported.

Use the semantic value directly when it cleanly expresses the persistence contract.
Introduce a private persistence document only when it materially isolates serialized vocabulary, version dispatch, or conversion from the live model.
Do not create a generic application schema registry or catch-all serialization namespace.

Managed-state schemas must not be derived solely from Boost.PFR, unannotated C++ member names, declaration order, enum ordinals, a `raw()` convention, or another implicit type rule.
This constrains schema derivation rather than banning reflection as a C++ implementation technique: any future reflection-backed schema must still expose owner-selected stable keys, representations, defaults, version policy, and validation as deliberate reviewable metadata.
Core [`ao::yaml` serialization helpers](../../include/ao/yaml/Serialization.h) and the lower-level [`RapidYAML adapter`](../../include/ao/yaml/RymlAdapter.h) remain domain-neutral: they may validate node kinds and keys, read and write explicitly named maps, traverse caller-selected sequences, convert scalars, own arena strings, parse YAML, and build bounded diagnostic context, but cannot include application ids, logging, runtime, UIModel, or frontend types.
The separate Core reflection helper is one-way output support and must not be selected for a format that Aobus later reads as managed state.

Do not retain a reflected or permissive fallback reader after introducing an explicit schema.
A compatibility reader or migration is product behavior and requires an explicit owner, documented input boundary, tests, and a current specification.

## Workflow

Work from the repository root.

1. Identify the semantic owner, writer authority, logical document, literal group, seeded defaults, and restore commit point.
2. Read the current domain specification and reference. Update those authorities when behavior or serialized shape changes.
3. Define an owner-local schema satisfying `ao::rt::ConfigSchema<T>` when the payload uses `ConfigStore`:

   ```cpp
   Result<> serialize(ryml::NodeRef output, T const& value) const;
   Result<T> deserialize(ryml::ConstNodeRef input, T const& seed) const;
   ```

4. Serialize every stable key explicitly. Use `MapWriter` scalar/value/sequence operations, `writeStringMap()` for an owner-selected dynamic string-keyed map, or the lower-level node helpers so the destination tree owns dynamic key and string bytes.
5. Deserialize into local values. Use `MapReader` required/optional scalar, nested-value, and sequence operations, `readStringMap()` for an owner-selected dynamic map, or the lower-level structural helpers, then perform owner-specific enum, identifier, range, and cross-field validation.
6. Read a version field before version-specific siblings and return `NotSupported` for an unsupported future version unless the owning specification defines another outcome.
7. Return one complete candidate. Never mutate the caller's live state during partial deserialization.
8. Pass the schema explicitly to `ConfigStore::load()` or `save()`. Use `saveTogether(configWrite(...), configWrite(...))` when sibling groups must be replaced atomically.
9. Update the managed-state registry and the domain-owned specification/reference. Mark an implementing RFC terminal only after code, tests, guardrails, and current documentation agree.

`ConfigStore::load()` returns `Result<bool>`: `false` means the group is absent and the seeded value is unchanged; `true` means the schema accepted a candidate and the store installed it.
Schema failures preserve their error code with group context.

## Test evidence

Add tests at the lowest owner layer that prove the payload contract rather than only exercising a file round trip.
Where applicable, cover:

- canonical serialize/deserialize round trip and exact emitted vocabulary;
- absent group behavior and seeded missing-field policy;
- wrong root and nested node kinds;
- missing required fields and malformed present optional fields;
- duplicate and unknown keys under the selected policy;
- every closed enum/token mapping and rejection of an unknown value;
- current, unsupported future, and intentionally supported older versions;
- invalid identifiers, ranges, and cross-field semantics;
- representative checked-in or inline documents used by the application;
- no live-state mutation after deserialize failure;
- no backing-file or live-document mutation after returned and thrown serialize failures;
- one successful and one failed atomic multi-group save when groups share a commit boundary;
- temporary key/value storage lifetime when a schema writes dynamic strings.

When a policy does not apply, make that absence evident in the owner contract; for example, an unversioned frontend preference schema has no artificial version test.

Repository configuration rejects restoration of `ConfigTraits.h`; CMake guardrails reject application managed-state selection of the one-way `Reflect.h` formatter, application schemas declared under `namespace ao::yaml`, and application/logging dependencies in the Core YAML helper boundary.
The guardrails deliberately do not ban Boost.PFR or reflection throughout application code; review searches and schema tests protect the narrower rule that unannotated reflection cannot define a managed-state schema.
Keep the guardrail source scopes aligned when adding a new application subtree.

## Validation

Run focused owner tests while developing, then complete the repository validation gate from the root:

```bash
./ao check
./ao docs check
```

Do not run formatting or lint-fix commands merely as part of this workflow; follow the repository's explicit formatting and linting policy.
Useful review searches are:

```bash
rg -n "ConfigTraits|loadExact|ao/yaml/Reflect|boost/pfr/core|namespace ao::yaml" app test CMakeLists.txt
rg -n "ConfigTraits|loadExact|reflection-based|ordinary deserialize|exact deserialize" doc --glob '!doc/rfc/**'
```

Historical RFC text may describe a removed mechanism; current architecture, specifications, references, development guides, and production code may not.

## Troubleshooting

- If emitted strings become corrupted after a save, verify that every key and value copied from temporary schema storage was moved into the destination tree arena.
- If a malformed field partially changes live state, make `deserialize()` construct and return a separate candidate and install it only through the store's success path.
- If a future-version document reports an error from a later payload field, dispatch the version before reading version-specific siblings.
- If adding one field unexpectedly changes compatibility, replace any type-derived emission with literal field mappings and record the missing/unknown policy in the owner reference.
- If two groups must remain coherent but can be observed from different file generations, pass their `configWrite()` descriptors to one `saveTogether()` call.
- If an owner needs resource limits beyond structural strictness, define them in that format's specification; the generic YAML helpers and `ConfigStore` do not invent document budgets.

## Related documents

- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Grouped configuration store specification](../spec/persistence/config-store.md)
- [Reusable YAML adapter specification](../spec/persistence/yaml-adapter.md)
- [Application managed-state surface](../reference/persistence/application-config.md)
- [Documentation system](../README.md)
- [Testing policy](test.md)
- [Validation and review](test/validation-and-review.md)
- [Linting](linting.md)
- [RFC 0032: explicit managed-state schemas](../rfc/0032-explicit-managed-state-schemas.md)
