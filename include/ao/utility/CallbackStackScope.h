// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::utility
{
  /**
   * @brief Allocation-free membership in the current thread's nested callback stack.
   *
   * @pre @p identityRaw is non-null and identifies an object that remains alive at the same address
   *      until this scope is destroyed. The scope observes only that address and does not extend the
   *      object's lifetime.
   * @pre The scope is destroyed on the thread that constructed it.
   * @pre Scopes on one thread are destroyed in strict reverse construction order.
   */
  class [[nodiscard]] CallbackStackScope final
  {
  public:
    explicit CallbackStackScope(void const* identityRaw) noexcept;
    ~CallbackStackScope();

    CallbackStackScope(CallbackStackScope const&) = delete;
    CallbackStackScope& operator=(CallbackStackScope const&) = delete;
    CallbackStackScope(CallbackStackScope&&) = delete;
    CallbackStackScope& operator=(CallbackStackScope&&) = delete;

    static bool containsIdentity(void const* identityRaw) noexcept;

  private:
    static CallbackStackScope*& current() noexcept;

    void const* _identityRaw = nullptr;
    CallbackStackScope* _previousRaw = nullptr;
  };
} // namespace ao::utility
