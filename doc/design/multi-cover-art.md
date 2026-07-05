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

The cold track record stores cover entries in the optional cover-art slot payload after the 32-byte
`TrackColdHeader`. Cold payload slots are written in deterministic order: cover art, classical
metadata, then custom metadata. The cover-art payload is omitted when the track has no covers.

Each cover entry is eight bytes, four-byte aligned, and contains the resource ID, picture type, and
reserved bytes. Image data remains deduplicated in `ResourceStore`.

The primary image used by existing single-image UI surfaces is the first `FrontCover` entry. If
there is no front cover, the first stored entry is used. An empty table has no primary image.

## GTK Thumbnails

GTK section headers use a shared `ThumbnailLoader` for cover thumbnails. The loader decodes image
bytes off the UI thread at a requested physical-pixel size, coalesces concurrent requests for the
same resource and size, and writes successful decodes into a small shared thumbnail cache. Requests
are size-aware: a larger cached thumbnail may satisfy a smaller request, but a smaller cached or
in-flight decode must not satisfy a larger request.

Section-header widgets still guard their own lifetime and binding generation. A decode may complete
after a recycled or destroyed header has moved on; in that case the loader may salvage the thumbnail
into cache, but the widget must drop the stale callback result.

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
