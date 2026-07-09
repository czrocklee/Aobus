// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackState.h>

#include <cstdint>
#include <functional>
#include <string_view>

namespace ao::uimodel
{
  enum class PreferencesChange : std::uint8_t
  {
    Theme,
    LayoutPreset,
    OutputDevice,
  };

  rt::AppPrefsState mergePreferenceChange(rt::AppPrefsState current,
                                          rt::AppPrefsState const& requested,
                                          PreferencesChange change);

  class PreferencesEditorModel final
  {
  public:
    using PersistCallback = std::function<void(rt::AppPrefsState const&, PreferencesChange)>;
    using ThemeApplyCallback = std::function<void(rt::ThemePresetId)>;
    using OutputApplyCallback = std::function<void(rt::OutputDeviceSelection const&)>;

    explicit PreferencesEditorModel(rt::AppPrefsState prefs,
                                    PersistCallback persist,
                                    ThemeApplyCallback applyTheme,
                                    OutputApplyCallback applyOutput);

    rt::AppPrefsState const& preferences() const noexcept { return _prefs; }

    void setTheme(rt::ThemePresetId theme);
    void setLayoutPreset(std::string_view presetId);
    void setOutputDeviceConfirmed(rt::OutputDeviceSelection const& selection);

  private:
    rt::AppPrefsState _prefs;
    PersistCallback _persist;
    ThemeApplyCallback _applyTheme;
    OutputApplyCallback _applyOutput;
  };
} // namespace ao::uimodel
