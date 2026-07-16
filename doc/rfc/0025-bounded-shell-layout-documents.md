---
id: rfc.0025.bounded-shell-layout-documents
type: rfc
status: draft
domain: application-shell
summary: Proposes strict version gates and resource budgets for shell layout decoding, validation, template expansion, installation, and persistence.
depends-on: none
---
# RFC 0025: Bounded shell layout documents

## Problem

Shell layout documents are user-controlled YAML that can drive recursive model construction and GTK widget creation, but the current loading boundary is permissive and effectively unbounded.

The serialized document contains a required `version` field and currently emits version `1`.
The decoder reads any numeric version into `LayoutDocument`, and `ShellLayoutStoreTest` explicitly proves that version `42` round-trips.
Component-state documents reject unsupported versions, while authored layout documents do not.
An older application can therefore interpret and later rewrite a future layout using version-1 assumptions.

The file path is constrained by preset id, but the content has no product limits for:

- input bytes read before YAML parsing;
- YAML/model node count and nesting depth;
- templates, children, properties, string lengths, or string-list entries;
- tooltip recursion;
- template expansion depth or expanded output size; and
- GTK components created by one build.

Cycle detection prevents a direct recursive template loop from recursing forever, but it does not bound a long acyclic chain or multiplicative expansion.
A small authored template graph can produce a much larger effective tree when referenced repeatedly.
Large values also flow through generic `ConfigStore`/RapidYAML decoding before the shell catalog has an opportunity to validate component semantics.

Malformed custom layouts currently fall back to the built-in preset, which is a useful availability policy.
However, parse failure, unsupported version, resource rejection, schema rejection, template-budget rejection, and component validation are not one typed candidate boundary.
Save is log-only, and an invalid or oversized candidate can be previewed or installed through paths whose validation obligations differ.

This is not a generic YAML policy problem.
Layout budgets must account for template expansion and widget construction, concepts that belong to the application shell format owner.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0010](0010-versioned-presentation-state.md).

RFC 0010 supplies the implemented stable presentation-state pattern; this RFC should align component, action, and node identifiers with its explicit-version and strict-candidate principles where the shell model shares those risks.
The current [atomic replacement contract](../spec/persistence/atomic-replacement.md) provides complete private-file replacement for saved custom documents.
The current [grouped configuration store](../spec/persistence/config-store.md) already isolates candidate encoding and reports whole-document replacement; this RFC must expose and classify that result at the shell workflow boundary.

## Goals

- Reject unsupported layout document versions before semantic interpretation or live installation.
- Bound input bytes, decoded model cost, nesting, template expansion, and effective GTK construction.
- Use one candidate decode/validate/expand pipeline for startup, editor preview, save, and programmatic installation.
- Preserve the last valid live shell when a candidate fails.
- Preserve unsupported or rejected custom files for recovery instead of rewriting them through an older model.
- Produce typed diagnostics that distinguish absence, parse, schema, version, budget, template, catalog, and I/O outcomes.
- Keep built-in fallback available and deterministic.
- Keep product budgets owned by the shell document contract rather than the generic YAML adapter.

## Non-goals

- Make the current GTK layout document the authority for the TUI shell.
- Add arbitrary scripting, loops, conditionals, includes, or network resources to layout YAML.
- Define exact component properties or action ids; those remain catalog/reference facts.
- Replace RapidYAML or `ConfigStore` solely for this feature.
- Make every visible unknown component fatal; diagnostic components can remain a deliberate compatibility behavior.
- Redesign the generic grouped-store candidate-save contract.

## Proposed design

### Shell-owned decode limits

Define a `LayoutDocumentLimits` product value with conservative defaults and injectable test values.
It covers at least:

```text
maximum file bytes before parse
maximum authored nodes and nesting depth
maximum templates and references
maximum children per node
maximum properties/layout entries per node
maximum key, scalar string, tooltip, and string-list sizes
maximum total decoded string bytes
maximum template expansion depth
maximum effective nodes and effective total value cost
maximum GTK components admitted for one build
```

