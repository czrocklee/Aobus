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
  class PlaybackService;
  class ResourceByteMemoryCache;
}
namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::async
{
  class Runtime;
  template<typename... Args>
  class Signal;
}
namespace ao::winui
{
  struct ShellState;
  class ThemeCoordinator;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;
  /**
   * @brief Build the now playing summary: cover art beside the title and artist.
   *
   * The cover is decoded and painted by a presenter the component owns, because
   * a presenter holds the very elements the generation is made of and has no
   * state worth carrying across a rebuild.
   */
  Result<std::unique_ptr<LayoutComponent>> makeNowPlayingInfo(LayoutBuildContext& ctx,
                                                              uimodel::LayoutNode const& node,
                                                              async::Runtime& asyncRuntime,
                                                              rt::PlaybackService& playback,
                                                              rt::ResourceByteMemoryCache& resourceBytes,
                                                              ThemeCoordinator& theme,
                                                              i18n::MessageCatalog const& textCatalog,
                                                              async::Signal<ShellState>& shellStateChanged);
} // namespace ao::winui::layout
