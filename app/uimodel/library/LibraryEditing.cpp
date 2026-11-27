// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/LibraryEditing.h>

#include <ao/rt/library/LibraryCommands.h>

namespace ao::uimodel
{
  rt::Library const& LibraryEditing::library() const noexcept
  {
    return _commands.get().library();
  }
} // namespace ao::uimodel
