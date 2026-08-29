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

Top-level feature ownership is mirrored across:

```text
app/include/ao/uimodel/<feature>/
app/uimodel/<feature>/
test/unit/uimodel/<feature>/
```

The current top-level capsules are `input`, `field`, `layout`, `library`, `playback`, `preference`, `presentation`, and `status`, plus deliberately reviewed root utilities.
Nested feature paths remain singular, such as `library/presentation`, `playback/now-playing`, and `status/activity`.
Adding a top-level feature path requires updating `cmake/AssertUimodelOrganization.cmake`.

This is an ownership mirror, not a file-for-file rule.
A cohesive public capsule may contain several related values and functions;
implementations may be partitioned by algorithm, and tests are grouped by
behavior. Do not create a public header or test file merely to mirror each
implementation unit.

Because folder context is not namespace context, public names carry enough feature meaning to remain unambiguous.
Prefer `TrackColumnState`, `ActivityCompactState`, and `ComponentSchema` over `ColumnState`, `State`, or `Schema`.

### Role names

The [naming convention](naming-convention.md#choosing-a-role) owns the role
decision tree and table. In UIModel, `ViewModel` means UI-facing state plus
actions, `InteractionModel` means transient input state, and `EditorModel` or
`FormModel` means a stateful draft with validation and collection. A frontend
surface is not made shared by calling it a model, and a pure function group
does not need a static-only role class.

### Dependency boundary

UIModel may own view projections, UI-local state machines and stores, editor codecs and patches, platform-neutral action state, picker/menu/detail state, and runtime subscriptions in view models.

It must not own GTK/GDK/Glibmm/Gio or FTXUI types, widgets and dialogs, CSS, platform scheduling, LMDB transactions or store views, direct audio player/engine/backend control, or platform-specific includes.
Inputs arrive through stable core/runtime values, narrow runtime services, DTO snapshots, requests, and platform-neutral callbacks.

### One frontend's vocabulary is not a shared model

Portability is not the test. Deciding which component types a shell accepts, which XAML element each one constructs, which style targets are compatible, and which themed surfaces exist is all pure C++ that compiles anywhere - and all of it belongs to one frontend.
A file that announces which frontend it serves is by that admission not shared, so the Windows-owned `app/windows-winui/layout/LayoutSchema.h`, `GtkThemeSurface.cpp`, and `TuiFramePolicy.h` do not belong in `ao_app_uimodel` whatever they include.

Such code goes to the frontend target that owns it. Do not create a cross-platform model target solely to make another host's gate compile frontend vocabulary. WinUI keeps these rules in the Windows-only `aobus-winui-lib`, under the `ao::winui` namespace with headers in `app/windows-winui/include/ao/winui/`.

Ownership decides the *target*; it does not decide where a rule is *gated*. A frontend-owned source that names no platform API is compiled straight into `ao_core_test` on every host, so its rules are covered by whichever gate runs. The Windows desktop settings schema, output-preference resolution, root-commit transaction, restart sequencing, and shell-state vocabulary qualify today: they stay Windows-owned and stay out of `ao_app_uimodel`, while `./ao check` still exercises them on Linux. Only sources that genuinely need native types — XAML elements, WinRT projections, resource dictionaries — remain gated to the native Windows suite. This is not a licence to relocate frontend vocabulary; it is the recognition that "one frontend owns this rule" and "only one host can test this rule" are separate claims, and the second one costs real coverage whenever it is asserted without cause.

What is left in UIModel is what more than one frontend genuinely decides the same way, expressed so no frontend's name appears in it. A shared traversal that a frontend extends takes the extension as data - see `LayoutDialect` - rather than naming the frontend in a branch.
`cmake/AssertUimodelFrontendNeutrality.cmake` enforces both halves: no file inside UIModel may be named after a frontend, and none may spell a frontend's API vocabulary in code. Naming a frontend in a comment stays allowed, because explaining that GTK derives expansion from a widget's children is exactly why a shared field is optional, and that reason belongs beside the field.

### Feature ownership

- `input` owns neutral chords and keymap state.
- `field` owns shared track-field display formatting.
- `layout` owns the neutral layout document and schema, component state, and shell session.
- `presentation` owns the cross-feature immutable authored-copy catalog; feature-specific projections still live with their feature capsule.
- `library/list` owns list-tree, saved-List authoring, and order policy.
- `library/presentation` owns track presentation catalogs, preferences, editors, recommendation, and column policy.
- `library/track` owns track filtering, edit decoding, patch construction, and stable-target authoring sessions, including saved-List membership edits.
- `library/detail` and `library/property` own their corresponding detail and properties presentation behavior.
- `playback` owns published playback presentation and interaction, never succession or session-save coordination.
- `preference` maps user choices to persisted deltas and platform-supplied appliers without owning GTK or config storage.
- `status/activity` owns the platform-neutral activity projection.

### Authored-copy classification

Before adding a user-visible string below a frontend, classify it by authority:

- **shared Aobus copy** is a label, description, placeholder, report template, or pluralized summary authored by the application and consumed by more than one interactive frontend; represent its input semantically and resolve it with `requiredText` / `requiredFormat` or a feature-local formatter over `MessageCatalog`;
- **user or external data** includes metadata, user-authored preset/list names, paths, and operating-system device names or descriptions; preserve it as data and escape it only at the frontend rendering boundary;
- **language or protocol text** includes query syntax, stable ids, persisted tokens, and CLI machine output; keep it with the owning grammar or format and never translate it as presentation copy;
- **diagnostic text** explains a failure to logs or typed errors and remains with the failure owner; UI control flow must not parse it; and
- **frontend-local copy** with no shared semantic consumer remains in that frontend rather than expanding the shared catalog.

Do not store both a shared semantic input and its resolved English form in runtime state.
Do not compare catalog output to select recovery, severity, grouping, ordering, aggregation, or persistence behavior.
Shared catalog-owned icon values are semantic kinds; GTK symbolic names and TUI glyphs stay in their adapters.
An explicitly frontend-local notification/content escape hatch must be named and reviewed as resolved presentation rather than reused by shared runtime producers.
An open extension id must have an explicit fallback, while a closed enum lookup must be exhaustive.

## Workflow

When adding UIModel behavior:

1. Select the existing feature capsule that owns the user concept.
2. Apply the [role decision tree](naming-convention.md#choosing-a-role), choosing no suffix when a domain noun is sufficient.
3. Keep the feature path consistent across public, implementation, and test
   ownership; choose files by cohesive behavior rather than one-to-one mirroring.
4. Keep platform mapping in the consuming frontend, and keep one frontend's vocabulary there too.
5. Update the organization guardrail only when introducing a justified new capsule.
6. For shared authored copy, add or extend a typed semantic input and lock its catalog coverage and fallback in focused tests.

Tests use the `ao::uimodel::test` namespace and tags shaped as `[uimodel][unit][feature][component]`.

## Validation

Build the affected application target, execute the focused UIModel tests, and run the normal completion gate.
For an earlier focused boundary check, build the `aobus_guardrails` target explicitly; `./ao check` includes it automatically.

## Troubleshooting

If a type needs a toolkit handle, split semantic state/policy from its frontend adapter.
If a proposed view model starts coordinating storage or audio services, move the orchestration to runtime and inject a narrow command surface.
If a name is generic only because the folder supplies context, add the domain prefix to the public symbol.

## Related documents

- [Presentation architecture](../architecture/presentation.md)
- [Application-layer review](application-layer-review.md)
- [Application shell architecture](../architecture/application-shell.md)
- [Activity-status specification](../spec/presentation/activity-status.md)
- [Presentation text catalog reference](../reference/presentation/text-catalog.md)
