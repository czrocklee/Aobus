# List Presentation Preferences

## Problem

Aobus currently treats `TrackPresentationSpec` as runtime view state. This
works for restoring navigation snapshots, but it leaves one user-facing rule
unclear: a presentation should describe how a list is normally viewed, while a
quick filter should only narrow that same view.

The confusing model is:

```text
list + filter => separate view shape => separate presentation decision
```

The desired model is:

```text
List = content set
Presentation = default viewing preference for that list
Filter = temporary narrowing of the current list and presentation
```

Under this model, if a user opens a list in `albums` presentation and types a
quick filter, the filtered result remains the same list in `albums`
presentation.

## Goals

- Keep list data in LMDB focused on library semantics.
- Store presentation preferences outside LMDB in user configuration.
- Make presentation selection follow `ListId`, not ad-hoc filter state.
- Let smart-list creation choose a default presentation, including an automatic
  recommendation mode.
- Keep quick filtering presentation-neutral.
- Preserve navigation history's ability to restore the exact presentation seen
  at a point in time.
- Remove or constrain query navigation paths that create filtered views without
  an explicit base list.

## Non-Goals

- Do not add `presentation` fields to `ListHeader`, `ListBuilder`, or
  `ListView`.
- Do not make quick-filter text persist as a list preference.
- Do not infer a new presentation every time quick-filter text changes.
- Do not remove `TrackPresentationSpec` from runtime view state or navigation
  history.
- Do not require every frontend to share GTK column layout details.

## Ownership Model

### LMDB List Data

LMDB list records remain library data:

- name
- description
- parent list id
- smart-list local filter
- manual track ids

They do not store presentation preferences.

### User Configuration

User configuration stores list viewing preferences:

```yaml
trackView:
  presentations:
    0: albums
    42: tagging
    77: classical-works
```

The existing `gtk_layout.yaml` already stores per-list column layout state.
Presentation preference can live in the same config file and section because it
is also frontend view state.

The value is a presentation id, not a full `TrackPresentationSpec`.

Reasons:

- Built-in presentation definitions can evolve with the application.
- Custom presentation ids can still be resolved through
  `TrackPresentationViewModel`.
- The config remains small and clearly user-owned.
- Navigation history already stores full specs where exact restore is required.

Column layout and presentation preferences should not be forced into one
misnamed DTO. Either split the persisted state into separate config keys or
rename the state object before adding presentation preferences.

Preferred first implementation:

```yaml
trackView:
  columnLayouts:
    42:
      - field: title
        width: 321
  presentations:
    0: albums
    42: tagging
```

This can still share `gtk_layout.yaml`, but the serialized shape should keep
column layout and list presentation preferences independent.

### Runtime View State

Runtime views continue to hold a full `TrackPresentationSpec`.

This state is the currently rendered shape of a view. It is needed for:

- projection sorting and grouping
- visible column selection
- navigation history snapshots
- session restoration of open views

The runtime view spec is initialized from the list preference, but later view
operations can still update it.

## Rules

### Opening a List

When opening `ListId L` with no explicit presentation:

1. If config has `presentations[L]`, resolve and use that presentation id.
2. Otherwise, if `L` is a smart list, infer a recommended built-in
   presentation from the smart-list filter.
3. Otherwise, use the recommendation fallback presentation.

The existing code-level default remains `kDefaultTrackPresentationId`
(`"songs"`). Do not change that constant as part of this feature. The list
recommendation fallback should be album-oriented for the player experience,
while low-level APIs can keep using the existing programmatic default where no
list context exists.

### Applying Quick Filter

Quick filter only updates `TrackListViewState::filterExpression` for the active
view.

It must not:

- change `presentations[L]`
- re-run automatic presentation inference
- create a second presentation preference for the filtered state

The filtered projection keeps the current view's presentation.

### Switching Presentation

When the user selects a presentation while viewing list `L`:

1. Apply the selected `TrackPresentationSpec` to the active view.
2. Save `presentations[L] = selectedPresentationId`.
3. If the active view has a quick filter, keep the filter unchanged.

This makes the user action mean "view this list this way by default" rather
than "view this filter result this way".

This rule also applies to `kAllTracksListId`. Although All Tracks is synthetic
and not an LMDB list row, it is still a stable list identity from the user's
perspective and should have its own presentation preference.

### Navigation History

History should continue storing complete navigation points:

```cpp
struct NavigationPoint
{
  ListId listId;
  std::string filterExpression;
  TrackPresentationSpec presentation;
};
```

History restores what the user saw at the time. It does not replace the
per-list preference store.

When history restores a point, it applies the stored presentation to the
restored view. It should not overwrite `presentations[listId]` unless the
restore operation is explicitly treated as a user presentation change.