Exact defaults become reference facts and are chosen from built-in-layout measurements plus a documented safety margin.
Budgets use checked arithmetic and report the observed/allowed dimension without echoing unbounded input.

The generic YAML reader may expose a byte-limited read primitive, but the shell owns the configured limit and the interpretation of rejection.

### Candidate pipeline

All authored layouts pass through one platform-neutral candidate pipeline:

```text
bounded bytes
  -> YAML parse with contained diagnostics
  -> strict root/schema decode
  -> supported-version dispatch
  -> authored-tree budget validation
  -> component/action/node-id semantic validation
  -> bounded template expansion
  -> effective-tree budget/catalog validation
  -> ValidatedLayoutCandidate
```

`ValidatedLayoutCandidate` is the only value accepted by `ShellLayoutSessionModel` installation and GTK `LayoutHost` build entry points.
Raw `LayoutDocument` remains useful for editor working copies and model tests but cannot bypass validation at a live boundary.

Startup, editor Apply, Save, reset, promotion, and built-in preset loading use the same validator with an explicit source classification.
Built-in documents are validated in tests and may fail fast as a packaging invariant; user documents produce recoverable outcomes.

### Strict version dispatch

Version `1` decoding validates the documented version-1 surface.
An absent, malformed, zero, or unsupported version yields a typed version/schema rejection before node interpretation.

The store retains the original custom file untouched on unsupported version.
It must not load the value into a permissive version-1 model and later overwrite unknown keys.
If a future version has a registered migrator, migration produces a separate candidate and persists only after validation and explicit commit success.

Version gates apply independently to authored layout and component-state documents because they have different schemas and lifecycle owners.

### Bounded model decode

Decode into a fresh candidate, never into the active document.
Traversal accounts for every root, child, tooltip, template root, property entry, sequence item, and owned string.

Reject wrong node kinds and missing required `type`/root structure rather than relying on empty defaults to become unknown components.
Unknown top-level or node keys follow an explicit version-1 policy: either reject them for fail-closed authoring or preserve them in an extension map.
Silently dropping them during re-encode is not acceptable for a document that the application claims to edit losslessly.

The initial proposal recommends strict rejection for unknown structural keys and catalog-governed handling for unknown component types/properties.

### Bounded template expansion

Template expansion receives a budget object and returns `Result<ExpandedLayout>` rather than encoding every failure as an ordinary diagnostic node.
It tracks:

- the current reference stack for cycle diagnostics;
- expansion depth;
- produced node count;
- copied/appended child and value cost; and
- tooltip expansion under the same budget.

Missing and unknown template ids may remain visible authoring diagnostics when within budget.
Cycles and budget exhaustion reject the candidate as a whole because a partial effective tree would not represent the authored structure reliably.

The budget is charged on produced output, so repeated references cannot multiply without limit even when the template graph is acyclic.

### Live installation and fallback

Build a new GTK component tree from a validated effective candidate before replacing the active host where toolkit constraints permit.
If validation or construction fails, retain the previous active session and tree.

At first startup with no previous tree:

- missing custom layout selects the built-in preset normally;
- rejected custom layout records a typed diagnostic and selects the matching validated built-in preset; and
- a packaged built-in validation failure is an application invariant failure, not a fallback to unbounded user input.

Fallback does not delete, truncate, normalize, or resave the rejected custom file.
The editor can expose recovery actions such as open raw location, reset customization, or duplicate-and-migrate, subject to platform policy.

### Save and preview

Apply validates and builds the working candidate but does not persist it.
Save validates once more against the exact document to be written, obtains a durable replacement result, then installs/acknowledges the saved candidate according to the shell transaction policy.

An over-budget or unsupported document cannot be saved through the structured editor.
Panel-size promotion and node-id regeneration must also preserve the limits and fail before changing the live session.

