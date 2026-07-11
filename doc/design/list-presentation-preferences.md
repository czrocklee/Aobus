# List Presentation Preferences

Aobus separates list content from the presentation normally used to view that
content. The presentation model itself — the spec, the builtin preset set, and
grouping/sorting semantics — is described in
[Track Presentation](track-presentation.md); this document covers how a
presentation is chosen for and remembered per list.

```text
List = content set
Presentation preference = default view shape for that list
Filter = temporary narrowing of the current list and presentation
```

This means a quick filter narrows the active list without choosing a new
presentation. If a list is normally viewed as albums, filtering that list still
shows the album presentation unless the user explicitly changes it.

## Ownership

Library list records remain domain data in LMDB:

- name
- description
- parent list id
- smart-list filter
- manual track ids

Presentation preferences live in GTK/user configuration, alongside column
layout state, through `GtkLayoutConfig` and
`uimodel::ListPresentationPreferenceStore`.

The store maps `ListId` to presentation id. It never writes presentation data
into `ListHeader`, `ListBuilder`, or `ListView`.

## Resolution

When a list is opened, the presentation is resolved in this order:

1. saved presentation id for the `ListId`
2. source-aware recommendation from `uimodel::recommendPresentation()`:
   `list-order` for manual lists, filter-derived presets for smart lists, and
   `albums` for other sources
3. catalog fallback from the available built-in/custom presentations

Unknown saved presentation ids are tolerated. Resolution falls back rather than
blocking the list from opening.

## Runtime State

`TrackPresentationSpec` remains runtime view state. Navigation history stores
the exact presentation snapshot that was active at the time of navigation, so
back/forward restores what the user saw without rewriting the saved list
preference.

Changing the presentation for an active list may update the list preference, but
restoring a history point is replay and must not do so.

## Quick Filters

Quick filters do not create independent presentation preferences. They are
temporary narrowing expressions applied to the current list and current
presentation.

Filtered navigation still carries both pieces of information explicitly:

- the base `ListId`
- the `filterExpression`

That keeps list identity, presentation preference, and transient filtering from
collapsing into one ambiguous view key.
