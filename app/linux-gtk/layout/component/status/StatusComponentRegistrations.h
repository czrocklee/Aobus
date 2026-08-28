// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
  class NotificationService;
  class LibraryJobs;
  class PlaybackService;
  class ViewService;
}
namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerPlaybackDetailsComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        i18n::MessageCatalog const& textCatalog);
  void registerNowPlayingStatusComponent(ComponentRegistry& registry,
                                         rt::PlaybackService& playback,
                                         i18n::MessageCatalog const& textCatalog);
  void registerActivityStatusComponent(ComponentRegistry& registry,
                                       rt::NotificationService& notifications,
                                       rt::LibraryJobs& libraryJobs,
                                       i18n::MessageCatalog const& textCatalog);
  void registerSelectionInfoComponent(ComponentRegistry& registry,
                                      rt::ViewService& views,
                                      i18n::MessageCatalog const& textCatalog);
  void registerLibraryTrackCountComponent(ComponentRegistry& registry,
                                          rt::AppRuntime& runtime,
                                          i18n::MessageCatalog const& textCatalog);
  void registerStatusMessageLabelComponent(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout
