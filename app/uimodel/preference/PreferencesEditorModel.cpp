// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/preference/PreferencesEditorModel.h>

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppState.h>
#include <ao/uimodel/preference/ThemePreset.h>

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
        current.preferredOutputSelection = requested.preferredOutputSelection;
        break;
    }

    return current;
  }

  PreferencesEditorModel::PreferencesEditorModel(rt::AppPrefsState prefs,
                                                 PersistCallback persist,
                                                 ThemeApplyCallback applyTheme,
                                                 OutputApplyCallback applyOutput)
    : _prefs{std::move(prefs)}
    , _persist{std::move(persist)}
    , _applyTheme{std::move(applyTheme)}
    , _applyOutput{std::move(applyOutput)}
  {
  }

  void PreferencesEditorModel::setTheme(ThemePreset const theme)
  {
    _prefs.lastThemePreset = themePresetId(theme);

    if (_persist)
    {
      _persist(_prefs, PreferencesChange::Theme);
    }

    if (_applyTheme)
    {
      _applyTheme(theme);
    }
  }

  void PreferencesEditorModel::setLayoutPreset(std::string_view const presetId)
  {
    _prefs.lastLayoutPreset = presetId.empty() ? "classic" : std::string{presetId};

    if (_persist)
    {
      _persist(_prefs, PreferencesChange::LayoutPreset);
    }
  }

  void PreferencesEditorModel::setPreferredOutputDevice(audio::OutputDeviceSelection const& selection)
  {
    _prefs.preferredOutputSelection = selection;

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
