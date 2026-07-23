// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisBridge.h"

#include "MprisPlaybackEndpoint.h"
#include "common/UStringConvert.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/utility/ScopedRegistration.h>

#include <giomm/dbusconnection.h>
#include <giomm/dbusintrospection.h>
#include <giomm/dbusmethodinvocation.h>
#include <giomm/dbusownname.h>
#include <giomm/error.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <glibmm/variantdbusstring.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::gtk::platform
{
  namespace
  {
    constexpr char const* kBusName = "org.mpris.MediaPlayer2.aobus";
    constexpr char const* kObjectPath = "/org/mpris/MediaPlayer2";
    constexpr char const* kRootInterface = "org.mpris.MediaPlayer2";
    constexpr char const* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr char const* kPropertiesInterface = "org.freedesktop.DBus.Properties";
    constexpr char const* kMprisError = "org.mpris.MediaPlayer2.Error.NotSupported";
    constexpr char const* kTrackObjectPathPrefix = "/org/mpris/MediaPlayer2/Track/";

    constexpr char const* kMprisIntrospectionXml = R"xml(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="Fullscreen" type="b" access="read"/>
    <property name="CanSetFullscreen" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek">
      <arg name="Offset" type="x" direction="in"/>
    </method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/>
      <arg name="Position" type="x" direction="in"/>
    </method>
    <signal name="Seeked">
      <arg name="Position" type="x"/>
    </signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)xml";

    using MetadataVariantMap = std::map<Glib::ustring, Glib::VariantBase>;
    using PropertiesChangedPayload = std::tuple<Glib::ustring, MetadataVariantMap, std::vector<Glib::ustring>>;
    using SeekedPayload = std::tuple<std::int64_t>;

    [[noreturn]] void throwGioError(Gio::Error::Code const code, char const* const message)
    {
      throw Gio::Error{code, message};
    }

    Glib::Variant<std::vector<Glib::ustring>> emptyStringArray()
    {
      return Glib::Variant<std::vector<Glib::ustring>>::create(std::vector<Glib::ustring>{});
    }

    Glib::Variant<std::vector<Glib::ustring>> stringArray(std::string_view const value)
    {
      return Glib::Variant<std::vector<Glib::ustring>>::create(std::vector{toUString(value)});
    }

    Glib::Variant<MetadataVariantMap> metadataVariant(MprisBridge::MetadataSnapshot const& snapshot)
    {
      auto metadata = MetadataVariantMap{};

      if (snapshot.trackObjectPath.empty())
      {
        return Glib::Variant<MetadataVariantMap>::create(metadata);
      }

      metadata.emplace(
        "mpris:trackid",
        Glib::Variant<Glib::DBusObjectPathString>::create(Glib::DBusObjectPathString{snapshot.trackObjectPath}));

      if (!snapshot.title.empty())
      {
        metadata.emplace("xesam:title", Glib::Variant<Glib::ustring>::create(toUString(snapshot.title)));
      }

      if (!snapshot.artist.empty())
      {
        metadata.emplace("xesam:artist", stringArray(snapshot.artist));
      }

      if (!snapshot.album.empty())
      {
        metadata.emplace("xesam:album", Glib::Variant<Glib::ustring>::create(toUString(snapshot.album)));
      }

      if (!snapshot.artUrl.empty())
      {
        metadata.emplace("mpris:artUrl", Glib::Variant<Glib::ustring>::create(toUString(snapshot.artUrl)));
      }

      if (snapshot.lengthUs > 0)
      {
        metadata.emplace("mpris:length", Glib::Variant<std::int64_t>::create(snapshot.lengthUs));
      }

      return Glib::Variant<MetadataVariantMap>::create(metadata);
    }
  } // namespace

  struct MprisBridge::Impl final
  {
    rt::PlaybackService& playback;
    uimodel::PlaybackCommandSurface& commands;
    Callbacks callbacks;
    MprisPlaybackEndpoint endpoint;
    Glib::RefPtr<Gio::DBus::Connection> connectionPtr{};
    Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoPtr{};
    utility::ScopedRegistration ownerRegistration{};
    utility::ScopedRegistration rootObjectRegistration{};
    utility::ScopedRegistration playerObjectRegistration{};
    bool nameAcquired = false;
    std::vector<async::Subscription> subscriptions{};
    rt::PlaybackSnapshot lastSnapshot{};
    utility::ScopedRegistration artRequest{};
    ResourceId artResourceId = kInvalidResourceId;
    std::string artUrl{};
    std::uint64_t artGeneration = 0;

    Impl(rt::PlaybackService& playbackRef, uimodel::PlaybackCommandSurface& commandsRef, Callbacks callbacksIn)
      : playback{playbackRef}
      , commands{commandsRef}
      , callbacks{std::move(callbacksIn)}
      , endpoint{playback, commands, callbacks}
      , lastSnapshot{playback.snapshot()}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { stop(); }

    void start()
    {
      if (ownerRegistration)
      {
        return;
      }

      subscribePlayback();
      auto const ownerId = Gio::DBus::own_name(
        Gio::DBus::BusType::SESSION,
        kBusName,
        [this](Glib::RefPtr<Gio::DBus::Connection> const& busConnectionPtr, Glib::ustring const& name)
        { handleBusAcquired(busConnectionPtr, name); },
        [this](Glib::RefPtr<Gio::DBus::Connection> const& busConnectionPtr, Glib::ustring const& name)
        { handleNameAcquired(busConnectionPtr, name); },
        [this](Glib::RefPtr<Gio::DBus::Connection> const& busConnectionPtr, Glib::ustring const& name)
        { handleNameLost(busConnectionPtr, name); },
        Gio::DBus::BusNameOwnerFlags::NONE);

      if (ownerId == 0)
      {
        clearArt();
        subscriptions.clear();
        APP_LOG_WARN("MPRIS disabled: failed to request D-Bus name ownership");
        return;
      }

      ownerRegistration = utility::ScopedRegistration{[ownerId] { Gio::DBus::unown_name(ownerId); }};
    }

    void stop()
    {
      clearArt();
      subscriptions.clear();
      releaseBusState();
    }

    void clearArt()
    {
      artRequest.reset();
      ++artGeneration;
      artResourceId = kInvalidResourceId;
      artUrl.clear();
    }

    void subscribePlayback()
    {
      subscriptions.push_back(commands.onAvailabilityChanged(
        [this] { emitPlayerPropertiesChanged({"CanPlay", "CanPause", "CanGoNext", "CanGoPrevious"}); }));
      lastSnapshot = playback.snapshot();
      refreshArt(lastSnapshot.transport);
      subscriptions.push_back(playback.events().onSnapshot(
        [this](rt::PlaybackSnapshot const& snapshot)
        {
          if (snapshot.transport.transport != lastSnapshot.transport.transport)
          {
            emitPlayerPropertiesChanged({"PlaybackStatus"});
          }

          if (snapshot.transport.nowPlaying != lastSnapshot.transport.nowPlaying ||
              snapshot.transport.duration != lastSnapshot.transport.duration)
          {
            refreshArt(snapshot.transport);
            emitPlayerPropertiesChanged({"Metadata", "CanSeek"});
          }

          if (MprisBridge::shouldEmitSeeked(lastSnapshot.transport, snapshot.transport))
          {
            emitSeeked(snapshot.transport.elapsed);
          }

          if (snapshot.transport.volume != lastSnapshot.transport.volume)
          {
            emitPlayerPropertiesChanged({"Volume"});
          }

          if (snapshot.succession.shuffle != lastSnapshot.succession.shuffle)
          {
            emitPlayerPropertiesChanged({"Shuffle"});
          }

          if (snapshot.succession.repeat != lastSnapshot.succession.repeat)
          {
            emitPlayerPropertiesChanged({"LoopStatus"});
          }

          lastSnapshot = snapshot;
        }));
    }

    bool dispatchPlayerMethod(std::string_view const methodName) const
    {
      return endpoint.dispatchPlayerMethod(methodName);
    }

    bool dispatchRootMethod(std::string_view const methodName) const { return endpoint.dispatchRootMethod(methodName); }

    bool dispatchSeek(std::int64_t const offsetUs) { return endpoint.dispatchSeek(offsetUs); }

    bool dispatchSetPosition(std::string_view const requestedTrackObjectPath, std::int64_t const positionUs)
    {
      return endpoint.dispatchSetPosition(requestedTrackObjectPath, positionUs);
    }

    bool dispatchSetRate(double const rate) const { return endpoint.dispatchSetRate(rate); }

    bool dispatchSetVolume(double const volume) { return endpoint.dispatchSetVolume(volume); }

    bool dispatchSetShuffle(bool const shuffle) { return endpoint.dispatchSetShuffle(shuffle); }

    bool dispatchSetLoopStatus(std::string_view const loopStatus) { return endpoint.dispatchSetLoopStatus(loopStatus); }

    std::optional<bool> playerCapabilityProperty(std::string_view const propertyName) const
    {
      return endpoint.playerCapabilityProperty(propertyName);
    }

    std::string artUrlForState(rt::PlaybackTransportSnapshot const& state) const
    {
      return state.nowPlaying.coverArtId == artResourceId ? artUrl : std::string{};
    }

    void refreshArt(rt::PlaybackTransportSnapshot const& state)
    {
      auto const resourceId = state.nowPlaying.coverArtId;

      if (resourceId == artResourceId)
      {
        return;
      }

      artRequest.reset();
      ++artGeneration;
      artResourceId = resourceId;
      artUrl.clear();

      if (resourceId == kInvalidResourceId || !callbacks.requestArtUrl)
      {
        return;
      }

      auto const generation = artGeneration;

      try
      {
        artRequest = callbacks.requestArtUrl(resourceId,
                                             [this, resourceId, generation](std::string resolvedUrl)
                                             {
                                               if (artResourceId != resourceId || artGeneration != generation)
                                               {
                                                 return;
                                               }

                                               artUrl = std::move(resolvedUrl);
                                               artRequest.reset();
                                               emitPlayerPropertiesChanged({"Metadata"});
                                             });
      }
      catch (std::exception const& e)
      {
        APP_LOG_WARN("MPRIS art URL request failed for resource {}: {}", resourceId.raw(), e.what());
      }
      catch (...)
      {
        APP_LOG_WARN("MPRIS art URL request failed for resource {}: unknown exception", resourceId.raw());
      }
    }

    void emitPlayerPropertiesChanged(std::initializer_list<std::string_view> propertyNames) const
    {
      if (!connectionPtr || !playerObjectRegistration)
      {
        return;
      }

      auto changed = MetadataVariantMap{};

      for (auto const propertyName : propertyNames)
      {
        if (auto const property = playerProperty(propertyName); property)
        {
          changed.emplace(toUString(propertyName), property);
        }
      }

      if (changed.empty())
      {
        return;
      }

      auto const propertiesChangedPayload = Glib::Variant<PropertiesChangedPayload>::create(
        PropertiesChangedPayload{Glib::ustring{kPlayerInterface}, changed, std::vector<Glib::ustring>{}});

      try
      {
        connectionPtr->emit_signal(
          kObjectPath, kPropertiesInterface, "PropertiesChanged", {}, propertiesChangedPayload);
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_WARN("MPRIS failed to emit PropertiesChanged: {}", e.what());
      }
    }

    void emitSeeked(std::chrono::milliseconds const elapsed) const
    {
      if (!connectionPtr || !playerObjectRegistration)
      {
        return;
      }

      auto const seekedPayload =
        Glib::Variant<SeekedPayload>::create(SeekedPayload{MprisBridge::microsecondsFromMilliseconds(elapsed)});

      try
      {
        connectionPtr->emit_signal(kObjectPath, kPlayerInterface, "Seeked", {}, seekedPayload);
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_WARN("MPRIS failed to emit Seeked: {}", e.what());
      }
    }

    Glib::VariantBase playerProperty(std::string_view const propertyName) const
    {
      auto const& snapshot = playback.snapshot();
      auto const& state = snapshot.transport;

      if (propertyName == "PlaybackStatus")
      {
        return Glib::Variant<Glib::ustring>::create(toUString(MprisBridge::playbackStatus(state.transport)));
      }

      if (propertyName == "LoopStatus")
      {
        return Glib::Variant<Glib::ustring>::create(toUString(MprisBridge::loopStatus(snapshot.succession.repeat)));
      }

      if (propertyName == "Rate" || propertyName == "MinimumRate" || propertyName == "MaximumRate")
      {
        return Glib::Variant<double>::create(1.0);
      }

      if (propertyName == "Volume")
      {
        return Glib::Variant<double>::create(static_cast<double>(state.volume.level));
      }

      if (propertyName == "Shuffle")
      {
        return Glib::Variant<bool>::create(snapshot.succession.shuffle == rt::ShuffleMode::On);
      }

      if (propertyName == "CanSeek")
      {
        return Glib::Variant<bool>::create(state.nowPlaying.trackId != kInvalidTrackId);
      }

      if (propertyName == "Metadata")
      {
        return metadataVariant(MprisBridge::metadataForState(state, artUrlForState(state)));
      }

      if (propertyName == "Position")
      {
        return Glib::Variant<std::int64_t>::create(MprisBridge::microsecondsFromMilliseconds(state.elapsed));
      }

      if (auto const optCapability = playerCapabilityProperty(propertyName); optCapability)
      {
        return Glib::Variant<bool>::create(*optCapability);
      }

      return {};
    }

    Glib::VariantBase rootProperty(std::string_view const propertyName) const
    {
      if (propertyName == "CanQuit")
      {
        return Glib::Variant<bool>::create(static_cast<bool>(callbacks.quit));
      }

      if (propertyName == "CanRaise")
      {
        return Glib::Variant<bool>::create(static_cast<bool>(callbacks.raise));
      }

      if (propertyName == "Fullscreen" || propertyName == "CanSetFullscreen" || propertyName == "HasTrackList")
      {
        return Glib::Variant<bool>::create(false);
      }

      if (propertyName == "Identity")
      {
        return Glib::Variant<Glib::ustring>::create("Aobus");
      }

      if (propertyName == "DesktopEntry")
      {
        return Glib::Variant<Glib::ustring>::create("aobus");
      }

      if (propertyName == "SupportedUriSchemes")
      {
        return emptyStringArray();
      }

      if (propertyName == "SupportedMimeTypes")
      {
        return emptyStringArray();
      }

      return {};
    }

    void registerObjects(Glib::RefPtr<Gio::DBus::Connection> const& busConnectionPtr)
    {
      auto const rootInfoPtr = nodeInfoPtr->lookup_interface(kRootInterface);
      auto const playerInfoPtr = nodeInfoPtr->lookup_interface(kPlayerInterface);

      if (!rootInfoPtr || !playerInfoPtr)
      {
        APP_LOG_WARN("MPRIS disabled: introspection data is missing required interfaces");
        return;
      }

      try
      {
        auto const rootRegistrationId = busConnectionPtr->register_object(
          kObjectPath,
          rootInfoPtr,
          [this](Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                 Glib::ustring const& sender,
                 Glib::ustring const& objectPath,
                 Glib::ustring const& interfaceName,
                 Glib::ustring const& methodName,
                 Glib::VariantContainerBase const& parameters,
                 Glib::RefPtr<Gio::DBus::MethodInvocation> const& invocationPtr)
          {
            handleRootMethodCall(
              connectionPtr, sender, objectPath, interfaceName, methodName, parameters, invocationPtr);
          },
          [this](Glib::VariantBase& property,
                 Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                 Glib::ustring const& sender,
                 Glib::ustring const& objectPath,
                 Glib::ustring const& interfaceName,
                 Glib::ustring const& propertyName)
          { handleRootGetProperty(property, connectionPtr, sender, objectPath, interfaceName, propertyName); });

        rootObjectRegistration =
          utility::ScopedRegistration{[busConnectionPtr, rootRegistrationId]
                                      { std::ignore = busConnectionPtr->unregister_object(rootRegistrationId); }};
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_WARN("MPRIS root object registration failed: {}", e.what());
        return;
      }

      try
      {
        auto const playerRegistrationId = busConnectionPtr->register_object(
          kObjectPath,
          playerInfoPtr,
          [this](Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                 Glib::ustring const& sender,
                 Glib::ustring const& objectPath,
                 Glib::ustring const& interfaceName,
                 Glib::ustring const& methodName,
                 Glib::VariantContainerBase const& parameters,
                 Glib::RefPtr<Gio::DBus::MethodInvocation> const& invocationPtr)
          {
            handlePlayerMethodCall(
              connectionPtr, sender, objectPath, interfaceName, methodName, parameters, invocationPtr);
          },
          [this](Glib::VariantBase& property,
                 Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                 Glib::ustring const& sender,
                 Glib::ustring const& objectPath,
                 Glib::ustring const& interfaceName,
                 Glib::ustring const& propertyName)
          { handlePlayerGetProperty(property, connectionPtr, sender, objectPath, interfaceName, propertyName); },
          [this](Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                 Glib::ustring const& sender,
                 Glib::ustring const& objectPath,
                 Glib::ustring const& interfaceName,
                 Glib::ustring const& propertyName,
                 Glib::VariantBase const& value)
          { return handlePlayerSetProperty(connectionPtr, sender, objectPath, interfaceName, propertyName, value); });

        playerObjectRegistration =
          utility::ScopedRegistration{[busConnectionPtr, playerRegistrationId]
                                      { std::ignore = busConnectionPtr->unregister_object(playerRegistrationId); }};
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_WARN("MPRIS player object registration failed: {}", e.what());
        rootObjectRegistration.reset();
      }
    }

    void releaseBusState()
    {
      playerObjectRegistration.reset();
      rootObjectRegistration.reset();
      ownerRegistration.reset();
      connectionPtr.reset();
      nodeInfoPtr.reset();
      nameAcquired = false;
    }

    void handleBusAcquired(Glib::RefPtr<Gio::DBus::Connection> const& busConnectionPtr, Glib::ustring const& /*name*/)
    {
      if (!busConnectionPtr)
      {
        APP_LOG_WARN("MPRIS disabled: failed to connect to the session bus");
        return;
      }

      connectionPtr = busConnectionPtr;

      if (!nodeInfoPtr)
      {
        try
        {
          nodeInfoPtr = Gio::DBus::NodeInfo::create_for_xml(kMprisIntrospectionXml);
        }
        catch (Glib::Error const& e)
        {
          APP_LOG_WARN("MPRIS disabled: failed to parse introspection XML: {}", e.what());
          return;
        }
      }

      registerObjects(busConnectionPtr);
    }

    void handleNameAcquired(Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/, Glib::ustring const& name)
    {
      if (!rootObjectRegistration || !playerObjectRegistration)
      {
        APP_LOG_WARN("MPRIS disabled: D-Bus name {} acquired without registered objects", name.raw());
        clearArt();
        subscriptions.clear();
        releaseBusState();
        return;
      }

      nameAcquired = true;
      APP_LOG_INFO("MPRIS name acquired: {}", name.raw());
    }

    void handleNameLost(Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/, Glib::ustring const& name)
    {
      clearArt();
      subscriptions.clear();
      releaseBusState();
      APP_LOG_WARN("MPRIS disabled: failed to acquire D-Bus name {}", name.raw());
    }

    void handleRootMethodCall(Glib::RefPtr<Gio::DBus::Connection> const& /*connectionPtr*/,
                              Glib::ustring const& /*sender*/,
                              Glib::ustring const& /*objectPath*/,
                              Glib::ustring const& /*interfaceName*/,
                              Glib::ustring const& methodName,
                              Glib::VariantContainerBase const& /*parameters*/,
                              Glib::RefPtr<Gio::DBus::MethodInvocation> const& invocationPtr) const
    {
      if (dispatchRootMethod(methodName.raw()))
      {
        invocationPtr->return_value({});
        return;
      }

      invocationPtr->return_dbus_error(kMprisError, "Unsupported MPRIS root method");
    }

    void handleRootGetProperty(Glib::VariantBase& property,
                               Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/,
                               Glib::ustring const& /*sender*/,
                               Glib::ustring const& /*objectPath*/,
                               Glib::ustring const& /*interfaceName*/,
                               Glib::ustring const& propertyName) const
    {
      property = rootProperty(propertyName.raw());
    }

    void handlePlayerMethodCall(Glib::RefPtr<Gio::DBus::Connection> const& /*connectionPtr*/,
                                Glib::ustring const& /*sender*/,
                                Glib::ustring const& /*objectPath*/,
                                Glib::ustring const& /*interfaceName*/,
                                Glib::ustring const& methodName,
                                Glib::VariantContainerBase const& parameters,
                                Glib::RefPtr<Gio::DBus::MethodInvocation> const& invocationPtr)
    {
      if (methodName == "Seek")
      {
        auto offsetUsVariant = Glib::Variant<std::int64_t>{};
        parameters.get_child(offsetUsVariant, 0);

        if (auto const offsetUs = offsetUsVariant.get(); dispatchSeek(offsetUs))
        {
          invocationPtr->return_value({});
          return;
        }

        invocationPtr->return_dbus_error(kMprisError, "No active track to seek");
        return;
      }

      if (methodName == "SetPosition")
      {
        auto requestedTrackObjectPathVariant = Glib::Variant<Glib::DBusObjectPathString>{};
        auto positionUsVariant = Glib::Variant<std::int64_t>{};
        parameters.get_child(requestedTrackObjectPathVariant, 0);
        parameters.get_child(positionUsVariant, 1);
        auto const requestedTrackObjectPath = requestedTrackObjectPathVariant.get();

        if (auto const positionUs = positionUsVariant.get();
            dispatchSetPosition(requestedTrackObjectPath.raw(), positionUs))
        {
          invocationPtr->return_value({});
          return;
        }

        invocationPtr->return_dbus_error(kMprisError, "No active track to seek");
        return;
      }

      if (dispatchPlayerMethod(methodName.raw()))
      {
        invocationPtr->return_value({});
        return;
      }

      invocationPtr->return_dbus_error(kMprisError, "Unsupported MPRIS player method");
    }

    void handlePlayerGetProperty(Glib::VariantBase& property,
                                 Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/,
                                 Glib::ustring const& /*sender*/,
                                 Glib::ustring const& /*objectPath*/,
                                 Glib::ustring const& /*interfaceName*/,
                                 Glib::ustring const& propertyName) const
    {
      property = playerProperty(propertyName.raw());
    }

    bool handlePlayerSetProperty(Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/,
                                 Glib::ustring const& /*sender*/,
                                 Glib::ustring const& /*objectPath*/,
                                 Glib::ustring const& /*interfaceName*/,
                                 Glib::ustring const& propertyName,
                                 Glib::VariantBase const& value)
    {
      if (propertyName == "Volume")
      {
        dispatchSetVolume(value.get_dynamic<double>());
        return true;
      }

      if (propertyName == "Rate")
      {
        if (dispatchSetRate(value.get_dynamic<double>()))
        {
          return true;
        }

        throwGioError(Gio::Error::INVALID_ARGUMENT, "Invalid MPRIS playback rate");
      }

      if (propertyName == "Shuffle")
      {
        dispatchSetShuffle(value.get_dynamic<bool>());
        return true;
      }

      if (propertyName == "LoopStatus")
      {
        if (auto const loopStatus = value.get_dynamic<Glib::ustring>(); dispatchSetLoopStatus(loopStatus.raw()))
        {
          return true;
        }

        throwGioError(Gio::Error::INVALID_ARGUMENT, "Unsupported MPRIS loop status");
      }

      throwGioError(Gio::Error::NOT_SUPPORTED, "Unsupported MPRIS player property");
    }
  };

  MprisBridge::MprisBridge(rt::PlaybackService& playback,
                           uimodel::PlaybackCommandSurface& commands,
                           Callbacks callbacks)
    : _implPtr{std::make_unique<Impl>(playback, commands, std::move(callbacks))}
  {
  }

  MprisBridge::~MprisBridge() = default;

  void MprisBridge::start()
  {
    _implPtr->start();
  }

  bool MprisBridge::isActive() const noexcept
  {
    return _implPtr->nameAcquired;
  }

  MprisBridge::MetadataSnapshot MprisBridge::metadataSnapshot() const
  {
    auto const& state = _implPtr->playback.snapshot().transport;
    return metadataForState(state, _implPtr->artUrlForState(state));
  }

  std::string_view MprisBridge::playbackStatus(audio::Transport const transport) noexcept
  {
    switch (transport)
    {
      case audio::Transport::Opening:
      case audio::Transport::Buffering:
      case audio::Transport::Seeking:
      case audio::Transport::Playing: return "Playing";
      case audio::Transport::Paused: return "Paused";
      case audio::Transport::Idle:
      case audio::Transport::Stopping:
      case audio::Transport::Error: return "Stopped";
    }

    return "Stopped";
  }

  std::string_view MprisBridge::loopStatus(rt::RepeatMode const mode) noexcept
  {
    switch (mode)
    {
      case rt::RepeatMode::Off: return "None";
      case rt::RepeatMode::One: return "Track";
      case rt::RepeatMode::All: return "Playlist";
    }

    return "None";
  }

  std::optional<rt::RepeatMode> MprisBridge::repeatModeForLoopStatus(std::string_view const loopStatus) noexcept
  {
    if (loopStatus == "None")
    {
      return rt::RepeatMode::Off;
    }

    if (loopStatus == "Track")
    {
      return rt::RepeatMode::One;
    }

    if (loopStatus == "Playlist")
    {
      return rt::RepeatMode::All;
    }

    return std::nullopt;
  }

  std::int64_t MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds const duration) noexcept
  {
    auto const count = duration.count();
    constexpr std::int64_t kScale = 1000;

    if (count > std::numeric_limits<std::int64_t>::max() / kScale)
    {
      return std::numeric_limits<std::int64_t>::max();
    }

    if (count < std::numeric_limits<std::int64_t>::min() / kScale)
    {
      return std::numeric_limits<std::int64_t>::min();
    }

    return static_cast<std::int64_t>(count) * kScale;
  }

  std::chrono::milliseconds MprisBridge::fromMprisMicroseconds(std::int64_t const value) noexcept
  {
    return std::chrono::milliseconds{value / 1000};
  }

  std::chrono::milliseconds MprisBridge::clampElapsed(rt::PlaybackTransportSnapshot const& state,
                                                      std::chrono::milliseconds const elapsed) noexcept
  {
    if (elapsed < std::chrono::milliseconds{0})
    {
      return std::chrono::milliseconds{0};
    }

    if (state.duration > std::chrono::milliseconds{0} && elapsed > state.duration)
    {
      return state.duration;
    }

    return elapsed;
  }

  std::chrono::milliseconds MprisBridge::seekTargetElapsed(rt::PlaybackTransportSnapshot const& state,
                                                           std::int64_t const offsetUs) noexcept
  {
    auto const offset = fromMprisMicroseconds(offsetUs).count();
    auto const elapsed = state.elapsed.count();
    std::int64_t target = 0;

    if (offset > 0 && elapsed > std::numeric_limits<std::int64_t>::max() - offset)
    {
      target = std::numeric_limits<std::int64_t>::max();
    }
    else if (offset < 0 && elapsed < std::numeric_limits<std::int64_t>::min() - offset)
    {
      target = std::numeric_limits<std::int64_t>::min();
    }
    else
    {
      target = static_cast<std::int64_t>(elapsed) + offset;
    }

    return clampElapsed(state, std::chrono::milliseconds{target});
  }

  bool MprisBridge::shouldEmitSeeked(rt::PlaybackTransportSnapshot const& before,
                                     rt::PlaybackTransportSnapshot const& after) noexcept
  {
    return after.finalSeekRevision != before.finalSeekRevision;
  }

  std::string MprisBridge::trackObjectPath(TrackId const trackId)
  {
    if (trackId == kInvalidTrackId)
    {
      return {};
    }

    return std::string{kTrackObjectPathPrefix} + std::to_string(trackId.raw());
  }

  MprisBridge::MetadataSnapshot MprisBridge::metadataForState(rt::PlaybackTransportSnapshot const& state,
                                                              std::string artUrl)
  {
    if (state.nowPlaying.trackId == kInvalidTrackId)
    {
      return {};
    }

    return MetadataSnapshot{.trackObjectPath = trackObjectPath(state.nowPlaying.trackId),
                            .title = state.nowPlaying.title,
                            .artist = state.nowPlaying.artist,
                            .album = state.nowPlaying.album,
                            .artUrl = std::move(artUrl),
                            .lengthUs = microsecondsFromMilliseconds(state.duration)};
  }
} // namespace ao::gtk::platform
