// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
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
    _customPresetsSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.cause == rt::WorkspaceChangeCause::Presets || changed.cause == rt::WorkspaceChangeCause::Restore)
        {
          _changed.emit();
        }
      });
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
      if (auto const optText = _textCatalog.builtinTrackPresentation(builtin->spec.id); optText)
      {
        return std::string{optText->label};
      }
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
      auto const optText = _textCatalog.builtinTrackPresentation(preset.spec.id);

      items.push_back(TrackPresentationMenuItem{
        .type = TrackPresentationMenuItemType::Preset,
        .id = std::string{preset.spec.id},
        .label = optText ? std::string{optText->label} : preset.spec.id,
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
      .label = std::string{_textCatalog.createCustomTrackPresentationLabel()},
    });

    return items;
  }

  void TrackPresentationCatalog::addCustomPresentation(rt::CustomTrackPresentationPreset const& state)
  {
    if (auto const result = _workspace.addCustomPreset(state); !result)
    {
      APP_LOG_ERROR("Failed to add custom track presentation: {}", result.error().message);
    }
  }
} // namespace ao::uimodel
