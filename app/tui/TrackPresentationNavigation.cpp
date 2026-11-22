// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPresentationNavigation.h"

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>
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
      auto const optText = uimodel::builtinTrackPresentation(textCatalog, preset.spec.id);
      items.push_back(TrackPresentationNavEntry{
        .id = preset.spec.id,
        .label = optText ? std::string{optText->label} : preset.spec.id,
        .detail = optText ? std::string{optText->description} : std::string{},
      });
    }

    for (auto const& preset : customPresets)
    {
      items.push_back(TrackPresentationNavEntry{
        .id = preset.spec.id,
        .label = preset.label.empty() ? preset.spec.id : preset.label,
        .detail =
          preset.basePresetId.empty()
            ? std::string{i18n::requiredText(textCatalog, MessageId::TuiPresentationCustom)}
            : i18n::requiredFormat(textCatalog, MessageId::TuiPresentationCustomFrom, {{"id", preset.basePresetId}}),
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

  std::string trackPresentationDisplayId(i18n::MessageCatalog const& textCatalog, std::string_view const presentationId)
  {
    return presentationId.empty() ? std::string{i18n::requiredText(textCatalog, MessageId::TuiPresentationDefault)}
                                  : std::string{presentationId};
  }

  std::string trackPresentationBadgeLabel(i18n::MessageCatalog const& textCatalog,
                                          std::string_view const presentationId)
  {
    auto const id = trackPresentationDisplayId(textCatalog, presentationId);
    return i18n::requiredFormat(textCatalog, MessageId::TuiPresentationBadge, {{"id", id}});
  }
} // namespace ao::tui
