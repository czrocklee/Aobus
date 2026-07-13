---
id: shell.layout-document
type: reference
status: current
domain: application-shell
summary: Enumerates the version 1 shell layout document, node, value, template, tooltip, and managed-file surface.
---
# Shell layout document

## Scope and version

This reference owns the exact serialized surface of `uimodel::LayoutDocument` version `1` and its `LayoutNode` and `LayoutValue` members.
Template expansion, build, editor, and fallback behavior belong to the [shell layout lifecycle specification](../../spec/shell/layout-lifecycle.md).

The `version` field is required and defaults to `1`, but the current decoder does not reject another numeric value.

## Code boundary

This surface belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
The model and YAML codec are under `app/include/ao/uimodel/layout/document/` and `app/uimodel/layout/document/`; GTK owns built-in documents, stores, and widget factories.

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

Unknown mapping keys are not read into the model and disappear on a later encode.
Missing `type` currently decodes as an empty string and builds as an unknown component.

## Value surface

`LayoutValue` is exactly one of:

| C++ alternative | YAML form |
|---|---|
| `monostate` | Null. |
| `bool` | Scalar `true` or `false`. |
| signed 64-bit integer | Integer scalar. |
| `double` | Floating-point scalar. |
| `string` | Any remaining scalar. |
| `vector<string>` | Sequence; readable scalar children become strings. |

Decode classifies scalar text in this order: canonical boolean, complete signed integer parse, complete double parse, then string.
Maps are not valid `LayoutValue` values.

Typed descriptor kinds are `Bool`, `Int`, `Double`, `String`, `Enum`, `StringList`, `CssClassList`, and `Size`.
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

- The `layout` group must decode as a mapping containing readable `version` and `root` children.
- A template reference with no `templateId`, an unknown id, or a recursive expansion chain becomes a diagnostic node.
- A stateful node needs a non-empty unique id after expansion to persist runtime state.
- Editor save rejects duplicate stateful ids; anonymous stateful nodes are warnings and remain non-persistent.
- Component type, property, child-count, surface, and action-binding validation uses the live catalog from the [catalog reference](layout-catalog.md).
- Preset ids used as file names reject empty values, `/`, `\`, and `..`.

## Compatibility and versioning

There is no current migration table and the loader does not gate `version`.
New optional node keys are ignored by an older decoder and removed if it re-encodes the document.
Changing a component type, prop name, template id, or action id can invalidate customized layouts and requires an explicit compatibility policy.

Built-in preset changes alter fallback/default structure but do not overwrite an existing customized preset file.
[RFC 0025](../../rfc/0025-bounded-shell-layout-documents.md) proposes strict supported-version dispatch, exact resource budgets, and preservation of rejected custom files; those are not current format behavior.

## Implementation authority

- [`LayoutDocument.h`](../../../app/include/ao/uimodel/layout/document/LayoutDocument.h) and [`LayoutNode.h`](../../../app/include/ao/uimodel/layout/document/LayoutNode.h) define values.
- [`LayoutDocument.cpp`](../../../app/uimodel/layout/document/LayoutDocument.cpp) defines the YAML codec.
- [`LayoutTemplateExpansion.cpp`](../../../app/uimodel/layout/document/LayoutTemplateExpansion.cpp) defines template merge behavior.
- [`GtkLayoutPresets.cpp`](../../../app/linux-gtk/layout/document/GtkLayoutPresets.cpp) and GTK layout YAML resources own built-in presets.

## Test authority

- [`LayoutModelTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutModelTest.cpp) protects fields, value classification, tooltips, and round trip.
- [`LayoutTemplateExpansionTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutTemplateExpansionTest.cpp) protects template success and error nodes.
- [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) protects the group and per-preset file boundary.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](../../spec/shell/layout-lifecycle.md)
- [Layout catalog and action reference](layout-catalog.md)
- [Layout component-state reference](layout-state.md)
- [Application managed-state surface](../persistence/application-config.md)
- [RFC 0025: bounded shell layout documents](../../rfc/0025-bounded-shell-layout-documents.md)
