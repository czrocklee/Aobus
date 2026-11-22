// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/AppState.h>

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/yaml/RymlAdapter.h>
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
        auto const& selection = state.preferredOutputSelection;
        auto writer = yaml::MapWriter{node};
        writer.scalar("version", kVersion)
          .scalar("lastOutputBackendId", selection.backendId)
          .scalar("lastOutputProfileId", selection.profileId)
          .scalar("lastOutputDeviceId", selection.deviceId)
          .scalar("lastLayoutPreset", state.lastLayoutPreset)
          .scalar("lastThemePreset", state.lastThemePreset);

        if (audio::supportsOutputRoutingPolicy(selection))
        {
          auto const allowsRerouting = selection.routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute;
          writer.scalar("allowSystemOutputReroute", allowsRerouting);
        }

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
                                                                "lastThemePreset",
                                                                "allowSystemOutputReroute"});

        auto state = seed;
        auto& selection = state.preferredOutputSelection;
        bool allowsRerouting = false;
        auto reader = yaml::MapReader{node, kKeys, kContext, yaml::UnknownKeyPolicy::Allow};
        reader.optionalScalar("lastOutputBackendId", selection.backendId)
          .optionalScalar("lastOutputProfileId", selection.profileId)
          .optionalScalar("lastOutputDeviceId", selection.deviceId)
          .optionalScalar("lastLayoutPreset", state.lastLayoutPreset)
          .optionalScalar("lastThemePreset", state.lastThemePreset)
          .optionalValue("allowSystemOutputReroute",
                         allowsRerouting,
                         [](ryml::ConstNodeRef child, std::string_view) -> Result<bool>
                         {
                           // A malformed permission must not discard a valid fixed route.
                           return child.has_val() && !child.is_val_quoted() && yaml::readBoolOr(child);
                         });

        // A partial preference can retain seeded identity, but cannot grant permission to a seeded route.
        auto const hasStoredRoute = yaml::findChild(node, "lastOutputBackendId").readable() &&
                                    yaml::findChild(node, "lastOutputDeviceId").readable() &&
                                    yaml::findChild(node, "lastOutputProfileId").readable();
        selection.routingPolicy = allowsRerouting && hasStoredRoute && audio::supportsOutputRoutingPolicy(selection)
                                    ? audio::OutputRoutingPolicy::AllowSystemReroute
                                    : audio::OutputRoutingPolicy::Fixed;
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
    bool tryLoadState(ConfigStore& store,
                      std::string_view group,
                      T& state,
                      Schema const& schema,
                      std::string_view description)
    {
      auto const res = store.load(group, state, schema);

      if (!res && res.error().code != Error::Code::NotFound)
      {
        APP_LOG_DEBUG("AppState: Failed to load {}: {}", description, res.error().message);
      }

      return res.value_or(false);
    }
  } // namespace

  void loadAppPrefs(ConfigStore& store, AppPrefsState& state)
  {
    if (!tryLoadState(store, kAppPrefsConfigGroup, state, AppPrefsStateYamlSchema{}, "application preferences"))
    {
      state.preferredOutputSelection.routingPolicy = audio::OutputRoutingPolicy::Fixed;
    }
  }

  Result<> saveAppPrefs(ConfigStore& store, AppPrefsState const& state)
  {
    return store.save(kAppPrefsConfigGroup, state, AppPrefsStateYamlSchema{});
  }

  void loadAppSession(ConfigStore& store, AppSessionState& state)
  {
    std::ignore =
      tryLoadState(store, kAppSessionConfigGroup, state, AppSessionStateYamlSchema{}, "application session");
    // The last active output was an observation, not permission to reroute.
    state.lastOutputSelection.routingPolicy = audio::OutputRoutingPolicy::Fixed;
  }

  Result<> saveAppSession(ConfigStore& store, AppSessionState const& state)
  {
    return store.save(kAppSessionConfigGroup, state, AppSessionStateYamlSchema{});
  }
} // namespace ao::rt
