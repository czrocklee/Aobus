---
id: shell.layout-document
type: reference
status: current
domain: application-shell
summary: Enumerates the version 1 shell layout document, node, value, template, tooltip, resource-limit, and managed-file surface.
---
# Shell layout document

## Scope and version

This reference owns the exact serialized surface of `uimodel::LayoutDocument` version `1` and its `LayoutNode` and `LayoutValue` members.
Template expansion, build, editor, and fallback behavior belong to the [shell layout lifecycle specification](../../spec/shell/layout-lifecycle.md).

The `version` field is required, emitted as `1`, and only version `1` is accepted.
An unsupported version returns `NotSupported` before the root or templates are interpreted.

## Code boundary

This surface belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
The model and YAML schema are under `app/include/ao/uimodel/layout/document/` and `app/uimodel/layout/document/`; GTK owns built-in documents, stores, and widget factories.

## Document surface

A serialized `LayoutDocument` is a mapping:

| Key | Type | Required | Meaning |
|---|---|---|---|
| `version` | Unsigned integer | Yes | Document version marker; current emitted value is `1`. |
| `root` | `LayoutNode` mapping | Yes | Root node of the authored layout. |
| `templates` | Mapping from template id to `LayoutNode` | No | Reusable authored subtrees. |

GTK persists the document inside the literal `layout` top-level `ConfigStore` group.
Built-in `classic` and `modern` presets are compiled from `default_layout.yaml` and `modern_layout.yaml` resources.

## Node surface

Each `LayoutNode` is a mapping:

| Key | Type | Emission | Meaning |
|---|---|---|---|
| `id` | String | Omitted when empty. | Stable node identity; required for persistent stateful behavior. |
| `type` | String | Always emitted. | Component type id or special `template` reference type. |
| `props` | Mapping from string to `LayoutValue` | Omitted when empty. | Component properties interpreted through its descriptor/factory. |
| `layout` | Mapping from string to `LayoutValue` | Omitted when empty. | Parent/common placement and CSS properties. |
| `children` | Sequence of `LayoutNode` | Omitted when empty. | Ordered structural children. |
| `tooltip` | One `LayoutNode` | Omitted when absent. | Tooltip-surface subtree. |

Node keys are closed to the six fields above.
Unknown or duplicate keys reject the whole document.
`type` is required and nonempty; `id` remains optional.
The `props` and `layout` mappings deliberately allow dynamic nonempty keys, while rejecting duplicates and malformed values.

## Value surface

`LayoutValue` is exactly one of:

| C++ alternative | YAML form |
|---|---|
| `monostate` | Null. |
| `bool` | Scalar `true` or `false`. |
| signed 64-bit integer | Integer scalar. |
| `double` | Floating-point scalar. |
| `string` | Any remaining scalar. |
| `vector<string>` | Sequence whose every element is a non-null scalar string. |

Deserialize preserves every quoted scalar as a string.
It classifies unquoted scalar text in this order: canonical boolean, complete signed integer parse, complete double parse, then string.
Maps and sequences containing a null or non-scalar child are not valid `LayoutValue` values.

Typed descriptor kinds are `Bool`, `Int`, `Double`, `String`, `Enum`, `StringList`, and `Size`.
The descriptor kind informs editors and validation; the stored representation remains a `LayoutValue`.

## Template surface

A template reference is a node with:

```yaml
type: template
props:
  templateId: playback.defaultBar
```

`templateId` is the only reserved template-reference prop.
The reference may also supply `id`, `layout`, other `props`, appended `children`, and `tooltip`; expansion precedence belongs to the lifecycle specification.

Template ids are strings and are exact, case-sensitive keys in the document's `templates` mapping.

## Example

```yaml
layout:
  version: 1
  root:
    id: app-root
    type: box
    props:
      orientation: vertical
    children:
      - id: playback-bar
        type: template
        props:
          templateId: playback.defaultBar
        layout:
          valign: start
  templates:
    playback.defaultBar:
      type: box
      props:
        orientation: horizontal
        spacing: 6
      children:
        - type: playback.playPauseButton
```

## Validation rules

