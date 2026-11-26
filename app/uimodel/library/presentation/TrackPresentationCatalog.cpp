// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  TrackPresentationCatalog::TrackPresentationCatalog(rt::WorkspaceService& workspace, i18n::MessageCatalog textCatalog)
    : _workspace{workspace}, _textCatalog{std::move(textCatalog)}
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

  rt::CustomTrackPresentationPreset const* TrackPresentationCatalog::findCustomPresetById(rt::CustomPresetId id) const
  {
    auto const presets = _workspace.customPresets();
    auto const it = std::ranges::find(presets, id, &rt::CustomTrackPresentationPreset::presetId);

    return it == presets.end() ? nullptr : &*it;
  }

  std::optional<rt::TrackPresentationSpec> TrackPresentationCatalog::specForId(rt::TrackPresetId const& id) const
  {
    if (auto const* builtin = std::get_if<rt::BuiltinPreset>(&id); builtin != nullptr)
    {
      return rt::builtinTrackPresentationPreset(*builtin).spec;
    }

    if (auto const* custom = findCustomPresetById(std::get<rt::CustomPresetId>(id)); custom != nullptr)
    {
      return custom->spec;
    }

    return std::nullopt;
  }

  std::string TrackPresentationCatalog::labelFor(rt::TrackPresetId const& id) const
  {
    if (auto const* builtin = std::get_if<rt::BuiltinPreset>(&id); builtin != nullptr)
    {
      return std::string{builtinTrackPresentation(_textCatalog, *builtin).label};
    }

    auto const* custom = findCustomPresetById(std::get<rt::CustomPresetId>(id));
    return custom != nullptr ? custom->label : rt::trackPresetKey(id);
  }

  std::vector<TrackPresentationMenuItem> TrackPresentationCatalog::menuItems() const
  {
    auto items = std::vector<TrackPresentationMenuItem>{};

    for (auto const& preset : builtinPresets())
    {
      auto const id = preset.presetId();
      items.push_back(TrackPresentationMenuItem{
        .type = TrackPresentationMenuItemType::Preset,
        .id = std::string{rt::builtinPresetKey(id)},
        .label = labelFor(id),
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
          .id = rt::trackPresetKey(preset.presetId()),
          .label = preset.label,
        });
      }
    }

    items.push_back(TrackPresentationMenuItem{
      .type = TrackPresentationMenuItemType::Separator,
    });
    items.push_back(TrackPresentationMenuItem{
      .type = TrackPresentationMenuItemType::CreateCustomView,
      .label = std::string{i18n::requiredText(_textCatalog, i18n::MessageId::CreateCustomTrackPresentation)},
    });

    return items;
  }

  void TrackPresentationCatalog::addCustomPresentation(rt::CustomTrackPresentationPreset const& state)
  {
    if (auto const res = _workspace.addCustomPreset(state); !res)
    {
      APP_LOG_ERROR("Failed to add custom track presentation: {}", res.error().message);
    }
  }

  void TrackPresentationCatalog::setTextCatalog(i18n::MessageCatalog textCatalog)
  {
    _textCatalog = std::move(textCatalog);
    _changed.emit();
  }
} // namespace ao::uimodel
