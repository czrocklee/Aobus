// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

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

    std::optional<Gdk::RGBA> renderedGradientBody(Gtk::Window& parent, AobusSoul& soul)
    {
      auto snapshotPtr = Gtk::Snapshot::create();
      parent.snapshot_child(soul, snapshotPtr);
      auto nodePtr = std::unique_ptr<::GskRenderNode, RenderNodeDeleter>{::gtk_snapshot_to_node(snapshotPtr->gobj())};
      return gradientBodyColor(nodePtr.get());
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

  TEST_CASE("AobusSoul - renders widget state and applies presentation setters", "[gtk][unit][app][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto soul = AobusSoul{};

    SECTION("initial widget state")
    {
      CHECK(soul.get_visible() == true);
      CHECK(soul.has_css_class("ao-soul"));
      CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Dormant);
      CHECK_FALSE(soul.shouldShowFullLogo());
    }

    SECTION("motion mode controls animation state")
    {
      soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
      CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Animating);
      CHECK_FALSE(soul.isTickActive());

      soul.setMotionMode(uimodel::AobusSoulMotionMode::Frozen);
      CHECK(soul.motionMode() == uimodel::AobusSoulMotionMode::Frozen);
      CHECK_FALSE(soul.isTickActive());
    }

    SECTION("tick lifecycle follows mapped breathing state")
    {
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

    SECTION("setAura updates color")
    {
      auto color = Gdk::RGBA{"#ff0000"};
      soul.setAura(color);
      CHECK(soul.aura() == color);
    }

    SECTION("presentation geometry setters retain custom values")
    {
      soul.setBaseStrokeWidth(5.0F);
      soul.setInnerGlyphScale(0.85F);

      CHECK(soul.baseStrokeWidth() == 5.0F);
      CHECK(soul.innerGlyphScale() == 0.85F);
    }

    SECTION("brand aura tokens map to source-of-truth colors")
    {
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Dormant) == Gdk::RGBA{"#00E5FF"});
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Veiled) == Gdk::RGBA{"#6B7280"});
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Radiant) == Gdk::RGBA{"#A855F7"});
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Flowing) == Gdk::RGBA{"#10B981"});
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Turbulent) == Gdk::RGBA{"#F59E0B"});
      CHECK(AobusSoul::mapSoulAura(uimodel::SoulAura::Burning) == Gdk::RGBA{"#EF4444"});
    }

    SECTION("setShowFullLogo updates render state")
    {
      soul.setShowFullLogo(true);
      CHECK(soul.shouldShowFullLogo());

      soul.setShowFullLogo(false);
      CHECK_FALSE(soul.shouldShowFullLogo());
    }

    SECTION("Gtk::Widget sizing contract")
    {
      std::int32_t min = -1;
      std::int32_t nat = -1;
      std::int32_t minB = -1;
      std::int32_t natB = -1;
      soul.measure(Gtk::Orientation::HORIZONTAL, 100, min, nat, minB, natB);
      CHECK(min >= 0);
      CHECK(nat >= 0);

      CHECK(soul.get_request_mode() == Gtk::SizeRequestMode::CONSTANT_SIZE);
    }
  }

  TEST_CASE("AobusSoul - paused motion freezes the drawn frame while quality aura remains live",
            "[gtk][regression][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto soul = AobusSoul{};
    soul.set_size_request(65, 65);
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(soul);
    windowFixture.present();
    soul.setAura(Gdk::RGBA{"#A855F7"});
    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);

    REQUIRE(pumpGtkEventsUntil([&soul] { return soul.visualFrame().motion.rotationDegrees > 0.1; }));
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
    CHECK(recolored.motion == frozen.motion);
    CHECK(recolored.gradientColors ==
          uimodel::aobusSoulGradientColors(uimodel::kAobusSoulTurbulent, frozen.motion.hueShiftDegrees));
    auto const optTurbulent = renderedGradientBody(windowFixture.window(), soul);
    REQUIRE(optTurbulent);
    CHECK(*optTurbulent == rgbaFromSoulRgb(recolored.gradientColors.body));

    soul.setMotionMode(uimodel::AobusSoulMotionMode::Animating);
    REQUIRE(soul.isTickActive());
    CHECK(pumpGtkEventsUntil([&soul, frozen] { return soul.visualFrame().motion != frozen.motion; }));
  }
} // namespace ao::gtk::test
