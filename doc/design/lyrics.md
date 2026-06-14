# Lyrics

> Status: proposed design. Not yet implemented. This document records the intended
> model and the reasoning behind it so implementation stays aligned with Aobus's
> identity as a lossless / classical listening tool.

## Guiding principle: lyrics are the text of the work, not a playback subtitle

Aobus stores lyrics as **the text of a work** — a Lied's German poem, an opera libretto,
an oratorio's text — not as a transient karaoke overlay. This single framing drives
every decision below.

- A subtitle is time-bound, dependent on playback, and disposable; bouncing-ball and
  scrolling-highlight presentations belong to that lineage.
- A work's text is part of the work itself. It must be readable, searchable, and
  pairable with a translation even when nothing is playing.

A classical-focused tool that cannot show the libretto of a cantata, or the original
text plus translation of a song cycle, is incomplete at its core mission. Lyrics are
therefore a requirement for doing classical seriously, not a concession to pop.

## Data model: mirror multi-cover art

Lyrics are structurally identical to [multi-cover art](multi-cover-art.md) and reuse the
same machinery rather than introducing a parallel mechanism.

- **Text lives in `ResourceStore`** as a deduplicated blob, exactly like cover image
  bytes. Lyrics can be large (a full opera libretto across several languages), so they
  must never inflate the cold record's read path.
- **The cold track record holds a small ordered reference table**, parallel to the cover
  table (`coverCount` / `coverOffset` and `SerializedCoverArtEntry`). New
  `lyricsCount` / `lyricsOffset` fields point at a four-byte-aligned table of entries:

  ```
  SerializedLyricsEntry  // 12 bytes, 4-byte aligned
    ResourceId    resourceId;   // text blob in ResourceStore
    DictionaryId  languageId;   // BCP-47 language tag, interned in DictionaryStore
    uint8_t       format;       // Plain | Lrc | EnhancedLrc (word-level)
    uint8_t       flags;        // bit0: isOriginal (original vs translation); source class
    uint8_t[2]    reserved;
  ```

The public value type (`Lyrics`) carries `resourceId`, `languageId`, `format`, and the
flags; the text bytes stay in `ResourceStore`. Reads go through a `TrackView::lyrics()`
proxy returning an ordered range, mirroring `CoverArtProxy`. Construction and mutation go
through a `TrackBuilder::lyrics()` builder, mirroring `CoverArtBuilder`.

This structure resolves three classical requirements at once:

1. **Multiple languages.** `languageId` makes "German original + English/Chinese
   translation" parallel entries rather than a single overwritable field.
2. **Original vs translation.** The `isOriginal` flag separates the work's own text from
   a translation — the distinction that separates a serious tool from a karaoke app.
3. **Synced is a format, not a separate concept.** `format` decides whether an entry
   carries a timeline.

## Synced is a superset of unsynced — do not fork

There is one lyrics concept, not separate `lyrics` and `syncedLyrics`. An LRC document
already contains the full text; timing is additive information.

- Store the richest available form (store the LRC when present).
- Derive plain text by stripping timestamps.
- The renderer reads `format` to decide whether to use the timeline; the data layer does
  not care.

This avoids the two-field, two-code-path bloat common to players that model synced and
unsynced lyrics as distinct things.

## Sourcing and provenance: trust the file, not the cloud

Lyrics come from two sources, neither of which reaches the network:

1. **Embedded tags** — FLAC `LYRICS` / `UNSYNCEDLYRICS`, ID3v2 `USLT` / `SYLT`. This is
   the file's own truth.
2. **Sidecar `.lrc`** in the same directory — the established convention among lossless
   collectors, who already keep `.lrc` next to the FLAC.

No online lyrics API. Scraped, ad-laden, fuzzily-matched lyrics are the single largest
source of a cheap feel, and they are out of scope (see Non-goals).

The source is recorded in the entry flags (embedded / sidecar / manual edit) for two
reasons: a serious tool should tell the user where text came from, and — the harder
engineering reason — **rescan must not overwrite hand-edited lyrics**. Corrections made
through the tag editor must survive re-import.

## Presentation: a quiet document by default

- **Default form:** a readable text panel with original and translation side by side,
  static, scrollable, with adjustable type size. This is the view designed for Lieder and
  opera.
- **Synced: supported but restrained.** A quiet current-line indication, never karaoke,
  never large animated scrolling. It assists reading; it is not the visual subject.
- **The visual subject is unchanged:** now-playing remains centered on cover art,
  metadata, and waveform; lyrics live in a tab or side panel.
- **"No lyrics" is a first-class state.** Much of the library is instrumental. An
  instrumental track must never show an empty lyrics screen or prompt to fetch anything.
  Absence is the norm and is accepted silently.

## Non-goals

- **No online lyrics API.** The defining boundary; the chief source of cheapness.
- **No structured speaker / act / scene model.** Opera text structure is tempting but is
  another order of engineering with uncertain payoff. Treat lyrics as text with an
  optional timeline.
- **No full-screen scrolling lyrics as a default presentation.**

## Optional, low cost

Expose "has lyrics" / "has synced lyrics" as filterable [tags](../../include/ao/library/TrackView.h)
(reusing the existing tag + bloom mechanism), so smart lists can select, for example,
tracks that carry a libretto. Pure addition, no model cost.
