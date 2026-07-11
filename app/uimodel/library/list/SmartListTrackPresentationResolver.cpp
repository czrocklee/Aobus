// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/list/SmartListTrackPresentationResolver.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  std::size_t resolveSmartListTrackPresentationIndex(std::optional<std::string> const& optPresentationId,
                                                     std::span<rt::TrackPresentationPreset const> builtinPresets)
  {
    if (!optPresentationId)
    {
      return kSmartListAutoTrackPresentationIndex;
    }

    auto const it =
      std::ranges::find(builtinPresets, *optPresentationId, [](auto const& preset) { return preset.spec.id; });

    if (it == builtinPresets.end())
    {
      return kSmartListAutoTrackPresentationIndex;
    }

    return static_cast<std::size_t>(std::ranges::distance(builtinPresets.begin(), it)) + 1;
  }

  std::string resolveSmartListTrackPresentationId(std::size_t selectedIndex,
                                                  bool selectedIndexValid,
                                                  std::string_view localExpression,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets)
  {
    if (!selectedIndexValid || selectedIndex == kSmartListAutoTrackPresentationIndex)
    {
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::Smart,
        .smartListFilter = localExpression,
      };
      return recommendPresentation(context, builtinPresets, customPresets).id;
    }

    if (auto const presetIndex = selectedIndex - 1; presetIndex < builtinPresets.size())
    {
      return std::string{builtinPresets[presetIndex].spec.id};
    }

    return std::string{rt::kDefaultTrackPresentationId};
  }
} // namespace ao::uimodel
