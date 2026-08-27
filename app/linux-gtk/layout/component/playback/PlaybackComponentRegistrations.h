// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::rt
{
  class AppRuntime;
  class PlaybackService;
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

  void registerOutputDeviceSelectorComponent(ComponentRegistry& registry,
                                             rt::PlaybackService& playback,
                                             i18n::MessageCatalog const& textCatalog,
                                             uimodel::OutputDeviceIntent const& outputDeviceIntent);
  void registerPlaybackImageComponent(ComponentRegistry& registry,
                                      rt::AppRuntime& runtime,
                                      ResourceImageLoader* imageLoader,
                                      i18n::MessageCatalog const& textCatalog);
  void registerSoulTransportButtonComponent(ComponentRegistry& registry,
                                            rt::PlaybackService& playback,
                                            uimodel::PlaybackCommandSurface* playbackCommandSurface,
                                            i18n::MessageCatalog const& textCatalog);
  void registerSoulButtonComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerTransportButtonComponent(ComponentRegistry& registry,
                                        rt::PlaybackService& playback,
                                        uimodel::PlaybackCommandSurface* playbackCommandSurface,
                                        i18n::MessageCatalog const& textCatalog);
  void registerVolumeControlComponent(ComponentRegistry& registry,
                                      rt::PlaybackService& playback,
                                      i18n::MessageCatalog const& textCatalog);
  void registerNowPlayingFieldComponent(ComponentRegistry& registry,
                                        rt::AppRuntime& runtime,
                                        i18n::MessageCatalog const& textCatalog);
  void registerSeekSliderComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerTimeLabelComponent(ComponentRegistry& registry, rt::PlaybackService& playback);
  void registerQualityIndicatorComponent(ComponentRegistry& registry, rt::AppRuntime& runtime);
  void registerAudioPipelinePanelComponent(ComponentRegistry& registry,
                                           rt::PlaybackService& playback,
                                           i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout
