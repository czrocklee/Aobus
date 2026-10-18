// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioRenderAdmission.h"

#include "backend/detail/CoreAudioControlMailbox.h"
#include "backend/detail/CoreAudioRunControl.h"
#include <ao/Contract.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace ao::audio::backend::detail
{
  CoreAudioRenderAdmission::Scope::Scope(CoreAudioRenderAdmission& admission, std::uint64_t const generation) noexcept
    : _admission{&admission}, _generation{generation}
  {
  }

  CoreAudioRenderAdmission::Scope::Scope(Scope&& other) noexcept
    : _admission{std::exchange(other._admission, nullptr)}, _generation{other._generation}
  {
  }

  CoreAudioRenderAdmission::Scope::~Scope()
  {
    if (_admission != nullptr)
    {
      _admission->_fence.leave();
    }
  }

  std::uint64_t CoreAudioRenderAdmission::Scope::generation() const noexcept
  {
    AO_RT_INVARIANT(_admission != nullptr, "A moved-from render admission scope holds no interval borrow");
    return _generation;
  }

  bool CoreAudioRenderAdmission::Scope::tryPublishFault(CoreAudioRunControl const& runControl,
                                                        CoreAudioControlMailbox& mailbox,
                                                        CoreAudioFault const fault) const noexcept
  {
    AO_RT_INVARIANT(_admission != nullptr, "A moved-from render admission scope cannot publish render faults");

    // No current() resampling: only the token of the interval that admitted
    // this callback may reach the mailbox.
    if (!runControl.isCurrent(_generation))
    {
      return false;
    }

    return mailbox.tryPublishFault(_generation, fault);
  }

  std::optional<CoreAudioRenderAdmission::Scope> CoreAudioRenderAdmission::tryEnter() noexcept
  {
    if (!_fence.tryEnter())
    {
      return std::nullopt;
    }

    // Capture the token only after successful admission: the value written by
    // the last open() is stable until the next quiesced write, and the fence
    // release in open() publishes that write to this acquire.
    return Scope{*this, _generation};
  }

  void CoreAudioRenderAdmission::open(std::uint64_t const generation) noexcept
  {
    AO_EXPECTS(generation != 0U, "A render admission generation must be nonzero");
    AO_EXPECTS(!_fence.isOpen(), "Cannot open render admission while a previous interval is still active");

    // Quiesce the closed interval before the token write: the callbacks
    // admitted by the previous interval are the only token readers, and the
    // fence release below is the only publisher of the new token.
    _fence.wait();

    _generation = generation;
    _fence.open();
  }

  void CoreAudioRenderAdmission::closeAndWait() noexcept
  {
    // The interval token intentionally survives closure: settling callbacks
    // keep their identity across pause, drain, and stop alike.
    _fence.closeAndWait();
  }
} // namespace ao::audio::backend::detail
