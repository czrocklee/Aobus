---
id: rfc.0025.bounded-shell-layout-documents
type: rfc
status: implemented
domain: application-shell
summary: Records the bounded layout-preparation gate, preserved-file fallback, and staged GTK tree replacement implemented for shell layout documents.
depends-on: none
---
# RFC 0025: Bounded shell layout documents

## Problem

Customized shell layouts are user-controlled YAML that can allocate a recursive model, expand reusable templates, and create a complete GTK widget tree.
The original path had strict schema parsing and template-cycle diagnostics, but no product limit on file size, authored model cost, acyclic template depth, repeated expansion, or GTK construction size.
Different load, preview, save, reset, and promotion paths could also reach rebuilding with different checks.

A malformed custom file already fell back to the matching built-in layout, which was the right availability policy.
The missing contract was a small shared safety boundary that preserved both the last live tree and the rejected file.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0010](0010-versioned-presentation-state.md), [RFC 0032](0032-explicit-managed-state-schemas.md).

RFC 0010 supplies the stable-id and explicit-version precedent.
RFC 0032 supplies the implemented owner-local layout schema and fail-closed grouped-store boundary.
This RFC owns only shell-layout resource limits, preparation, fallback, and GTK replacement.

## Goals

- Bound customized files before parsing and replacement.
- Bound authored structure and template-expanded output with one shell-owned policy.
- Require a prepared proof at the GTK construction boundary.
- Keep the previous live tree and rejected customization when preparation fails.
- Preserve existing diagnostic components and error codes where they remain safe.
- Close those risks without introducing migration, quarantine, or a general candidate framework.

## Decision

Implement one compact layout safety kernel rather than the original broad candidate framework.

Every live GTK layout is derived from `uimodel::prepareLayout()`.
That function validates the current version, meters the complete authored document, performs bounded template expansion, meters the effective tree, and returns `PreparedLayout`.
`LayoutRuntime` and `LayoutHost` accept `PreparedLayout`, so raw `LayoutDocument` cannot enter GTK construction directly.

Raw documents remain the appropriate values for editor working copies, persistence, and `ShellLayoutSessionModel` authority.
This avoids making preparation a second application-session model or duplicating the authored document inside a large candidate object.

## Implemented limits

`LayoutDocumentLimits` owns the product defaults and remains injectable for tests.

| Dimension | Default |
|---|---:|
| Serialized custom-layout file | 256 KiB |
| Authored entries | 4,096 |
| Authored depth | 64 |
| Authored owned string bytes | 256 KiB |
| Effective entries | 2,048 |
| Effective depth | 64 |
| Effective owned string bytes | 512 KiB |

The file limit applies before parsing an existing file and before replacing it with newly serialized bytes.
The generic YAML file reader and `ConfigStore` expose an optional byte ceiling, while `ShellLayoutStore` owns the selected value.

Entry accounting charges each layout node, template-map entry, `props`/`layout` map entry, and string-list element.
String accounting charges owned ids, types, template keys, property/layout keys, string values, and string-list values.
Depth includes child and tooltip edges; effective depth also includes template-reference traversal.
All arithmetic is checked before incrementing a meter.

Authored accounting includes every template, including unused templates.
Effective accounting charges every produced copy, so repeated references cannot multiply a small authored template without limit.
It also charges reference overlays even when they replace a value copied from the template, bounding transient expansion work rather than only the final retained shape.
Missing template ids, unknown template ids, and cycles retain the existing bounded diagnostic-node behavior; exhausting any budget rejects the whole preparation.

## Persistence and fallback

`ShellLayoutStore::load()` distinguishes absence from rejection with `Result<optional<LayoutDocument>>`:

- absence returns a successful empty optional and selects the built-in preset;
- malformed, unsupported, or over-budget custom content returns an error;
- startup logs the bounded error, selects and prepares the matching built-in document, and leaves the custom file byte-identical.

`ShellLayoutStore::save()` prepares the candidate before serialization.
When a custom file already exists, the store also requires that existing document to load and prepare before replacement.
An older application therefore does not overwrite an unsupported, malformed, or over-budget customization through the structured save path.
Each successful write still uses the existing atomic complete-file replacement mechanism.

The implementation reuses existing `Error::Code` values instead of adding a parallel layout error taxonomy.
In particular, schema failures use `FormatRejected`, unsupported versions use `NotSupported`, size and tree limits use `ValueTooLarge`, allocation failure uses `ResourceExhausted`, and storage/construction failures retain their existing I/O or state codes.

## Staged GTK replacement

`LayoutHost::prepare()` builds a detached candidate component tree against a candidate view of preset, component state, edit mode, callbacks, and the next component-state generation.
It does not remove the active child or advance the active generation.

`LayoutHost::commit()` first advances the generation, then destroys the retiring tree and installs the prepared tree.
The generation ordering prevents retiring components from writing state into the replacement document.
A failed or discarded preparation leaves the active tree, session, runtime state, and generation unchanged.

Startup load, editor preview/save, runtime-state reset, and panel-size promotion all prepare before committing a replacement.
Editor Save also applies the same document limits before emitting a save request.

## Deliberately omitted

The following parts of the initial proposal were not needed to close the verified risks:

- no migration registry, legacy reader, automatic rewrite, or quarantine/rename policy;
- no generic YAML-wide product budget or parser redesign;
- no separate limits for every property kind, child collection, template reference, or component family;
- no new diagnostic hierarchy, recovery UI, or durable workflow receipt;
- no requirement that unknown components or bounded template mistakes become fatal; and
- no large `ValidatedLayoutCandidate` spanning persistence, catalogs, session authority, and GTK.

Catalog/action validation remains owned by the editor and existing catalog policies.
Unknown component types remain visible GTK diagnostic components.
The safety proof required by GTK is intentionally only `PreparedLayout`.

## Compatibility

Version `1` documents within the limits keep their existing template merge and rendering behavior.
Unsupported versions, malformed structures, and over-budget documents are rejected and preserved rather than normalized or overwritten.
There is no compatibility or migration path beyond deterministic built-in fallback.

Built-in `classic` and `modern` layouts must prepare under the default limits in CI.

## Validation

Implemented coverage includes:

- exactly-at-limit and limit-plus-one entry, depth, and string-byte cases;
- unused templates, long acyclic chains, and repeated template expansion;
- built-in preset preparation under default limits;
- oversized file rejection before parsing and oversized serialized-candidate rejection;
- byte-identical preservation of oversized and unsupported existing custom files;
- startup fallback from an oversized customization without changing it;
- preservation of the active GTK tree when an editor preview exceeds the effective budget;
- editor rejection of an over-budget save;
- staged-host construction failure and discarded-candidate preservation; and
- generation invalidation before retiring stateful components.

Current authority is documented in the [application shell architecture](../architecture/application-shell.md), [persistence architecture](../architecture/persistence-and-managed-state.md), [layout lifecycle specification](../spec/shell/layout-lifecycle.md), and [layout document reference](../reference/shell/layout-document.md).
