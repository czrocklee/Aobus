// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/CallbackAdmissionGate.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace ao::winui::test
{
  static_assert(!std::is_copy_constructible_v<CallbackAdmissionGate>);
  static_assert(!std::is_move_constructible_v<CallbackAdmissionGate>);

  TEST_CASE("CallbackAdmissionGate - retirement invalidates existing callback tokens", "[winui][unit][lifetime]")
  {
    auto const emptyToken = CallbackAdmissionGate::Token{};
    CHECK_FALSE(emptyToken.admits());

    auto gate = CallbackAdmissionGate{};
    auto const token = gate.token();

    REQUIRE(gate.isOpen());
    REQUIRE(token.admits());

    gate.retire();
    gate.retire();

    CHECK_FALSE(gate.isOpen());
    CHECK_FALSE(token.admits());
  }

  TEST_CASE("CallbackAdmissionGate - renewal cannot reopen an older generation", "[winui][unit][lifetime]")
  {
    auto gate = CallbackAdmissionGate{};
    auto const oldToken = gate.token();

    gate.renew();
    auto const currentToken = gate.token();

    CHECK(gate.isOpen());
    CHECK_FALSE(oldToken.admits());
    CHECK(currentToken.admits());

    gate.retire();
    CHECK_FALSE(gate.isOpen());
    CHECK_FALSE(currentToken.admits());
  }
} // namespace ao::winui::test
