// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/UiToggleManager.h"

#include "ao/utility/Log.h"

#include <sigc++/signal.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  void UiToggleManager::setToggleState(std::string_view const key, bool state)
  {
    auto const keyStr = std::string{key};
    bool const changed = !_states.contains(keyStr) || (_states[keyStr] != state);

    if (changed)
    {
      _states[keyStr] = state;
      APP_LOG_TRACE("UiToggleManager: Toggle '{}' changed to {}", keyStr, state);
      _signals[keyStr].emit(state);
    }
  }

  bool UiToggleManager::getToggleState(std::string_view const key) const
  {
    auto const it = _states.find(key);

    if (it == _states.end())
    {
      APP_LOG_WARN("UiToggleManager: Attempted to get unknown toggle state '{}'", key);
      return false;
    }

    return it->second;
  }

  sigc::signal<void(bool)>& UiToggleManager::signalToggleChanged(std::string_view const key)
  {
    return _signals[std::string{key}];
  }

  void UiToggleManager::loadStates(std::map<std::string, bool, std::less<>> states)
  {
    _states = std::move(states);

    for (auto const& [key, state] : _states)
    {
      _signals[key].emit(state);
    }
  }
} // namespace ao::gtk::layout
