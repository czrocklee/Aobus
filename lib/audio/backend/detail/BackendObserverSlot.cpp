// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendObserverSlot.h"

#include <ao/utility/CallbackStackScope.h>

#include <mutex>

namespace ao::audio::backend::detail
{
  BackendObserverCalls::Borrow::~Borrow()
  {
    auto const lock = std::scoped_lock{calls._mutex};
    --calls._active;

    if (calls._closed && calls._active == 0)
    {
      calls._quiescent.notify_all();
    }
  }

  void BackendObserverCalls::retire() noexcept
  {
    auto lock = std::unique_lock{_mutex};
    _closed = true;

    if (!utility::CallbackStackScope::containsIdentity(this))
    {
      _quiescent.wait(lock, [this] { return _active == 0; });
    }
  }
} // namespace ao::audio::backend::detail
