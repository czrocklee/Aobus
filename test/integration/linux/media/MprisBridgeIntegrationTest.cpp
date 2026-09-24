// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisTestSupport.h"
#include "media/linux/MprisBridge.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <gio/gio.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::media::test
{
  namespace
  {
    constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

    MprisBridgeOptions optionsFor(PrivateBus const& bus,
                                  std::string busName = "org.mpris.MediaPlayer2.aobus.integration",
                                  bool const uniqueInstance = false,
                                  std::string identity = "Aobus Integration")
    {
      return {
        .busName = std::move(busName),
        .identity = std::move(identity),
        .desktopEntry = "aobus-integration",
        .uniqueInstance = uniqueInstance,
        .optBusAddress = bus.address(),
      };
    }

    void startAndAwait(MprisBridge& bridge, rt::test::QueuedExecutor& executor)
    {
      bridge.start();
      CHECK_FALSE(bridge.isActive());
      REQUIRE(executor.tryDrainUntil([&bridge] { return bridge.isActive(); }));
      REQUIRE_FALSE(bridge.busName().empty());
    }

    void requireUnitReply(CallResult const& result)
    {
      REQUIRE(result);
      REQUIRE(result.value() != nullptr);
      CHECK(::g_variant_is_of_type(result.value(), G_VARIANT_TYPE("()")) == TRUE);
    }

    VariantPtr getReplyValue(CallResult const& result)
    {
      REQUIRE(result);
      REQUIRE(result.value() != nullptr);
      REQUIRE(::g_variant_is_of_type(result.value(), G_VARIANT_TYPE("(v)")) == TRUE);
      auto boxedPtr = takeVariant(::g_variant_get_child_value(result.value(), 0));
      REQUIRE(boxedPtr);
      return takeVariant(::g_variant_get_variant(boxedPtr.get()));
    }

    std::string requireStringGet(CallResult const& result)
    {
      auto valuePtr = getReplyValue(result);
      REQUIRE(::g_variant_is_of_type(valuePtr.get(), G_VARIANT_TYPE_STRING) == TRUE);
      return std::string{::g_variant_get_string(valuePtr.get(), nullptr)};
    }

    PendingCall getProperty(BusClient& client,
                            std::string const& destination,
                            std::string_view const interfaceName,
                            std::string_view const propertyName)
    {
      auto const interfaceText = std::string{interfaceName};
      auto const propertyText = std::string{propertyName};
      return client.callAsync(destination,
                              kPropertiesInterface,
                              "Get",
                              ::g_variant_new("(ss)", interfaceText.c_str(), propertyText.c_str()),
                              G_VARIANT_TYPE("(v)"));
    }

    void checkRemoteError(CallResult const& result, std::string_view const expectedName)
    {
      REQUIRE_FALSE(result);
      REQUIRE(result.error() != nullptr);
      CHECK(result.remoteErrorName() == expectedName);
      CHECK_FALSE(result.isTimeout());
    }

    bool hasBusOwner(::GDBusConnection* connection, std::string const& name)
    {
      ::GError* error = nullptr;
      auto result = CallResult{::g_dbus_connection_call_sync(connection,
                                                             "org.freedesktop.DBus",
                                                             "/org/freedesktop/DBus",
                                                             "org.freedesktop.DBus",
                                                             "NameHasOwner",
                                                             ::g_variant_new("(s)", name.c_str()),
                                                             G_VARIANT_TYPE("(b)"),
                                                             G_DBUS_CALL_FLAGS_NONE,
                                                             2000,
                                                             nullptr,
                                                             &error),
                               error};
      REQUIRE(result);
      ::gboolean owned = FALSE;
      ::g_variant_get(result.value(), "(b)", &owned);
      return owned == TRUE;
    }

    std::uint32_t changeBusName(::GDBusConnection* connection, char const* method, std::string const& name)
    {
      ::GError* error = nullptr;
      auto result = CallResult{::g_dbus_connection_call_sync(connection,
                                                             "org.freedesktop.DBus",
                                                             "/org/freedesktop/DBus",
                                                             "org.freedesktop.DBus",
                                                             method,
                                                             method == std::string_view{"RequestName"}
                                                               ? ::g_variant_new("(su)", name.c_str(), 0U)
                                                               : ::g_variant_new("(s)", name.c_str()),
                                                             G_VARIANT_TYPE("(u)"),
                                                             G_DBUS_CALL_FLAGS_NONE,
                                                             2000,
                                                             nullptr,
                                                             &error),
                               error};
      REQUIRE(result);
      ::guint32 reply = 0;
      ::g_variant_get(result.value(), "(u)", &reply);
      return reply;
    }
  } // namespace

  TEST_CASE("MprisBridge private bus - properties and native methods cross the owner executor",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    fixture.makePlaybackReady();
    auto const ownerThread = std::this_thread::get_id();
    std::int32_t playSelectionCount = 0;
    bool playSelectionOnOwner = false;
    auto actions = uimodel::PlaybackActions{playback,
                                            [&]
                                            {
                                              ++playSelectionCount;
                                              playSelectionOnOwner = std::this_thread::get_id() == ownerThread &&
                                                                     fixture.executor->isCurrent();
                                            }};
    std::int32_t raiseCount = 0;
    bool raiseOnOwner = false;
    auto bridge = MprisBridge{*fixture.executor,
                              playback,
                              actions,
                              MprisBridge::Callbacks{
                                .raise =
                                  [&]
                                {
                                  ++raiseCount;
                                  raiseOnOwner =
                                    std::this_thread::get_id() == ownerThread && fixture.executor->isCurrent();
                                  return true;
                                },
                                .quit = [] {},
                              },
                              optionsFor(bus)};
    startAndAwait(bridge, *fixture.executor);
    auto const destination = std::string{bridge.busName()};
    auto client = BusClient{bus.address()};

    auto identityCall = getProperty(client, destination, kRootInterface, "Identity");
    fixture.executor->checkQueued();
    CHECK_FALSE(identityCall.isReady());
    auto identityReply = awaitCall(identityCall, *fixture.executor);
    CHECK(requireStringGet(identityReply) == "Aobus Integration");

    auto getAllCall = client.callAsync(
      destination, kPropertiesInterface, "GetAll", ::g_variant_new("(s)", kRootInterface), G_VARIANT_TYPE("(a{sv})"));
    auto getAllReply = awaitCall(getAllCall, *fixture.executor);
    REQUIRE(getAllReply);
    REQUIRE(::g_variant_is_of_type(getAllReply.value(), G_VARIANT_TYPE("(a{sv})")) == TRUE);
    auto propertiesPtr = takeVariant(::g_variant_get_child_value(getAllReply.value(), 0));
    REQUIRE(propertiesPtr);
    CHECK(::g_variant_n_children(propertiesPtr.get()) == 9);
    auto allIdentityPtr = takeVariant(::g_variant_lookup_value(propertiesPtr.get(), "Identity", G_VARIANT_TYPE_STRING));
    REQUIRE(allIdentityPtr);
    CHECK(std::string_view{::g_variant_get_string(allIdentityPtr.get(), nullptr)} == "Aobus Integration");
    auto canQuitPtr = takeVariant(::g_variant_lookup_value(propertiesPtr.get(), "CanQuit", G_VARIANT_TYPE_BOOLEAN));
    REQUIRE(canQuitPtr);
    CHECK(::g_variant_get_boolean(canQuitPtr.get()) == TRUE);

    auto volumeSetCall =
      client.callAsync(destination,
                       kPropertiesInterface,
                       "Set",
                       ::g_variant_new("(ssv)", kPlayerInterface, "Volume", ::g_variant_new_double(0.37)),
                       G_VARIANT_TYPE("()"));
    auto volumeSetReply = awaitCall(volumeSetCall, *fixture.executor);
    requireUnitReply(volumeSetReply);
    CHECK(playback.snapshot().transport.volume.level == Catch::Approx{0.37F});

    auto volumeGetCall = getProperty(client, destination, kPlayerInterface, "Volume");
    auto volumeGetReply = awaitCall(volumeGetCall, *fixture.executor);
    auto volumePtr = getReplyValue(volumeGetReply);
    REQUIRE(::g_variant_is_of_type(volumePtr.get(), G_VARIANT_TYPE_DOUBLE) == TRUE);
    CHECK(::g_variant_get_double(volumePtr.get()) == Catch::Approx{0.37});

    auto readOnlyCall =
      client.callAsync(destination,
                       kPropertiesInterface,
                       "Set",
                       ::g_variant_new("(ssv)", kPlayerInterface, "PlaybackStatus", ::g_variant_new_string("Playing")),
                       G_VARIANT_TYPE("()"));
    auto readOnlyReply = awaitCall(readOnlyCall, *fixture.executor);
    // GDBus validates introspected access before forwarding to the owner.
    checkRemoteError(readOnlyReply, "org.freedesktop.DBus.Error.InvalidArgs");

    auto wrongTypeCall =
      client.callAsync(destination,
                       kPropertiesInterface,
                       "Set",
                       ::g_variant_new("(ssv)", kPlayerInterface, "Volume", ::g_variant_new_string("loud")),
                       G_VARIANT_TYPE("()"));
    auto wrongTypeReply = awaitCall(wrongTypeCall, *fixture.executor);
    checkRemoteError(wrongTypeReply, "org.freedesktop.DBus.Error.InvalidArgs");

    auto unknownCall = getProperty(client, destination, kPlayerInterface, "NotAProperty");
    auto unknownReply = awaitCall(unknownCall, *fixture.executor);
    checkRemoteError(unknownReply, "org.freedesktop.DBus.Error.InvalidArgs");

    auto raiseCall =
      client.callAsync(destination, kRootInterface, "Raise", ::g_variant_new("()"), G_VARIANT_TYPE("()"));
    auto raiseReply = awaitCall(raiseCall, *fixture.executor);
    requireUnitReply(raiseReply);
    CHECK(raiseCount == 1);
    CHECK(raiseOnOwner);

    auto playCall =
      client.callAsync(destination, kPlayerInterface, "Play", ::g_variant_new("()"), G_VARIANT_TYPE("()"));
    auto playReply = awaitCall(playCall, *fixture.executor);
    requireUnitReply(playReply);
    CHECK(playSelectionCount == 1);
    CHECK(playSelectionOnOwner);
  }

  TEST_CASE("MprisBridge private bus - DesktopEntry is available only when configured",
            "[platform][integration][mpris]")
  {
    auto const desktopEntry = std::string{GENERATE("", "aobus")};
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto options = optionsFor(bus);
    options.desktopEntry = desktopEntry;
    auto bridge = MprisBridge{*fixture.executor, playback, actions, {}, std::move(options)};
    startAndAwait(bridge, *fixture.executor);
    auto const destination = std::string{bridge.busName()};
    auto client = BusClient{bus.address()};

    auto getAllCall = client.callAsync(
      destination, kPropertiesInterface, "GetAll", ::g_variant_new("(s)", kRootInterface), G_VARIANT_TYPE("(a{sv})"));
    auto getAllReply = awaitCall(getAllCall, *fixture.executor);
    REQUIRE(getAllReply);
    auto propertiesPtr = takeVariant(::g_variant_get_child_value(getAllReply.value(), 0));
    auto entryPtr = takeVariant(::g_variant_lookup_value(propertiesPtr.get(), "DesktopEntry", nullptr));
    auto canQuitPtr = takeVariant(::g_variant_lookup_value(propertiesPtr.get(), "CanQuit", G_VARIANT_TYPE_BOOLEAN));
    REQUIRE(canQuitPtr);
    CHECK(::g_variant_get_boolean(canQuitPtr.get()) == FALSE);

    auto getCall = getProperty(client, destination, kRootInterface, "DesktopEntry");
    auto getReply = awaitCall(getCall, *fixture.executor);
    auto introspectCall = client.callAsync(
      destination, "org.freedesktop.DBus.Introspectable", "Introspect", ::g_variant_new("()"), G_VARIANT_TYPE("(s)"));
    auto introspectReply = awaitCall(introspectCall, *fixture.executor);
    REQUIRE(introspectReply);
    auto xmlPtr = takeVariant(::g_variant_get_child_value(introspectReply.value(), 0));
    auto nodePtr = utility::makeUniquePtr<::g_dbus_node_info_unref>(
      ::g_dbus_node_info_new_for_xml(::g_variant_get_string(xmlPtr.get(), nullptr), nullptr));
    REQUIRE(nodePtr);
    auto* const root = ::g_dbus_node_info_lookup_interface(nodePtr.get(), kRootInterface);
    REQUIRE(root != nullptr);
    auto const* const entry = ::g_dbus_interface_info_lookup_property(root, "DesktopEntry");

    if (desktopEntry.empty())
    {
      CHECK_FALSE(entryPtr);
      CHECK(entry == nullptr);
      CHECK(::g_variant_n_children(propertiesPtr.get()) == 8);
      // GDBus rejects a property absent from introspection before owner dispatch.
      checkRemoteError(getReply, "org.freedesktop.DBus.Error.InvalidArgs");
      CHECK(getReply.errorMessage().contains("DesktopEntry"));
    }
    else
    {
      REQUIRE(entryPtr);
      REQUIRE(::g_variant_is_of_type(entryPtr.get(), G_VARIANT_TYPE_STRING) == TRUE);
      CHECK(std::string_view{::g_variant_get_string(entryPtr.get(), nullptr)} == "aobus");
      CHECK(requireStringGet(getReply) == "aobus");
      REQUIRE(entry != nullptr);
      CHECK(std::string_view{entry->signature} == "s");
      CHECK(entry->flags == G_DBUS_PROPERTY_INFO_FLAGS_READABLE);
      CHECK(::g_variant_n_children(propertiesPtr.get()) == 9);
    }
  }

  TEST_CASE("MprisBridge private bus - canonical and unique instances coexist", "[platform][integration][mpris]")
  {
    constexpr auto kBaseName = "org.mpris.MediaPlayer2.aobus.multi";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto canonicalActions = uimodel::PlaybackActions{playback, [] {}};
    auto firstActions = uimodel::PlaybackActions{playback, [] {}};
    auto secondActions = uimodel::PlaybackActions{playback, [] {}};
    auto canonical =
      MprisBridge{*fixture.executor, playback, canonicalActions, {}, optionsFor(bus, kBaseName, false, "Canonical")};
    auto first =
      MprisBridge{*fixture.executor, playback, firstActions, {}, optionsFor(bus, kBaseName, true, "First unique")};
    auto second =
      MprisBridge{*fixture.executor, playback, secondActions, {}, optionsFor(bus, kBaseName, true, "Second unique")};

    canonical.start();
    first.start();
    second.start();
    REQUIRE(
      fixture.executor->tryDrainUntil([&] { return canonical.isActive() && first.isActive() && second.isActive(); }));

    auto const canonicalName = std::string{canonical.busName()};
    auto const firstName = std::string{first.busName()};
    auto const secondName = std::string{second.busName()};
    CHECK(canonicalName == kBaseName);
    CHECK(firstName.starts_with(std::string{kBaseName} + ".instance"));
    CHECK(secondName.starts_with(std::string{kBaseName} + ".instance"));
    CHECK(firstName != secondName);
    CHECK(firstName != canonicalName);
    CHECK(secondName != canonicalName);

    auto client = BusClient{bus.address()};
    auto canonicalCall = getProperty(client, canonicalName, kRootInterface, "Identity");
    auto firstCall = getProperty(client, firstName, kRootInterface, "Identity");
    auto secondCall = getProperty(client, secondName, kRootInterface, "Identity");
    auto canonicalReply = awaitCall(canonicalCall, *fixture.executor);
    auto firstReply = awaitCall(firstCall, *fixture.executor);
    auto secondReply = awaitCall(secondCall, *fixture.executor);
    CHECK(requireStringGet(canonicalReply) == "Canonical");
    CHECK(requireStringGet(firstReply) == "First unique");
    CHECK(requireStringGet(secondReply) == "Second unique");
  }

  TEST_CASE("MprisBridge private bus - same-name contender cannot revoke or inherit the current owner",
            "[platform][integration][mpris][concurrency]")
  {
    constexpr auto kName = "org.mpris.MediaPlayer2.aobus.lifetime";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto client = BusClient{bus.address()};
    auto ownerPtr = std::make_unique<MprisBridge>(
      *fixture.executor, playback, actions, MprisBridge::Callbacks{}, optionsFor(bus, kName));
    ownerPtr->start();
    REQUIRE(fixture.executor->tryDrainUntil([&] { return ownerPtr->isActive(); }));
    REQUIRE(hasBusOwner(client.nativeConnection(), kName));
    fixture.executor->drain();
    REQUIRE(fixture.executor->queuedCount() == 0);

    std::int32_t subscribed = 0;
    auto source = MprisBridge::PlaybackSource{
      .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
      .onSnapshot =
        [&](rt::PlaybackSnapshotObserver)
      {
        ++subscribed;
        return async::Subscription{};
      },
      .elapsed = [&playback] { return playback.elapsed(); },
    };
    auto contenderPtr = std::make_unique<MprisBridge>(
      *fixture.executor, playback, actions, MprisBridge::Callbacks{}, std::move(source), optionsFor(bus, kName));
    contenderPtr->start();
    // A separate private connection must fail with no queue: it cannot take
    // ownership even after the successful owner releases its name.
    fixture.executor->checkQueued();
    fixture.executor->drain();
    CHECK_FALSE(contenderPtr->isActive());
    CHECK(subscribed == 0);
    CHECK(ownerPtr->isActive());
    CHECK(hasBusOwner(client.nativeConnection(), kName));
    contenderPtr.reset();
    CHECK(ownerPtr->isActive());
    CHECK(hasBusOwner(client.nativeConnection(), kName));
    fixture.executor->drain();
    REQUIRE(fixture.executor->queuedCount() == 0);

    auto waitingContender =
      MprisBridge{*fixture.executor, playback, actions, MprisBridge::Callbacks{}, optionsFor(bus, kName)};
    waitingContender.start();
    fixture.executor->checkQueued();
    fixture.executor->drain();
    CHECK_FALSE(waitingContender.isActive());
    ownerPtr.reset();
    CHECK_FALSE(hasBusOwner(client.nativeConnection(), kName));
    fixture.executor->drain();
    CHECK_FALSE(waitingContender.isActive());
    CHECK_FALSE(hasBusOwner(client.nativeConnection(), kName));

    auto replacement = MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus, kName)};
    startAndAwait(replacement, *fixture.executor);
    CHECK(hasBusOwner(client.nativeConnection(), kName));
  }

  TEST_CASE("MprisBridge private bus - external ownership rejects a contender without queueing a promotion",
            "[platform][integration][mpris][concurrency]")
  {
    constexpr auto kName = "org.mpris.MediaPlayer2.aobus.external";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto peer = BusClient{bus.address()};
    auto observer = BusClient{bus.address()};
    REQUIRE(changeBusName(peer.nativeConnection(), "RequestName", kName) == 1U);
    REQUIRE(hasBusOwner(observer.nativeConnection(), kName));
    fixture.executor->drain();
    REQUIRE(fixture.executor->queuedCount() == 0);

    auto contenderPtr = std::make_unique<MprisBridge>(
      *fixture.executor, playback, actions, MprisBridge::Callbacks{}, optionsFor(bus, kName));
    contenderPtr->start();
    fixture.executor->checkQueued();
    fixture.executor->drain();
    CHECK_FALSE(contenderPtr->isActive());
    CHECK(hasBusOwner(observer.nativeConnection(), kName));
    REQUIRE(changeBusName(peer.nativeConnection(), "ReleaseName", kName) == 1U);
    CHECK_FALSE(hasBusOwner(observer.nativeConnection(), kName));
    contenderPtr.reset();
    CHECK_FALSE(hasBusOwner(observer.nativeConnection(), kName));

    auto replacement = MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus, kName)};
    startAndAwait(replacement, *fixture.executor);
    CHECK(hasBusOwner(observer.nativeConnection(), kName));
  }

  TEST_CASE("MprisBridge private bus - failed startup joins and permits replacement without resurrection",
            "[platform][integration][mpris][concurrency]")
  {
    enum class FailureStage : std::uint8_t
    {
      Snapshot,
      Artwork,
      Subscription,
    };
    auto const failureStage = GENERATE(FailureStage::Snapshot, FailureStage::Artwork, FailureStage::Subscription);
    constexpr auto kName = "org.mpris.MediaPlayer2.aobus.startup";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto executor = rt::test::ManualExecutor{};
    auto observer = BusClient{bus.address()};
    REQUIRE_FALSE(hasBusOwner(observer.nativeConnection(), kName));

    auto snapshot = rt::PlaybackSnapshot{};
    snapshot.transport.occurrenceId = rt::PlaybackOccurrenceId{1};
    snapshot.transport.nowPlaying = rt::NowPlayingInfo{.trackId = TrackId{1}, .coverArtId = ResourceId{11}};
    bool failStartup = false;
    std::int32_t snapshotReads = 0;
    std::int32_t subscriptions = 0;
    std::int32_t detachments = 0;
    std::int32_t requests = 0;
    std::int32_t cancellations = 0;
    auto requestedResourceId = ResourceId{kInvalidResourceId};
    auto capturedCompletion = MprisBridge::OnArtUrlReady{};
    auto bridge =
      MprisBridge{executor,
                  playback,
                  actions,
                  MprisBridge::Callbacks{
                    .requestArtUrl = [&](ResourceId const resourceId,
                                         MprisBridge::OnArtUrlReady complete) -> utility::ScopedRegistration
                    {
                      requestedResourceId = resourceId;
                      capturedCompletion = std::move(complete);
                      ++requests;

                      if (failStartup && failureStage == FailureStage::Artwork)
                      {
                        throw std::runtime_error{"startup failure"};
                      }

                      return utility::ScopedRegistration{[&]
                                                         {
                                                           ++cancellations;
                                                           capturedCompletion("file:///tmp/cancelled-art.png");
                                                         }};
                    },
                  },
                  MprisBridge::PlaybackSource{
                    .snapshot = [&] -> rt::PlaybackSnapshot const&
                    {
                      ++snapshotReads;

                      if (failStartup && failureStage == FailureStage::Snapshot)
                      {
                        throw std::runtime_error{"startup failure"};
                      }

                      return snapshot;
                    },
                    .onSnapshot =
                      [&](rt::PlaybackSnapshotObserver publish)
                    {
                      ++subscriptions;

                      if (failStartup && failureStage == FailureStage::Subscription)
                      {
                        // The new bridge subscribes before requesting its initial art.
                        // Publish a new cover while subscribing to prove that rollback
                        // cancels an already outstanding artwork request.
                        auto changed = snapshot;
                        changed.transport.nowPlaying.trackId = TrackId{2};
                        changed.transport.nowPlaying.coverArtId = ResourceId{22};
                        publish(changed);
                        throw std::runtime_error{"startup failure"};
                      }

                      return async::Subscription{[&] { ++detachments; }};
                    },
                    .elapsed = [&playback] { return playback.elapsed(); },
                  },
                  optionsFor(bus, kName)};
    REQUIRE(snapshotReads == 1);
    // Construction reads a valid snapshot; failure starts only in the acquired
    // callback, delivered to this test by the throwing ManualExecutor turn.
    failStartup = true;
    bridge.start();
    CHECK_FALSE(bridge.isActive());
    executor.checkQueued();
    REQUIRE_THROWS_WITH(executor.tryRunOne(), "startup failure");
    executor.runUntilIdle();
    CHECK_FALSE(bridge.isActive());
    CHECK(bridge.busName().empty());
    CHECK_FALSE(hasBusOwner(observer.nativeConnection(), kName));
    CHECK(subscriptions == (failureStage == FailureStage::Snapshot ? 0 : 1));
    CHECK(requests == (failureStage == FailureStage::Snapshot ? 0 : 1));
    CHECK(cancellations == (failureStage == FailureStage::Subscription ? 1 : 0));
    CHECK(detachments == (failureStage == FailureStage::Artwork ? 1 : 0));

    if (failureStage != FailureStage::Snapshot)
    {
      CHECK(requestedResourceId == (failureStage == FailureStage::Subscription ? ResourceId{22} : ResourceId{11}));
      REQUIRE(capturedCompletion);
    }
    else
    {
      CHECK(requestedResourceId == kInvalidResourceId);
    }

    failStartup = false;

    if (capturedCompletion)
    {
      capturedCompletion("file:///tmp/late-art.png");
      CHECK(bridge.metadataSnapshot().artUrl.empty());
    }

    auto const readsAfterFailure = snapshotReads;
    bridge.start();
    executor.runUntilIdle();
    CHECK_FALSE(bridge.isActive());
    CHECK(bridge.busName().empty());
    CHECK_FALSE(hasBusOwner(observer.nativeConnection(), kName));
    CHECK(snapshotReads == readsAfterFailure);
    CHECK(requests == (failureStage == FailureStage::Snapshot ? 0 : 1));

    auto replacement = MprisBridge{executor, playback, actions, {}, optionsFor(bus, kName)};
    replacement.start();
    REQUIRE(executor.tryDrainUntil([&] { return replacement.isActive(); }));
    CHECK(replacement.busName() == kName);
    CHECK(hasBusOwner(observer.nativeConnection(), kName));
    CHECK_FALSE(bridge.isActive());
  }

  TEST_CASE("MprisBridge private bus - replacement survives retiring an unsettled or acquired owner",
            "[platform][integration][mpris][concurrency]")
  {
    constexpr auto kName = "org.mpris.MediaPlayer2.aobus.retirement";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto observer = BusClient{bus.address()};
    bool settleFirst = false;

    SECTION("before acquisition")
    {
      settleFirst = false;
    }

    SECTION("after acquisition")
    {
      settleFirst = true;
    }

    {
      auto first = MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus, kName)};
      first.start();

      if (settleFirst)
      {
        REQUIRE(fixture.executor->tryDrainUntil([&] { return first.isActive(); }));
        CHECK(hasBusOwner(observer.nativeConnection(), kName));
      }
      else
      {
        CHECK_FALSE(first.isActive());
      }
    }

    auto replacement = MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus, kName)};
    startAndAwait(replacement, *fixture.executor);
    fixture.executor->drain();
    CHECK(replacement.isActive());
    CHECK(hasBusOwner(observer.nativeConnection(), kName));
  }

  TEST_CASE("MprisBridge private bus - Quit replies before deferred host destruction",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto bridgePtr = std::unique_ptr<MprisBridge>{};
    bool quitOnOwner = false;
    bool destroyed = false;
    auto callbacks = MprisBridge::Callbacks{
      .quit =
        [&]
      {
        quitOnOwner = fixture.executor->isCurrent();
        bridgePtr.reset();
        destroyed = true;
      },
    };
    bridgePtr =
      std::make_unique<MprisBridge>(*fixture.executor, playback, actions, std::move(callbacks), optionsFor(bus));
    startAndAwait(*bridgePtr, *fixture.executor);
    auto const destination = std::string{bridgePtr->busName()};
    auto client = BusClient{bus.address()};

    auto quitCall = client.callAsync(destination, kRootInterface, "Quit", ::g_variant_new("()"), G_VARIANT_TYPE("()"));
    auto quitReply = awaitCall(quitCall, *fixture.executor);
    requireUnitReply(quitReply);
    REQUIRE(fixture.executor->tryDrainUntil([&] { return destroyed; }));
    CHECK(quitOnOwner);
    CHECK_FALSE(bridgePtr);
  }

  TEST_CASE("MprisBridge private bus - queued invocation outlives native join without host access",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    std::int32_t raiseCount = 0;
    auto bridgePtr = std::make_unique<MprisBridge>(*fixture.executor,
                                                   playback,
                                                   actions,
                                                   MprisBridge::Callbacks{
                                                     .raise =
                                                       [&]
                                                     {
                                                       ++raiseCount;
                                                       return true;
                                                     },
                                                   },
                                                   optionsFor(bus));
    startAndAwait(*bridgePtr, *fixture.executor);
    fixture.executor->drain();
    REQUIRE(fixture.executor->queuedCount() == 0);
    auto const destination = std::string{bridgePtr->busName()};
    auto client = BusClient{bus.address()};

    auto heldCall = client.callAsync(destination, kRootInterface, "Raise", ::g_variant_new("()"), G_VARIANT_TYPE("()"));
    fixture.executor->checkQueued();
    CHECK_FALSE(heldCall.isReady());

    auto const retirementStart = std::chrono::steady_clock::now();
    bridgePtr->retire();
    bridgePtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    CHECK(retirementElapsed < std::chrono::seconds{2});

    auto heldReply = awaitCallWithoutOwnerProgress(heldCall);
    checkRemoteError(heldReply, "org.freedesktop.DBus.Error.NoReply");
    CHECK((heldReply.errorMessage().contains("disconnected") || heldReply.errorMessage().contains("closed")));
    CHECK(raiseCount == 0);

    fixture.executor->drain();
    CHECK(raiseCount == 0);
  }

  TEST_CASE("MprisBridge private bus - an empty address disables admission", "[platform][unit][mpris]")
  {
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    std::int32_t subscriptionCount = 0;
    auto source = MprisBridge::PlaybackSource{
      .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
      .onSnapshot =
        [&](rt::PlaybackSnapshotObserver observer)
      {
        ++subscriptionCount;
        return playback.events().onSnapshot(std::move(observer));
      },
      .elapsed = [&playback] { return playback.elapsed(); },
    };
    auto options = MprisBridgeOptions{.optBusAddress = std::string{}};
    auto bridge = MprisBridge{*fixture.executor, playback, actions, {}, std::move(source), std::move(options)};

    bridge.start();
    CHECK_FALSE(bridge.isActive());
    CHECK(subscriptionCount == 0);
  }

  TEST_CASE("MprisBridge private bus - an autolaunch fallback forbids acquisition", "[platform][unit][mpris]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto control =
      MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus, "org.mpris.MediaPlayer2.aobus.allowed")};
    startAndAwait(control, *fixture.executor);

    auto options = optionsFor(bus, "org.mpris.MediaPlayer2.aobus.rejected");
    // A working first address makes removal of the policy observable without
    // allowing a regression to autolaunch anything on the user's desktop.
    options.optBusAddress = bus.address() + ";autolaunch:";
    auto bridge = MprisBridge{*fixture.executor, playback, actions, {}, std::move(options)};
    bridge.start();
    CHECK_FALSE(fixture.executor->tryDrainUntil([&bridge] { return bridge.isActive(); }, std::chrono::seconds{1}));
    CHECK(control.isActive());
  }

  TEST_CASE("MprisBridge private bus - invalid addresses degrade without interests",
            "[platform][integration][mpris][concurrency]")
  {
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    std::int32_t subscriptionCount = 0;
    auto source = MprisBridge::PlaybackSource{
      .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
      .onSnapshot =
        [&](rt::PlaybackSnapshotObserver observer)
      {
        ++subscriptionCount;
        return playback.events().onSnapshot(std::move(observer));
      },
      .elapsed = [&playback] { return playback.elapsed(); },
    };
    auto const missingSocket = fixture.tempDir.path() / "no-session-bus";
    auto options = MprisBridgeOptions{.optBusAddress = "unix:path=" + missingSocket.string()};
    auto bridge = MprisBridge{*fixture.executor, playback, actions, {}, std::move(source), std::move(options)};

    bridge.start();
    fixture.executor->checkQueued();
    fixture.executor->drain();
    CHECK_FALSE(bridge.isActive());
    CHECK(subscriptionCount == 0);
  }

  TEST_CASE("MprisBridge private bus - disappearance retires an active adapter",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto bridge = MprisBridge{*fixture.executor, playback, actions, {}, optionsFor(bus)};
    startAndAwait(bridge, *fixture.executor);

    bus.stop();
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !bridge.isActive(); }));
    CHECK(bridge.busName().empty());
  }

  TEST_CASE("MprisBridge private bus - retirement suppresses queued acquisition",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    std::int32_t subscriptionCount = 0;
    auto source = MprisBridge::PlaybackSource{
      .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
      .onSnapshot =
        [&](rt::PlaybackSnapshotObserver observer)
      {
        ++subscriptionCount;
        return playback.events().onSnapshot(std::move(observer));
      },
      .elapsed = [&playback] { return playback.elapsed(); },
    };
    auto bridgePtr = std::make_unique<MprisBridge>(
      *fixture.executor, playback, actions, MprisBridge::Callbacks{}, std::move(source), optionsFor(bus));

    fixture.executor->drain();
    REQUIRE(fixture.executor->queuedCount() == 0);
    bridgePtr->start();
    fixture.executor->checkQueued();
    CHECK_FALSE(bridgePtr->isActive());
    bridgePtr->retire();
    bridgePtr.reset();
    fixture.executor->drain();
    CHECK(subscriptionCount == 0);
  }
} // namespace ao::media::test
