---
id: shell.layout-state
type: reference
status: current
domain: application-shell
summary: Enumerates the version 1 per-preset component-state document, entry guards, current stateful types, and promoted fields.
---
# Shell layout component state

## Scope and version

This reference owns the exact standalone YAML surface of `LayoutComponentStateDocument` version `1` and `LayoutComponentStateEntry` version `1`.
Restoration, pruning, reset, promotion, and rebuild behavior belong to the [shell layout lifecycle specification](../../spec/shell/layout-lifecycle.md).

## Code boundary

The model and policy belong to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
The concrete file, locking, and atomic-write adapter belongs to the GTK frontend.

## Document surface

The file is a standalone mapping, not a `ConfigStore` group:

| Key | Type | Required | Current value |
|---|---|---|---|
| `version` | Unsigned integer | Yes | `1`. |
| `preset` | String | Yes | Preset id matching the file name. |
| `components` | Mapping from node id to entry | Yes | May be empty. |

Each component entry is:

| Key | Type | Required | Meaning |
|---|---|---|---|
| `type` | String | Yes | Layout component type. |
| `stateVersion` | Unsigned integer | Yes | Current value `1`. |
| `baselineHash` | String | Yes | XXH3-64 hexadecimal hash of state-relevant authored defaults. |
| `state` | Mapping from string to `LayoutValue` | Yes | Type-specific runtime state. |

## Stateful component inventory

| Type | Runtime state keys | Authored baseline inputs | Promotion |
|---|---|---|---|
| `split` | `positionPercent` number. | `orientation`, `initialPositionPercent`, `position`, `resizeStart`, `resizeEnd`, `shrinkStart`, `shrinkEnd`. | Writes clamped `initialPositionPercent`; removes authored `position` and runtime `positionPercent`. |
| `collapsibleSplit` | `size` integer and `revealed` boolean. | `orientation`, `collapseSide`, `initialPositionPercent`, `position`, `revealed`. | Writes `position` with minimum `50`; removes authored `initialPositionPercent` and runtime `size`; retains `revealed`. |

The canonical stateful type set is declared in `StatefulLayoutComponentType.h` and contains exactly these two values.

## Example

```yaml
version: 1
preset: classic
components:
  library-detail-split:
    type: collapsibleSplit
    stateVersion: 1
    baselineHash: 0a1b2c3d4e5f6789
    state:
      size: 420
      revealed: true
```

The hash above is illustrative; conforming files use the value computed from the matching authored node.

## Validation rules

- The explicit schema requires exactly `version`, `preset`, and `components` at the root and rejects unknown or duplicate root keys.
- It requires exactly `type`, `stateVersion`, `baselineHash`, and `state` in every entry and rejects unknown or duplicate entry keys.
- `version` is checked before other root fields, and `stateVersion` is checked before other entry fields; an unsupported value returns `NotSupported` and rejects the whole candidate.
- The preset id, component ids, entry types, and baseline hashes must be nonempty.
- `components` and each `state` are dynamic mappings, but duplicate or empty keys and malformed `LayoutValue` values reject the whole candidate.
- The deserialized `preset` must equal the requested/file-name preset before the GTK store returns the document.
- Resolution of an in-memory document requires file version `1`, non-empty node id, matching entry type, entry version `1`, and exact baseline hash.
- A missing, unknown, mismatched, or stale entry is ignored.
- Pruning removes entries whose expanded node is absent or whose type, version, or baseline no longer matches.
- Preset ids reject empty values, `/`, `\`, `..`, and NUL.

## Compatibility and versioning

There is no migration path.
The file adapter preserves live/default state when structural deserialization fails.
Serialized document- or entry-version mismatches are rejected rather than admitted for later pruning, and there is no legacy fallback.
Programmatically constructed mismatched in-memory entries remain unusable and prunable.
A changed authored baseline intentionally invalidates the old interaction state.

Runtime state is regenerable and may be deleted without changing the authored layout.
It is stored under the platform state location rather than configuration.

## Implementation authority

- [`LayoutComponentState.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentState.h) defines the document and entry.
- [`LayoutComponentStateYaml.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentStateYaml.h) declares the owner-local schema.
- [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp) defines explicit YAML mapping, baseline, resolution, and pruning.
- [`LayoutStatePromoter.cpp`](../../../app/uimodel/layout/component/LayoutStatePromoter.cpp) defines promoted fields.
- [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) owns the GTK file boundary.

## Test authority

- [`LayoutComponentStateTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutComponentStateTest.cpp) protects strict schema structure, version dispatch, unknown keys, whole-candidate rejection, hash, resolution, and pruning.
- [`LayoutStatePromoterTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutStatePromoterTest.cpp) protects exact promotion fields and residual state.
- [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protects preset validation, locking, files, and replacement.
- [`SplitComponentTest.cpp`](../../../test/unit/linux-gtk/layout/components/SplitComponentTest.cpp) and [`CollapsibleSplitComponentTest.cpp`](../../../test/unit/linux-gtk/layout/components/CollapsibleSplitComponentTest.cpp) protect GTK state consumption.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](../../spec/shell/layout-lifecycle.md)
- [Layout document reference](layout-document.md)
- [Managed file locations](../persistence/location.md)
- [Application managed-state surface](../persistence/application-config.md)
