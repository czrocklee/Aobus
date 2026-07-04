# Library File Identity

Status: Current behavior. Aobus stores audio payload signatures in the file
manifest, writes them during scan imports, automatically relinks unambiguous
in-root file moves, and exposes an explicit relink CLI for unresolved cases.

## Ownership

The library database owns edited metadata. File tags initialize track metadata
on first import, but changed-file rescans refresh technical audio properties
only. A file identity mechanism must therefore identify the audio payload, not
the whole tagged file, so external retagging does not break identity.

## Audio Payload

`tag::TagFile::audioPayload()` returns the encoded audio byte range borrowed
from the mapped file:

- FLAC: bytes after the final metadata block.
- MP4: the single top-level `mdat` payload.
- MP3: bytes from the first MPEG frame after ID3v2 to before trailing
  ID3v1/APEv2.

The range excludes known tag and non-audio container regions. FLAC currently
treats every byte after the final metadata block as payload and rejects an
empty payload. MP3 skips junk between ID3v2 and the first MPEG frame; encoder
padding inside MPEG frames remains part of the encoded payload. Callers hash
the returned span; no decode is required.

## Signature

The current content signature is `utility::Hash128` produced by
`utility::fnv1a128()`. This is a non-cryptographic 128-bit signature for local
content identity, not a security boundary. Aobus stores payload length with the
128-bit signature so accidental collisions require both the byte count and the
full signature to match; 128-bit is used instead of 64-bit to keep the birthday
collision margin comfortably outside realistic local-library sizes without
adding meaningful storage cost.

The manifest stores the signature bytes in canonical big-endian numeric order
(`hash.high` most-significant byte first, then `hash.low` most-significant byte
first). Incremental hashing is chunk-boundary invariant: hashing one span or
the same bytes split across `Fnv1a128Accumulator::mix()` calls produces the same
signature. Scan application hashes audio payloads through
`Fnv1a128Accumulator` in bounded chunks so callers receive fingerprinting
progress and cancellation is honored while hashing large files.

## Manifest Format

`FileManifestHeader` is 48 bytes with 4-byte alignment:

- `TrackId`
- whole-file size
- whole-file mtime
- audio payload length
- 128-bit audio payload signature
- file status

The library format version is `2`. Aobus does not carry compatibility
migration code for older metadata headers; opening a library whose stored
version differs from the current version reports `CorruptData`. Development
databases must be reset and rescanned after a format bump. There is no separate
signature-algorithm version field; a future signature algorithm change must
bump the library format version, causing existing development databases to be
rejected until reset and rescanned.

## Scan Matching And Writes

`ScanPlanExecutor` writes payload length and signature for New and Changed
classifications. It reports `Updating` progress before applying each item and
`Fingerprinting` progress while hashing the item's audio payload. Missing rows
preserve their previous signature because they are rebuilt from the existing
manifest view and only the status changes.

After the scanner's URI-first pass, missing manifest entries with stored audio
identity are grouped by payload length and signature. New files are opened only
when at least one missing identity exists; their payload length is read first,
and the payload is hashed only when that length can match a missing row.

A group is auto-relinked only when it contains exactly one missing row and
exactly one new row with the same payload length and signature. The new scan
item becomes `ScanClassification::Moved`, carrying the old URI and old
`TrackId`, and the matched missing item is removed from the plan. Duplicate
audio groups remain as `Missing` plus `New` so a human can resolve them later.
They are not auto-relinked in later stages unless the ambiguity disappears or a
manual `lib relink --from ... --to ...` command resolves one pair.

`ScanPlanExecutor` applies a moved item by rebuilding the existing track from
the database record, setting the cold URI to the new root-relative URI,
refreshing technical audio properties from the file, removing the old manifest
row, and writing the new manifest row with the existing `TrackId`.
Before rebinding, it re-fingerprints the live file and compares that identity to
the planned identity; if the file changed between scan planning and apply, the
relink fails and the write transaction is not committed.

Successful moved items increment `ScanApplyResult::relinkedCount`; successfully
marked missing rows increment `ScanApplyResult::missingCount`. The CLI plain
scan output and GTK scan completion notification use those counts to report
`Relinked N moved file(s)` and `N missing file(s) need review` after the
transaction commits.

If scan application is cancelled, `ScanPlanExecutor` stops between
fingerprinting chunks and returns a cancelled result before committing its write
transaction. LMDB aborts the uncommitted transaction, so partially processed
tracks, manifest rows, signatures, and relinks are not persisted.

Moved-item failures that occur after relink processing starts also abort the
write transaction. A relink must update the track URI and manifest row together
or not at all.

## Explicit Relink CLI

`aobus lib relink` is the manual recovery path for duplicate-content or
otherwise unresolved `Missing` plus `New` groups. With no `--from`/`--to`
arguments it lists unresolved missing rows, new files, and exact audio-identity
candidates. The scanner fingerprints only `New` files whose payload length
matches at least one missing row; files with different lengths cannot be exact
identity candidates.

`aobus lib relink --from <old-uri> --to <new-uri>` validates that both sides are
currently unresolved and that the audio payload length and signature match. The
apply path builds a one-item `Moved` scan plan and runs `ScanPlanExecutor`, so
manual relinks preserve the same dual-rebind invariant as automatic relinks.
`--dry-run` validates and reports the planned binding without mutating the
library.
