// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/compat/MoveOnlyFunction.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ao::compat::test
{
  namespace
  {
    // The bulk of the suite targets the portable implementation directly: on
    // Linux and Windows ao::compat::MoveOnlyFunction resolves to
    // std::move_only_function, so testing only the alias would leave this code
    // covered on macOS alone.
    template<typename Signature>
    using Portable = detail::PortableMoveOnlyFunction<Signature>;

    /**
     * @brief Callable that reports how many instances are alive.
     *
     * Instance counting rather than destruction counting keeps the assertions
     * valid for both storage paths: an inline target is move-constructed into
     * its new home and the old one destroyed, while a heap target only has its
     * pointer relocated.
     */
    template<std::size_t Padding>
    class LiveCounter final
    {
    public:
      explicit LiveCounter(std::int32_t& liveCount, std::int32_t value)
        : _liveCount{&liveCount}, _value{value}
      {
        ++*_liveCount;
      }

      LiveCounter(LiveCounter&& other) noexcept
        : _liveCount{other._liveCount}, _value{other._value}
      {
        ++*_liveCount;
      }

      LiveCounter(LiveCounter const&) = delete;
      LiveCounter& operator=(LiveCounter const&) = delete;
      LiveCounter& operator=(LiveCounter&&) = delete;

      ~LiveCounter() { --*_liveCount; }

      std::int32_t operator()() const { return _value; }

    private:
      std::int32_t* _liveCount;
      std::int32_t _value;
      [[maybe_unused]] std::array<std::byte, Padding> _padding{};
    };

    // Sized to land on opposite sides of the inline-storage threshold.
    using SmallCounter = LiveCounter<1>;
    using LargeCounter = LiveCounter<128>;

    std::int32_t freeFunction()
    {
      return 7;
    }
  } // namespace

  TEST_CASE("MoveOnlyFunction - a default constructed function is empty", "[core][unit][move-only-function]")
  {
    auto function = Portable<void()>{};
    CHECK_FALSE(function);
    CHECK(function == nullptr);

    auto fromNull = Portable<void()>{nullptr};
    CHECK_FALSE(fromNull);
  }

  TEST_CASE("MoveOnlyFunction - invokes its target and returns the result", "[core][unit][move-only-function]")
  {
    auto add = Portable<std::int32_t(std::int32_t, std::int32_t)>{[](std::int32_t left, std::int32_t right)
                                                                  { return left + right; }};

    REQUIRE(add);
    CHECK(add(2, 3) == 5);
  }

  TEST_CASE("MoveOnlyFunction - holds a move-only capture", "[core][unit][move-only-function]")
  {
    auto ownedPtr = std::make_unique<std::int32_t>(42);
    auto function = Portable<std::int32_t()>{[heldPtr = std::move(ownedPtr)] { return *heldPtr; }};

    REQUIRE(function);
    CHECK(function() == 42);
  }

  TEST_CASE("MoveOnlyFunction - forwards arguments without copying", "[core][unit][move-only-function]")
  {
    auto append = Portable<void(std::string&)>{[](std::string& text) { text += "-seen"; }};

    auto text = std::string{"value"};
    append(text);
    CHECK(text == "value-seen");
  }

  TEST_CASE("MoveOnlyFunction - a null function pointer produces an empty function", "[core][unit][move-only-function]")
  {
    std::int32_t (*target)() = nullptr;
    auto function = Portable<std::int32_t()>{target};
    CHECK_FALSE(function);

    auto valid = Portable<std::int32_t()>{&freeFunction};
    REQUIRE(valid);
    CHECK(valid() == 7);
  }

  TEST_CASE("MoveOnlyFunction - moving empties the source and keeps one live target",
            "[core][unit][move-only-function]")
  {
    std::int32_t liveCount = 0;

    {
      auto source = Portable<std::int32_t()>{SmallCounter{liveCount, 11}};
      REQUIRE(source);
      CHECK(liveCount == 1);

      auto moved = std::move(source);
      CHECK_FALSE(source); // NOLINT(bugprone-use-after-move)
      REQUIRE(moved);
      CHECK(moved() == 11);
      CHECK(liveCount == 1);
    }

    CHECK(liveCount == 0);
  }

  TEST_CASE("MoveOnlyFunction - a large target survives moves and is released once", "[core][unit][move-only-function]")
  {
    std::int32_t liveCount = 0;

    {
      auto source = Portable<std::int32_t()>{LargeCounter{liveCount, 23}};
      REQUIRE(source);
      CHECK(liveCount == 1);

      auto moved = std::move(source);
      REQUIRE(moved);
      CHECK(moved() == 23);
      CHECK(liveCount == 1);
    }

    CHECK(liveCount == 0);
  }

  TEST_CASE("MoveOnlyFunction - move assignment releases the previous target", "[core][unit][move-only-function]")
  {
    std::int32_t replacedLiveCount = 0;
    std::int32_t adoptedLiveCount = 0;

    {
      auto function = Portable<std::int32_t()>{SmallCounter{replacedLiveCount, 1}};
      auto replacement = Portable<std::int32_t()>{LargeCounter{adoptedLiveCount, 2}};

      REQUIRE(replacedLiveCount == 1);
      REQUIRE(adoptedLiveCount == 1);

      function = std::move(replacement);

      CHECK(replacedLiveCount == 0);
      CHECK(adoptedLiveCount == 1);
      REQUIRE(function);
      CHECK(function() == 2);
    }

    CHECK(adoptedLiveCount == 0);
  }

  TEST_CASE("MoveOnlyFunction - assigning nullptr empties the function", "[core][unit][move-only-function]")
  {
    std::int32_t liveCount = 0;

    auto function = Portable<std::int32_t()>{SmallCounter{liveCount, 5}};
    REQUIRE(function);
    REQUIRE(liveCount == 1);

    function = nullptr;
    CHECK_FALSE(function);
    CHECK(liveCount == 0);
  }

  TEST_CASE("MoveOnlyFunction - swap exchanges targets", "[core][unit][move-only-function]")
  {
    auto first = Portable<std::int32_t()>{[] { return 1; }};
    auto second = Portable<std::int32_t()>{[] { return 2; }};

    swap(first, second);

    CHECK(first() == 2);
    CHECK(second() == 1);
  }
} // namespace ao::compat::test

namespace ao::compat::test
{
  TEST_CASE("MoveOnlyFunction - the project alias satisfies the same contract", "[core][unit][move-only-function]")
  {
    auto empty = MoveOnlyFunction<std::int32_t()>{};
    CHECK_FALSE(empty);

    auto ownedPtr = std::make_unique<std::int32_t>(19);
    auto function = MoveOnlyFunction<std::int32_t()>{[heldPtr = std::move(ownedPtr)] { return *heldPtr; }};

    REQUIRE(function);
    CHECK(function() == 19);

    auto moved = std::move(function);
    REQUIRE(moved);
    CHECK(moved() == 19);
  }
} // namespace ao::compat::test
