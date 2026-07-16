// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/runtime/ComponentTooltipController.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/LibraryImportExportWorkflow.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/Base64.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/picture.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>

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

    std::string encodedPng(Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
    {
      gchar* rawBuffer = nullptr;
      gsize bufferSize = 0;
      pixbufPtr->save_to_buffer(rawBuffer, bufferSize, "png");
      auto bufferPtr = std::unique_ptr<gchar, decltype(&::g_free)>{rawBuffer, &::g_free};
      auto const bytes = std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(bufferPtr.get()), static_cast<std::size_t>(bufferSize)};
      return utility::base64Encode(bytes);
    }

    void writeCoverImport(std::filesystem::path const& path, std::optional<std::string_view> optEncodedCover)
    {
      auto output = std::ofstream{path};
      REQUIRE(output);
      output << "version: 2\n"
                "export_mode: full\n"
                "library:\n"
                "  tracks:\n"
                "    - uri: mutable-cover.flac\n";

      if (optEncodedCover)
      {
        output << "      covers:\n"
                  "        - type: 3\n"
                  "          data: "
               << *optEncodedCover << '\n';
      }

      output << "  lists: []\n";
      REQUIRE(output.good());
    }
  } // namespace

  TEST_CASE("PlaybackImage - applies declarative image properties", "[gtk][unit][image]")
  {
    auto mutableCoverTrackId = kInvalidTrackId;
    auto coverTrackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.playback_image_test",
                                        [&](library::MusicLibrary& musicLibrary)
                                        {
                                          auto const fixtureUri = audio::test::installAudioFixture(
                                            musicLibrary.rootPath(), "basic_metadata.flac", "cover-track.flac");
                                          mutableCoverTrackId =
                                            library::test::addTrack(musicLibrary,
                                                                    library::test::TrackSpec{
                                                                      .title = "Mutable Cover Track",
                                                                      .uri = fixtureUri,
                                                                      .coverArtId = ResourceId{42},
                                                                      .duration = std::chrono::seconds{1},
                                                                    });
                                          coverTrackId = library::test::addTrack(musicLibrary,
                                                                                 library::test::TrackSpec{
                                                                                   .title = "Cover Track",
                                                                                   .uri = fixtureUri,
                                                                                   .coverArtId = ResourceId{42},
                                                                                   .duration = std::chrono::seconds{1},
                                                                                 });
                                        }};
    auto imageCachePtr = std::make_unique<ImageCache>(10);
    auto& ctx = fixture.context();
    fixture.dependencies().imageCache = imageCachePtr.get();

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

      CHECK_FALSE(picture->has_css_class("ao-nowplaying-image-thumb"));

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
      auto* const picture = dynamic_cast<Gtk::Picture*>(slot->get_first_child());
      REQUIRE(picture != nullptr);

      CHECK(button->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(picture->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK_FALSE(widget.get_hexpand());
      CHECK_FALSE(widget.get_vexpand());

      std::int32_t buttonWidth = 0;
      std::int32_t buttonHeight = 0;
      button->get_size_request(buttonWidth, buttonHeight);
      CHECK(buttonWidth == -1);
      CHECK(buttonHeight == -1);

      std::int32_t width = 0;
      std::int32_t height = 0;
      picture->get_size_request(width, height);
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
      CHECK(picture->get_width() == 60);
      CHECK(picture->get_height() == 60);
      CHECK(button->get_opacity() == Catch::Approx{0.5}.margin(0.01));
    }

    SECTION("now playing cover art drives the image resource")
    {
      rt::test::addReadyAudioProvider(fixture.runtime().playback());
      ao::gtk::test::drainGtkEvents();

      auto const coverArtId = ResourceId{42};
      imageCachePtr->put(ImageCacheKey::full(coverArtId), ao::gtk::test::makePixbuf(80, 80));

      auto const throwingSubscription = fixture.runtime().playback().onNowPlayingChanged(
        [](rt::PlaybackService::NowPlayingChanged const&)
        { throwException<Exception>("scripted observer failure before cover art"); });

      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const picture = dynamic_cast<Gtk::Picture*>(button->get_child());
      REQUIRE(picture != nullptr);

      REQUIRE(fixture.runtime().playback().playTrack(coverTrackId, rt::kAllTracksListId));
      ao::gtk::test::drainGtkEvents();

      auto const paintablePtr = picture->get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 64 * picture->get_scale_factor());
      CHECK(paintablePtr->get_intrinsic_height() == 64 * picture->get_scale_factor());

      std::ignore = fixture.runtime().playback().stop();
      ao::gtk::test::drainGtkEvents();

      CHECK_FALSE(button->get_visible());
      CHECK_FALSE(picture->get_paintable());
    }

    SECTION("current track cover art follows library mutations")
    {
      rt::test::addReadyAudioProvider(fixture.runtime().playback());
      ao::gtk::test::drainGtkEvents();

      auto const firstCoverArtId = ResourceId{42};
      auto const secondCoverArtId = ResourceId{43};
      imageCachePtr->put(ImageCacheKey::full(firstCoverArtId), ao::gtk::test::makePixbuf(80, 80));
      imageCachePtr->put(ImageCacheKey::full(secondCoverArtId), ao::gtk::test::makePixbuf(96, 96));

      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto* const picture = dynamic_cast<Gtk::Picture*>(button->get_child());
      REQUIRE(picture != nullptr);
      CHECK_FALSE(button->get_visible());

      REQUIRE(fixture.runtime().playback().playTrack(mutableCoverTrackId, rt::kAllTracksListId));
      ao::gtk::test::drainGtkEvents();

      REQUIRE(button->get_visible());
      auto const firstPaintablePtr = picture->get_paintable();
      REQUIRE(firstPaintablePtr);

      std::int32_t importCount = 0;
      auto callbacks = portal::ImportExportCallbacks{
        .onLibraryDataMutated = [&importCount] { ++importCount; },
        .requestLibraryRestoreConfirmation = [](rt::ImportReport const&, std::function<void(bool)> completion)
        { completion(true); },
      };
      auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};
      auto const importPath = fixture.runtime().musicRoot() / "cover-import.yaml";
      auto const secondCover = encodedPng(ao::gtk::test::makePixbuf(96, 96));
      writeCoverImport(importPath, secondCover);
      workflow.importFrom(importPath);
      REQUIRE(ao::gtk::test::pumpGtkEventsUntil([&importCount] { return importCount == 1; }));

      REQUIRE(button->get_visible());
      auto const secondPaintablePtr = picture->get_paintable();
      REQUIRE(secondPaintablePtr);
      CHECK(secondPaintablePtr != firstPaintablePtr);

      writeCoverImport(importPath, std::nullopt);
      workflow.importFrom(importPath);
      REQUIRE(ao::gtk::test::pumpGtkEventsUntil([&importCount] { return importCount == 2; }));

      CHECK_FALSE(button->get_visible());
      CHECK_FALSE(picture->get_paintable());
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
