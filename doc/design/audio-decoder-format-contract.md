# Audio Decoder Format Contract

Aobus decoders expose source stream metadata through `DecodedStreamInfo::sourceFormat` and the actual PCM produced by the decoder through `DecodedStreamInfo::outputFormat`.

The playback path first opens a decoder with an incomplete output format to discover native stream properties. A decoder must therefore return a complete `outputFormat` after `open()` even when the requested format only sets container-level defaults such as interleaving.

## Native Rate And Channels

Decoders are responsible for preserving native sample rate and channel count. Aobus does not currently provide general-purpose resampling or channel remapping in the decoder layer, so decoder implementations must not silently convert those dimensions to satisfy a requested output format.

If a caller requests a concrete sample rate or channel count that does not match the stream, the decoder should fail with `NotSupported`. `TrackSession` performs device negotiation after source metadata is known and rejects playback when resampling or channel remapping would be required.

## Sample Representation

Decoders may support sample representation changes when they can produce correctly labeled PCM without changing timing or channel layout.

Current behavior:
- FLAC and ALAC support integer bit-depth conversion according to their implementation limits.
- MP3 uses libmpg123 and supports 16-bit signed integer output and 32-bit float output.
- MP3 rejects unsupported integer widths, such as requested 32-bit integer PCM, instead of returning 16-bit bytes with 32-bit metadata.

MP3 is a lossy source. Its compressed stream has native rate and channel metadata, while decoded PCM bit depth describes the decoder output representation rather than a lossless source depth.

Decoder blocks that contain PCM data must be consumable before the decoder reports an empty end-of-stream block. `firstFrameIndex` identifies the actual PCM frame offset for the returned block, including after decoder-level seek adjustment.
