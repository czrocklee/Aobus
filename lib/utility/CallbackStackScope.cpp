// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/CallbackStackScope.h>

#include <ao/Contract.h>

namespace ao::utility
{
  CallbackStackScope::CallbackStackScope(void const* const identityRaw) noexcept
    : _identityRaw{identityRaw}, _previousRaw{current()}
  {
    AO_INVARIANT(_identityRaw != nullptr, "CallbackStackScope identity must not be null");
    current() = this;
  }

  CallbackStackScope::~CallbackStackScope()
  {
    AO_INVARIANT(current() == this, "CallbackStackScope must be destroyed in LIFO order on its construction thread");
    current() = _previousRaw;
  }

  bool CallbackStackScope::containsIdentity(void const* const identityRaw) noexcept
  {
    AO_INVARIANT(identityRaw != nullptr, "CallbackStackScope lookup identity must not be null");

    for (auto const* scopeRaw = current(); scopeRaw != nullptr; scopeRaw = scopeRaw->_previousRaw)
    {
      if (scopeRaw->_identityRaw == identityRaw)
      {
        return true;
      }
    }

    return false;
  }

  CallbackStackScope*& CallbackStackScope::current() noexcept
  {
    static thread_local CallbackStackScope* scopeRaw = nullptr;
    return scopeRaw;
  }
} // namespace ao::utility
