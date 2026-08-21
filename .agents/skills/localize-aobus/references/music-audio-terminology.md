# Music and audio terminology

Use this reference to establish the English concept before choosing an idiomatic target-language term. It is not a table of mandatory literal translations: grammar, platform convention, and established music-player vocabulary in the target language still decide the final wording.

## Library and organization

| Concept | Meaning in Aobus | Avoid confusing it with |
|---|---|---|
| Library | The user's indexed music collection and its database-backed views. | A software/code library or one playlist. |
| List | A saved user-authored collection. Some messages distinguish a regular Playlist from a rule-driven Smart List. | The entire Library, the playback queue, or a generic visual list widget. |
| Playlist | A List whose membership/order is explicitly authored. | The transient playback queue. |
| Smart List | A saved List whose membership is produced by a filter expression. | A manually ordered Playlist. |
| Queue | The transient playback sequence. | A saved List or the Library's current sort order. |
| Presentation / view | A named arrangement of grouping, sorting, and visible columns. | The underlying tracks or a persisted translation identity. |
| Tag | User/library classification metadata attached to tracks. | An ID3 container generally, a filename token, or a UI label. |

Retain the project's capitalized `List` terminology only where the English product vocabulary uses it as the Aobus domain object. Choose a natural equivalent rather than reproducing unusual English capitalization when the target language does not support that distinction.

## Track and classical metadata

| Concept | Meaning in Aobus | Avoid confusing it with |
|---|---|---|
| Track | One playable music item in the Library; in number fields, its position on a disc. | A low-level audio stream/channel. |
| Track artist / Artist | The performer credited for the individual track. | Album Artist or Composer. |
| Album Artist | The credit used to group an album/discography, which may differ from each track's artist. | A literal phrase assembled from separate “album” and “artist” translations when the locale has an established term. |
| Album | A release/container grouping tracks and possibly multiple discs. | A saved Playlist or filesystem folder. |
| Disc | A disc/volume within a multi-disc release; `Disc` and `Total Discs` are numeric metadata. | An arbitrary storage disk. |
| Composer | The person who composed the musical work. | Performer, songwriter credit in general, or Conductor. |
| Conductor | The person directing an orchestra or ensemble. | Composer or lead performer. |
| Ensemble | A performing group in classical metadata. | The set of all track artists or an audio channel group. |
| Soloist | A featured individual performer in classical metadata. | Every Track Artist. |
| Work | The parent musical composition, especially in classical music. | Employment, a verb, or the recording itself. |
| Movement | A subdivision of a Work. `Movement No.` and `Total Movements` are numeric metadata. | Playback movement, seeking, or Track Number. |
| Genre | The track's musical style/category metadata. | A user Tag when the UI specifically addresses the Genre field. |

## Audio and playback

| Concept | Meaning in Aobus | Avoid confusing it with |
|---|---|---|
| Codec | The encoded audio format/codec reported for a track. | The container filename extension unless they are actually identical. |
| Sample rate | Samples per second, displayed with `Hz` or `kHz`. | Bitrate or bit depth. |
| Bit depth | Bits per sample, displayed with the established `-bit` syntax. | Bitrate. |
| Bitrate | Encoded data rate, displayed with `kbps`. | Sample rate. |
| Channel | One audio signal channel; Mono/Stereo are localized lexical descriptions. | A Track or an output device. |
| Output device | The selected playback endpoint. | The audio backend/server. |
| Audio backend | The platform audio system used to reach devices, such as PipeWire, ALSA, or WASAPI. | A particular speaker/headphone endpoint or an output profile. |
| Shared mode | Playback mixed by the operating system with other applications. | Sharing a Library or Playlist. |
| Exclusive mode | Direct/exclusive access to an output device. | Exclusive ownership of a track or account. |
| Resampling | Conversion from one sample rate to another. | Re-encoding to another codec. |
| Channel mapping | Reassignment or conversion of channel layouts/counts. | Track ordering. |
| Software attenuation/amplification | Digital gain applied in the software path. | Hardware volume control. |
| Hardware volume control | Gain/volume modification attributed to device hardware. | Any visible volume slider regardless of where gain is applied. |
| Muted | Audio intentionally silenced. | Paused or stopped playback. |
| Clipping | Signal peaks exceeding the representable range. | UI/content truncation or an incomplete file. |
| Lossless | Encoding or processing that preserves the represented audio samples. | Bit-perfect output through the complete playback path. |
| Bit-perfect | The output path preserves the relevant digital sample values and format without modifying stages. | Merely using a lossless source codec. |

The current presentation contract intentionally keeps ASCII digits, clock durations, ISO-style dates, codec symbols, and the established technical units (`Hz`, `kHz`, `kbps`, `KB`/`MB`/`GB`, `dB`, and `-bit`) locale-neutral. Do not localize those fragments unless the owning specification changes; localize the complete surrounding message and its grammar.

## Wording checks

- Use the target language's normal music-player term, not whichever source-language cognate looks closest.
- Keep labels concise, but do not erase distinctions such as Album Artist versus Artist to save width.
- Prefer verbs for actions and nouns for states/headings according to the target platform's convention.
- Keep repeated concepts consistent across GTK, TUI, and WinUI even when sentence structure differs.
- Preserve external names and metadata values exactly when they arrive as message arguments.
