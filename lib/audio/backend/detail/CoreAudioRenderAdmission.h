// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "CallbackFence.h"

#include <cstdint>
#include <optional>

namespace ao::audio::backend::detail
{
  class CoreAudioRunControl;
  class CoreAudioControlMailbox;

  enum class CoreAudioFault : std::uint8_t;

  // Binds an admitted render interval to the run-control generation token that
  // was published before admission opened. open() and closeAndWait() run on
  // the control thread only and must be externally serialized with each other.
  // open() quiesces the closed fence before it writes the token and publishes
  // the token before it releases admission, so an admitted callback always
  // observes the token of the interval that admitted it, even after the
  // control generation has already advanced. closeAndWait() never touches the
  // token: pause, drain, and stop settle callbacks without rewriting the
  // captured interval identity. Every Scope must end before this object is destroyed;
  // a Scope never extends its owner's lifetime.
  class CoreAudioRenderAdmission final
  {
  public:
    // Move-only borrowed ownership of one admitted callback. The destructor
    // returns the fence borrow unless this scope was moved from. A moved-from
    // scope holds no borrow: its destructor stays inert, and using its
    // generation or publishing through it is a fatal realtime contract error.
    class [[nodiscard]] Scope final
    {
    public:
      Scope(Scope const&) = delete;
      Scope& operator=(Scope const&) = delete;
      Scope& operator=(Scope&&) = delete;

      Scope(Scope&& other) noexcept;
      ~Scope();

      std::uint64_t generation() const noexcept;

      // Publishes a render-origin fault under the interval token captured at
      // admission. Realtime safe: no lock, no allocation, and no current()
      // resampling. The captured generation must still be current to publish;
      // worker and target-delivery generation checks remain separate.
      bool tryPublishFault(CoreAudioRunControl const& runControl,
                           CoreAudioControlMailbox& mailbox,
                           CoreAudioFault fault) const noexcept;

    private:
      friend class CoreAudioRenderAdmission;
      Scope(CoreAudioRenderAdmission& admission, std::uint64_t generation) noexcept;

      CoreAudioRenderAdmission* _admission;
      std::uint64_t _generation;
    };

    // Realtime callback entry: admits only while an interval is open and
    // captures the interval token only after successful admission.
    std::optional<Scope> tryEnter() noexcept;

    // Control thread only, externally serialized: activates admission for a
    // nonzero run generation. Rejects an already active interval before any
    // token write, then quiesces the closed fence before publishing the new
    // token and opening admission.
    void open(std::uint64_t generation) noexcept;

    // Control thread only, externally serialized: closes admission and waits
    // for the admitted callbacks to settle. The interval token stays valid.
    void closeAndWait() noexcept;

  private:
    CallbackFence _fence{};
    std::uint64_t _generation = 0U;
  };
} // namespace ao::audio::backend::detail
