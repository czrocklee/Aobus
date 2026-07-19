// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AppConfigStore.h"

#include "WindowState.h"
#include <ao/Error.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    template<typename T, rt::ConfigSchema<T> Schema>
    void loadState(rt::ConfigStore& store,
                   std::string_view group,
                   T& state,
                   Schema const& schema,
                   std::string_view description)
    {
      if (auto const result = store.load(group, state, schema); !result && result.error().code != Error::Code::NotFound)
      {
        APP_LOG_DEBUG("AppConfigStore: Failed to load {}: {}", description, result.error().message);
      }
    }

    template<typename T, rt::ConfigSchema<T> Schema>
    void saveState(rt::ConfigStore& store,
                   std::string_view group,
                   T const& state,
                   Schema const& schema,
                   std::string_view description)
    {
      if (auto const result = store.save(group, state, schema); !result)
      {
        APP_LOG_ERROR("AppConfigStore: Failed to save {}: {}", description, result.error().message);
      }
    }

    struct WindowStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, WindowState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("width", state.width).scalar("height", state.height).scalar("maximized", state.maximized);
        return {};
      }

      Result<WindowState> deserialize(ryml::ConstNodeRef node, WindowState const& seed) const
      {
        constexpr auto kContext = std::string_view{"window config"};
        constexpr auto kKeys = std::to_array<std::string_view>({"width", "height", "maximized"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("width", state.width)
          .optionalScalar("height", state.height)
          .optionalScalar("maximized", state.maximized);
        return std::move(reader).finish(std::move(state));
      }
    };

    struct AppPrefsStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, rt::AppPrefsState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("lastOutputBackendId", state.lastOutputBackendId)
          .scalar("lastOutputProfileId", state.lastOutputProfileId)
          .scalar("lastOutputDeviceId", state.lastOutputDeviceId)
          .scalar("lastLayoutPreset", state.lastLayoutPreset)
          .scalar("lastThemePreset", state.lastThemePreset);
        return {};
      }

      Result<rt::AppPrefsState> deserialize(ryml::ConstNodeRef node, rt::AppPrefsState const& seed) const
      {
        constexpr auto kContext = std::string_view{"application preferences"};
        constexpr auto kKeys = std::to_array<std::string_view>(
          {"lastOutputBackendId", "lastOutputProfileId", "lastOutputDeviceId", "lastLayoutPreset", "lastThemePreset"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("lastOutputBackendId", state.lastOutputBackendId)
          .optionalScalar("lastOutputProfileId", state.lastOutputProfileId)
          .optionalScalar("lastOutputDeviceId", state.lastOutputDeviceId)
          .optionalScalar("lastLayoutPreset", state.lastLayoutPreset)
          .optionalScalar("lastThemePreset", state.lastThemePreset);
        return std::move(reader).finish(std::move(state));
      }
    };

    struct AppSessionStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, rt::AppSessionState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("lastLibraryPath", state.lastLibraryPath)
          .scalar("lastOutputBackendId", state.lastOutputBackendId)
          .scalar("lastOutputProfileId", state.lastOutputProfileId)
          .scalar("lastOutputDeviceId", state.lastOutputDeviceId);
        return {};
      }

      Result<rt::AppSessionState> deserialize(ryml::ConstNodeRef node, rt::AppSessionState const& seed) const
      {
        constexpr auto kContext = std::string_view{"application session"};
        constexpr auto kKeys = std::to_array<std::string_view>(
          {"lastLibraryPath", "lastOutputBackendId", "lastOutputProfileId", "lastOutputDeviceId"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("lastLibraryPath", state.lastLibraryPath)
          .optionalScalar("lastOutputBackendId", state.lastOutputBackendId)
          .optionalScalar("lastOutputProfileId", state.lastOutputProfileId)
          .optionalScalar("lastOutputDeviceId", state.lastOutputDeviceId);
        return std::move(reader).finish(std::move(state));
      }
    };
  } // namespace

  AppConfigStore::AppConfigStore(std::filesystem::path const& configPath)
    : _storePtr{std::make_unique<rt::ConfigStore>(configPath)}
  {
  }

  AppConfigStore::~AppConfigStore() = default;

  AppConfigStore::AppConfigStore(AppConfigStore&&) noexcept = default;
  AppConfigStore& AppConfigStore::operator=(AppConfigStore&&) noexcept = default;

  void AppConfigStore::loadWindow(WindowState& state) const
  {
    loadState(*_storePtr, "window", state, WindowStateYamlSchema{}, "window config");
  }

  void AppConfigStore::saveWindow(WindowState const& state)
  {
    saveState(*_storePtr, "window", state, WindowStateYamlSchema{}, "window config");
  }

  void AppConfigStore::loadAppPrefs(rt::AppPrefsState& state) const
  {
    loadState(*_storePtr, "runtime", state, AppPrefsStateYamlSchema{}, "app prefs");
  }

  void AppConfigStore::saveAppPrefs(rt::AppPrefsState const& state)
  {
    saveState(*_storePtr, "runtime", state, AppPrefsStateYamlSchema{}, "app prefs");
  }

  void AppConfigStore::loadAppSession(rt::AppSessionState& state) const
  {
    loadState(*_storePtr, "session", state, AppSessionStateYamlSchema{}, "app session");
  }

  void AppConfigStore::saveAppSession(rt::AppSessionState const& state)
  {
    saveState(*_storePtr, "session", state, AppSessionStateYamlSchema{}, "app session");
  }

  rt::ConfigStore& AppConfigStore::playbackSessionStore() noexcept
  {
    return *_storePtr;
  }

  uimodel::KeymapModel AppConfigStore::loadKeymap(uimodel::KeymapBindings defaults) const
  {
    return uimodel::loadKeymap(*_storePtr, std::move(defaults));
  }

  void AppConfigStore::saveKeymap(uimodel::KeymapModel const& keymap)
  {
    uimodel::saveKeymap(*_storePtr, keymap);
  }
} // namespace ao::gtk
