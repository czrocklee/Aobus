// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AppConfigStore.h"

#include "WindowState.h"
#include <ao/Error.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
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
  } // namespace

  AppConfigStore::AppConfigStore(std::filesystem::path const& configPath)
    : _storePtr{std::make_unique<rt::ConfigStore>(configPath)}
  {
  }

  AppConfigStore::AppConfigStore(rt::ConfigStore::NoLocation /*noLocation*/)
    : _storePtr{std::make_unique<rt::ConfigStore>(rt::ConfigStore::NoLocation{})}
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
    rt::loadAppPrefs(*_storePtr, state);
  }

  void AppConfigStore::saveAppPrefs(rt::AppPrefsState const& state)
  {
    if (auto const result = rt::saveAppPrefs(*_storePtr, state); !result)
    {
      APP_LOG_ERROR("AppConfigStore: Failed to save app prefs: {}", result.error().message);
    }
  }

  void AppConfigStore::loadAppSession(rt::AppSessionState& state) const
  {
    rt::loadAppSession(*_storePtr, state);
  }

  Result<> AppConfigStore::saveAppSession(rt::AppSessionState const& state)
  {
    return rt::saveAppSession(*_storePtr, state);
  }

  rt::ConfigStore& AppConfigStore::playbackSessionStore() noexcept
  {
    return *_storePtr;
  }

  uimodel::KeymapModel AppConfigStore::loadKeymap(uimodel::KeymapBindings defaults) const
  {
    return uimodel::loadKeymap(*_storePtr, std::move(defaults));
  }

  Result<> AppConfigStore::saveKeymap(uimodel::KeymapModel const& keymap)
  {
    return uimodel::saveKeymap(*_storePtr, keymap);
  }

  uimodel::OutputDeviceIntent preferredOutputDeviceRecorder(std::shared_ptr<AppConfigStore> configStorePtr)
  {
    return uimodel::OutputDeviceIntent::recordedBy(
      [configStorePtr = std::move(configStorePtr)](audio::OutputDeviceSelection const& selection)
      {
        auto prefs = rt::AppPrefsState{};
        configStorePtr->loadAppPrefs(prefs);
        prefs.preferredOutputSelection = selection;
        configStorePtr->saveAppPrefs(prefs);
      });
  }
} // namespace ao::gtk
