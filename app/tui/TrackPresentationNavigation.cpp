// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPresentationNavigation.h"

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::tui
{
  using i18n::MessageId;

  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    i18n::MessageCatalog const& textCatalog,
    ListId const listId,
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    auto items = std::vector<TrackPresentationNavEntry>{};
    items.reserve(builtinPresets.size() + customPresets.size());

    for (auto const& preset : builtinPresets)
    {
      auto const id = preset.presetId();
      auto const key = std::string{rt::builtinPresetKey(id)};
      auto const text = uimodel::builtinTrackPresentation(textCatalog, id);
      items.push_back(TrackPresentationNavEntry{
        .id = key,
        .label = std::string{text.label},
        .detail = std::string{text.description},
      });
    }

    for (auto const& preset : customPresets)
    {
      auto const key = rt::trackPresetKey(preset.presetId());
      items.push_back(TrackPresentationNavEntry{
        .id = key,
        .label = preset.label.empty() ? key : preset.label,
        .detail = preset.optBasePresetId
                    ? i18n::requiredFormat(textCatalog,
                                           MessageId::TuiPresentationCustomFrom,
                                           {{"id", rt::trackPresetKey(*preset.optBasePresetId)}})
                    : std::string{i18n::requiredText(textCatalog, MessageId::TuiPresentationCustom)},
      });
    }

    for (auto& item : items)
    {
      auto eligibility = uimodel::trackPresentationEligibility(textCatalog, listId, item.id);
      item.enabled = eligibility.enabled;

      if (!item.enabled)
      {
        item.detail = std::move(eligibility.disabledReason);
      }
    }

    return items;
  }

  std::string trackPresentationDisplayLabel(i18n::MessageCatalog const& textCatalog,
                                            rt::TrackPresetId const& presetId,
                                            std::span<TrackPresentationNavEntry const> const entries)
  {
    auto key = rt::trackPresetKey(presetId);

    if (auto const it = std::ranges::find(entries, key, &TrackPresentationNavEntry::id); it != entries.end())
    {
      return it->label;
    }

    if (auto const* builtin = std::get_if<rt::BuiltinPreset>(&presetId); builtin != nullptr)
    {
      return std::string{uimodel::builtinTrackPresentation(textCatalog, *builtin).label};
    }

    return key;
  }
} // namespace ao::tui
