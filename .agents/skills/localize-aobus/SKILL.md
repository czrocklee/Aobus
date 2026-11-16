---
name: localize-aobus
description: Add, translate, or review Aobus UI copy, i18n catalogs (zh_Hans, zh_Hant, root.txt, MessageInventory.def), and locales across GTK, TUI, WinUI, and AppKit. Do not load for CLI text or Unicode/collation without copy changes.
---

# Localize Aobus

Review is read-only unless fixes are requested. Trace each affected message
through [`MessageInventory.def`](../../../app/include/ao/i18n/MessageInventory.def),
[`root.txt`](../../../app/i18n/catalog/root.txt), [`app/i18n/catalog/`](../../../app/i18n/catalog),
and its C++ call sites or semantic formatter.

- [Localization workflow](../../../doc/development/localization.md):
  [meaning and sparse fallback probes](../../../doc/development/localization.md#1-establish-meaning-and-reachability)
  (`pilot_english_fallback`, `pilot_simplified_only_probe`),
  [WinUI `{0}` and zero-arg `x:Uid` projection](../../../doc/development/localization.md#5-add-only-required-winui-projection-metadata),
  [adding a locale](../../../doc/development/localization.md#add-a-maintained-locale),
  and [validation](../../../doc/development/localization.md#validation).
- [Presentation text semantics](../../../doc/system/presentation/text-catalog.md):
  [text ownership boundary](../../../doc/system/presentation/text-catalog.md#text-ownership-boundary),
  domain mappings, locale-neutral formatting, and normative
  [Chinese metadata terminology](../../../doc/system/presentation/text-catalog.md#chinese-metadata-terminology).
- [Interactive localization contract](../../../doc/system/presentation/localization.md):
  [message and argument contract](../../../doc/system/presentation/localization.md#message-and-argument-contract),
  [ICU/MRT matrix](../../../doc/system/presentation/localization.md#icu-and-mrt-compatibility-matrix),
  and [WinUI projection](../../../doc/system/presentation/localization.md#frontend-resolution-and-winui-projection).
- [Music and audio terminology](references/music-audio-terminology.md):
  Aobus-specific domain concepts and translation traps.
