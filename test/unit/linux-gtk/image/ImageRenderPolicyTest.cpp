// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageRenderPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("ImageRenderPolicy - fits source dimensions into render target", "[gtk][unit][image]")
  {
    SECTION("landscape source is constrained by target width")
    {
      auto const source = RenderTarget{.width = 1000, .height = 500};
      auto const target = RenderTarget{.width = 200, .height = 200};
      auto const fit = fitSourceIntoTarget(source, target);

      CHECK(fit.width == 200);
      CHECK(fit.height == 100);
    }

    SECTION("portrait source is constrained by target height")
    {
      auto const source = RenderTarget{.width = 500, .height = 1000};
      auto const target = RenderTarget{.width = 200, .height = 200};
      auto const fit = fitSourceIntoTarget(source, target);

      CHECK(fit.width == 100);
      CHECK(fit.height == 200);
    }

    SECTION("small source is not upscaled")
    {
      auto const source = RenderTarget{.width = 40, .height = 40};
      auto const target = RenderTarget{.width = 80, .height = 80};
      auto const fit = fitSourceIntoTarget(source, target);

      CHECK(fit.width == 40);
      CHECK(fit.height == 40);
    }

    SECTION("square source fits square target exactly")
    {
      auto const source = RenderTarget{.width = 100, .height = 100};
      auto const target = RenderTarget{.width = 50, .height = 50};
      auto const fit = fitSourceIntoTarget(source, target);

      CHECK(fit.width == 50);
      CHECK(fit.height == 50);
    }
  }

  TEST_CASE("ImageRenderPolicy - refreshes only after meaningful target-size changes", "[gtk][unit][image]")
  {
    auto const current = RenderTarget{.width = 100, .height = 100};

    CHECK_FALSE(shouldRefresh(current, RenderTarget{.width = 100, .height = 100}));
    CHECK_FALSE(shouldRefresh(current, RenderTarget{.width = 104, .height = 100}));
    CHECK_FALSE(shouldRefresh(current, RenderTarget{.width = 100, .height = 104}));
    CHECK(shouldRefresh(current, RenderTarget{.width = 105, .height = 100}));
    CHECK(shouldRefresh(current, RenderTarget{.width = 100, .height = 105}));

    auto const small = RenderTarget{.width = 20, .height = 20};
    CHECK_FALSE(shouldRefresh(small, RenderTarget{.width = 21, .height = 20}));
    CHECK(shouldRefresh(small, RenderTarget{.width = 22, .height = 20}));

    auto const large = RenderTarget{.width = 400, .height = 400};
    CHECK_FALSE(shouldRefresh(large, RenderTarget{.width = 419, .height = 419}));
    CHECK(shouldRefresh(large, RenderTarget{.width = 420, .height = 400}));

    auto const odd = RenderTarget{.width = 101, .height = 101};
    CHECK_FALSE(shouldRefresh(odd, RenderTarget{.width = 106, .height = 101}));
    CHECK(shouldRefresh(odd, RenderTarget{.width = 107, .height = 101}));
  }
} // namespace ao::gtk::test
