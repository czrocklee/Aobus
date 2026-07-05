# Classical Work / Movement

Aobus models a classical recording as a **work** subdivided into ordered **movements**.
`Work` already existed end to end; this document describes the `Movement` companion added
beside it and, in particular, the sort order that makes the classical presentations read
correctly.

## The model

A track may carry seven classical fields in the optional classical cold extension block:

- `workId` — the work, e.g. *Symphony No. 9 in D minor, Op. 125* (interned `DictionaryId`).
- `movementId` — the movement name, e.g. *II. Molto vivace* (interned `DictionaryId`).
- `conductorId` — the conductor, e.g. *Carlos Kleiber* (interned `DictionaryId`).
- `ensembleId` — the ensemble/orchestra/band, e.g. *Vienna Philharmonic* (interned `DictionaryId`).
- `soloistId` — the principal soloist credit for the v1 model (interned `DictionaryId`).
- `movementNumber` — the movement's ordinal within the work (`std::uint16_t`).
- `movementTotal` — the number of movements in the work (`std::uint16_t`).

`movementNumber` / `movementTotal` mirror the existing `trackNumber` / `trackTotal` pair
and the `n/total` shape of the source tags. The movement **name** is what the classical
views display; the movement **number** is what they sort by. The two are deliberately
decoupled: a row reads "Molto vivace", but its position is governed by the integer 2.

Movement is a **leaf, not a collection**. It is a `TrackSortField` and a `TrackField`
(display + edit), but **not** a `TrackGroupKey` — you group by Work and order/label by
Movement. `movementNumber` / `movementTotal` are also full `TrackField`s
(`Movement No.` / `Total Movements`), editable like track/disc numbers, so a user can
correct the sort key.

Core credits v1 adds `Conductor`, `Ensemble`, and `Soloist` without making them hot
fields. `Conductor` and `Ensemble` are group keys and sort fields; `Soloist` is a
sort/display/edit field but not a group key. All three can be filtered with
`$conductor`, `$ensemble`, and `$soloist`, and value completion scans their cold
dictionary ids.

## Tag sources

Movement fields are imported from the standard classical tags, alongside `WORK`:

| Container | Name | Number | Total |
|-----------|------|--------|-------|
| FLAC / Vorbis | `MOVEMENTNAME` | `MOVEMENT` (`n` or `n/total`) | `MOVEMENTTOTAL` |
| MP4 / iTunes | `©mvn` (text) | `©mvi` (binary integer) | `©mvc` (binary integer) |
| ID3v2 | `MVNM` | `MVIN` (`n/total`) | — |

MP4 `©mvi` / `©mvc` are binary big-endian integer atoms (not the text-encoded numerics
like `©day`), so they go through a dedicated integer-atom handler.

## The sort order (the reason this exists)

Grouping by `Work` deliberately omits album from the group key, so multiple recordings of
the same work merge into one section (Karajan's *Ninth* and Kleiber's *Fifth* of the same
work sit together). If movements were ordered purely by `movementNumber` within that merged
section, performances would interleave:

```
Karajan – I, Kleiber – I, Karajan – II, Kleiber – II, ...
```

The fix is the placement of the `Movement` sort term. In both classical presets the sort is:

```
Composer → Work → Year → Album → Movement → DiscNumber → TrackNumber → Title
```

`Album` precedes `Movement`, so each performance stays contiguous and each performance's
movements stay in movement order:

```
Karajan – I, II, III, IV,  Kleiber – I, II, III, IV
```

This is the single most important design decision in the feature, and it is covered by
`TrackListProjection - movement sort keeps performances contiguous`.

## Presentations

Both built-in classical presets use the Movement **name** as the per-row label instead of
`Title` (a classical `Title` redundantly repeats "Symphony No. 9: II. ...").

- **Classical: Works** — `groupBy = Work`. Header carries the work; `Composer` and `Work`
  are redundant. Columns: track # · **Movement** · Artist · Album · Year · Duration.
- **Classical: Composers** — `groupBy = Composer`. Within a composer the work varies, so
  `Work` is a visible column (not redundant); only `Composer` is redundant. Columns:
  track # · **Work** · **Movement** · Artist · Album · Year · Duration.

## Storage / compatibility

The fixed cold header stays small: `TrackColdHeader` is 32 bytes and carries audio properties,
track/disc numbers, five cold payload offset slots, and the URI tail offsets. Classical metadata is
stored in the classical slot payload containing the five dictionary IDs plus `movementNumber` and
`movementTotal`; no payload is written when all seven fields are empty/default. Each cold record and
each payload start is four-byte aligned.

The unreleased on-disk format is gated by the centralized `kLibraryVersion`. Per project policy there
is no migration path — a rescan rebuilds the library from the source files' tags.

## Out of scope

- **Splitting merged performances into separate sections.** Grouping by Work still merges
  recordings; the sort order keeps each performance contiguous, but a future option could
  split sections by album/performer.
- **Movement display fallback when untagged.** A track with no `movementId` shows a blank
  movement label in the classical views; movements tagged with a number but no name still
  sort correctly.
- **Multi-credit performer ontology.** v1 keeps one `Soloist` string and one `Ensemble`
  string. Instrument-specific performers, multiple soloists, choir/quartet membership, and
  structured performer roles remain future work.