Diagnostics use the reporting architecture and identify preset, document version, failure class, and bounded location/evidence.
They do not place full untrusted YAML in notifications or logs.

## Alternatives

### Rely on file-size limits only

A byte limit bounds parser input but not recursive stack depth, semantic collection counts, template multiplication, or widget construction.
It is necessary but insufficient.

### Trust the component catalog to reject bad layouts

Catalog validation occurs after YAML/model allocation and does not own template expansion cost.
It also cannot distinguish unsupported document versions from unknown components.

### Truncate oversized documents

Truncation creates a different layout with ambiguous structure and may accidentally authorize a partial save.
Rejecting the candidate preserves user data and live state.

### Render error nodes for every failure

Visible unknown-component/template diagnostics are useful for bounded semantic mistakes.
Version, parse, cycle, and resource-budget failures cannot safely produce a partial tree and should trigger whole-candidate fallback.

### Put global limits in the YAML adapter

Reusable parser safety may provide absolute ceilings, but layout-specific output and widget budgets remain the shell format owner's responsibility.

## Compatibility and migration

Valid version-1 layouts within the selected budgets retain their current structure and rendering.
Documents with unsupported versions, missing required structure, silently ignored keys, or excessive cost will now be rejected and preserved instead of permissively loaded.

The version-1 unknown-key policy may expose files that previously appeared to work while losing data on save.
Before rollout, repository fixtures and representative user customizations should be audited and migration diagnostics made actionable.

No automatic rewrite occurs merely because a version-1 file loads.
Future migrations operate on a separate candidate and use observable atomic replacement.

Built-in presets must remain well below every default budget; CI records their measured authored/effective cost to detect accidental growth.

## Validation

- Version tests cover missing, malformed, zero, current, and unsupported future versions; future files remain byte-identical after fallback.
- Boundary tests cover exactly-at-limit and limit-plus-one for every configured byte/count/depth/string/sequence budget.
- Template tests cover cycles, long chains, repeated expansion, tooltip recursion, appended children, and output multiplication.
- Candidate tests prove wrong node kinds, missing root/type, unknown structural keys, and catalog failures do not alter the active session.
- Startup tests prove deterministic custom-to-built-in fallback with one bounded diagnostic.
- Editor tests prove Apply and Save share the candidate validator and cannot bypass it through promotion or regenerated ids.
- Fault-injection tests preserve the last live tree and custom file on parse, validation, build, and persistence failure.
- Built-in layout budget measurements and catalog validation run in CI.
- Parser/validator fuzzing exercises arbitrary bounded YAML without unbounded recursion, allocation, or crash.
- GTK tests assert that one build cannot create more than the admitted effective component count.
- A full `./ao check` passes after implementation.

## Open questions

- What measured defaults provide enough customization headroom while bounding memory and GTK build latency?
- Should unknown structural keys be rejected or preserved through an explicit extension map in version 1?
- Can GTK component construction be made fully candidate-first before host replacement for every component with side effects?
- Should rejected custom layouts be quarantined/renamed automatically, or only preserved in place with explicit recovery actions?
- Which absolute parser ceilings belong in `ao::yaml` in addition to the shell-owned product limits?

## Promotion plan

If accepted and implemented:

- update the [application shell architecture](../architecture/application-shell.md) with the validated-candidate and bounded-expansion boundary;
- update the [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with strict version dispatch and preservation of unsupported authored documents;
- update the [shell layout lifecycle specification](../spec/shell/layout-lifecycle.md) with candidate, fallback, preview, save, and failure transitions;
- update the [layout document reference](../reference/shell/layout-document.md) with supported versions, exact limits, unknown-key policy, and typed rejection surface;
- update shell editor/user guidance with recovery and reset behavior; and
- record the selected limits and unknown-key compatibility policy in a decision when accepted.
