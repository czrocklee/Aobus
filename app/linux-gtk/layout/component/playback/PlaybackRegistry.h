// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
}
namespace ao::uimodel
{
  class PlaybackCommandSurface;
  class OutputDeviceIntent;
}
namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::gtk
{
  class ResourceImageLoader;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  /**
   * @brief Register playback-related semantic components.
   */
  void registerPlaybackComponents(ComponentRegistry& registry,
                                  rt::AppRuntime& runtime,
                                  uimodel::PlaybackCommandSurface* playbackCommandSurface,
                                  ResourceImageLoader* imageLoader,
                                  i18n::MessageCatalog const& textCatalog,
                                  uimodel::OutputDeviceIntent const& outputDeviceIntent);
} // namespace ao::gtk::layout
