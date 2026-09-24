// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gdkmm/rgba.h>
#include <gsk/gsk.h>
#include <gtk/gtk.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    struct RenderNodeDeleter final
    {
      void operator()(::GskRenderNode* node) const noexcept { ::gsk_render_node_unref(node); }
    };

    std::optional<Gdk::RGBA> gradientBodyColor(::GskRenderNode const* node)
    {
      if (node == nullptr)
      {
        return std::nullopt;
      }

      switch (::gsk_render_node_get_node_type(node))
      {
        case GSK_LINEAR_GRADIENT_NODE:
        {
          ::gsize stopCount = 0;
          auto const* const stops = ::gsk_linear_gradient_node_get_color_stops(node, &stopCount);

          if (stopCount == 0)
          {
            return std::nullopt;
          }

          auto const& body = stops[stopCount - 1].color;
          return Gdk::RGBA{body.red, body.green, body.blue, body.alpha};
        }
        case GSK_CONTAINER_NODE:
        {
          auto const childCount = ::gsk_container_node_get_n_children(node);

          for (::guint childIndex = 0; childIndex < childCount; ++childIndex)
          {
            if (auto const optColor = gradientBodyColor(::gsk_container_node_get_child(node, childIndex)); optColor)
            {
              return optColor;
            }
          }

          break;
        }
        case GSK_TRANSFORM_NODE: return gradientBodyColor(::gsk_transform_node_get_child(node));
        case GSK_STROKE_NODE: return gradientBodyColor(::gsk_stroke_node_get_child(node));
        case GSK_OPACITY_NODE: return gradientBodyColor(::gsk_opacity_node_get_child(node));
        default: break;
      }

      return std::nullopt;
    }

    std::unique_ptr<::GskRenderNode, RenderNodeDeleter> snapshotSoul(Gtk::Window& parent, AobusSoul& soul)
    {
      auto snapshotPtr = Gtk::Snapshot::create();
      parent.snapshot_child(soul, snapshotPtr);
      return std::unique_ptr<::GskRenderNode, RenderNodeDeleter>{::gtk_snapshot_to_node(snapshotPtr->gobj())};
    }

    std::optional<Gdk::RGBA> renderedGradientBody(Gtk::Window& parent, AobusSoul& soul)
    {
      auto const nodePtr = snapshotSoul(parent, soul);
      return gradientBodyColor(nodePtr.get());
    }

    struct StrokeGeometry final
    {
      float width = 0.0F;
      ::graphene_rect_t bounds{};
    };

    std::vector<StrokeGeometry> strokeGeometry(::GskRenderNode* node)
    {
      if (node == nullptr)
      {
        return {};
      }

      switch (::gsk_render_node_get_node_type(node))
      {
        case GSK_STROKE_NODE:
        {
          auto stroke = StrokeGeometry{.width = ::gsk_stroke_get_line_width(::gsk_stroke_node_get_stroke(node))};
          ::gsk_render_node_get_bounds(node, &stroke.bounds);
          return {stroke};
        }
        case GSK_CONTAINER_NODE:
        {
          auto strokes = std::vector<StrokeGeometry>{};

          for (::guint i = 0; i < ::gsk_container_node_get_n_children(node); ++i)
          {
            auto const child = strokeGeometry(::gsk_container_node_get_child(node, i));
            strokes.insert(strokes.end(), child.begin(), child.end());
          }

          return strokes;
        }
        case GSK_TRANSFORM_NODE:
        {
          auto strokes = strokeGeometry(::gsk_transform_node_get_child(node));

          for (auto& stroke : strokes)
          {
            auto transformed = ::graphene_rect_t{};
            ::gsk_transform_transform_bounds(::gsk_transform_node_get_transform(node), &stroke.bounds, &transformed);
            stroke.bounds = transformed;
          }

          return strokes;
        }
        case GSK_OPACITY_NODE: return strokeGeometry(::gsk_opacity_node_get_child(node));
        default: return {};
      }
    }

    std::vector<StrokeGeometry> renderedStrokes(Gtk::Window& parent, AobusSoul& soul)
    {
      auto const nodePtr = snapshotSoul(parent, soul);
      return strokeGeometry(nodePtr.get());
    }

    Gdk::RGBA rgbaFromSoulRgb(uimodel::AobusSoulRgb const color)
    {
      constexpr float kMaxChannel = 255.0F;
      return Gdk::RGBA{static_cast<float>(color.red) / kMaxChannel,
                       static_cast<float>(color.green) / kMaxChannel,
                       static_cast<float>(color.blue) / kMaxChannel,
                       1.0F};
    }
  } // namespace

  TEST_CASE("AobusSoul - starts dormant without a minimum allocation", "[gtk][unit][app][soul][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    CHECK(soul.get_visible());
    CHECK(soul.has_css_class("ao-soul"));
    CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Dormant);
    CHECK_FALSE(soul.shouldShowFullLogo());

    std::int32_t min = -1;
    std::int32_t nat = -1;
    std::int32_t minB = -1;
    std::int32_t natB = -1;
    soul.measure(Gtk::Orientation::HORIZONTAL, 100, min, nat, minB, natB);
    CHECK(min == 0);
    CHECK(nat == 0);
    CHECK(soul.get_request_mode() == Gtk::SizeRequestMode::CONSTANT_SIZE);
  }

  TEST_CASE("AobusSoul - tick lifecycle follows mapped breathing state", "[gtk][unit][app][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
    CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Animating);
    CHECK_FALSE(soul.isTickActive());
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Frozen);
    CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Frozen);
    CHECK_FALSE(soul.isTickActive());

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
    CHECK_FALSE(soul.isTickActive());
    windowFixture.present();
    CHECK(soul.isTickActive());
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Frozen);
    CHECK_FALSE(soul.isTickActive());
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
    CHECK(soul.isTickActive());
    windowFixture.unmount();
    CHECK_FALSE(soul.isTickActive());
  }

  TEST_CASE("AobusSoul - paints the requested aura", "[gtk][unit][app][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.set_size_request(65, 65);
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    windowFixture.present();

    auto const color = Gdk::RGBA{"#ff0000"};
    soul.setAura(color);
    CHECK(soul.aura() == color);
    auto const expected = uimodel::aobusSoulVisualFrame({255, 0, 0}, soul.visualFrame().motion);
    auto const optRendered = renderedGradientBody(windowFixture.window(), soul);
    REQUIRE(optRendered);
    CHECK(*optRendered == rgbaFromSoulRgb(expected.gradientColors.body));
  }

  TEST_CASE("AobusSoul - geometry setters update rendered glyph strokes", "[gtk][unit][app][soul][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.set_size_request(65, 65);
    soul.setInnerGlyph(AobusSoul::InnerGlyph::Sigil);
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    windowFixture.present();

    auto const original = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(original.size() == 2);
    CHECK(original[1].width == Catch::Approx(9.0F / 65.0F));

    soul.setBaseStrokeWidth(5.0F);
    CHECK(soul.baseStrokeWidth() == 5.0F);
    auto const narrowed = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(narrowed.size() == 2);
    CHECK(narrowed[1].width == Catch::Approx(5.0F / 65.0F));

    soul.setInnerGlyphScale(0.85F);
    CHECK(soul.innerGlyphScale() == 0.85F);
    auto const scaled = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(scaled.size() == 2);
    CHECK(scaled[1].width == narrowed[1].width);
    CHECK(scaled[1].bounds.size.width == Catch::Approx(narrowed[1].bounds.size.width * 0.85F));
    CHECK(scaled[1].bounds.size.height == Catch::Approx(narrowed[1].bounds.size.height * 0.85F));
    CHECK(scaled[0].bounds.size.width == narrowed[0].bounds.size.width);
    CHECK(scaled[0].bounds.size.height == narrowed[0].bounds.size.height);
  }

  TEST_CASE("AobusSoul - maps brand aura tokens to native colors", "[gtk][unit][app][soul]")
  {
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Dormant) == Gdk::RGBA{"#00E5FF"});
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Veiled) == Gdk::RGBA{"#6B7280"});
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Radiant) == Gdk::RGBA{"#A855F7"});
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Flowing) == Gdk::RGBA{"#10B981"});
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Turbulent) == Gdk::RGBA{"#F59E0B"});
    CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Burning) == Gdk::RGBA{"#EF4444"});
  }

  TEST_CASE("AobusSoul - full-logo mode adds and removes the rendered anchor", "[gtk][unit][app][soul][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.set_size_request(65, 65);
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    windowFixture.present();
    auto const original = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(original.size() == 1);

    soul.setShowFullLogo(true);
    CHECK(soul.shouldShowFullLogo());
    auto const fullLogo = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(fullLogo.size() == 2);
    CHECK(fullLogo[0].width == Catch::Approx(10.0F / 65.0F));
    CHECK(fullLogo[0].bounds.origin.x < fullLogo[1].bounds.origin.x);

    soul.setShowFullLogo(false);
    CHECK_FALSE(soul.shouldShowFullLogo());
    auto const restored = renderedStrokes(windowFixture.window(), soul);
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].bounds.size.width == original[0].bounds.size.width);
    CHECK(restored[0].bounds.size.height == original[0].bounds.size.height);
  }

  TEST_CASE("AobusSoul - paused motion freezes the drawn frame while quality aura remains live",
            "[gtk][unit][app][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.set_size_request(65, 65);
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    windowFixture.present();
    soul.setAura(Gdk::RGBA{"#A855F7"});
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);

    REQUIRE(tryPumpGtkEventsUntil([&soul] { return soul.visualFrame().motion.rotationDegrees > 0.1; }));
    auto const animated = soul.visualFrame();

    soul.setMotionMode(uimodel::AobusSoulMotionMode::Frozen);

    REQUIRE(soul.motionMode() == uimodel::AobusSoulMotionMode::Frozen);
    REQUIRE_FALSE(soul.isTickActive());
    auto const frozen = soul.visualFrame();
    CHECK(frozen == animated);

    auto const optRadiant = renderedGradientBody(windowFixture.window(), soul);
    REQUIRE(optRadiant);
    CHECK(*optRadiant == rgbaFromSoulRgb(frozen.gradientColors.body));

    soul.setAura(Gdk::RGBA{"#F59E0B"});

    CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Frozen);
    CHECK_FALSE(soul.isTickActive());
    auto const recolored = soul.visualFrame();
    CHECK(recolored == uimodel::aobusSoulVisualFrame(uimodel::kAobusSoulTurbulent, frozen.motion));
    auto const optTurbulent = renderedGradientBody(windowFixture.window(), soul);
    REQUIRE(optTurbulent);
    CHECK(*optTurbulent == rgbaFromSoulRgb(recolored.gradientColors.body));

    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
    REQUIRE(soul.isTickActive());
    CHECK(tryPumpGtkEventsUntil([&soul, frozen] { return soul.visualFrame().motion != frozen.motion; }));
  }
} // namespace ao::gtk::test
