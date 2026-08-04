// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <ao/Error.h>
#include <ao/Exception.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    Result<> succeeds()
    {
      return {};
    }

    Result<> fails()
    {
      return makeError(Error::Code::InitFailed, "native attachment failed");
    }

    Result<> throws()
    {
      throwException<Exception>("native attachment threw");
    }
  } // namespace

  TEST_CASE("ShellGenerationSequence - a staged candidate stays closed until it is published",
            "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const candidatePtr = sequence.stage();

    CHECK(sequence.activeId() == ShellGenerationId::None);
    CHECK(sequence.stagedCount() == 1);
    CHECK_FALSE(candidatePtr->isActive());

    auto const retired = sequence.publish(candidatePtr->id(), succeeds);

    REQUIRE(retired.has_value());
    CHECK(*retired == ShellGenerationId::None);
    CHECK(sequence.activeId() == candidatePtr->id());
    CHECK(sequence.stagedCount() == 0);
    CHECK(candidatePtr->isActive());
  }

  TEST_CASE("ShellGenerationSequence - publication reports the generation the caller must destroy",
            "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const firstPtr = sequence.stage();
    REQUIRE(sequence.publish(firstPtr->id(), succeeds).has_value());

    auto const secondPtr = sequence.stage();
    // The outgoing generation stays live while the candidate is constructed.
    CHECK(firstPtr->isActive());
    CHECK_FALSE(secondPtr->isActive());

    auto const retired = sequence.publish(secondPtr->id(), succeeds);

    REQUIRE(retired.has_value());
    CHECK(*retired == firstPtr->id());
    CHECK(sequence.activeId() == secondPtr->id());
    CHECK(secondPtr->isActive());
    CHECK_FALSE(firstPtr->isActive());
  }

  TEST_CASE("ShellGenerationSequence - a failed attachment restores the previous generation",
            "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const firstPtr = sequence.stage();
    REQUIRE(sequence.publish(firstPtr->id(), succeeds).has_value());

    auto const candidatePtr = sequence.stage();
    auto const published = sequence.publish(candidatePtr->id(), fails);

    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().code == Error::Code::InitFailed);
    CHECK(sequence.activeId() == firstPtr->id());
    CHECK(firstPtr->isActive());
    CHECK_FALSE(candidatePtr->isActive());
    CHECK(sequence.stagedCount() == 0);
  }

  TEST_CASE("ShellGenerationSequence - a throwing attachment restores the previous generation",
            "[uimodel][regression][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const firstPtr = sequence.stage();
    REQUIRE(sequence.publish(firstPtr->id(), succeeds).has_value());

    auto const candidatePtr = sequence.stage();
    auto const published = sequence.publish(candidatePtr->id(), throws);

    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().code == Error::Code::InitFailed);
    CHECK(sequence.activeId() == firstPtr->id());
    CHECK(firstPtr->isActive());
    CHECK_FALSE(candidatePtr->isActive());
    CHECK(sequence.stagedCount() == 0);
  }

  TEST_CASE("ShellGenerationSequence - a discarded candidate can never be published", "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const candidatePtr = sequence.stage();
    sequence.discard(candidatePtr->id());

    CHECK(sequence.stagedCount() == 0);
    CHECK_FALSE(candidatePtr->isActive());

    auto const published = sequence.publish(candidatePtr->id(), succeeds);

    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().code == Error::Code::InvalidState);
    CHECK(sequence.activeId() == ShellGenerationId::None);
  }

  TEST_CASE("ShellGenerationSequence - generation ids are never reused", "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto seen = std::vector<ShellGenerationId>{};

    for (std::int32_t pass = 0; pass < 4; ++pass)
    {
      auto const candidatePtr = sequence.stage();
      REQUIRE(sequence.publish(candidatePtr->id(), succeeds).has_value());
      seen.push_back(candidatePtr->id());
    }

    auto const discardedPtr = sequence.stage();
    sequence.discard(discardedPtr->id());
    seen.push_back(discardedPtr->id());

    for (std::size_t index = 1; index < seen.size(); ++index)
    {
      CHECK(seen[index] != seen[index - 1]);
      CHECK(seen[index] > seen[index - 1]);
    }
  }

  TEST_CASE("isGenerationActive - late callbacks from a retired generation are suppressed",
            "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto firstPtr = sequence.stage();
    REQUIRE(sequence.publish(firstPtr->id(), succeeds).has_value());

    // A view-local asynchronous operation captures its generation weakly.
    auto const pendingPtr = std::weak_ptr<ShellGenerationGate>{firstPtr};
    CHECK(isGenerationActive(pendingPtr));

    auto const secondPtr = sequence.stage();
    REQUIRE(sequence.publish(secondPtr->id(), succeeds).has_value());
    CHECK_FALSE(isGenerationActive(pendingPtr));

    // The frontend destroys the retired view, so even the gate goes away.
    firstPtr.reset();
    CHECK_FALSE(isGenerationActive(pendingPtr));
    CHECK(isGenerationActive(std::weak_ptr<ShellGenerationGate>{secondPtr}));
  }

  TEST_CASE("ShellGenerationSequence - teardown closes the live generation's gate", "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    CHECK(sequence.retireActive() == ShellGenerationId::None);

    auto const activePtr = sequence.stage();
    REQUIRE(sequence.publish(activePtr->id(), succeeds).has_value());

    CHECK(sequence.retireActive() == activePtr->id());
    CHECK(sequence.activeId() == ShellGenerationId::None);
    CHECK_FALSE(activePtr->isActive());
    CHECK_FALSE(isGenerationActive(std::weak_ptr<ShellGenerationGate>{activePtr}));
  }

  TEST_CASE("isGenerationActive - a staged generation cannot run callbacks yet", "[uimodel][unit][layout][shell]")
  {
    auto sequence = ShellGenerationSequence{};
    auto const candidatePtr = sequence.stage();

    CHECK_FALSE(isGenerationActive(std::weak_ptr<ShellGenerationGate>{candidatePtr}));
    CHECK_FALSE(isGenerationActive(std::weak_ptr<ShellGenerationGate>{}));
  }
} // namespace ao::uimodel::test
