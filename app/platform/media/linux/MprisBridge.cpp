// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisBridge.h"

#include "MprisArtUrlSession.h"
#include "MprisBusSession.h"
#include "MprisPlaybackEndpoint.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/utility/Raii.h>

#include <gio/gio.h>
#include <giomm/dbusconnection.h>
#include <giomm/dbusintrospection.h>
#include <giomm/init.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <glibmm/variantdbusstring.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::media
{
  namespace
  {
    constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
    constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
    constexpr auto kMprisError = "org.mpris.MediaPlayer2.Error.NotSupported";
    constexpr auto kTrackObjectPathPrefix = "/org/mpris/MediaPlayer2/Track/";

    constexpr auto kMprisIntrospectionXml = R"xml(
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
    {}
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
    <method name="Seek"><arg name="Offset" type="x" direction="in"/></method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/>
      <arg name="Position" type="x" direction="in"/>
    </method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{{sv}}" access="read"/>
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

    MprisBridge::PlaybackSource playbackSourceFor(rt::PlaybackService& playback)
    {
      return {
        .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
        .onSnapshot = [&playback](rt::PlaybackSnapshotObserver observer)
        { return playback.events().onSnapshot(std::move(observer)); },
        .elapsed = [&playback] { return playback.elapsed(); },
      };
    }

    Glib::ustring toUString(std::string_view value)
    {
      return Glib::ustring{value.begin(), value.end()};
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
        metadata.emplace(
          "xesam:artist", Glib::Variant<std::vector<Glib::ustring>>::create(std::vector{toUString(snapshot.artist)}));
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

    std::string sessionBusAddress(MprisBridgeOptions const& options)
    {
      if (options.optBusAddress)
      {
        return *options.optBusAddress;
      }

      if (auto const* const address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); address != nullptr)
      {
        return address;
      }

      auto const* const runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");

      if (runtimeDirectory != nullptr && runtimeDirectory[0] == '/')
      {
        auto const path = std::string{runtimeDirectory} + "/bus";
        using FileStatus = struct ::stat;
        // Match GIO's existing runtime-bus ownership/socket checks, without its
        // runtime-directory defaults or autolaunch fallback. Any stat failure
        // leaves the optional adapter off; filesystem latency is not bounded.
        if (auto status = FileStatus{};
            ::stat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() && S_ISSOCK(status.st_mode))
        {
          auto const escapedPathPtr = utility::makeUniquePtr<::g_free>(::g_dbus_address_escape_value(path.c_str()));
          return std::string{"unix:path="} + escapedPathPtr.get();
        }
      }

      APP_LOG_INFO("MPRIS disabled: no existing implicit session-bus candidate");
      return {};
    }

    void replyError(MprisBusSession::Invocation invocationPtr, char const* name, char const* message)
    {
      ::g_dbus_method_invocation_return_dbus_error(invocationPtr.release(), name, message);
    }
  } // namespace

  struct MprisBridge::Impl final
  {
    struct CallbackState final
    {
      // Only the callback executor reads or revokes this borrow. Native code
      // only copies the weak handle into deferred, independently owned work.
      Impl* owner;
    };

    async::Executor& executor;
    uimodel::PlaybackActions& actions;
    Callbacks callbacks;
    PlaybackSource playbackSource;
    MprisBridgeOptions options;
    MprisPlaybackEndpoint endpoint;
    Glib::RefPtr<Gio::DBus::Connection> connectionPtr;
    Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoPtr;
    std::shared_ptr<CallbackState> callbackStatePtr;
    std::unique_ptr<MprisBusSession> busSessionPtr;
    std::string activeBusName;
    bool started = false;
    bool retired = false;
    std::vector<async::Subscription> subscriptions;
    rt::PlaybackSnapshot lastSnapshot;
    MprisArtUrlSession artUrlSession;

    Impl(async::Executor& executorRef,
         rt::PlaybackService& playback,
         uimodel::PlaybackActions& actionsRef,
         Callbacks callbacksIn,
         PlaybackSource playbackSourceIn,
         MprisBridgeOptions optionsIn)
      : executor{executorRef}
      , actions{actionsRef}
      , callbacks{std::move(callbacksIn)}
      , playbackSource{std::move(playbackSourceIn)}
      , options{std::move(optionsIn)}
      , endpoint{playback, actions, callbacks}
      , callbackStatePtr{std::make_shared<CallbackState>(CallbackState{.owner = this})}
      , lastSnapshot{playbackSource.snapshot()}
      , artUrlSession{callbacks.requestArtUrl, [this] { emitPlayerPropertiesChanged({"Metadata"}); }}
    {
      AO_EXPECTS(executor.isCurrent(), "MPRIS construction requires the playback callback executor");
      Gio::init();
    }

    ~Impl()
    {
      retire();
      // Native producers are joined before any borrowed callback target or
      // captured artwork requester is released. Queued requests are inert.
      busSessionPtr.reset();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start()
    {
      AO_EXPECTS(executor.isCurrent(), "MPRIS start requires the playback callback executor");

      if (started || retired)
      {
        return;
      }

      started = true;
      auto address = sessionBusAddress(options);

      if (address.empty())
      {
        retire();
        return;
      }

      // Explicit-address connection creation also understands autolaunch
      // transports, including fallback entries. Never start a session bus.
      if (address.starts_with("autolaunch:") || address.contains(";autolaunch:"))
      {
        disable("Session-bus autolaunch is not supported");
        return;
      }

      try
      {
        nodeInfoPtr = Gio::DBus::NodeInfo::create_for_xml(std::format(
          kMprisIntrospectionXml,
          options.desktopEntry.empty() ? "" : R"xml(<property name="DesktopEntry" type="s" access="read"/>)xml"));
        auto const weakStatePtr = std::weak_ptr{callbackStatePtr};
        busSessionPtr = std::make_unique<MprisBusSession>(
          executor,
          std::move(address),
          options.busName,
          options.uniqueInstance,
          nodeInfoPtr,
          MprisBusSession::Outputs{
            .handleRequest =
              [weakStatePtr](MprisBusSession::Invocation invocationPtr)
            {
              if (auto const statePtr = weakStatePtr.lock(); statePtr && statePtr->owner != nullptr)
              {
                statePtr->owner->handleRequest(std::move(invocationPtr));
              }
            },
            .acquired =
              [weakStatePtr](Glib::RefPtr<Gio::DBus::Connection> connectionPtr, std::string name)
            {
              if (auto const statePtr = weakStatePtr.lock(); statePtr && statePtr->owner != nullptr)
              {
                auto& owner = *statePtr->owner;
                owner.connectionPtr = std::move(connectionPtr);
                owner.activeBusName = std::move(name);
                APP_LOG_INFO("MPRIS name acquired: {}", owner.activeBusName);

                try
                {
                  owner.subscribePlayback();
                }
                catch (...)
                {
                  owner.retire();
                  owner.busSessionPtr.reset();
                  throw;
                }
              }
            },
            .unavailable =
              [weakStatePtr](std::string message)
            {
              if (auto const statePtr = weakStatePtr.lock(); statePtr && statePtr->owner != nullptr)
              {
                statePtr->owner->disable(message);
              }
            },
          });
      }
      catch (Glib::Error const& error)
      {
        disable(error.what());
      }
      catch (std::system_error const& error)
      {
        // A native thread cannot be started; the optional adapter degrades.
        disable(error.what());
      }
    }

    void retire()
    {
      AO_EXPECTS(executor.isCurrent(), "MPRIS retirement requires the playback callback executor");

      if (std::exchange(retired, true))
      {
        return;
      }

      callbackStatePtr->owner = nullptr;
      callbackStatePtr.reset();
      activeBusName.clear();
      connectionPtr.reset();
      subscriptions.clear();
      artUrlSession.clear();

      if (busSessionPtr)
      {
        busSessionPtr->retire();
      }
    }

    void disable(std::string_view message)
    {
      if (!retired)
      {
        APP_LOG_WARN("MPRIS disabled: {}", message);
        retire();
      }
    }

    void subscribePlayback()
    {
      subscriptions.push_back(actions.onAvailabilityChanged(
        [this] { emitPlayerPropertiesChanged({"CanPlay", "CanPause", "CanGoNext", "CanGoPrevious"}); }));
      lastSnapshot = playbackSource.snapshot();
      subscriptions.push_back(playbackSource.onSnapshot(
        [this](rt::PlaybackSnapshot const& snapshot)
        {
          if (retired)
          {
            return;
          }

          if (snapshot.transport.transport != lastSnapshot.transport.transport)
          {
            emitPlayerPropertiesChanged({"PlaybackStatus"});
          }

          if (!retired && (snapshot.transport.nowPlaying != lastSnapshot.transport.nowPlaying ||
                           snapshot.transport.duration != lastSnapshot.transport.duration))
          {
            artUrlSession.refresh(snapshot.transport.nowPlaying.coverArtId);
          }

          if (MprisBridge::shouldEmitMetadataChanged(lastSnapshot.transport, snapshot.transport))
          {
            emitPlayerPropertiesChanged({"Metadata", "CanSeek"});
          }

          if (MprisBridge::shouldEmitSeeked(lastSnapshot.transport, snapshot.transport))
          {
            auto const payload = Glib::Variant<std::tuple<std::int64_t>>::create(
              std::tuple{MprisBridge::microsecondsFromMilliseconds(snapshot.transport.elapsed)});
            emitSignal(kPlayerInterface, "Seeked", payload);
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
      // Synchronous cover completion may retire the bridge on a failed signal;
      // all subscriptions must already exist so retirement cannot resurrect one.
      artUrlSession.refresh(lastSnapshot.transport.nowPlaying.coverArtId);
    }

    void emitPlayerPropertiesChanged(std::initializer_list<std::string_view> propertyNames)
    {
      if (!connectionPtr || retired)
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

      if (!changed.empty())
      {
        auto const payload = Glib::Variant<PropertiesChangedPayload>::create(
          PropertiesChangedPayload{Glib::ustring{kPlayerInterface}, std::move(changed), {}});
        emitSignal(kPropertiesInterface, "PropertiesChanged", payload);
      }
    }

    void emitSignal(char const* interfaceName, char const* signalName, Glib::VariantBase payload)
    {
      if (!connectionPtr || retired)
      {
        return;
      }

      ::GError* error = nullptr;
      auto const emitted = ::g_dbus_connection_emit_signal(
        connectionPtr->gobj(), nullptr, kObjectPath, interfaceName, signalName, payload.gobj(), &error);
      auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);

      if (emitted == 0)
      {
        disable(error != nullptr ? error->message : "Could not emit MPRIS signal");
      }
    }

    Glib::VariantBase playerProperty(std::string_view propertyName) const
    {
      auto const& snapshot = playbackSource.snapshot();
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

      if (propertyName == "Metadata")
      {
        return metadataVariant(MprisBridge::metadataForState(state, artUrlSession.urlFor(state.nowPlaying.coverArtId)));
      }

      if (propertyName == "Position")
      {
        return Glib::Variant<std::int64_t>::create(MprisBridge::microsecondsFromMilliseconds(playbackSource.elapsed()));
      }

      if (auto const optCapability = endpoint.playerCapabilityProperty(propertyName, state); optCapability)
      {
        return Glib::Variant<bool>::create(*optCapability);
      }

      return {};
    }

    Glib::VariantBase rootProperty(std::string_view propertyName) const
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
        return Glib::Variant<Glib::ustring>::create(toUString(options.identity));
      }

      if (propertyName == "DesktopEntry" && !options.desktopEntry.empty())
      {
        return Glib::Variant<Glib::ustring>::create(toUString(options.desktopEntry));
      }

      if (propertyName == "SupportedUriSchemes" || propertyName == "SupportedMimeTypes")
      {
        return Glib::Variant<std::vector<Glib::ustring>>::create({});
      }

      return {};
    }

    Glib::VariantBase property(std::string_view interfaceName, std::string_view propertyName) const
    {
      return interfaceName == kRootInterface ? rootProperty(propertyName) : playerProperty(propertyName);
    }

    void handleProperties(MprisBusSession::Invocation invocationPtr, std::string_view methodName)
    {
      auto* const parameters = ::g_dbus_method_invocation_get_parameters(invocationPtr.get());
      auto const* signature = "(ssv)";

      if (methodName == "Get")
      {
        signature = "(ss)";
      }
      else if (methodName == "GetAll")
      {
        signature = "(s)";
      }

      if (::g_variant_is_of_type(parameters, G_VARIANT_TYPE(signature)) == 0)
      {
        replyError(
          std::move(invocationPtr), "org.freedesktop.DBus.Error.InvalidArgs", "Incorrect Properties arguments");
        return;
      }

      auto const interfaceVariant = Glib::VariantContainerBase{parameters, true}.get_child(0);
      auto const interfaceName = interfaceVariant.get_dynamic<Glib::ustring>();
      auto* const interface = ::g_dbus_node_info_lookup_interface(nodeInfoPtr->gobj(), interfaceName.c_str());

      if (interface == nullptr)
      {
        replyError(std::move(invocationPtr), "org.freedesktop.DBus.Error.UnknownInterface", "Unknown MPRIS interface");
        return;
      }

      if (methodName == "GetAll")
      {
        auto properties = MetadataVariantMap{};

        for (auto const* const* entry = interface->properties; *entry != nullptr; ++entry)
        {
          if (auto value = property(interfaceName.raw(), (*entry)->name); value)
          {
            properties.emplace((*entry)->name, std::move(value));
          }
        }

        auto payload = Glib::Variant<std::tuple<MetadataVariantMap>>::create(std::tuple{std::move(properties)});
        ::g_dbus_method_invocation_return_value(invocationPtr.release(), payload.gobj());
        return;
      }

      auto const nameVariant = Glib::VariantContainerBase{parameters, true}.get_child(1);
      auto const propertyName = nameVariant.get_dynamic<Glib::ustring>();
      auto* const info = ::g_dbus_interface_info_lookup_property(interface, propertyName.c_str());

      if (info == nullptr)
      {
        replyError(std::move(invocationPtr), "org.freedesktop.DBus.Error.UnknownProperty", "Unknown MPRIS property");
        return;
      }

      if (methodName == "Get")
      {
        auto value = property(interfaceName.raw(), propertyName.raw());

        if (!value)
        {
          replyError(std::move(invocationPtr), "org.freedesktop.DBus.Error.UnknownProperty", "Unmapped MPRIS property");
          return;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- GVariant ABI pairs the literal with a GVariant*.
        ::g_dbus_method_invocation_return_value(invocationPtr.release(), ::g_variant_new("(v)", value.gobj()));
        return;
      }

      if (methodName != "Set" || (info->flags & G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE) == 0)
      {
        replyError(std::move(invocationPtr), "org.freedesktop.DBus.Error.PropertyReadOnly", "Read-only MPRIS property");
        return;
      }

      auto boxedPtr = utility::makeUniquePtr<::g_variant_unref>(::g_variant_get_child_value(parameters, 2));
      auto valuePtr = utility::makeUniquePtr<::g_variant_unref>(::g_variant_get_variant(boxedPtr.get()));

      if (::g_variant_is_of_type(valuePtr.get(), G_VARIANT_TYPE(info->signature)) == 0)
      {
        replyError(std::move(invocationPtr), "org.freedesktop.DBus.Error.InvalidArgs", "Incorrect MPRIS property type");
        return;
      }

      bool accepted = true;

      if (auto const& name = propertyName.raw(); name == "Volume")
      {
        endpoint.dispatchSetVolume(::g_variant_get_double(valuePtr.get()));
      }
      else if (name == "Rate")
      {
        accepted = endpoint.tryDispatchSetRate(::g_variant_get_double(valuePtr.get()));
      }
      else if (name == "Shuffle")
      {
        endpoint.dispatchSetShuffle(::g_variant_get_boolean(valuePtr.get()) != 0);
      }
      else if (name == "LoopStatus")
      {
        accepted = endpoint.tryDispatchSetLoopStatus(::g_variant_get_string(valuePtr.get(), nullptr));
      }
      else
      {
        replyError(std::move(invocationPtr), kMprisError, "Unsupported MPRIS property");
        return;
      }

      if (!accepted)
      {
        ::g_dbus_method_invocation_return_error_literal(
          invocationPtr.release(), G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid MPRIS property value");
        return;
      }

      ::g_dbus_method_invocation_return_value(invocationPtr.release(), nullptr);
    }

    void handleRequest(MprisBusSession::Invocation invocationPtr)
    {
      AO_EXPECTS(executor.isCurrent(), "MPRIS requests require the playback callback executor");
      auto const methodName = std::string_view{::g_dbus_method_invocation_get_method_name(invocationPtr.get())};
      auto const* const requestedInterface = ::g_dbus_method_invocation_get_interface_name(invocationPtr.get());
      // D-Bus permits an omitted interface when the method is unambiguous.
      auto interfaceName = std::string_view{kPlayerInterface};

      if (requestedInterface != nullptr)
      {
        interfaceName = requestedInterface;
      }
      else if (methodName == "Raise" || methodName == "Quit")
      {
        interfaceName = kRootInterface;
      }
      else if (methodName == "Get" || methodName == "GetAll" || methodName == "Set")
      {
        interfaceName = kPropertiesInterface;
      }

      if (interfaceName == kPropertiesInterface)
      {
        handleProperties(std::move(invocationPtr), methodName);
        return;
      }

      if (interfaceName == kRootInterface)
      {
        if (methodName == "Quit" && callbacks.quit)
        {
          // The reply is submitted before a later owner turn may start close.
          // Keep the callable independent of the bridge it may destroy. An
          // expired scope still prevents borrowing a host already retired.
          auto const weakStatePtr = std::weak_ptr{callbackStatePtr};
          executor.defer(
            [weakStatePtr, quit = callbacks.quit]
            {
              if (auto const statePtr = weakStatePtr.lock(); statePtr && statePtr->owner != nullptr)
              {
                quit();
              }
            });
          ::g_dbus_method_invocation_return_value(invocationPtr.release(), nullptr);
          return;
        }

        if (endpoint.tryDispatchRootMethod(methodName))
        {
          ::g_dbus_method_invocation_return_value(invocationPtr.release(), nullptr);
          return;
        }
      }
      else if (interfaceName == kPlayerInterface)
      {
        auto* const parameters = ::g_dbus_method_invocation_get_parameters(invocationPtr.get());

        if (methodName == "Seek")
        {
          ::gint64 offsetUs = 0;
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- GVariant ABI requires gint64* for x.
          ::g_variant_get(parameters, "(x)", &offsetUs);
          endpoint.handleSeek(offsetUs);
        }
        else if (methodName == "SetPosition")
        {
          ::gchar const* path = nullptr;
          ::gint64 positionUs = 0;
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- GVariant ABI requires gchar const** and gint64*.
          ::g_variant_get(parameters, "(&ox)", &path, &positionUs);
          endpoint.handleSetPosition(path, positionUs);
        }
        else if (!endpoint.tryDispatchPlayerMethod(methodName))
        {
          replyError(std::move(invocationPtr), kMprisError, "Unsupported MPRIS player method");
          return;
        }

        ::g_dbus_method_invocation_return_value(invocationPtr.release(), nullptr);
        return;
      }

      replyError(std::move(invocationPtr), kMprisError, "Unsupported MPRIS method");
    }
  };

  MprisBridge::MprisBridge(async::Executor& executor,
                           rt::PlaybackService& playback,
                           uimodel::PlaybackActions& actions,
                           Callbacks callbacks,
                           MprisBridgeOptions options)
    : MprisBridge{executor, playback, actions, std::move(callbacks), playbackSourceFor(playback), std::move(options)}
  {
  }

  MprisBridge::MprisBridge(async::Executor& executor,
                           rt::PlaybackService& playback,
                           uimodel::PlaybackActions& actions,
                           Callbacks callbacks,
                           PlaybackSource playbackSource,
                           MprisBridgeOptions options)
    : _implPtr{std::make_unique<Impl>(executor,
                                      playback,
                                      actions,
                                      std::move(callbacks),
                                      std::move(playbackSource),
                                      std::move(options))}
  {
  }

  MprisBridge::~MprisBridge() = default;

  void MprisBridge::start()
  {
    _implPtr->start();
  }

  void MprisBridge::retire()
  {
    _implPtr->retire();
  }

  bool MprisBridge::isActive() const noexcept
  {
    return !_implPtr->activeBusName.empty();
  }

  std::string_view MprisBridge::busName() const noexcept
  {
    return _implPtr->activeBusName;
  }

  Glib::VariantBase MprisBridge::playerProperty(std::string_view propertyName) const
  {
    return _implPtr->playerProperty(propertyName);
  }

  MprisBridge::MetadataSnapshot MprisBridge::metadataSnapshot() const
  {
    auto const& state = _implPtr->playbackSource.snapshot().transport;
    return metadataForState(state, _implPtr->artUrlSession.urlFor(state.nowPlaying.coverArtId));
  }

  std::string_view MprisBridge::playbackStatus(audio::Transport transport) noexcept
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

  std::string_view MprisBridge::loopStatus(rt::RepeatMode mode) noexcept
  {
    switch (mode)
    {
      case rt::RepeatMode::Off: return "None";
      case rt::RepeatMode::One: return "Track";
      case rt::RepeatMode::All: return "Playlist";
    }

    return "None";
  }

  std::optional<rt::RepeatMode> MprisBridge::repeatModeForLoopStatus(std::string_view loopStatus) noexcept
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

  std::int64_t MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds duration) noexcept
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

  std::chrono::milliseconds MprisBridge::fromMprisMicroseconds(std::int64_t value) noexcept
  {
    return std::chrono::milliseconds{value / 1000};
  }

  bool MprisBridge::shouldEmitSeeked(rt::PlaybackTransportSnapshot const& before,
                                     rt::PlaybackTransportSnapshot const& after) noexcept
  {
    return after.finalSeekRevision != before.finalSeekRevision;
  }

  bool MprisBridge::shouldEmitMetadataChanged(rt::PlaybackTransportSnapshot const& before,
                                              rt::PlaybackTransportSnapshot const& after) noexcept
  {
    return after.occurrenceId != before.occurrenceId || after.nowPlaying != before.nowPlaying ||
           after.duration != before.duration;
  }

  std::string MprisBridge::trackObjectPath(TrackId trackId, rt::PlaybackOccurrenceId occurrenceId)
  {
    if (trackId == kInvalidTrackId || occurrenceId.value == 0)
    {
      return {};
    }

    return std::string{kTrackObjectPathPrefix} + std::to_string(trackId.raw()) + "_" +
           std::to_string(occurrenceId.value);
  }

  MprisBridge::MetadataSnapshot MprisBridge::metadataForState(rt::PlaybackTransportSnapshot const& state,
                                                              std::string artUrl)
  {
    if (state.nowPlaying.trackId == kInvalidTrackId || state.occurrenceId.value == 0)
    {
      return {};
    }

    return {.trackObjectPath = trackObjectPath(state.nowPlaying.trackId, state.occurrenceId),
            .title = state.nowPlaying.title,
            .artist = state.nowPlaying.artist,
            .album = state.nowPlaying.album,
            .artUrl = std::move(artUrl),
            .lengthUs = microsecondsFromMilliseconds(state.duration)};
  }
} // namespace ao::media
