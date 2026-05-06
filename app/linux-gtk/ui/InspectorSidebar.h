// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArtCache.h"
#include "TagEditor.h"
#include "TrackRow.h"
#include <ao/Type.h>
#include <runtime/CorePrimitives.h>
#include <runtime/ProjectionTypes.h>

#include <gtkmm.h>

#include <memory>
#include <vector>

namespace ao::app
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackRowDataProvider;

  /**
   * @brief InspectorSidebar displays details and allows editing of track metadata.
   * It slides in from the right and reacts to the current selection.
   */
  class InspectorSidebar final : public Gtk::Box
  {
  public:
    using TagEditRequestedSignal = sigc::signal<void(std::vector<ao::TrackId> const&, Gtk::Widget*)>;

    explicit InspectorSidebar(ao::app::AppSession& session, CoverArtCache& coverArtCache);
    ~InspectorSidebar() override;

    TagEditRequestedSignal& signalTagEditRequested() { return _tagEditRequested; }

    /**
     * @brief Update the sidebar with the current selection.
     * @param selectedRows The list of currently selected track rows.
     */
    void updateSelection(TrackRowDataProvider* dataProvider, std::vector<Glib::RefPtr<TrackRow>> const& selectedRows);

    /// Bind to a runtime detail projection for cover art + audio property auto-updates.
    void bindToDetailProjection(std::shared_ptr<ao::app::ITrackDetailProjection> projection);

  private:
    // Multi-select Aggregation
    struct AggregatedMetadata final
    {
      std::string title;
      std::string artist;
      std::string album;
      bool sameTitle = true;
      bool sameArtist = true;
      bool sameAlbum = true;
    };

    void setupUi();
    void buildHeroSection();
    void buildMetadataSection();
    void buildTagsSection();
    void buildAudioSection();

    void updateEmptyState();
    void updateSingleSelection(TrackRowDataProvider* dataProvider, Glib::RefPtr<TrackRow> const& row);
    void updateMultiSelection(TrackRowDataProvider* dataProvider, std::vector<Glib::RefPtr<TrackRow>> const& rows);

    void onTitleEdited();
    void onArtistEdited();
    void onAlbumEdited();

    ao::app::AppSession& _session;
    CoverArtCache& _coverArtCache;

    TrackRowDataProvider* _dataProvider = nullptr;
    std::vector<Glib::RefPtr<TrackRow>> _currentSelection;
    std::shared_ptr<ao::app::ITrackDetailProjection> _detailProjection;
    ao::app::Subscription _detailSub;

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
