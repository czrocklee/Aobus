// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ao::compat::detail
{
  /**
   * @brief Portable stand-in for std::move_only_function.
   *
   * libc++ still does not implement std::move_only_function -- through LLVM 22
   * its <version> header keeps __cpp_lib_move_only_function commented out -- so
   * a macOS build cannot name the standard type. This supplies the subset the
   * project uses: the unqualified R(Args...) form, with no const, ref or
   * noexcept qualified signatures.
   *
   * Callables up to kStorageSize that move without throwing are stored inline;
   * anything larger goes to the heap. Calling an empty instance is a
   * precondition violation, matching the standard type.
   *
   * Defined on every platform rather than only where it is selected, so the
   * implementation is compiled and unit tested everywhere instead of only on
   * the one platform that has no alternative.
   *
   * Retirement condition: doc/development/macos-portability.md.
   */
  template<typename Signature>
  class PortableMoveOnlyFunction;

  template<typename R, typename... Args>
  class PortableMoveOnlyFunction<R(Args...)> final
  {
  public:
    PortableMoveOnlyFunction() noexcept = default;
    PortableMoveOnlyFunction(std::nullptr_t) noexcept {}

    template<typename F, typename Callable = std::decay_t<F>>
      requires(!std::is_same_v<Callable, PortableMoveOnlyFunction> && std::is_invocable_r_v<R, Callable&, Args...>)
    PortableMoveOnlyFunction(F&& callable)
    {
      if constexpr (std::is_pointer_v<Callable> || std::is_member_pointer_v<Callable>)
      {
        // The standard type is empty when built from a null pointer, and
        // ScopedRegistration-style `if (function)` guards depend on that.
        if (callable == nullptr)
        {
          return;
        }
      }

      construct<Callable>(std::forward<F>(callable));
    }

    PortableMoveOnlyFunction(PortableMoveOnlyFunction const&) = delete;
    PortableMoveOnlyFunction& operator=(PortableMoveOnlyFunction const&) = delete;

    PortableMoveOnlyFunction(PortableMoveOnlyFunction&& other) noexcept { adopt(other); }

    PortableMoveOnlyFunction& operator=(PortableMoveOnlyFunction&& other) noexcept
    {
      if (this != &other)
      {
        reset();
        adopt(other);
      }

      return *this;
    }

    ~PortableMoveOnlyFunction() { reset(); }

    PortableMoveOnlyFunction& operator=(std::nullptr_t) noexcept
    {
      reset();
      return *this;
    }

    explicit operator bool() const noexcept { return _operations != nullptr; }

    // A generic parameter pack cannot adopt type-specific naming after instantiation.
    R operator()(Args... args) // NOLINT(aobus-readability-result-naming-convention)
    {
      return _operations->invoke(_storage.data(), std::forward<Args>(args)...);
    }

    void swap(PortableMoveOnlyFunction& other) noexcept
    {
      PortableMoveOnlyFunction held = std::move(other);
      other = std::move(*this);
      *this = std::move(held);
    }

    friend void swap(PortableMoveOnlyFunction& left, PortableMoveOnlyFunction& right) noexcept { left.swap(right); }

    friend bool operator==(PortableMoveOnlyFunction const& function, std::nullptr_t) noexcept { return !function; }

  private:
    static constexpr std::size_t kStorageSize = 4 * sizeof(void*);

    template<typename Callable>
    static constexpr bool kStoredInline =
      sizeof(Callable) <= kStorageSize && alignof(Callable) <= alignof(std::max_align_t) &&
      std::is_nothrow_move_constructible_v<Callable>;

    struct Operations final
    {
      R (*invoke)(std::byte*, Args&&...);
      void (*relocate)(std::byte*, std::byte*) noexcept;
      void (*destroy)(std::byte*) noexcept;
    };

    template<typename Callable>
    static Callable* target(std::byte* storage) noexcept
    {
      if constexpr (kStoredInline<Callable>)
      {
        // The callable's lifetime was started in this aligned byte storage.
        return std::launder(
          reinterpret_cast<Callable*>(storage)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      }
      else
      {
        // The heap branch stores its owning pointer in the same byte storage.
        return *std::launder(
          reinterpret_cast<Callable**>(storage)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      }
    }

    template<typename Callable>
    static Operations const* operationsFor() noexcept
    {
      static constexpr Operations kOperations{
        [](std::byte* storage, Args&&... args) -> R // NOLINT(aobus-readability-chrono-naming-convention)
        {
          if constexpr (std::is_void_v<R>)
          {
            std::invoke(*target<Callable>(storage), std::forward<Args>(args)...);
          }
          else
          {
            return std::invoke(*target<Callable>(storage), std::forward<Args>(args)...);
          }
        },
        [](std::byte* destination, std::byte* source) noexcept
        {
          if constexpr (kStoredInline<Callable>)
          {
            ::new (static_cast<void*>(destination)) Callable{std::move(*target<Callable>(source))};
            target<Callable>(source)->~Callable();
          }
          else
          {
            ::new (static_cast<void*>(destination)) Callable*(target<Callable>(source));
          }
        },
        [](std::byte* storage) noexcept
        {
          if constexpr (kStoredInline<Callable>)
          {
            target<Callable>(storage)->~Callable();
          }
          else
          {
            std::default_delete<Callable>{}(target<Callable>(storage));
          }
        }};
      return &kOperations;
    }

    template<typename Callable, typename F>
    void construct(F&& callable)
    {
      if constexpr (kStoredInline<Callable>)
      {
        ::new (static_cast<void*>(_storage.data())) Callable{std::forward<F>(callable)};
      }
      else
      {
        auto callablePtr = std::make_unique<Callable>(std::forward<F>(callable));
        ::new (static_cast<void*>(_storage.data())) Callable*(callablePtr.release());
      }

      _operations = operationsFor<Callable>();
    }

    void adopt(PortableMoveOnlyFunction& other) noexcept
    {
      if (other._operations != nullptr)
      {
        other._operations->relocate(_storage.data(), other._storage.data());
        _operations = std::exchange(other._operations, nullptr);
      }
    }

    void reset() noexcept
    {
      if (_operations != nullptr)
      {
        _operations->destroy(_storage.data());
        _operations = nullptr;
      }
    }

    alignas(std::max_align_t) std::array<std::byte, kStorageSize> _storage{};
    Operations const* _operations = nullptr;
  };
} // namespace ao::compat::detail

namespace ao::compat
{
  /**
   * @brief Owning, move-only type-erased callable.
   *
   * Resolves to std::move_only_function wherever the standard library provides
   * it (libstdc++, Microsoft STL) and to the portable implementation above on
   * libc++. Naming this alias instead of the standard type is what keeps the
   * macOS build possible; the day libc++ implements the paper, call sites
   * switch over with no edit.
   */
#ifdef __cpp_lib_move_only_function
  template<typename Signature>
  using MoveOnlyFunction = std::move_only_function<Signature>;
#else
  template<typename Signature>
  using MoveOnlyFunction = detail::PortableMoveOnlyFunction<Signature>;
#endif
} // namespace ao::compat
