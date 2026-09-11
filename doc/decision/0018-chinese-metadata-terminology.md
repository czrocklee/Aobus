---
id: decision.0018.chinese-metadata-terminology
type: decision
status: accepted
domain: presentation
summary: Records the choice of consistent Chinese metadata terminology across frontends.
---
# Decision 0018: choose consistent Chinese metadata terminology

## Context

The TUI detail-panel terminology review questioned 中繼資料 and requested a researched, repository-wide choice for both maintained Chinese catalogs.
The resulting terminology change selected 後設資料 for Traditional Chinese and retained 元数据 for Simplified Chinese across GTK, TUI, and WinUI.
The first review of that change requested that the supporting sources and alternatives move from the text-catalog reference into this decision record.

Traditional Chinese has several established translations of metadata, and Aobus uses one shared `zh_Hant` catalog for regional requests including `zh-TW` and `zh-HK`.
No single candidate represents every regional or platform convention.

## Decision

Use 元数据 and 自定义元数据 for Simplified Chinese, and 後設資料 and 自訂後設資料 for Traditional Chinese.
Keep the same concept names across frontends while allowing sentence grammar to vary.

The Simplified Chinese term follows established usage, including the title of [GB/T 42743-2023](https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=4CF4B6F5A6389B7C89CC841DFE81BF02).
Taiwan's Ministry of Education dictionary entry, sourced from the National Academy for Educational Research, lists [詮釋資料, 後設資料, and 元資料](https://pedia.cloud.edu.tw/Entry/Detail/?search=%E8%A9%AE%E9%87%8B%E8%B3%87%E6%96%99&title=%E8%A9%AE%E9%87%8B%E8%B3%87%E6%96%99) as translations of metadata.
Aobus chooses 後設資料, which Apple also uses in its [Traditional Chinese audio requirements](https://podcasters.apple.com/zh-tw/support/893-audio-requirements).
These sources establish usage and variation; none governs Aobus wording.

## Alternatives considered

- Retain Microsoft's [中繼資料](https://learn.microsoft.com/zh-tw/powerquery-m/m-spec-operators): established platform usage, but the review chose 後設資料 as the common term across Aobus frontends.
- Use 詮釋資料 or 元資料: established dictionary alternatives, but not selected for the shared catalog once 後設資料 was chosen.
- Use the Hong Kong Lands Department's [元數據](https://www.hkmapmeta.gov.hk/mcs/home/tc/metadata.htm): established regional usage, but choosing it would not make the shared catalog satisfy every region's preference either.

## Consequences

The maintained catalogs have one project vocabulary for metadata and custom metadata across frontends.
The Traditional Chinese choice does not promise each region's preferred terminology.
The change affects display copy only; message identities, serialized keys, and user-supplied metadata names and values remain unchanged.
Future messages for the same concepts need terminology review against the canonical reference.

## Current authorities

- [Chinese metadata terminology](../reference/presentation/text-catalog.md#chinese-metadata-terminology)
- [Interactive localization](../spec/presentation/localization.md)
