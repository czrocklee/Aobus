# Tag Editor Layout

The tag editor presents current tags, suggested tags, and an add action as chips that flow and
wrap together seamlessly instead of occupying separate stacked rows. The editor is a custom
`Gtk::Widget` that lays its chips out directly via a hand-rolled left-to-right wrapping flow (see
`measure_vfunc`/`size_allocate_vfunc`); it deliberately does **not** use `Gtk::FlowBox`. A FlowBox
is a grid that sizes every column to its widest child, so a chip inherits the width of whatever
happens to share its column — a wide neighbour (notably the open add entry) stretches unrelated
chips and makes widths look arbitrary. In the custom flow each chip is instead allocated its own
natural width (clamped to the line width, never below its minimum), so chip
widths are predictable and independent. The gap between chips (both the horizontal gap and the
gap between wrapped rows) is theme-aware: it reads the theme class from the editor's root window so
the dense *classic* theme keeps chips tight while the airy *modern* theme spaces them out; before
the widget is rooted it falls back to the dense gap. This inter-chip gap is distinct from the
intra-chip composition gap (label to remove button, `+` glyph to label), which is theme-independent.
Three chip kinds share that flow, ordered current → suggested → add trigger:

- **Current tags (Type A):** solid `ao-tag-chip-current` chips. The label ellipsizes when
  horizontal space is constrained, while a dedicated remove button keeps a 20-pixel minimum
  allocation and remains clickable. The remove button is the only removal target; the chip body
  is inert to avoid accidental, irreversible removals. Layout is delegated to a `Gtk::Box`
  instead of duplicating box rules in the custom widget.
- **Suggested tags (Type B):** outlined `ao-tag-chip-suggested` chips for high-frequency tags not
  on the selection. A small (12-pixel) `+` glyph prefixes the label so the affordance reads as
  "add" without overpowering the text. Clicking one adds it, promoting it to a current chip in
  place.
- **Add trigger (Type C):** a low-emphasis `ao-tag-add-trigger` ghost action labelled `Add…` that
  swaps in place for a text entry on demand (instant swap, no animation). Unlike the chips it is
  frameless with no fill at rest (only a faint rounded background on hover), so it reads as an
  action rather than a selectable tag; it also drops the `+` glyph the suggested chips use, to keep
  the two visually distinct. Enter commits the tag and the entry then
  clears and stays focused for rapid successive adds; Escape, tabbing away, or a press anywhere
  outside the trigger all dismiss the entry without committing, so the add box never feels stuck
  open. Because clicking blank, non-focusable areas does not move keyboard focus in GTK, the
  outside dismissal is driven by a capture-phase click watch on the toplevel window (the same
  approach the detail-field editors use), installed only while the entry is open. While the entry
  is open the current chips are filtered out of the flow, so add/search mode presents only the
  (live-filtered) suggestions and the entry; the current chips reappear on dismissal. Typing in
  the entry live-filters the suggested chips (case-insensitive substring). Any non-empty tag name
  is accepted; query serialization uses a bare `#name` for ASCII alphanumeric/underscore names and
  a quoted form such as `#"90s Rock"` for other names; see
  [query-expression-language.md](query-expression-language.md) for the full syntax. The trigger is the persistent
  trailing child of the flow, so its open state and focus survive chip rebuilds.

Each chip and the trigger centre vertically within their flow row (via `valign`), so a taller
neighbour — the open inline entry — never stretches the chips beside it; the row simply grows to
the entry's height. Horizontally each child keeps its own natural width, so the inline entry can
be a comfortable typing width (longer input scrolls) without affecting any chip. Filtering is
applied by toggling each chip's visibility (current chips hidden while the entry is open;
suggestions hidden when they fail the live substring match) and the flow skips hidden children.

The editor is height-for-width with a horizontal minimum of 0, so it stays fully compressible for
narrow detail panes (over-wide chips clamp to the line width and ellipsize). GTK tests cover long
tag labels, narrow allocation and the minimum-width contract, remove-button interaction,
suggested-to-current promotion, inline add submission, live suggestion filtering, outside-click
dismissal, hiding current tags while adding, the open entry leaving a neighbouring suggested
chip at its own natural width (the grid-column regression), and the theme-aware gap widening the
flow's natural width under the modern theme.
