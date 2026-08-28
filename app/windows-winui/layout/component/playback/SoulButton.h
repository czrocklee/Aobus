// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>

#include <memory>

namespace ao::uimodel
{
  struct ComponentSchema;
  struct LayoutNode;
  class PlaybackActions;
}
namespace ao::rt
{
  class PlaybackService;
}
namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::async
{
  template<typename... Args>
  class Signal;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;
  struct WindowActivityState;

  /**
   * @brief Build the soul: the animated disc every shell puts at its centre.
   *
   * The soul always reports playback, and additionally drives it when the
   * document leaves its primary click alone. Whichever role it takes, it carries
   * the audio pipeline explanation, because that belongs to the soul rather than
   * to the shell that placed it.
   */
  Result<std::unique_ptr<LayoutComponent>> makeSoulButton(LayoutBuildContext& ctx,
                                                          uimodel::LayoutNode const& node,
                                                          uimodel::ComponentSchema const& schema,
                                                          rt::PlaybackService& playback,
                                                          uimodel::PlaybackActions& playbackActions,
                                                          i18n::MessageCatalog const& textCatalog,
                                                          async::Signal<WindowActivityState>& windowActivityChanged);
} // namespace ao::winui::layout
