---
id: development.uimodel-organization
type: development
status: current
domain: development
summary: Defines UIModel namespace, feature-capsule, role-name, dependency, and test-organization rules.
---
# UIModel organization

## Scope

This guide owns contributor-facing organization and naming rules for `ao_app_uimodel`.
Behavioral ownership belongs to the [presentation architecture](../architecture/presentation.md); general identifiers and file names belong to the [naming convention](naming-convention.md).

## Policy

### Namespace and folders

Public UIModel declarations use the flat `ao::uimodel` namespace.
Folder paths express feature ownership and do not add public namespace components.
The only permitted nested public namespace is `ao::uimodel::detail`; tests use `ao::uimodel::test`.

Singular feature capsules are mirrored across:

```text
app/include/ao/uimodel/<feature>/
app/uimodel/<feature>/
test/unit/uimodel/<feature>/
```

The current top-level capsules are `input`, `field`, `layout`, `library`, `playback`, `preference`, and `status`, plus deliberately reviewed root utilities.
Nested capsules remain singular, such as `library/presentation`, `playback/now-playing`, and `status/activity`.
Adding a feature path requires updating `cmake/AssertUimodelOrganization.cmake`.

Because folder context is not namespace context, public names carry enough feature meaning to remain unambiguous.
Prefer `TrackColumnState`, `ActivityCompactState`, and `LayoutActionDescriptor` over `ColumnState`, `State`, or `Descriptor`.

### Role names

| Suffix | Responsibility |
| --- | --- |
| `ViewModel` | Long-lived runtime/service observer that publishes view state and exposes user actions. |
| `InteractionModel` | Transient gesture or input state with no runtime-service ownership. |
| `EditorModel` / `FormModel` | Stateful draft, validation, option selection, and typed collection. |
| `Workflow` | Stateless or short-lived user operation; not a subscription owner. |
| `Store` | UI-local preference/session state and change publication; not implicit file persistence. |
| `Catalog` | Available options, descriptors, and lookup; not application or navigation. |
| `Policy` | Deterministic platform-free decisions. |
| `Schema` | Static or definition-derived UI structure. |
| `Projection` | Runtime/domain snapshot to UI-facing state or tree. |
| `Formatter` | Values to display text. |
| `Codec` | Edit text to typed value and back. |
| `Resolver` | User/config reference to domain/runtime value. |
| `Recommender` | Context to preferred default. |

Use bare `Model` only when no narrower role is accurate.

### Dependency boundary

UIModel may own view projections, UI-local state machines and stores, editor codecs and patches, platform-neutral action state, picker/menu/detail state, and runtime subscriptions in view models.

It must not own GTK/GDK/Glibmm/Gio or FTXUI types, widgets and dialogs, CSS, platform scheduling, LMDB transactions or store views, direct audio player/engine/backend control, or platform-specific includes.
Inputs arrive through stable core/runtime values, narrow runtime services, DTO snapshots, requests, and platform-neutral callbacks.

### Feature ownership

- `input` owns neutral chords and keymap state.
- `field` owns shared track-field formatting, edit codecs, patch policy, and inline-edit workflow.
- `layout` owns the neutral layout document, action/component catalogs, component state, and shell session.
- `library/list` owns list-tree and Smart List authoring policy.
- `library/presentation` owns track presentation catalogs, preferences, editors, recommendation, and column policy.
- `library/track`, `library/detail`, and `library/property` own their corresponding list, detail, and properties presentation behavior.
- `playback` owns published playback presentation and interaction, never succession or session-save coordination.
- `preference` maps user choices to persisted deltas and platform-supplied appliers without owning GTK or config storage.
- `status/activity` owns the platform-neutral activity projection.

## Workflow

When adding UIModel behavior:

1. Select the existing feature capsule that owns the user concept.
2. Choose the narrowest role suffix from the table.
3. Mirror public header, implementation, and test paths.
4. Keep platform mapping in the consuming frontend.
5. Update the organization guardrail only when introducing a justified new capsule.

Tests use the `ao::uimodel::test` namespace and tags shaped as `[uimodel][unit][feature][component]`.

## Validation

Build the affected application target so `ao_uimodel_organization_guardrail` and forbidden-include checks run, then execute the focused UIModel tests and the normal completion gate.

## Troubleshooting

If a type needs a toolkit handle, split semantic state/policy from its frontend adapter.
If a proposed view model starts coordinating storage or audio services, move the orchestration to runtime and inject a narrow command surface.
If a name is generic only because the folder supplies context, add the domain prefix to the public symbol.

## Related documents

- [Presentation architecture](../architecture/presentation.md)
- [Application-layer review](application-layer-review.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Activity-status specification](../spec/presentation/activity-status.md)
