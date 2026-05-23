// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "image/ImageCache.h"
#include "runtime/CorePrimitives.h"
#include "runtime/ProjectionTypes.h"
#include "tag/TagEditor.h"

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/picture.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <memory>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryMutationService;
  class ListSourceStore;
}

namespace ao::gtk
{
  /**
   * @brief TrackInspectorPanel displays details and allows editing of track metadata.
   * It slides in from the right and reacts to the current selection.
   */
  class TrackInspectorPanel final : public Gtk::Box
  {
  public:
    using TagEditRequestedSignal = sigc::signal<void(std::vector<TrackId> const&, Gtk::Widget*)>;

    explicit TrackInspectorPanel(library::MusicLibrary& library,
                                 rt::LibraryMutationService& mutation,
                                 rt::ListSourceStore& sources,
                                 ImageCache& imageCache);
    ~TrackInspectorPanel() override;

    TrackInspectorPanel(TrackInspectorPanel const&) = delete;
    TrackInspectorPanel& operator=(TrackInspectorPanel const&) = delete;
    TrackInspectorPanel(TrackInspectorPanel&&) = delete;
    TrackInspectorPanel& operator=(TrackInspectorPanel&&) = delete;

    TagEditRequestedSignal& signalTagEditRequested() { return _tagEditRequested; }

    /// Bind to a runtime detail projection for cover art + audio property auto-updates.
    void bindToDetailProjection(std::shared_ptr<rt::ITrackDetailProjection> projection);

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

    void onTrackDetailSnapshot(rt::TrackDetailSnapshot const& snap);
    void updateImage(rt::TrackDetailSnapshot const& snap);
    Glib::RefPtr<Gdk::Pixbuf> loadImageFromLibrary(ResourceId resourceId);
    void updateAudioMetadata(rt::TrackDetailSnapshot const& snap);

    library::MusicLibrary& _library;
    rt::LibraryMutationService& _mutation;
    rt::ListSourceStore& _sources;
    ImageCache& _imageCache;

    std::vector<TrackId> _currentTrackIds;
    std::shared_ptr<rt::ITrackDetailProjection> _detailProjection;
    rt::Subscription _detailSub;

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
