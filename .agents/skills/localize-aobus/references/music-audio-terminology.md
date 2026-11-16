# Music and audio terminology

Aobus-specific product concepts and non-obvious translation traps. Normative overrides live in [presentation text semantics](../../../../doc/system/presentation/text-catalog.md): [text ownership boundary](../../../../doc/system/presentation/text-catalog.md#text-ownership-boundary) (untranslated ids, backend labels, and external values), [locale-neutral formatting](../../../../doc/system/presentation/text-catalog.md#locale-neutral-formatting), and [Chinese metadata terminology](../../../../doc/system/presentation/text-catalog.md#chinese-metadata-terminology).

## Library and lists

| Catalog term, key, or concept | Aobus meaning and translation trap |
|---|---|
| `Library` | The indexed music collection and its database views—not a software code library or a single saved `List`. |
| `List` vs `Playlist` | `List` is the general saved collection (`zh_Hans`: `列表`; `zh_Hant`: `列表`). `Playlist` wording (`zh_Hans`: `播放列表`; `zh_Hant`: `播放清單`) refers to [direct tag-backed membership editing](../../../../doc/system/library/mutation.md#tag-backed-membership-editing), not a persisted kind or a distinction based on manual ordering. Retain capitalized `List` only where English names the domain object; do not mimic awkward capitalization in locales without case distinction. |
| `smart_list_*` key prefix | Internal key namespace for `List` authoring—`"Smart List"` never appears in English UI copy (`gtk_smart_list_new_title` is `"New List"`). Translate the English text (`List` / `Playlist`), never the key prefix. Membership supports both direct tag-backed editing (`smart_list_membership_direct`) and computed filter expressions (`smart_list_membership_computed`). |
| No user playback queue | Aobus has no user-facing enqueue/dequeue track queue; prepared-next audio and internal command FIFOs are runtime internals. Never translate `Playlist`, `List`, or `{source, ...}` selectors as a playback queue. |
| `Presentation` / `View` | Shipped English uses `"Presentation"` (`track_presentation_picker_label`) and `"Create Custom View..."` (`create_custom_track_presentation`) for the same grouping/sorting/column arrangement; keep distinct from its persisted preset id. |
| `Tag` vs `Genre` | Both are live fields: `Tag` is user/library classification metadata (not an ID3 container or UI label); `Genre` is musical style metadata. |

## Metadata and classical fields

| Catalog term or concept | Aobus meaning and translation trap |
|---|---|
| `Track` | Playable library item, or numeric disc position in `track_field_track_number` (`"Track"`)—not an audio channel/stream. |
| `Album Artist` vs `Artist` | `Album Artist` groups albums/discographies and may differ from per-track `Artist`; use the locale's established music-player term rather than a literal word-by-word compound, and never merge the two to save width. |
| `Disc` / `Total Discs` | Numeric multi-disc release positions, not storage media. |
| `Work` / `Movement` | Classical parent composition and its subdivision (`Movement No.`, `Total Movements`)—never employment/verb or playback seeking/motion. |

## Audio path and quality findings

| Catalog term or concept | Aobus meaning and translation trap |
|---|---|
| `Bit Depth` | Signal/source precision in bits per sample, which can be narrower than the PCM container width ([locale-neutral formatting](../../../../doc/system/presentation/text-catalog.md#locale-neutral-formatting)). |
| `Channel` (`Mono` / `Stereo`) | Shared lexical labels obey different consumer contracts in [mapping and fallback contracts](../../../../doc/system/presentation/text-catalog.md#mapping-and-fallback-contracts) (track-field count branches vs. runtime audio-quality layout identity). |
| `Output device` vs `Audio backend` | Selected endpoint vs. platform audio system (`PipeWire`, `ALSA`, `WASAPI`, `Core Audio`). Backend names themselves are untranslated identities; only `audio_backend_*_description` and `*_output_fallback` are translated. |
| `Shared Mode` / `Exclusive Mode` | OS-mixed playback vs. direct hardware device access—not library/playlist sharing or account exclusivity. |
| `Software volume attenuation` / `amplification` vs `Hardware volume control` | Digital gain in the software path vs. gain attributed to device hardware (distinct from `Device volume ... (source unverified)`). |
| `Signal clipping detected` / `Clipping risk` | Signal peaks exceeding representable range—not UI text truncation. |
| `Bit-transparent` vs `Bit-perfect PCM (observed software path)` | `Bit-transparent` (`audio_finding_lossless_*`) describes a lossless sample conversion stage (integer padding, float mapping, float round-trip). `Bit-perfect PCM (observed software path)` (`audio_quality_bit_perfect_playback`) describes unmodified PCM across the observed software boundary—always preserve the `"observed software path"` hedge rather than promising end-to-end hardware bit-perfection. |
