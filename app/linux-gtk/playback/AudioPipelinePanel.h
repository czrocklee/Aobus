// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/box.h>

#include <cstdint>

namespace ao::gtk
{
  enum class AudioPipelinePanelVariant : std::uint8_t
  {
    Inline,
    Compact,
    Tooltip,
  };

  class AudioPipelinePanel final : public Gtk::Box
  {
  public:
    explicit AudioPipelinePanel(AudioPipelinePanelVariant variant = AudioPipelinePanelVariant::Inline);
    ~AudioPipelinePanel() override = default;

    AudioPipelinePanel(AudioPipelinePanel const&) = delete;
    AudioPipelinePanel& operator=(AudioPipelinePanel const&) = delete;
    AudioPipelinePanel(AudioPipelinePanel&&) = delete;
    AudioPipelinePanel& operator=(AudioPipelinePanel&&) = delete;

    void setVariant(AudioPipelinePanelVariant variant);
    void apply(uimodel::AudioPipelineView const& view);

  private:
    AudioPipelinePanelVariant _variant;
  };
} // namespace ao::gtk
