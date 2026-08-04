// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace ao::winui::layout
{
  /**
   * @brief Narrow library-session capabilities available during shell construction.
   *
   * The owning ShellBuilder stores this object so generation components may
   * retain individual callbacks without borrowing a temporary build context.
   */
  struct ShellLibraryAccess final
  {
    std::filesystem::path libraryRoot;
    std::function<uimodel::ListTreeProjection()> listTreeProjection;
    std::function<std::optional<rt::TrackPresentationSpec>(ListId)> preferredPresentation;
    std::function<void(ListId, std::string)> rememberPresentation;
    std::function<Result<>(rt::ViewId, TrackId)> playTrack;
  };
} // namespace ao::winui::layout