### Session Restore

Open-view session restore should keep using saved view configs so the app can
return to the previous workspace shape.

Per-list presentation preferences are separate:

- Session restore answers: "what views were open last time?"
- List preferences answer: "how should this list open by default?"

## Query and Filter Semantics

All filtered views must have a base list.

Current runtime code allows a navigation target of `std::string` query that
creates a `TrackListViewConfig` with only `filterExpression`. That path should
be constrained:

- If query navigation starts from a focused list, use that focused list id.
- If query navigation is global, use `kAllTracksListId`.
- Do not create query views with an implicit or invalid base list.

Keep the `std::string` navigation target for compatibility, but resolve it
internally to `TrackListViewConfig{.listId = kAllTracksListId,
.filterExpression = query}` unless the caller has an explicit focused-list
variant. This is a semantic constraint: query navigation is global search over
All Tracks by default, not a list-less view.

After this cleanup, every filter expression is interpreted as:

```text
base list membership AND filterExpression
```

Presentation lookup can then always use `baseListId`.

## Smart List Creation UX

The smart-list dialog should include a presentation selector.

Recommended choices:

- `Auto`
- `Albums`
- `Songs`
- `Album Artists`
- `Artists`
- `Genres`
- `Years`
- `Classical: Composers`
- `Classical: Works`
- `Tagging`

Custom presentations are out of scope for the first dialog version. They remain
available through the existing presentation button.

Default selection is `Auto`.

When `Auto` is selected:

1. The dialog computes a recommendation from the local smart-list expression.
2. The preview area may show the recommended presentation label.
3. On create, the resolved presentation id is stored in config for the new
   list id.

`Auto` is resolve-once for the first implementation. The app stores the
resolved presentation id, not a live "auto" mode. If the user later edits the
smart-list filter enough to change intent, they can choose `Auto` again or pick
an explicit presentation.

When an explicit presentation is selected:

1. On create, that presentation id is stored in config for the new list id.
2. No inference is needed.

When editing an existing smart list:

- The selector should show the saved per-list preference if present.
- If no saved preference exists, show `Auto`.
- Saving the dialog should update the per-list preference according to the
  selector.

## Automatic Recommendation

Automatic recommendation should use the query AST, not ad-hoc string matching.

The recommender belongs in the shared application UI model layer, not GTK and
not `WorkspaceService`. Proposed location:

```text
app/include/ao/uimodel/track/TrackPresentationRecommender.h
app/uimodel/track/TrackPresentationRecommender.cpp
```

The public helper should return a presentation id or spec using only query AST
data and the available built-in/custom presentation catalog.

Implementation sketch:

1. Parse the local smart-list expression with `ao::query::parse()`.
2. Traverse `VariableExpression` nodes.
3. Score fields by presentation intent.
4. Pick the highest scoring presentation.
5. Fall back to the album-oriented recommendation fallback.

Recommended first-pass scoring:

| Variables | Recommendation |
|---|---|
| `$work` | `classical-works` |
| `$composer` | `classical-composers` |
| `#tag` plus curation fields | `tagging` |
| `$genre` as the dominant field | `genres` or `albums` |
| `$year` as the dominant field | `years` or `albums` |
| `$albumArtist` | `album-artists` |
| `$artist` | `albums` |
| `$album` | `albums` |
| technical fields such as `@sampleRate`, `@bitDepth`, `@bitrate` | album-oriented audiophile preset |
| unknown or mixed fields | album-oriented recommendation fallback |

The first implementation should prefer stable player behavior over cleverness.
For most music filters, `albums` is a better default than making a one-group
view such as `genres` for `$genre = 'Rock'`.

Suggested heuristic:

- Classical variables win immediately.
- Technical variables choose the album-oriented player presentation.
- Tag-heavy curation queries choose `tagging`.
- Single exact equality on a grouping field still chooses `albums`.
- Broad range/browse queries on a grouping field may choose that grouping
  presentation.

## Data Model Changes

Do not add presentation preferences directly to `ColumnLayoutState` unless that
type is renamed. `ColumnLayoutState` should either remain column-only or be
replaced by a broader persistent-state type.

Preferred model:

```cpp
struct TrackColumnLayoutState final
{
  std::map<ListId, std::vector<ColumnState>> listLayouts;
};

struct ListPresentationPreferenceState final
{
  std::map<ListId, std::string> presentations;
};
```

These may be loaded from the same YAML file through separate config keys, but
the in-memory types should remain conceptually separate.

Because `Auto` resolves once, no mode enum is needed in persisted state.
Absence of a `ListId` entry means "no explicit preference yet; infer when
opening or creating."

## Service Responsibilities

