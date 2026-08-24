// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/compat/AtomicSharedPtr.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace ao::compat::test
{
  namespace
  {
    // Targets the portable implementation directly: on Linux and Windows
    // ao::compat::AtomicSharedPtr resolves to std::atomic<std::shared_ptr<T>>, so
    // testing only the alias would leave this code covered on macOS alone.
    template<typename T>
    using Portable = detail::PortableAtomicSharedPtr<T>;
  } // namespace

  TEST_CASE("AtomicSharedPtr - a default constructed slot is empty", "[core][unit][atomic-shared-ptr]")
  {
    auto slot = Portable<std::int32_t>{};
    CHECK(slot.load() == nullptr);
    CHECK_FALSE(slot.is_lock_free());
  }

  TEST_CASE("AtomicSharedPtr - stores and loads a value", "[core][unit][atomic-shared-ptr]")
  {
    auto slot = Portable<std::int32_t>{std::make_shared<std::int32_t>(3)};
    REQUIRE(slot.load() != nullptr);
    CHECK(*slot.load() == 3);

    slot.store(std::make_shared<std::int32_t>(9));
    CHECK(*slot.load() == 9);
  }

  TEST_CASE("AtomicSharedPtr - exchange returns the previous value", "[core][unit][atomic-shared-ptr]")
  {
    auto slot = Portable<std::int32_t>{std::make_shared<std::int32_t>(1)};

    auto const previousPtr = slot.exchange(std::make_shared<std::int32_t>(2));

    REQUIRE(previousPtr != nullptr);
    CHECK(*previousPtr == 1);
    CHECK(*slot.load() == 2);
  }

  TEST_CASE("AtomicSharedPtr - a stored value stays alive while the slot holds it", "[core][unit][atomic-shared-ptr]")
  {
    auto ownedPtr = std::make_shared<std::int32_t>(5);
    auto weakPtr = std::weak_ptr<std::int32_t>{ownedPtr};

    auto slot = Portable<std::int32_t>{std::move(ownedPtr)};
    CHECK_FALSE(weakPtr.expired());

    slot.store(nullptr);
    CHECK(weakPtr.expired());
  }

  TEST_CASE("AtomicSharedPtr - compare-exchange replaces a matching expectation", "[core][unit][atomic-shared-ptr]")
  {
    auto const initialPtr = std::make_shared<std::int32_t>(1);
    auto const replacementPtr = std::make_shared<std::int32_t>(2);
    auto slot = Portable<std::int32_t>{initialPtr};

    auto expectedPtr = initialPtr;
    CHECK(slot.compare_exchange_strong(expectedPtr, replacementPtr));
    // A successful exchange leaves the caller's expectation alone.
    CHECK(expectedPtr == initialPtr);
    CHECK(slot.load() == replacementPtr);
  }

  TEST_CASE("AtomicSharedPtr - compare-exchange reports the current value when the expectation is stale",
            "[core][unit][atomic-shared-ptr]")
  {
    auto const initialPtr = std::make_shared<std::int32_t>(1);
    auto slot = Portable<std::int32_t>{initialPtr};

    // Equal pointee, different owner: the slot value is the pointer, not what
    // it points at.
    auto stalePtr = std::make_shared<std::int32_t>(1);
    CHECK_FALSE(slot.compare_exchange_strong(stalePtr, std::make_shared<std::int32_t>(2)));
    CHECK(stalePtr == initialPtr);
    CHECK(slot.load() == initialPtr);
  }

  TEST_CASE("AtomicSharedPtr - compare-exchange rejects an unrelated owner of the same object",
            "[core][unit][atomic-shared-ptr]")
  {
    std::int32_t object = 7;
    auto const keepAlive = [](std::int32_t*) noexcept {};
    auto const heldPtr = std::shared_ptr<std::int32_t>{&object, keepAlive};
    auto unrelatedPtr = std::shared_ptr<std::int32_t>{&object, keepAlive};
    REQUIRE(heldPtr.get() == unrelatedPtr.get());

    auto slot = Portable<std::int32_t>{heldPtr};

    // Same stored pointer, independent control blocks. Comparing get() alone
    // would call this a match.
    CHECK_FALSE(slot.compare_exchange_strong(unrelatedPtr, std::make_shared<std::int32_t>(9)));
    CHECK(slot.load() == heldPtr);
    // Failure hands back the ownership the slot actually holds.
    CHECK_FALSE(unrelatedPtr.owner_before(heldPtr));
    CHECK_FALSE(heldPtr.owner_before(unrelatedPtr));
  }

  TEST_CASE("AtomicSharedPtr - compare-exchange matches an empty slot against an empty expectation",
            "[core][unit][atomic-shared-ptr]")
  {
    auto slot = Portable<std::int32_t>{};
    auto const replacementPtr = std::make_shared<std::int32_t>(5);

    auto expectedPtr = std::shared_ptr<std::int32_t>{};
    CHECK(slot.compare_exchange_strong(expectedPtr, replacementPtr));
    CHECK(slot.load() == replacementPtr);
  }

  TEST_CASE("AtomicSharedPtr - the weak form does not fail spuriously here", "[core][unit][atomic-shared-ptr]")
  {
    auto const initialPtr = std::make_shared<std::int32_t>(3);
    auto slot = Portable<std::int32_t>{initialPtr};

    // Serialised by a mutex, so a single attempt suffices where the standard
    // would only promise progress across a loop.
    auto expectedPtr = initialPtr;
    CHECK(slot.compare_exchange_weak(expectedPtr, std::make_shared<std::int32_t>(4)));
    CHECK(*slot.load() == 4);
  }

  TEST_CASE("AtomicSharedPtr - the project alias behaves the same", "[core][unit][atomic-shared-ptr]")
  {
    auto slot = AtomicSharedPtr<std::int32_t>{std::make_shared<std::int32_t>(4)};
    REQUIRE(slot.load() != nullptr);
    CHECK(*slot.load() == 4);

    slot.store(std::make_shared<std::int32_t>(6));
    CHECK(*slot.load() == 6);

    // Compare-exchange through the alias is what pins the two implementations
    // together: this runs against std::atomic<std::shared_ptr<T>> wherever the
    // standard library has it and against the portable class on libc++.
    auto expectedPtr = slot.load();
    auto const replacementPtr = std::make_shared<std::int32_t>(8);
    CHECK(slot.compare_exchange_strong(expectedPtr, replacementPtr));
    CHECK(slot.load() == replacementPtr);

    auto stalePtr = std::make_shared<std::int32_t>(8);
    CHECK_FALSE(slot.compare_exchange_strong(stalePtr, std::make_shared<std::int32_t>(9)));
    CHECK(stalePtr == replacementPtr);
    CHECK(slot.load() == replacementPtr);
  }

  TEST_CASE("AtomicSharedPtr - exposes the same explicit API on every standard library",
            "[core][unit][atomic-shared-ptr]")
  {
    using Slot = AtomicSharedPtr<std::int32_t>;
    using Pointer = std::shared_ptr<std::int32_t>;

    STATIC_CHECK_FALSE(std::is_convertible_v<Slot, Pointer>);
    STATIC_CHECK_FALSE(std::is_assignable_v<Slot&, Pointer>);
  }
} // namespace ao::compat::test
