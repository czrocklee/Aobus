// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPresentationNavigation.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  using i18n::MessageId;

  std::vector<TrackPresentationNavEntry> makeTrackPresentationNavigation(
    uimodel::PresentationTextCatalog const& textCatalog,
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    auto items = std::vector<TrackPresentationNavEntry>{};
    items.reserve(builtinPresets.size() + customPresets.size());

    for (auto const& preset : builtinPresets)
    {
      auto const optText = textCatalog.builtinTrackPresentation(preset.spec.id);
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
        .detail = preset.basePresetId.empty()
                    ? std::string{textCatalog.text(MessageId::TuiPresentationCustom)}
                    : textCatalog.format(MessageId::TuiPresentationCustomFrom, {{"id", preset.basePresetId}}),
      });
    }

    return items;
  }

  std::string trackPresentationDisplayId(uimodel::PresentationTextCatalog const& textCatalog,
                                         std::string_view const presentationId)
  {
    return presentationId.empty() ? std::string{textCatalog.text(MessageId::TuiPresentationDefault)}
                                  : std::string{presentationId};
  }

  std::string trackPresentationBadgeLabel(uimodel::PresentationTextCatalog const& textCatalog,
                                          std::string_view const presentationId)
  {
    auto const id = trackPresentationDisplayId(textCatalog, presentationId);
    return textCatalog.format(MessageId::TuiPresentationBadge, {{"id", id}});
  }
} // namespace ao::tui
