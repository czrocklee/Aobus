---
id: decision.0010.never-write-audio-files
type: decision
status: accepted
domain: library
summary: Records that Aobus only ever reads audio files, which is the premise that makes cover-art re-extraction sound.
---
# Decision 0010: never write to an audio file

## Context

Aobus has always read media files and never modified them. The media file layer
exposes readers and an audio payload view, and no writer; curated metadata,
tags, list membership, and ordering live in the library database, which is the
only thing Aobus owns.

That was an implementation fact rather than a recorded decision until
externalized cover art made it load-bearing: the change replaced stored cover
blobs with descriptors, and now re-extracts a cover from any audio file that
references it. Re-extraction is only sound because the files a
scan read are not rewritten behind the scan: a digest recorded at scan time
still names content the file carries, and when it does not, the file was
changed by something outside Aobus and the next scan replaces the reference.

The premise is easy to question again later, and answering it with "no writer
exists yet" invites someone to add one. This record states it as policy instead.

## Decision

Aobus never writes to an audio file. It does not write tags, embed or replace
pictures, rewrite containers, normalize metadata in place, or update a file's
modification time. Every file the application touches is opened for reading.

Everything Aobus curates is stored in the library database. Where the file and
the database disagree about a fact the file owns — its technical properties and
its embedded pictures — the file wins at the next scan, and the database is
corrected rather than the file.

Reversing this decision requires a separate decision record, because the
cover-art design depends on it.

## Alternatives considered

### Write curated metadata back to tags

Rejected. It turns reading a library into modifying the user's media, makes
every scan ambiguous about who last changed a file, and requires a writer for
every supported container. A user who wants their tags updated is asking for a
tagging tool, which Aobus is not.

### Write cover art into files, and treat the file as the store

Rejected for the same reasons, with an additional one: it would make the
database the authority over content the file owns, inverting the direction this
project settled on. It would also rewrite whole files to change an image.

### Keep the payload in the database so no file needs to be read

That is the design externalized cover art replaced. It cost 77 MiB of stored
covers on the measured collection and made every transfer document carry base64 image bytes,
while the same content already sat in the music files.

## Consequences

- A cover read may open a media file, and a cover survives only while some
  referencing file still carries it or a cache entry still holds it.
- A track's cover references follow the file: `Changed` and `Moved` scan items
  replace them from what the file now carries. A full import restores a
  reference graph from a transfer document; nothing else writes them.
- External retagging is detected, not prevented. The library converges at the
  next scan of that file.
- User-authored cover art, which is a stated non-goal, cannot be implemented by
  embedding an image into a file. It would need a separate content store and its
  own decision.
- Aobus needs no container writers, so malformed-write and partial-write
  failure modes stay out of the product entirely.

## Current authorities

- [Cover-art delivery specification](../spec/resource/cover-art-delivery.md)
- [Scan and identity specification](../spec/library/runtime/scan-and-identity.md)
- [Media file reading specification](../spec/media/file-reading.md)
- [Resource delivery architecture](../architecture/resource-delivery.md)

## Supersession

Not superseded.
