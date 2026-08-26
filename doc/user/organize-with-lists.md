---
id: user.organize-with-lists
type: user-guide
status: current
domain: library
summary: Creates expression-backed Lists and Playlists, edits membership, arranges Manual Order, and manages nested List lifecycles.
---
# Organize music with Lists and Playlists

## Outcome

You can define a reusable set of tracks with an expression, use a visible tag for direct Playlist membership, and save a manual order without giving up album, artist, year, or other presentations.

## Prerequisites

- Open an indexed music library in GTK or the Windows desktop.
- Use a saved List rather than the virtual **All Tracks** row when you want to store an order.

## Understand the model

Every user-created row in the List sidebar is the same kind of List.
Its filter decides membership, and its optional saved positions decide source order.
An empty filter means “all tracks from the parent.”

**New Playlist...** is a convenient List template, not a second storage type.
It creates a List whose filter is one visible tag and selects **Manual Order** as its presentation.
The tag remains ordinary global track metadata, so it can also appear in the tag editor, queries, and other Lists.

Nesting means “based on this parent List.”
A child sees only its parent's members, applies its own local filter, and inherits the parent's effective order before applying its own saved positions.
Nesting is therefore a data dependency, not a folder used only for visual organization.

## Steps

### Create an expression-backed List

1. Right-click **All Tracks** or the List that should be the source, then choose **New List...**.
2. Enter a name and an optional description.
3. Enter a local filter expression, or leave it empty to inherit every parent member.
4. Check the inherited filter, effective filter, and preview.
5. Choose the initial presentation and select **Create**.

Use the **Based on** source deliberately.
Changing it later may change both membership and the unranked fallback order.

### Create and fill a Playlist

1. In GTK, choose **New Playlist...**, enter the name, and confirm the separate **Membership Tag** field.
   The name initially suggests the tag, but you can choose another visible tag.
   In Windows, choose **New List...**, enter one positive tag such as `#road-trip` as the local filter, and choose **Manual Order** as the presentation.
2. Create the Playlist.
3. Select tracks, right-click, and choose the Playlist under **Add to Playlist**.

Direct Add/Remove is available only when the complete local filter is one positive tag, such as `#road-trip`.
A computed expression such as `#road-trip and $year >= 2020` cannot be safely reversed into metadata, so Aobus directs you to edit its expression or track tags instead.
The **Add to Playlist** submenu lists writable Playlists by their visible name and tag while commands continue to use stable List identities.
GTK summarizes omitted computed Lists once and offers **Manage Lists...** instead of filling the menu with disabled computed targets; when no writable Playlist exists, it offers **Create a Playlist...**.
Windows shows one disabled explanation when no eligible target exists.
For a nested Playlist, every added track must already belong to the parent List.

Renaming a Playlist does not rename its membership tag.
The tag may be referenced by tracks and other expressions independently.

### Remove a track deliberately

Use **Remove from _Playlist_ (_tag_)** when the track should leave the Playlist and its saved position should be forgotten.
The result message reports both the global tag change and the forgotten position.
Adding the track again later places it at the unranked tail.

Removing the same tag in the ordinary tag editor is a softer operation.
The track leaves current membership, but its hidden saved position remains.
If the tag is restored later, the track returns to that position.

### Arrange Manual Order

1. Choose **Manual Order**, or another custom presentation that is flat, ungrouped, and has no sort fields.
2. Clear the quick filter for gap dragging or relative movement.
3. In GTK, drag the handle beside a row to a visible gap.
   When dragging part of a selection, selected tracks retain their current relative order.
   Windows currently has no drag handle.
4. In either desktop frontend, use the selected-row **Manual Order** submenu or these shortcuts:

   | Command | Shortcut |
   |---|---|
   | Move Up | `Alt+Up` |
   | Move Down | `Alt+Down` |
   | Move to Top | `Alt+Home` |
   | Move to Bottom | `Alt+End` |

GTK accepts one order mutation per physical key press.
Holding a shortcut key does not repeat the move through OS auto-repeat; release it and press again for another committed step.

With a quick filter active, hidden rows make relative gaps ambiguous.
Drag, Move Up, and Move Down are therefore unavailable, but Move to Top and Move to Bottom still have an unambiguous meaning in the complete List.

The first effective move saves the complete current List order lazily.
Switching to Albums, Artists, Years, or another sorted presentation does not erase it.
Those presentations control visible and playback order while active; switching back to Manual Order restores the saved order.

**All Tracks** has no saved-position record.
Manual Order is disabled there; create a root List with an empty filter if you need a manually arranged whole-library view.

### Reset or prune saved positions

- **Reset Order** forgets every saved position and returns the List to filtered parent order; both desktop frontends expose it in the Manual Order submenu.
- **Forget Hidden Positions** forgets positions only for tracks outside current List membership and keeps the current visible order; it is currently exposed by GTK.

Ordinary filter, parent, and tag edits do not automatically prune hidden positions.
This lets a temporarily excluded track recover its former place.

### Delete Lists safely

Ordinary **Delete List** is rejected when the List has descendants.
Choose **Delete List and Descendants...** to preview the complete derived subtree and delete it atomically.
A writable-tag List also offers an unchecked-by-default option to remove its visible tag from affected tracks and warns when other Lists reference that tag.
Tags used by nested Playlists are kept.

In either desktop frontend, open these commands from the saved List's context menu.
Deleting the active List returns the view to All Tracks after the committed deletion.

Deleting a List never deletes music files.

## Verify the result

- Reopen the List and confirm that its expression still produces the intended members.
- Switch away from Manual Order and back; the saved sequence should return.
- Start playback from the List; succession follows the same current presentation order shown in the view.
- After explicit **Remove from Playlist**, confirm that the message mentions both tag removal and the forgotten saved position.

## Troubleshooting

- **Library is busy**: scanning, import, or identity maintenance temporarily blocks order and membership commits. Wait for maintenance to finish and start the gesture again.
- **The List changed**: membership, presentation, filter, or library revision changed during the gesture. Retry from the current rows.
- **No drag handle in GTK**: use a saved List, choose a flat unsorted presentation, clear the quick filter, and resolve any expression error. Windows currently uses menu and keyboard movement only.
- **Playlist missing from Add to Playlist**: its filter is not one directly writable positive tag, or the selected tracks are outside its parent source.

## Related reference

- [Predicate language](../reference/query/predicate-language.md)
- [Track presentation presets](../reference/presentation/track-preset.md)
- [CLI command reference](../reference/cli/command.md)
- [List model](../reference/library/model/list.md)
