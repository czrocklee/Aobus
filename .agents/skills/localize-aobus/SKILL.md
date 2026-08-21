---
name: localize-aobus
description: Add or review Aobus interactive UI translations, catalog messages, or locale assets with ICU MessageFormat safety, music/audio terminology, fallback parity, and visual localization checks. Do not use for Unicode normalization or collation changes that do not alter displayed copy.
---

# Localize Aobus

Translate semantic UI messages, not isolated English strings. Keep catalog mechanics deterministic and review language quality in the interface where the text appears.

## Required reading

Before changing a catalog, read:

- `doc/spec/presentation/localization.md` for locale admission, fallback, frontend boundaries, and generated-resource behavior;
- `doc/reference/presentation/text-catalog.md` for message ownership, signatures, and intentionally locale-neutral formatting;
- `app/i18n/catalog/root.txt` for the canonical English patterns;
- the target locale catalog, when one exists.

Read [music and audio terminology](references/music-audio-terminology.md) when translating track metadata, library/List vocabulary, playback, audio routing, or quality-analysis messages. Inspect a message's call site whenever its key and English pattern do not establish one unambiguous UI meaning.

## Authoring workflow

1. Choose the resource locale deliberately. Prefer a neutral language catalog when one wording serves its regions; use a script-qualified catalog when writing systems require separate assets. Follow the ICU-id to BCP 47/MRT mapping already owned by the localization specification and `app/CMakeLists.txt`.
2. Work by semantic UI slice, such as metadata, library authoring, playback, or preferences. Review neighboring messages together so terminology, tone, capitalization, and action phrasing stay coherent.
3. Preserve the canonical key and the pattern's programmatic structure. Translate literal spans only; keep argument names, argument kinds, `plural`/`select` selectors, `#`, braces, and the required `other` branch intact. In authored ICU patterns, encode a literal apostrophe as `''`.
4. Keep identifiers and external values out of translated literals. Commands, shortcut tokens, action ids, paths, URIs, device names, application names, and track metadata remain arguments or raw data as specified by the owning reference.
5. Do not shorten an accurate translation merely to hide a layout defect. First verify the surface's finite-width policy; use wrapping, ellipsis, or a full-text tooltip where the UI contract calls for it.

When adding a maintained locale, update the canonical catalog build/package list, generated WinUI projection inputs, native parity matrix, fallback specification, implementation map, and focused catalog tests together. Do not hand-edit generated `.res`, `.dat`, or `.resw` output. Preserve deliberate fallback probes: `pilot_english_fallback` remains root-only, and `pilot_simplified_only_probe` remains specific to the Simplified Chinese fallback test unless the specification changes.

## Language review

- Prefer idiomatic music-application language over word-for-word correspondence with English.
- Distinguish metadata concepts that ordinary dictionaries conflate, especially track/stream, album artist/track artist, work/movement, disc/track number, sample rate/bitrate, and library/List/queue.
- Translate complete messages rather than concatenating localized fragments. Verify plural grammar with representative values such as 0, 1, and 2, and verify every `select` branch.
- Treat the English root as user-facing copy too. If translation exposes an ambiguous or grammatically incorrect root pattern, make that English change explicit and cover its intended output.
- If a term remains uncertain, report the alternatives and UI context instead of silently guessing. A fluent or domain-informed review is the quality gate for a newly maintained language.

## Runtime review

Build before the first launch after catalog edits; use `-n` only for later launches against an already rebuilt tree. Exercise the requested locale in each affected interactive frontend, for example:

```bash
LANG=fr_FR.UTF-8 ./ao run gtk
LANG=zh_TW.UTF-8 ./ao run tui
```

Review at normal and constrained widths. Check menus, headings, field labels, buttons, tooltips, empty states, errors, dynamic arguments, plural/select output, and truncation. Use `qps-ploc` to expose structural omissions, then inspect the real locale because pseudo-localization cannot judge terminology or natural phrasing.

For a new locale or any WinUI projection change, run the native Windows gate described in `doc/development/windows.md`; it verifies ICU-to-MRT selection and formatting parity. Do not infer native parity from the Linux catalog build.

## Validation and handoff

Use the repository portal and finish with the validation required by `doc/development/test/validation-and-review.md`. At minimum, ensure the catalog compiler runs through a normal build, run the focused localization tests while iterating, and run one full `./ao check` for completed work. Run `./ao docs check` when governed localization documents change.

Report:

- locale and UI slices added or reviewed;
- terminology decisions and any deliberate English fallback;
- ICU pattern/signature and WinUI parity coverage;
- frontends and viewport widths inspected;
- validation commands and any native platform not exercised.
