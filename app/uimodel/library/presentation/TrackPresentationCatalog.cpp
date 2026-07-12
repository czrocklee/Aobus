// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  TrackPresentationCatalog::TrackPresentationCatalog(rt::WorkspaceService& workspace)
    : _workspace{workspace}
  {
    _customPresetsSub = _workspace.onCustomPresetsChanged([this] { _changed.emit(); });
  }

  std::span<rt::TrackPresentationPreset const> TrackPresentationCatalog::builtinPresets() const noexcept
  {
    return rt::builtinTrackPresentationPresets();
  }

  std::span<rt::CustomTrackPresentationPreset const> TrackPresentationCatalog::customPresentations() const noexcept
  {
    return _workspace.customPresets();
  }

  rt::CustomTrackPresentationPreset const* TrackPresentationCatalog::findCustomPresetById(std::string_view id) const
  {
    auto const presets = _workspace.customPresets();
    auto const it = std::ranges::find(presets, id, [](auto const& preset) { return preset.spec.id; });

    return it == presets.end() ? nullptr : &*it;
  }

  std::optional<rt::TrackPresentationSpec> TrackPresentationCatalog::specForId(std::string_view id) const
  {
    if (auto const* builtin = rt::builtinTrackPresentationPreset(id); builtin != nullptr)
    {
      return builtin->spec;
    }

    if (auto const* custom = findCustomPresetById(id); custom != nullptr)
    {
      return custom->spec;
    }

    return std::nullopt;
  }

  std::string TrackPresentationCatalog::labelForId(std::string_view id) const
  {
    if (auto const* builtin = rt::builtinTrackPresentationPreset(id); builtin != nullptr)
    {
      return std::string{builtin->label};
    }

    if (auto const* custom = findCustomPresetById(id); custom != nullptr)
    {
      return custom->label;
    }

    return std::string{id};
  }

  std::vector<TrackPresentationMenuItem> TrackPresentationCatalog::menuItems() const
  {
    auto items = std::vector<TrackPresentationMenuItem>{};

    for (auto const& preset : builtinPresets())
    {
      items.push_back(TrackPresentationMenuItem{
        .type = TrackPresentationMenuItemType::Preset,
        .id = std::string{preset.spec.id},
        .label = std::string{preset.label},
      });
    }

    if (auto const customs = customPresentations(); !customs.empty())
    {
      items.push_back(TrackPresentationMenuItem{
        .type = TrackPresentationMenuItemType::Separator,
      });

      for (auto const& preset : customs)
      {
        items.push_back(TrackPresentationMenuItem{
          .type = TrackPresentationMenuItemType::Preset,
          .id = preset.spec.id,
          .label = preset.label,
        });
      }
    }

    items.push_back(TrackPresentationMenuItem{
      .type = TrackPresentationMenuItemType::Separator,
    });
    items.push_back(TrackPresentationMenuItem{
      .type = TrackPresentationMenuItemType::CreateCustomView,
      .label = "Create Custom View...",
    });

    return items;
  }

  void TrackPresentationCatalog::addCustomPresentation(rt::CustomTrackPresentationPreset const& state)
  {
    _workspace.addCustomPreset(state);
  }

  void TrackPresentationCatalog::removeCustomPresentation(std::string_view id)
  {
    _workspace.removeCustomPreset(id);
  }
} // namespace ao::uimodel
