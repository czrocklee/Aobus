// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <functional>

namespace ao::rt
{
  class Library;
  class LibraryCommands;
}

namespace ao::uimodel
{
  /**
   * Borrowed entry point for UIModel library authoring workflows.
   *
   * The runtime owning the commands must outlive every copy and any submitted
   * operation. Callers can read from the library but cannot extract the writer.
   */
  class LibraryEditing final
  {
  public:
    explicit LibraryEditing(rt::LibraryCommands& commands) noexcept
      : _commands{commands}
    {
    }

    rt::Library const& library() const noexcept;

  private:
    std::reference_wrapper<rt::LibraryCommands> _commands;

    friend class TrackAuthoringSession;
    friend class ListMembershipAuthoringSession;
    friend class ListOrderAuthoringSession;
    friend struct ListAuthoringAccess;
  };
} // namespace ao::uimodel
