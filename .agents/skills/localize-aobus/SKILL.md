---
name: localize-aobus
description: Add or review Aobus UI messages, translations, and locales. Excludes Unicode normalization and collation changes without displayed copy.
---

# Localize Aobus

Review is read-only unless fixes are requested. For the affected messages, read
`app/i18n/catalog/root.txt`, the target catalog, and call sites needed to resolve
UI meaning.

Read by contract:

- `doc/reference/presentation/text-catalog.md`: message ownership, signatures,
  external values, and locale-neutral formatting.
- `doc/spec/presentation/localization.md`: adding a locale, fallback rules,
  frontend admission, generated resources, and native parity.
- [Music and audio terminology](references/music-audio-terminology.md): translating
  metadata, library/List, playback, routing, or quality-analysis vocabulary.

For reviews:

- Verify external reports against the English source, actual handlers, and the
  governing document section; a formatter's rule may have a narrower scope.
- Render complete sentences with reachable counts and scope arguments. ICU parity
  does not prove grammar or preserve distinctions such as all versus partial results.
- Equal text is only a deduplication candidate. Compare meaning and grammatical
  role; trace render branches and narrow-layout command access before removing UI.

Preserve ICU argument names and kinds, selectors, `#`, braces, and `other` branches;
encode literal apostrophes as `''`. Edit authored catalogs, not generated `.res`,
`.dat`, or `.resw` output. Preserve the intentional root-only
`pilot_english_fallback` and Simplified Chinese `pilot_simplified_only_probe`.

When adding a locale, update the build/package inputs, WinUI projection, parity
cases, and governing fallback specification together. Build catalogs before
launching the UI; `-n` is only valid against an already rebuilt tree.

Inspect the requested locale in affected frontends at normal and constrained
widths. Pseudo-localization (`qps-ploc`) detects structural omissions but does
not validate wording. Do not hide layout defects by distorting translations.
A newly maintained language needs fluent or domain-informed review.
Check every selection and expanded popup at the GTK window-manager minimum;
character counts do not establish rendered width. Confirm the displayed locale
and inspect images and layout warnings; UI Automation alone does not prove correct rendering.
Record DPI and client size before comparing desktop widths with layout breakpoints.

Use the completion policy in `doc/development/test/validation-and-review.md`.
Use its catalog-only route for text edits within maintained locales. New locales
and changes to WinUI projection rules require native Windows parity evidence;
ordinary translated text does not change those rules. Inspect Windows UI when it
consumes the affected messages. Report uncertainties, fallback, and untested surfaces.