### TrackPresentationViewModel

Own the list presentation preference map in the UI model layer:

```cpp
std::optional<std::string_view> presentationIdForList(ListId listId) const;
void setPresentationIdForList(ListId listId, std::string_view presentationId);
void clearPresentationForList(ListId listId);
rt::TrackPresentationSpec presentationForList(ListId listId) const;
```

The view model should continue resolving ids through built-in and custom
presentation collections. It should also call the recommender for smart lists
when no saved preference exists.

`TrackPresentationViewModel` is the preferred home for preference resolution
because it already bridges presentation catalog state, active list state, and
GTK persistence. `WorkspaceService` should not gain a direct dependency on GTK
config.

### WorkspaceService

Workspace navigation should receive or be supplied with the presentation
selected for the target list. Do not make `WorkspaceService` own the per-list
preference map.

Preferred direction:

- `TrackPresentationViewModel` resolves `presentationForList(listId)`.
- UI/workspace integration passes that spec when opening the list.
- `WorkspaceService` continues to own navigation/history, not preference
  storage.

Presentation selection command handling should update both:

- active view presentation
- per-list presentation preference for that active view's `listId`

### ViewService

`ViewService` remains low-level runtime state.

It should not own the per-list preference map. It only applies the
`TrackPresentationSpec` it receives.

### GtkLayoutConfig

`GtkLayoutConfig` should load and save both column layout state and list
presentation preference state.

The file remains `gtk_layout.yaml`.

Use separate config keys or a renamed aggregate state so the YAML structure does
not make column layout and presentation preferences inseparable.

## Cleanup Rules

- Remove or rewrite runtime paths that create filtered views without a base
  list.
- Avoid using `filterExpression` as part of the key for presentation
  preferences.
- Keep `filterExpression` in navigation history and session state because it is
  view state.
- Clean stale `presentations` entries for deleted lists. The preferred
  place is the existing list mutation/deletion flow that already closes affected
  views. Loading should also tolerate stale ids by ignoring preferences for
  missing lists.

Session restore intentionally uses per-list presentation preferences rather
than preserving an uncommitted ad-hoc presentation from the last run. This is
consistent with "presentation follows list." If exact last-session presentation
restore becomes necessary later, add an optional session-only presentation id to
open-view session state without changing LMDB list data.

## Test Plan

### Unit Tests

Add tests for the recommendation helper:

- empty expression falls back to the recommendation fallback
- `$composer = ...` recommends `classical-composers`
- `$work = ...` recommends `classical-works`
- `$artist = ...` recommends album-oriented presentation
- `$genre = ...` does not create a one-group browse view unless the heuristic
  explicitly opts into it
- technical fields recommend the album-oriented player presentation
- invalid expression falls back without throwing through the public helper

Add `TrackPresentationViewModel` tests:

- stores per-list presentation id
- updates existing id
- clears id
- ignores invalid list ids
- emits layout/presentation preference change signal
- resolves `kAllTracksListId` preferences

Add config tests:

- `GtkLayoutConfig` persists `presentations`
- loading old config without `presentations` keeps an empty map
- unknown presentation ids are ignored or fall back during resolution
- strong `ListId` map keys serialize and deserialize as numeric ids

Add workspace tests:

- opening a list uses saved presentation preference
- opening a list without saved preference uses smart-list recommendation
- quick filter keeps current presentation
- changing presentation while filtered updates list preference
- navigation history restore applies snapshot presentation without changing the
  saved list preference

### GTK Tests

Add dialog tests:

- new smart list defaults to `Auto`
- selecting explicit presentation stores that id after create
- editing list shows saved preference when present
- `Auto` displays a recommendation for representative expressions

## Implementation Order

1. Clean up query navigation so every filtered view has an explicit base list.
2. Add `TrackPresentationRecommender` in `ao::uimodel::track` and test it.
3. Rename or split persistent track-view state before adding presentation
   preferences.
4. Extend `GtkLayoutConfig` load/save tests for the new state shape.
5. Add list preference APIs to `TrackPresentationViewModel`.
6. Route list opening through list presentation preference lookup, guarded so
   lists without preferences keep current behavior until the fallback is wired.
7. Update presentation selection handling to save the active list preference.
8. Make quick filter presentation-neutral and test the behavior.
9. Add smart-list dialog selector and creation/edit persistence.
10. Wire stale preference cleanup into list deletion handling.

## Open Decisions

- Whether stale presentation ids should be removed eagerly on load or lazily on
  list deletion. Recommendation: tolerate on load and clean on deletion.
- Whether the recommendation fallback should use the existing `albums` preset or
  a new album-oriented player preset with `DisplayTrackNumber` and
  `TechnicalSummary`.
