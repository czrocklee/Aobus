// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArtCache.h"
#include "TagEditor.h"
#include <ao/Type.h>
#include <runtime/CorePrimitives.h>
#include <runtime/ProjectionTypes.h>

#include <gtkmm.h>

#include <memory>
#include <vector>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  /**
   * @brief InspectorSidebar displays details and allows editing of track metadata.
   * It slides in from the right and reacts to the current selection.
   */
  class InspectorSidebar final : public Gtk::Box
  {
  public:
    using TagEditRequestedSignal = sigc::signal<void(std::vector<ao::TrackId> const&, Gtk::Widget*)>;

    explicit InspectorSidebar(ao::rt::AppSession& session, CoverArtCache& coverArtCache);
    ~InspectorSidebar() override;

    TagEditRequestedSignal& signalTagEditRequested() { return _tagEditRequested; }

    /// Bind to a runtime detail projection for cover art + audio property auto-updates.
    void bindToDetailProjection(std::shared_ptr<ao::rt::ITrackDetailProjection> projection);

  private:
    void setupUi();
    void buildHeroSection();
    void buildMetadataSection();
    void buildTagsSection();
    void buildAudioSection();

    void updateEmptyState();

    void onTitleEdited();
    void onArtistEdited();
    void onAlbumEdited();

    void onTrackDetailSnapshot(ao::rt::TrackDetailSnapshot const& snap);
    void updateCoverArt(ao::rt::TrackDetailSnapshot const& snap);
    Glib::RefPtr<Gdk::Pixbuf> loadCoverArtFromLibrary(ao::ResourceId resourceId);
    void updateAudioMetadata(ao::rt::TrackDetailSnapshot const& snap);

    ao::rt::AppSession& _session;
    CoverArtCache& _coverArtCache;

    std::vector<ao::TrackId> _currentTrackIds;
    std::shared_ptr<ao::rt::ITrackDetailProjection> _detailProjection;
    ao::rt::Subscription _detailSub;

    // UI Components
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::Box _contentBox{Gtk::Orientation::VERTICAL};

    // Hero Section
    Gtk::Box _heroBox{Gtk::Orientation::VERTICAL};
    Gtk::Picture _coverImage;
    Gtk::Label _noCoverLabel{"No Artwork"};

    // Metadata Section
    Gtk::Box _metadataBox{Gtk::Orientation::VERTICAL};
    Gtk::EditableLabel _titleLabel;
    Gtk::EditableLabel _artistLabel;
    Gtk::EditableLabel _albumLabel;

    // Audio Section
    Gtk::Box _audioBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _formatLabel;
    Gtk::Label _sampleRateLabel;
    Gtk::Label _channelsLabel;
    Gtk::Label _durationLabel;

    // Tags Section
    TagEditor _tagEditor;

    TagEditRequestedSignal _tagEditRequested;
  };
} // namespace ao::gtk
