// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>

#include <memory>

namespace ao::uimodel
{
  struct LayoutNode;
}
namespace ao::rt
{
  class ResourceByteLoader;
  class WorkspaceService;
}
namespace ao::async
{
  class Runtime;
}
namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::winui
{
  class ThemeCoordinator;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;

  /**
   * @brief Build the cover art of whatever the focused view has selected.
   *
   * The component owns its artwork end to end: it reads the shared detail
   * projection, hands the resource to its own presenter, and draws the authored
   * placeholder style when the selection carries no artwork. Its size is not
   * its own, though - the frame gives it a width, and it keeps itself square
   * inside whatever it is given.
   */
  Result<std::unique_ptr<LayoutComponent>> makeTrackCoverArt(LayoutBuildContext& ctx,
                                                             uimodel::LayoutNode const& node,
                                                             async::Runtime& asyncRuntime,
                                                             rt::WorkspaceService& workspace,
                                                             rt::ResourceByteLoader& resourceBytes,
                                                             ThemeCoordinator& theme,
                                                             i18n::MessageCatalog textCatalog);
} // namespace ao::winui::layout