- The `layout` group must deserialize as a mapping containing `version` and `root`; only optional `templates` may be omitted.
- Root and node mappings reject unknown and duplicate fixed keys; dynamic prop, layout, and template mappings reject duplicate or empty keys.
- The version must be `1`, and every node must have a nonempty scalar `type`.
- Every sequence element is validated; malformed string-list, child, or template entries reject the complete candidate rather than being skipped.
- A template reference with no `templateId`, an unknown id, or a recursive expansion chain becomes a diagnostic node.
- A stateful node needs a non-empty unique id after expansion to persist runtime state.
- Editor save rejects duplicate stateful ids; anonymous stateful nodes are warnings and remain non-persistent.
- Component type, property, child-count, surface, and action-binding validation uses the live catalog from the [catalog reference](layout-catalog.md).
- Preset ids used as file names reject empty values, `/`, `\`, and `..`.

## Resource limits

`LayoutDocumentLimits` owns the defaults below.
`ShellLayoutStore` applies the file ceiling, and every document used by the GTK shell must pass the authored/effective ceilings through `prepareLayout()`:

| Dimension | Limit |
|---|---:|
| Serialized custom-layout file | 256 KiB |
| Authored entries | 4,096 |
| Authored depth | 64 |
| Authored owned string bytes | 256 KiB |
| Effective entries after template expansion | 2,048 |
| Effective depth, including template traversal | 64 |
| Effective owned string bytes | 512 KiB |

Root depth is `1`.
Child and tooltip edges increase depth by one; following a template reference also increases effective depth by one.

An entry is one layout node, template-map entry, `props`/`layout` map entry, or string-list element.
Owned string bytes include node ids and types, template keys, property/layout keys, string values, and string-list values.
Numeric, Boolean, and null values consume their containing map entry but no owned string bytes.

Authored limits cover the root and every template, including unused templates.
Effective limits cover the expanded root and charge each repeated expansion separately.
Reference id, property, layout, and tooltip overlays are charged when applied even when they replace a value already copied from the template; this bounds transient expansion work as well as the retained tree.
Missing, unknown, and recursive template references may produce diagnostic nodes only while the resulting diagnostic tree remains within the same effective limits.

The file ceiling is checked before YAML parsing and before atomic replacement with serialized output.
Tree-limit exhaustion returns `ValueTooLarge`; an allocation failure during preparation returns `ResourceExhausted`.

## Compatibility and versioning

There is no migration table; only version `1` is accepted and there is no legacy or permissive fallback.
Because fixed mappings reject unknown keys, extending the version-1 root or node vocabulary requires an explicit compatibility decision.
Changing a component type, prop name, template id, or action id can invalidate customized layouts and requires an explicit compatibility policy.

Built-in preset changes alter fallback/default structure but do not overwrite an existing customized preset file.
A rejected customized file remains in place and byte-identical during startup fallback.
The structured save path also refuses to replace an existing customized file that is malformed, unsupported, or over budget.
There is no automatic migration, quarantine, or normalization policy.

## Implementation authority

- [`LayoutDocument.h`](../../../app/include/ao/uimodel/layout/document/LayoutDocument.h) and [`LayoutNode.h`](../../../app/include/ao/uimodel/layout/document/LayoutNode.h) define values.
- [`LayoutYaml.h`](../../../app/include/ao/uimodel/layout/document/LayoutYaml.h) declares the owner-local value, node, and document schema boundary.
- [`LayoutDocument.cpp`](../../../app/uimodel/layout/document/LayoutDocument.cpp) defines explicit YAML mapping and validation.
- [`LayoutPreparation.h`](../../../app/include/ao/uimodel/layout/document/LayoutPreparation.h) defines the limits and prepared proof type.
- [`LayoutPreparation.cpp`](../../../app/uimodel/layout/document/LayoutPreparation.cpp) defines authored metering and bounded template expansion.
- [`GtkLayoutPresets.cpp`](../../../app/linux-gtk/layout/document/GtkLayoutPresets.cpp) and GTK layout YAML resources own built-in presets.

## Test authority

- [`LayoutModelTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutModelTest.cpp) protects fields, value classification, tooltips, strict node/list validation, version dispatch, unknown-key rejection, and round trip.
- [`LayoutTemplateExpansionTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutTemplateExpansionTest.cpp) protects template success and error nodes.
- [`LayoutPreparationTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutPreparationTest.cpp) protects tree-limit boundaries and expansion multiplication.
- [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) protects the group, file ceiling, and rejected-file preservation boundary.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](../../spec/shell/layout-lifecycle.md)
- [Layout catalog and action reference](layout-catalog.md)
- [Layout component-state reference](layout-state.md)
- [Application managed-state surface](../persistence/application-config.md)
- [RFC 0025: bounded shell layout documents](../../rfc/0025-bounded-shell-layout-documents.md)
