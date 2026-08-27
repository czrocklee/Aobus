// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/AppState.h>

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <string_view>
#include <utility>

namespace ao::rt
{
  namespace
  {
    struct AppPrefsStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, AppPrefsState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("lastOutputBackendId", state.preferredOutputSelection.backendId)
          .scalar("lastOutputProfileId", state.preferredOutputSelection.profileId)
          .scalar("lastOutputDeviceId", state.preferredOutputSelection.deviceId)
          .scalar("lastLayoutPreset", state.lastLayoutPreset)
          .scalar("lastThemePreset", state.lastThemePreset);
        return {};
      }

      Result<AppPrefsState> deserialize(ryml::ConstNodeRef node, AppPrefsState const& seed) const
      {
        constexpr auto kContext = std::string_view{"application preferences"};
        constexpr auto kKeys = std::to_array<std::string_view>(
          {"lastOutputBackendId", "lastOutputProfileId", "lastOutputDeviceId", "lastLayoutPreset", "lastThemePreset"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("lastOutputBackendId", state.preferredOutputSelection.backendId)
          .optionalScalar("lastOutputProfileId", state.preferredOutputSelection.profileId)
          .optionalScalar("lastOutputDeviceId", state.preferredOutputSelection.deviceId)
          .optionalScalar("lastLayoutPreset", state.lastLayoutPreset)
          .optionalScalar("lastThemePreset", state.lastThemePreset);
        return std::move(reader).finish(std::move(state));
      }
    };

    struct AppSessionStateYamlSchema final
    {
      Result<> serialize(ryml::NodeRef node, AppSessionState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("lastLibraryPath", state.lastLibraryPath)
          .scalar("lastOutputBackendId", state.lastOutputSelection.backendId)
          .scalar("lastOutputProfileId", state.lastOutputSelection.profileId)
          .scalar("lastOutputDeviceId", state.lastOutputSelection.deviceId);
        return {};
      }

      Result<AppSessionState> deserialize(ryml::ConstNodeRef node, AppSessionState const& seed) const
      {
        constexpr auto kContext = std::string_view{"application session"};
        constexpr auto kKeys = std::to_array<std::string_view>(
          {"lastLibraryPath", "lastOutputBackendId", "lastOutputProfileId", "lastOutputDeviceId"});

        auto state = seed;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("lastLibraryPath", state.lastLibraryPath)
          .optionalScalar("lastOutputBackendId", state.lastOutputSelection.backendId)
          .optionalScalar("lastOutputProfileId", state.lastOutputSelection.profileId)
          .optionalScalar("lastOutputDeviceId", state.lastOutputSelection.deviceId);
        return std::move(reader).finish(std::move(state));
      }
    };

    template<typename T, ConfigSchema<T> Schema>
    void loadState(ConfigStore& store,
                   std::string_view group,
                   T& state,
                   Schema const& schema,
                   std::string_view description)
    {
      if (auto const result = store.load(group, state, schema); !result && result.error().code != Error::Code::NotFound)
      {
        APP_LOG_DEBUG("AppState: Failed to load {}: {}", description, result.error().message);
      }
    }
  } // namespace

  void loadAppPrefs(ConfigStore& store, AppPrefsState& state)
  {
    loadState(store, kAppPrefsConfigGroup, state, AppPrefsStateYamlSchema{}, "application preferences");
  }

  Result<> saveAppPrefs(ConfigStore& store, AppPrefsState const& state)
  {
    return store.save(kAppPrefsConfigGroup, state, AppPrefsStateYamlSchema{});
  }

  void loadAppSession(ConfigStore& store, AppSessionState& state)
  {
    loadState(store, kAppSessionConfigGroup, state, AppSessionStateYamlSchema{}, "application session");
  }

  Result<> saveAppSession(ConfigStore& store, AppSessionState const& state)
  {
    return store.save(kAppSessionConfigGroup, state, AppSessionStateYamlSchema{});
  }
} // namespace ao::rt
