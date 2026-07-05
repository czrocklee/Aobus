# Track Column Widths

Track tables use explicit column widths. They do not use GTK auto-sizing or a
single expanding column, because realized-row measurement changes while
scrolling and a single expansion target makes one text column absorb all spare
space.

## Column Roles

Columns are either fixed or flexible.

- Fixed columns hold bounded values such as year, disc/track numbers, duration,
  codec, sample rate, bitrate, file size, modified time, display track number,
  quality, and technical summary. Their width is a concrete unit value and does
  not change when the viewport changes.
- Flexible columns hold unbounded text such as title, artist, album, album
  artist, genre, composer, work, movement, tags, and file path. They divide the
  remaining viewport by weight.

Each column has both a default width and a minimum width. The minimum is a hard
lower bound; it is intentionally separate from the default so fixed columns can
be narrowed by the user without collapsing to zero.

## Solver Contract

`TrackColumnWidthSolver` is a `uimodel` pure-function component. Each solve spec
contains the field, optional fixed width, optional flexible weight, default
width, and minimum width. Widths are unit-independent: GTK fills specs with
pixels, while the TUI fills specs with terminal columns.

When the viewport is wide enough and at least one visible column is flexible,
the solver returns widths that sum to the viewport. If the viewport cannot fit
the fixed widths plus all flexible minimums, flexible columns stay at minimum
and the table overflows horizontally. If every visible column is fixed, the
solver returns the fixed widths and leaves trailing space.

## User Resize

Dragging a flexible column edits weights. The resized column keeps the target
width when representable; the width delta is absorbed by flexible columns to
its right first, then by flexible columns to its left. Dragging a fixed column
stores a fixed width and may create overflow if the remaining flexible columns
reach their minimums.

Stored layout entries are canonical:

- Flexible columns store `{width = -1, weight = w}`.
- Fixed columns store `{width = px, weight = -1}`.

Weights are rounded to three decimal places before entering
`TrackColumnLayoutStore`, so exact equality remains stable and spurious
configuration writes are avoided.

## Frontends

GTK `ColumnView` columns all use `fixed_width`; no column uses `expand`. The
controller reads the visible viewport width from the horizontal adjustment page
size, falls back to widget width before mapping, and delegates all width math to
the solver.

The TUI uses the same solver in terminal-column units. Manual TUI resize
overrides remain terminal-column overrides; any overridden column is treated as
fixed for solving, while non-overridden flexible text columns continue to
resize with the terminal.
