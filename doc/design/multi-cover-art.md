# Multi-Cover Art

Tracks may reference multiple cover images. Each entry contains a `ResourceId` and a
`PictureType` compatible with ID3v2 APIC and FLAC picture type values.

## API

`CoverArt` is the public value type. It contains a `resourceId` and `type`; image bytes remain in
`ResourceStore`.

Resource identity remains strongly typed as `ResourceId` through storage, playback, cache, and UI
interfaces. `ResourceId{0}` (`kInvalidResourceId`) is the sole missing/invalid ID representation;
resource ID fields do not use `std::optional`. `ResourceStore` never allocates ID zero. Optional
cover-art values still represent whether a complete `CoverArt` entry exists, not whether an ID exists.

All track reads go through `TrackView::coverArt()`, which returns `CoverArtProxy`. The proxy exposes
the ordered range, indexed access, and `primary()`. `primary()` returns the first front cover, falls
back to the first entry, and returns `std::nullopt` when the range is empty. `TrackView` does not
provide a separate primary-cover shortcut.

All construction and mutation go through `TrackBuilder::coverArt()`. `CoverArtBuilder` accepts either
image bytes or an existing `ResourceId`, preserves insertion order, and supports indexed erase and
clearing the complete set. Cover art is not part of `MetadataBuilder`.

## Storage

The cold track record stores an ordered table of eight-byte cover entries immediately after
`TrackColdHeader`, before the custom metadata table.
The table is four-byte aligned and contains the resource ID, picture type, and reserved bytes.
Image data remains deduplicated in `ResourceStore`.

The primary image used by existing single-image UI surfaces is the first `FrontCover` entry. If
there is no front cover, the first stored entry is used. An empty table has no primary image.

## Tag Import

FLAC picture blocks, ID3v2 APIC frames, and MP4 cover entries are accumulated in source order.
Unknown picture type values are stored as `PictureType::Other`. MP4 does not carry an APIC-style
picture role, so its entries are treated as front covers.

## Library YAML

Cover art is represented as an ordered sequence:

```yaml
covers:
  - type: 3
    data: <base64 image data>
  - type: 4
    data: <base64 image data>
```

Full and metadata exports always emit `covers`, including `covers: []` when a track has no artwork.
Delta exports omit the field when the library and file tags have the same ordered cover set. They
emit the complete sequence when it differs; `covers: []` therefore records an intentional removal.

During import, an omitted field leaves the baseline cover set unchanged. A present field replaces
the complete set. Invalid or empty base64 entries are skipped.
