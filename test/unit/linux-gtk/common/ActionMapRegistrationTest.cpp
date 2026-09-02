// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/ActionMapRegistration.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <stdexcept>

namespace ao::gtk::test
{
  TEST_CASE("ActionMapRegistration - reset revokes retained actions and preserves replacements",
            "[gtk][unit][action-registration]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto actionGroupPtr = Gio::SimpleActionGroup::create();
    std::int32_t oldActivationCount = 0;
    std::int32_t replacementActivationCount = 0;

    auto oldActionPtr = Gio::SimpleAction::create("example");
    auto registration = ActionMapRegistration{*actionGroupPtr, 1};
    registration.add(oldActionPtr, [&oldActivationCount](Glib::VariantBase const&) { ++oldActivationCount; });
    oldActionPtr->activate();
    REQUIRE(oldActivationCount == 1);

    auto replacementActionPtr = Gio::SimpleAction::create("example");
    auto replacementRegistration = ActionMapRegistration{*actionGroupPtr, 1};
    replacementRegistration.add(
      replacementActionPtr, [&replacementActivationCount](Glib::VariantBase const&) { ++replacementActivationCount; });

    registration.reset();
    auto const currentActionPtr = actionGroupPtr->lookup_action("example");
    REQUIRE(currentActionPtr);
    CHECK(currentActionPtr.get() == replacementActionPtr.get());

    oldActionPtr->activate();
    CHECK(oldActivationCount == 1);
    replacementActionPtr->activate();
    CHECK(replacementActivationCount == 1);
  }

  TEST_CASE("ActionMapRegistration - removal observation can reenter reset safely",
            "[gtk][regression][action-registration]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto actionGroupPtr = Gio::SimpleActionGroup::create();
    auto actionPtr = Gio::SimpleAction::create("reentrant");
    auto registration = ActionMapRegistration{*actionGroupPtr, 1};
    registration.add(actionPtr, [](Glib::VariantBase const&) {});
    std::int32_t removalCount = 0;
    auto removalConnection = sigc::scoped_connection{actionGroupPtr->signal_action_removed().connect(
      [&registration, &removalCount](Glib::ustring const&)
      {
        ++removalCount;
        registration.reset();
      })};

    registration.reset();

    CHECK(removalCount == 1);
    CHECK(actionGroupPtr->lookup_action("reentrant") == nullptr);
  }

  TEST_CASE("ActionMapRegistration - stack unwinding cleans a partial registration",
            "[gtk][regression][action-registration]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto actionGroupPtr = Gio::SimpleActionGroup::create();
    auto retainedActionPtr = Gio::SimpleAction::create("partial");
    std::int32_t activationCount = 0;

    auto registerPartially = [&]
    {
      auto registration = ActionMapRegistration{*actionGroupPtr, 2};
      registration.add(retainedActionPtr, [&activationCount](Glib::VariantBase const&) { ++activationCount; });
      throw std::runtime_error{"registration failure"};
    };

    REQUIRE_THROWS_AS(registerPartially(), std::runtime_error);
    CHECK(actionGroupPtr->lookup_action("partial") == nullptr);

    retainedActionPtr->activate();
    CHECK(activationCount == 0);
  }
} // namespace ao::gtk::test
