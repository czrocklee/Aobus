// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/preferences/PreferencesModel.h>

#include <string_view>
#include <utility>

namespace ao::uimodel
{
  rt::AppPrefsState mergePreferenceChange(rt::AppPrefsState current,
                                          rt::AppPrefsState const& requested,
                                          PreferencesChange const change)
  {
    switch (change)
    {
      case PreferencesChange::Theme: current.lastThemePreset = requested.lastThemePreset; break;
      case PreferencesChange::LayoutPreset: current.lastLayoutPreset = requested.lastLayoutPreset; break;
      case PreferencesChange::OutputDevice:
        current.lastOutputBackendId = requested.lastOutputBackendId;
        current.lastOutputDeviceId = requested.lastOutputDeviceId;
        current.lastOutputProfileId = requested.lastOutputProfileId;
        break;
    }

    return current;
  }

  PreferencesModel::PreferencesModel(rt::AppPrefsState prefs,
                                     PersistCallback persist,
                                     ThemeApplyCallback applyTheme,
                                     OutputApplyCallback applyOutput)
    : _prefs{std::move(prefs)}
    , _persist{std::move(persist)}
    , _applyTheme{std::move(applyTheme)}
    , _applyOutput{std::move(applyOutput)}
  {
  }

  void PreferencesModel::setTheme(rt::ThemePresetId const theme)
  {
    _prefs.lastThemePreset = rt::themePresetToString(theme);

    if (_persist)
    {
      _persist(_prefs, PreferencesChange::Theme);
    }

    if (_applyTheme)
    {
      _applyTheme(theme);
    }
  }

  void PreferencesModel::setLayoutPreset(std::string_view const presetId)
  {
    _prefs.lastLayoutPreset = presetId.empty() ? "classic" : std::string{presetId};

    if (_persist)
    {
      _persist(_prefs, PreferencesChange::LayoutPreset);
    }
  }

  void PreferencesModel::setOutputDeviceConfirmed(rt::OutputDeviceSelection const& selection)
  {
    _prefs.lastOutputBackendId = selection.backendId.raw();
    _prefs.lastOutputDeviceId = selection.deviceId.raw();
    _prefs.lastOutputProfileId = selection.profileId.raw();

    if (_persist)
    {
      _persist(_prefs, PreferencesChange::OutputDevice);
    }

    if (_applyOutput)
    {
      _applyOutput(selection);
    }
  }
} // namespace ao::uimodel
