// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/AppState.h>

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string_view>
#include <utility>

namespace ao::rt
{
  namespace
  {
    // Protected configuration (decision 0020): an older build never overwrites a newer group.
    struct AppPrefsStateYamlSchema final
    {
      static constexpr std::uint32_t kVersion = 1;

      Result<> serialize(ryml::NodeRef node, AppPrefsState const& state) const
      {
        auto writer = yaml::MapWriter{node};
        writer.scalar("version", kVersion)
          .scalar("lastOutputBackendId", state.preferredOutputSelection.backendId)
          .scalar("lastOutputProfileId", state.preferredOutputSelection.profileId)
          .scalar("lastOutputDeviceId", state.preferredOutputSelection.deviceId)
          .scalar("lastLayoutPreset", state.lastLayoutPreset)
          .scalar("lastThemePreset", state.lastThemePreset);
        return {};
      }

      Result<AppPrefsState> deserialize(ryml::ConstNodeRef node, AppPrefsState const& seed) const
      {
        constexpr auto kContext = std::string_view{"application preferences"};

        if (auto const res = yaml::requireMap(node, kContext); !res)
        {
          return std::unexpected{res.error()};
        }

        auto const versionRes = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

        if (!versionRes)
        {
          return std::unexpected{versionRes.error()};
        }

        // Version 1 is the first version any release writes, and the store refuses a
        // newer one first. Every older version stays readable (decision 0020), so the
        // first increment replaces this check with a reader for each older version.
        static_assert(kVersion == 1, "an older protected config version needs its own reader under decision 0020");

        if (*versionRes != kVersion)
        {
          return makeError(
            Error::Code::NotSupported, std::format("Unsupported application preferences version {}", *versionRes));
        }

        constexpr auto kKeys = std::to_array<std::string_view>({"version",
                                                                "lastOutputBackendId",
                                                                "lastOutputProfileId",
                                                                "lastOutputDeviceId",
                                                                "lastLayoutPreset",
                                                                "lastThemePreset"});

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
      if (auto const res = store.load(group, state, schema); !res && res.error().code != Error::Code::NotFound)
      {
        APP_LOG_DEBUG("AppState: Failed to load {}: {}", description, res.error().message);
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
