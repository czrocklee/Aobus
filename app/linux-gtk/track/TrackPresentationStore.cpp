// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationStore.h"

#include <ao/utility/Log.h>

#include <algorithm>
#include <ranges>

namespace ao::gtk
{
  rt::TrackPresentationSpec specFromState(CustomTrackPresentationState const& state)
  {
    auto spec = rt::TrackPresentationSpec{
      .id = state.id,
      .groupBy = static_cast<rt::TrackGroupKey>(state.groupBy),
    };

    spec.sortBy = state.sortBy |
                  std::views::transform(
                    [](auto const& term)
                    {
                      return rt::TrackSortTerm{
                        .field = static_cast<rt::TrackSortField>(term.field),
                        .ascending = term.ascending,
                      };
                    }) |
                  std::ranges::to<std::vector>();

    spec.visibleFields = state.visibleFields |
                         std::views::transform([](auto f) { return static_cast<rt::TrackPresentationField>(f); }) |
                         std::ranges::to<std::vector>();

    spec.redundantFields = state.redundantFields |
                           std::views::transform([](auto f) { return static_cast<rt::TrackPresentationField>(f); }) |
                           std::ranges::to<std::vector>();

    return spec;
  }

  TrackPresentationStore::TrackPresentationStore(std::shared_ptr<rt::ConfigStore> configStore)
    : _configStore{std::move(configStore)}
  {
    load();
  }

  std::span<rt::TrackPresentationPreset const> TrackPresentationStore::builtinPresets() const noexcept
  {
    return rt::builtinTrackPresentationPresets();
  }

  std::vector<CustomTrackPresentationState> const& TrackPresentationStore::customPresentations() const noexcept
  {
    return _state.customPresentations;
  }

  std::optional<rt::TrackPresentationSpec> TrackPresentationStore::specForId(std::string_view id) const
  {
    if (auto const* builtin = rt::builtinTrackPresentationPreset(id))
    {
      return builtin->spec;
    }

    auto const it = std::ranges::find(_state.customPresentations, id, &CustomTrackPresentationState::id);

    if (it != _state.customPresentations.end())
    {
      return specFromState(*it);
    }

    return std::nullopt;
  }

  void TrackPresentationStore::setCustomPresentations(std::vector<CustomTrackPresentationState> presentations)
  {
    _state.customPresentations = std::move(presentations);
    save();
    _changed.emit();
  }

  void TrackPresentationStore::addCustomPresentation(CustomTrackPresentationState const& state)
  {
    removeCustomPresentation(state.id);
    _state.customPresentations.push_back(state);
    save();
    _changed.emit();
  }

  void TrackPresentationStore::removeCustomPresentation(std::string_view id)
  {
    auto const removed = std::erase_if(_state.customPresentations, [&](auto const& s) { return s.id == id; });

    if (removed > 0)
    {
      save();
      _changed.emit();
    }
  }

  void TrackPresentationStore::load()
  {
    if (auto const res = _configStore->load("custom_presentations", _state);
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load custom presentations: {}", res.error().message);
    }
  }

  void TrackPresentationStore::save()
  {
    _configStore->save("custom_presentations", _state);
  }
} // namespace ao::gtk
