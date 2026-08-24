// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

namespace ao::compat::detail
{
  /**
   * @brief Portable stand-in for std::atomic<std::shared_ptr<T>>.
   *
   * libc++ does not implement the shared_ptr partial specialization of
   * std::atomic, so naming it there selects the primary template and fails the
   * "T must be trivially copyable" mandate. This supplies the subset the
   * project uses: load, store, exchange and compare-exchange. Only the TUI's
   * Windows signal watcher compares-and-exchanges today, and that translation
   * unit never selects this class -- which is why the operation is implemented
   * and tested here rather than left out until something misses it.
   *
   * Serialised by a mutex rather than being genuinely lock-free. That matches
   * how libstdc++ and libc++ implement shared_ptr atomics internally (both use
   * a lock table), so is_lock_free() reports false and callers that would
   * deadlock taking a lock must not use this in a signal handler -- the fatal
   * path's async-signal-safe writer in lib/utility/Fatal.cpp is separate.
   *
   * Defined on every platform rather than only where it is selected, so the
   * implementation is compiled and unit tested everywhere instead of only on
   * the one platform that has no alternative.
   *
   * Retirement condition: doc/development/macos-portability.md.
   */
  template<typename T>
  class PortableAtomicSharedPtr final
  {
  public:
    using value_type = std::shared_ptr<T>;

    // Standard spelling is intentional: the selected implementation is a
    // drop-in subset of std::atomic<std::shared_ptr<T>>.
    static constexpr bool is_always_lock_free = false; // NOLINT(readability-identifier-naming)

    PortableAtomicSharedPtr() noexcept = default;

    PortableAtomicSharedPtr(std::shared_ptr<T> desired) noexcept
      : _value{std::move(desired)}
    {
    }

    PortableAtomicSharedPtr(PortableAtomicSharedPtr const&) = delete;
    PortableAtomicSharedPtr& operator=(PortableAtomicSharedPtr const&) = delete;
    PortableAtomicSharedPtr(PortableAtomicSharedPtr&&) = delete;
    PortableAtomicSharedPtr& operator=(PortableAtomicSharedPtr&&) = delete;

    ~PortableAtomicSharedPtr() = default;

    bool is_lock_free() const noexcept // NOLINT(readability-identifier-naming) -- standard compatibility API
    {
      return false;
    }

    std::shared_ptr<T> load([[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      auto const lock = std::scoped_lock{_mutex};
      return _value;
    }

    void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      // Routed through exchange rather than assigning under the lock. The
      // replaced pointer may be the last owner of its object, and running ~T()
      // inside _mutex would order whatever locks that destructor takes against
      // this one. exchange already hands the old value back, so the temporary
      // here dies after the lock is released.
      std::ignore = exchange(std::move(desired), order);
    }

    std::shared_ptr<T> exchange(std::shared_ptr<T> desired,
                                [[maybe_unused]] std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      auto const lock = std::scoped_lock{_mutex};
      return std::exchange(_value, std::move(desired));
    }

    /**
     * @brief Replaces the held pointer when it is equivalent to @p expected.
     *
     * Returns false and reports the current value through @p expected
     * otherwise, matching std::atomic<std::shared_ptr<T>>.
     */
    bool compare_exchange_strong( // NOLINT(readability-identifier-naming) -- standard compatibility API
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      [[maybe_unused]] std::memory_order success = std::memory_order_seq_cst,
      [[maybe_unused]] std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
      // Whichever pointer this call drops -- the replaced value on success, the
      // caller's stale expectation on failure -- is released after the lock,
      // for the reason store() explains.
      auto discardedPtr = std::shared_ptr<T>{};
      bool succeeded = false;
      {
        auto const lock = std::scoped_lock{_mutex};

        if (equivalent(_value, expected))
        {
          discardedPtr = std::exchange(_value, std::move(desired));
          succeeded = true;
        }
        else
        {
          discardedPtr = std::exchange(expected, _value);
        }
      }

      return succeeded;
    }

    /**
     * @brief Serialised compare-exchange, which therefore never fails spuriously.
     *
     * The standard permits the weak form to fail even on a match, so callers
     * already loop; being total here only ends those loops sooner.
     */
    bool compare_exchange_weak( // NOLINT(readability-identifier-naming) -- standard compatibility API
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success = std::memory_order_seq_cst,
      std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
      return compare_exchange_strong(expected, std::move(desired), success, failure);
    }

  private:
    /**
     * @brief Reports the equivalence std::atomic<std::shared_ptr<T>> compares by.
     *
     * [util.smartptr.atomic.shared] calls two shared_ptr objects equivalent
     * when they store the same pointer and either share ownership or are both
     * empty. Comparing get() alone would also accept two independent control
     * blocks aimed at one object, which is a different slot value; mutual
     * owner_before separates those, and is false both ways for empty pointers.
     */
    static bool equivalent(std::shared_ptr<T> const& left, std::shared_ptr<T> const& right) noexcept
    {
      return left.get() == right.get() && !left.owner_before(right) && !right.owner_before(left);
    }

    mutable std::mutex _mutex;
    std::shared_ptr<T> _value;
  };

#ifdef __cpp_lib_atomic_shared_ptr
  template<typename T>
  using AtomicSharedPtrBackend = std::atomic<std::shared_ptr<T>>;
#else
  template<typename T>
  using AtomicSharedPtrBackend = PortableAtomicSharedPtr<T>;
#endif
} // namespace ao::compat::detail

namespace ao::compat
{
  /**
   * @brief Atomically replaceable shared_ptr slot.
   *
   * The public surface is intentionally identical on every platform. Its
   * backend is std::atomic<std::shared_ptr<T>> where available and the portable
   * implementation above on libc++. Keeping the backend behind this wrapper
   * prevents implicit std::atomic conversion/assignment syntax from compiling
   * on one standard library and failing only when the same code reaches macOS.
   */
  template<typename T>
  class AtomicSharedPtr final
  {
  public:
    using value_type = std::shared_ptr<T>;

    static constexpr bool is_always_lock_free = // NOLINT(readability-identifier-naming)
      detail::AtomicSharedPtrBackend<T>::is_always_lock_free;

    AtomicSharedPtr() noexcept = default;

    AtomicSharedPtr(std::shared_ptr<T> desired) noexcept
      : _backend{std::move(desired)}
    {
    }

    AtomicSharedPtr(AtomicSharedPtr const&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr const&) = delete;
    AtomicSharedPtr(AtomicSharedPtr&&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

    ~AtomicSharedPtr() = default;

    bool is_lock_free() const noexcept // NOLINT(readability-identifier-naming) -- standard compatibility API
    {
      return _backend.is_lock_free();
    }

    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      return _backend.load(order);
    }

    void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      _backend.store(std::move(desired), order);
    }

    std::shared_ptr<T> exchange(std::shared_ptr<T> desired,
                                std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return _backend.exchange(std::move(desired), order);
    }

    bool compare_exchange_strong( // NOLINT(readability-identifier-naming) -- standard compatibility API
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success = std::memory_order_seq_cst,
      std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
      return _backend.compare_exchange_strong(expected, std::move(desired), success, failure);
    }

    bool compare_exchange_weak( // NOLINT(readability-identifier-naming) -- standard compatibility API
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success = std::memory_order_seq_cst,
      std::memory_order failure = std::memory_order_seq_cst) noexcept
    {
      return _backend.compare_exchange_weak(expected, std::move(desired), success, failure);
    }

  private:
    detail::AtomicSharedPtrBackend<T> _backend;
  };
} // namespace ao::compat
