// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/image/CoverArtView.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/image/ResourceImageLoader.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentTooltipController.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/LibraryImportExportWorkflow.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::hasController;

  namespace
  {
    class StaticWidgetComponent final : public LayoutComponent
    {
    public:
      explicit StaticWidgetComponent(Gtk::Widget& widget)
        : _widget{widget}
      {
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      Gtk::Widget& _widget;
    };

    /**
     * @brief Writes a `full` document naming @p optCover by digest.
     *
     * A document carries no cover bytes, so the import writes a reference and the
     * cover is materialized from a source afterwards; the caller installs those
     * bytes in the cover cache for synthetic imagery that no audio file carries.
     */
    void writeCoverImport(std::filesystem::path const& path, std::optional<std::span<std::byte const>> optCover)
    {
      auto output = std::ofstream{path};
      REQUIRE(output);
      output << "version: 4\n"
                "export_mode: full\n"
                "library:\n";

      if (optCover)
      {
        output << "  resources:\n"
                  "    - digest: "
               << utility::sha256Hex(utility::computeSha256(*optCover)) << "\n"
               << "      length: " << optCover->size() << '\n';
      }
      else
      {
        output << "  resources: []\n";
      }

      output << "  tracks:\n"
                "    - uri: mutable-cover.flac\n";

      if (optCover)
      {
        output << "      covers:\n"
                  "        - type: 3\n"
                  "          resource: "
               << utility::sha256Hex(utility::computeSha256(*optCover)) << '\n';
      }

      output << "  lists: []\n";
      REQUIRE(output.good());
    }

    void startPlayback(rt::AppRuntime& runtime, TrackId const trackId)
    {
      runtime.reloadAllTracks();
      auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
      REQUIRE(ao::gtk::test::waitForPlaybackSettlement(runtime, trackId));
    }
  } // namespace

  TEST_CASE("PlaybackImage - applies declarative image properties", "[gtk][unit][image]")
  {
    auto mutableCoverTrackId = kInvalidTrackId;
    auto coverTrackId = kInvalidTrackId;
    auto noCoverTrackId = kInvalidTrackId;
    auto corruptCoverTrackId = kInvalidTrackId;
    auto coverResourceId = kInvalidResourceId;
    auto corruptCoverResourceId = kInvalidResourceId;
    auto const coverBytes = ao::gtk::test::encodePng(ao::gtk::test::makePixbuf(80));
    constexpr auto kCorruptCoverBytes = std::array{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.playback_image_test",
      [&](library::MusicLibrary& musicLibrary)
      {
        auto const mutableCoverUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "mutable-cover.flac");
        auto const coverUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "cover-track.flac");
        auto const noCoverUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "no-cover-track.flac");
        auto const corruptCoverUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "corrupt-cover-track.flac");
        coverResourceId = ao::gtk::test::writeRawResource(musicLibrary, coverBytes);
        mutableCoverTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                          library::test::TrackSpec{
                                                                            .title = "Mutable Cover Track",
                                                                            .uri = mutableCoverUri,
                                                                            .coverArtId = coverResourceId,
                                                                            .duration = std::chrono::seconds{1},
                                                                          });
        coverTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                   library::test::TrackSpec{
                                                                     .title = "Cover Track",
                                                                     .uri = coverUri,
                                                                     .coverArtId = coverResourceId,
                                                                     .duration = std::chrono::seconds{1},
                                                                   });
        noCoverTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                     library::test::TrackSpec{
                                                                       .title = "No Cover Track",
                                                                       .uri = noCoverUri,
                                                                       .duration = std::chrono::seconds{1},
                                                                     });
        corruptCoverResourceId = ao::gtk::test::writeRawResource(musicLibrary, kCorruptCoverBytes);
        corruptCoverTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary,
                                                                          library::test::TrackSpec{
                                                                            .title = "Corrupt Cover Track",
                                                                            .uri = corruptCoverUri,
                                                                            .coverArtId = corruptCoverResourceId,
                                                                            .duration = std::chrono::seconds{1},
                                                                          });
      }};
    ao::gtk::test::installCoverCacheEntry(fixture.cacheDirectory(), coverBytes);
    ao::gtk::test::installCoverCacheEntry(fixture.cacheDirectory(), kCorruptCoverBytes);

    auto imageCachePtr = std::make_unique<ImageCache>(10);
    auto byteLoader = rt::ResourceByteLoader{fixture.runtime()};
    auto imageLoaderPtr = std::make_unique<ResourceImageLoader>(byteLoader, *imageCachePtr, fixture.runtime().async());
    auto& ctx = fixture.context();
    fixture.dependencies().imageLoader = imageLoaderPtr.get();

    SECTION("default image has no extra styling")
    {
      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const picture = button->get_child();
      REQUIRE(picture != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(picture);
      REQUIRE(coverArt != nullptr);

      CHECK_FALSE(picture->has_css_class("ao-nowplaying-image-thumb"));
      CHECK(button->get_visible());
      CHECK(coverArt->showingPlaceholder());
      CHECK(coverArt->placeholderPresentation().style == CoverArtPlaceholderStyle::Equalizer);

      std::int32_t width = -1;
      std::int32_t height = -1;
      picture->get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);
    }

    SECTION("declarative properties control size and opacity")
    {
      auto node = LayoutNode{.type = "playback.image"};
      node.props["targetSize"] = LayoutValue{static_cast<std::int64_t>(60)};
      node.props["forceSquare"] = LayoutValue{true};
      node.props["opacity"] = LayoutValue{std::string{"0.5"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const slot = button->get_child();
      REQUIRE(slot != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(slot->get_first_child());
      REQUIRE(coverArt != nullptr);

      CHECK(button->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(coverArt->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK_FALSE(widget.get_hexpand());
      CHECK_FALSE(widget.get_vexpand());

      std::int32_t buttonWidth = 0;
      std::int32_t buttonHeight = 0;
      button->get_size_request(buttonWidth, buttonHeight);
      CHECK(buttonWidth == -1);
      CHECK(buttonHeight == -1);

      std::int32_t width = 0;
      std::int32_t height = 0;
      coverArt->get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);

      std::int32_t minimum = -1;
      std::int32_t natural = -1;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      slot->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      CHECK(minimum == 60);
      CHECK(natural == 60);

      slot->measure(Gtk::Orientation::VERTICAL, 64, minimum, natural, minimumBaseline, naturalBaseline);
      CHECK(minimum == 0);
      CHECK(natural == 0);

      slot->size_allocate(Gtk::Allocation{0, 0, 60, 64}, -1);
      CHECK(coverArt->get_width() == 60);
      CHECK(coverArt->get_height() == 60);
      CHECK(button->get_opacity() == Catch::Approx{0.5}.margin(0.01));
    }

    SECTION("now playing cover art drives the image resource")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();

      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));

      // An earlier snapshot observer must not delay the component's own update.
      bool earlierObserverEntered = false;
      auto const earlierSubscription = fixture.runtime().playback().events().onSnapshot(
        [&earlierObserverEntered](rt::PlaybackSnapshot const&) noexcept { earlierObserverEntered = true; });

      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(button->get_child());
      REQUIRE(coverArt != nullptr);

      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK(earlierObserverEntered);
      auto const paintablePtr = coverArt->imagePaintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 64 * coverArt->get_scale_factor());
      CHECK(paintablePtr->get_intrinsic_height() == 64 * coverArt->get_scale_factor());

      fixture.runtime().playback().commands().stop();
      ao::gtk::test::drainGtkEvents();

      CHECK(button->get_visible());
      CHECK_FALSE(coverArt->hasImage());
      CHECK(coverArt->showingPlaceholder());
      CHECK(coverArt->placeholderPresentation().style == CoverArtPlaceholderStyle::Equalizer);
    }

    SECTION("no-cover placeholder preserves the configured playback action")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();

      auto node = LayoutNode{.type = "playback.image"};
      node.props["action"] = LayoutValue{std::string{"jumpToAlbum"}};
      node.props["placeholderStyle"] = LayoutValue{std::string{"monogram"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(button->get_child());
      REQUIRE(coverArt != nullptr);

      auto revealedTrackId = kInvalidTrackId;
      auto const revealSubscription = fixture.runtime().playback().events().onRevealTrackRequested(
        [&revealedTrackId](auto const& request) noexcept { revealedTrackId = request.trackId; });

      startPlayback(fixture.runtime(), noCoverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK(button->get_visible());
      CHECK(coverArt->showingPlaceholder());
      CHECK(coverArt->placeholderPresentation().style == CoverArtPlaceholderStyle::Monogram);
      CHECK(coverArt->placeholderPresentation().monogram == "A");

      emitClicked(*button);

      CHECK(revealedTrackId == noCoverTrackId);
    }

    SECTION("playback image tooltip opens for cover art and closes when no cover remains")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();
      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));
      auto manualHoverTimeout = sigc::signal<bool()>{};
      ctx.timeoutScheduler = [&](std::chrono::milliseconds const interval, sigc::slot<bool()> callback)
      {
        CHECK(interval == std::chrono::milliseconds{500});
        return manualHoverTimeout.connect(std::move(callback));
      };

      auto node = LayoutNode{.type = "playback.image"};
      node.props["action"] = LayoutValue{std::string{"jumpToAlbum"}};
      node.optTooltip = BoxedLayoutNode{LayoutNode{.type = "playback.image"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const popover = findWidget<Gtk::Popover>(*button);
      REQUIRE(popover != nullptr);
      auto* const tooltipButton = dynamic_cast<Gtk::Button*>(popover->get_child());
      REQUIRE(tooltipButton != nullptr);

      fixture.window().set_child(*button);
      auto const windowDetach = utility::ScopedRegistration{[&fixture] { fixture.window().unset_child(); }};
      fixture.window().present();
      ao::gtk::test::drainGtkEvents();

      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      REQUIRE(tooltipButton->get_visible());
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      REQUIRE(popover->get_visible());

      fixture.runtime().playback().commands().stop();
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(popover->get_visible());
      CHECK_FALSE(tooltipButton->get_visible());

      startPlayback(fixture.runtime(), noCoverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK(button->get_visible());
      CHECK(button->get_sensitive());
      CHECK_FALSE(tooltipButton->get_visible());
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      CHECK_FALSE(popover->get_visible());

      fixture.runtime().playback().commands().stop();
      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK(tooltipButton->get_visible());
      CHECK_FALSE(popover->get_visible());
      REQUIRE(ao::gtk::test::emitPointerLeave(*button));
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      CHECK(popover->get_visible());

      fixture.runtime().playback().commands().stop();
      startPlayback(fixture.runtime(), corruptCoverTrackId);
      bool corruptDecodeSettled = false;
      [[maybe_unused]] auto const corruptDecodeProbe = imageLoaderPtr->requestFull(
        corruptCoverResourceId, [&corruptDecodeSettled](auto const&) { corruptDecodeSettled = true; });
      REQUIRE(ao::gtk::test::pumpGtkEventsUntil([&corruptDecodeSettled] { return corruptDecodeSettled; }));

      CHECK_FALSE(tooltipButton->get_visible());
      CHECK_FALSE(popover->get_visible());
      REQUIRE(ao::gtk::test::emitPointerLeave(*button));
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      CHECK_FALSE(popover->get_visible());
    }

    SECTION("playback image tooltip survives a track change that keeps cover art available")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();
      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));
      auto manualHoverTimeout = sigc::signal<bool()>{};
      ctx.timeoutScheduler = [&](std::chrono::milliseconds const /*interval*/, sigc::slot<bool()> callback)
      { return manualHoverTimeout.connect(std::move(callback)); };

      auto node = LayoutNode{.type = "playback.image"};
      node.optTooltip = BoxedLayoutNode{LayoutNode{.type = "playback.image"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const popover = findWidget<Gtk::Popover>(*button);
      REQUIRE(popover != nullptr);
      auto* const tooltipButton = dynamic_cast<Gtk::Button*>(popover->get_child());
      REQUIRE(tooltipButton != nullptr);

      fixture.window().set_child(*button);
      auto const windowDetach = utility::ScopedRegistration{[&fixture] { fixture.window().unset_child(); }};
      fixture.window().present();
      ao::gtk::test::drainGtkEvents();

      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      REQUIRE(tooltipButton->get_visible());
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      REQUIRE(popover->get_visible());

      // Advancing straight to another track whose cover is already decoded never passes through
      // an unavailable state, so nothing reports a transition; availability must still hold.
      auto const viewRes = fixture.runtime().workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(fixture.runtime().playback().commands().startFromView(*viewRes, mutableCoverTrackId));
      REQUIRE(ao::gtk::test::waitForPlaybackSettlement(fixture.runtime(), mutableCoverTrackId));
      ao::gtk::test::drainGtkEvents();

      CHECK(tooltipButton->get_visible());
      CHECK(button->get_visible());
      // The tooltip root never went hidden, so the open popover is left alone.
      CHECK(popover->get_visible());
    }

    SECTION("hidden tooltip content does not arm hover until the pointer re-enters")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();
      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));
      auto manualHoverTimeout = sigc::signal<bool()>{};
      std::size_t scheduledHoverCount = 0;
      ctx.timeoutScheduler = [&](std::chrono::milliseconds const /*interval*/, sigc::slot<bool()> callback)
      {
        ++scheduledHoverCount;
        return manualHoverTimeout.connect(std::move(callback));
      };

      auto node = LayoutNode{.type = "playback.image"};
      node.optTooltip = BoxedLayoutNode{LayoutNode{.type = "playback.image"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const popover = findWidget<Gtk::Popover>(*button);
      REQUIRE(popover != nullptr);
      auto* const tooltipButton = dynamic_cast<Gtk::Button*>(popover->get_child());
      REQUIRE(tooltipButton != nullptr);

      fixture.window().set_child(*button);
      auto const windowDetach = utility::ScopedRegistration{[&fixture] { fixture.window().unset_child(); }};
      fixture.window().present();
      ao::gtk::test::drainGtkEvents();

      startPlayback(fixture.runtime(), noCoverTrackId);
      ao::gtk::test::drainGtkEvents();

      REQUIRE_FALSE(tooltipButton->get_visible());
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      CHECK(scheduledHoverCount == 0);

      fixture.runtime().playback().commands().stop();
      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      REQUIRE(tooltipButton->get_visible());
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      CHECK_FALSE(popover->get_visible());
      CHECK(scheduledHoverCount == 0);

      REQUIRE(ao::gtk::test::emitPointerLeave(*button));
      REQUIRE(ao::gtk::test::emitPointerEnter(*button));
      REQUIRE(scheduledHoverCount == 1);
      manualHoverTimeout.emit();
      ao::gtk::test::drainGtkEvents();
      CHECK(popover->get_visible());
    }

    SECTION("authored visibility and cover art availability both gate the tooltip")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();
      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));

      auto tooltipNode = LayoutNode{.type = "playback.image"};
      tooltipNode.layout["visible"] = LayoutValue{true};
      auto node = LayoutNode{.type = "playback.image"};
      node.optTooltip = BoxedLayoutNode{tooltipNode};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const popover = findWidget<Gtk::Popover>(*button);
      REQUIRE(popover != nullptr);
      auto* const tooltipButton = dynamic_cast<Gtk::Button*>(popover->get_child());
      REQUIRE(tooltipButton != nullptr);

      // Authored visibility is applied after construction and must not defeat the availability gate.
      CHECK_FALSE(tooltipButton->get_visible());

      startPlayback(fixture.runtime(), noCoverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(tooltipButton->get_visible());

      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      CHECK(tooltipButton->get_visible());
    }

    SECTION("authored visibility can still hide an available cover")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();
      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));

      auto node = LayoutNode{.type = "playback.image"};
      node.layout["visible"] = LayoutValue{false};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);

      startPlayback(fixture.runtime(), coverTrackId);
      ao::gtk::test::drainGtkEvents();

      // The persistent surface would otherwise force itself visible on every snapshot.
      CHECK_FALSE(button->get_visible());
    }

    SECTION("current track cover art follows library mutations")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      ao::gtk::test::drainGtkEvents();

      imageCachePtr->put(ImageCacheKey::full(coverResourceId), ao::gtk::test::makePixbuf(80, 80));

      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(button->get_child());
      REQUIRE(coverArt != nullptr);
      CHECK(button->get_visible());
      CHECK(coverArt->showingPlaceholder());

      startPlayback(fixture.runtime(), mutableCoverTrackId);
      ao::gtk::test::drainGtkEvents();

      REQUIRE(button->get_visible());
      auto const firstPaintablePtr = coverArt->imagePaintable();
      REQUIRE(firstPaintablePtr);

      auto callbacks = portal::ImportExportCallbacks{
        .requestLibraryRestoreConfirmation = [](rt::ImportReport const&, std::function<void(bool)> completion)
        { completion(true); },
      };
      auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};
      auto const importPath = fixture.runtime().musicRoot() / "cover-import.yaml";
      auto const secondCover = ao::gtk::test::encodePng(ao::gtk::test::makePixbuf(96, 96));
      ao::gtk::test::installCoverCacheEntry(fixture.cacheDirectory(), secondCover);
      writeCoverImport(importPath, std::span<std::byte const>{secondCover});
      workflow.importFrom(importPath);
      REQUIRE(ao::gtk::test::pumpGtkEventsUntil(
        [&]
        {
          auto const currentPaintablePtr = coverArt->imagePaintable();
          return currentPaintablePtr && currentPaintablePtr != firstPaintablePtr;
        }));

      REQUIRE(button->get_visible());
      auto const secondPaintablePtr = coverArt->imagePaintable();
      REQUIRE(secondPaintablePtr);
      CHECK(secondPaintablePtr != firstPaintablePtr);

      writeCoverImport(importPath, std::nullopt);
      workflow.importFrom(importPath);
      REQUIRE(ao::gtk::test::pumpGtkEventsUntil([&] { return coverArt->showingPlaceholder(); }));

      CHECK(button->get_visible());
      CHECK(coverArt->showingPlaceholder());
    }
  }

  TEST_CASE("ComponentTooltipController - copies only popover shell classes", "[gtk][unit][layout][component]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.tooltip_controller_test");

    auto target = Gtk::Button{};
    auto tooltipBox = Gtk::Box{};
    tooltipBox.add_css_class("ao-popover-transparent");
    tooltipBox.add_css_class("ao-opacity-80");

    auto tooltipComponent = StaticWidgetComponent{tooltipBox};
    auto controller = ComponentTooltipController{};
    controller.attach(target, tooltipComponent);

    auto* const popover = findWidget<Gtk::Popover>(target);
    REQUIRE(popover != nullptr);
    CHECK(popover->has_css_class("ao-popover-transparent"));
    CHECK_FALSE(popover->has_css_class("ao-opacity-80"));
  }

  TEST_CASE("ComponentTooltipController - detaches target controller on destruction",
            "[gtk][unit][layout-component][regression]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.tooltip_controller_lifecycle_test");

    auto target = Gtk::Button{};
    auto tooltipBox = Gtk::Box{};
    auto tooltipComponent = StaticWidgetComponent{tooltipBox};

    {
      auto controller = ComponentTooltipController{};
      controller.attach(target, tooltipComponent);

      CHECK(hasController<Gtk::EventControllerMotion>(target));
      CHECK(findWidget<Gtk::Popover>(target) != nullptr);
    }

    CHECK_FALSE(hasController<Gtk::EventControllerMotion>(target));
    CHECK(findWidget<Gtk::Popover>(target) == nullptr);
  }
} // namespace ao::gtk::layout::test
