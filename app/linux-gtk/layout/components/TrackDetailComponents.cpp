// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/components/TrackDetailComponents.h"

#include "image/ImageWidget.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/utility/Log.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glibmm/refptr.h>
#include <gtkmm/adjustment.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>
#include <sigc++/signal.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr float kLabelOpacity = 0.6F;
    constexpr std::uint16_t kCodecIdFlac = 3;
    constexpr std::uint16_t kCodecIdMp3 = 0x55;
    constexpr double kKhzMultiplier = 1000.0;
    constexpr int kSecondsPerHour = 3600;
    constexpr int kSecondsPerMinute = 60;

    Glib::ustring formatCodecId(std::uint16_t codecId)
    {
      switch (codecId)
      {
        case kCodecIdFlac: return "FLAC";
        case kCodecIdMp3: return "MP3";
        default: return codecId == 0 ? "Unknown" : std::format("Codec (0x{:02x})", codecId);
      }
    }

    Glib::ustring formatSampleRate(std::uint32_t rate)
    {
      if (rate == 0)
      {
        return "Unknown";
      }

      if (rate % 1000 == 0)
      {
        return std::format("{} kHz", rate / 1000);
      }

      return std::format("{:.1f} kHz", rate / kKhzMultiplier);
    }

    std::string formatDuration(std::chrono::milliseconds ms)
    {
      auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
      auto const minutes = (totalSeconds % kSecondsPerHour) / kSecondsPerMinute;
      auto const seconds = totalSeconds % kSecondsPerMinute;

      if (totalSeconds >= kSecondsPerHour)
      {
        return std::format("{}:{:02}:{:02}", totalSeconds / kSecondsPerHour, minutes, seconds);
      }

      return std::format("{}:{:02}", minutes, seconds);
    }

    // Helper to walk widget tree and reset scrolled windows
    void resetScrollAdjustments(Gtk::Widget* widget)
    {
      if (widget == nullptr)
      {
        return;
      }

      if (auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(widget); sw != nullptr)
      {
        if (auto const vadjPtr = sw->get_vadjustment(); vadjPtr != nullptr)
        {
          vadjPtr->set_value(vadjPtr->get_lower());
        }
      }

      for (auto* child = widget->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        resetScrollAdjustments(child);
      }
    }

    class TrackDetailScopeComponent final
      : public ILayoutComponent
      , public ITrackDetailScope
    {
    public:
      TrackDetailScopeComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}
        , _projection{rt::FocusedViewTarget{},
                      ctx.runtime.views(),
                      ctx.runtime.musicLibrary(),
                      ctx.runtime.workspace(),
                      ctx.runtime.mutation()}
      {
        _currentSnap = _projection.snapshot();

        // Intercept context
        auto* previousScope = ctx.track.detailScope;
        ctx.track.detailScope = this;

        // Build children
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        // Restore context
        ctx.track.detailScope = previousScope;

        // Apply styles
        if (auto const it = node.layout.find("cssClasses"); it != node.layout.end())
        {
          if (auto const* const classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
          {
            for (auto const& className : *classes)
            {
              _box.add_css_class(className);
            }
          }
          else if (auto const className = it->second.asString(); !className.empty())
          {
            _box.add_css_class(className);
          }
        }

        // Subscribe to projection
        _sub = _projection.subscribe([this](auto const& snap) { onSnapshot(snap); });
      }

      Gtk::Widget& widget() override { return _box; }

      rt::TrackDetailSnapshot const& snapshot() const override { return _currentSnap; }
      bool isEditLocked() const override { return _editLocked; }
      void setEditLocked(bool locked) override
      {
        if (_editLocked != locked)
        {
          _editLocked = locked;
          _signalEditLockChanged.emit(_editLocked);
        }
      }

      sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() override
      {
        return _signalSnapshotChanged;
      }
      sigc::signal<void(bool)>& signalEditLockChanged() override { return _signalEditLockChanged; }

    private:
      void onSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        bool const selectionChanged = _currentSnap.trackIds != snap.trackIds;
        _currentSnap = snap;
        _signalSnapshotChanged.emit(snap);

        if (selectionChanged)
        {
          resetScrollAdjustments(&_box);
        }
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;

      rt::TrackDetailProjection _projection;
      rt::Subscription _sub;
      rt::TrackDetailSnapshot _currentSnap;
      bool _editLocked = true;

      sigc::signal<void(rt::TrackDetailSnapshot const&)> _signalSnapshotChanged;
      sigc::signal<void(bool)> _signalEditLockChanged;
    };

    class TrackSelectionRegionComponent final : public ILayoutComponent
    {
    public:
      TrackSelectionRegionComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}, _showWhen{node.getProp<std::string>("showWhen", "any")}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn = ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap)
                                                                              { updateVisibility(snap); });
          updateVisibility(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateVisibility(rt::TrackDetailSnapshot const& snap)
      {
        bool visible = false;

        if (_showWhen == "none")
        {
          visible = (snap.selectionKind == rt::SelectionKind::None);
        }
        else if (_showWhen == "single")
        {
          visible = (snap.selectionKind == rt::SelectionKind::Single);
        }
        else if (_showWhen == "multiple")
        {
          visible = (snap.selectionKind == rt::SelectionKind::Multiple);
        }
        else if (_showWhen == "any")
        {
          visible = (snap.selectionKind != rt::SelectionKind::None);
        }

        _box.set_visible(visible);
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
      std::string _showWhen;
      sigc::connection _scopeConn;
    };

    class TrackCoverArtComponent final : public ILayoutComponent
    {
    public:
      TrackCoverArtComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}, _imageWidget{ctx.runtime.musicLibrary(), *ctx.detail.imageCache}
      {
        _box.append(_imageWidget);

        std::int32_t width = -1;
        std::int32_t height = -1;

        if (auto const it = node.layout.find("widthRequest"); it != node.layout.end())
        {
          width = static_cast<std::int32_t>(it->second.asInt());
        }

        if (auto const it = node.layout.find("heightRequest"); it != node.layout.end())
        {
          height = static_cast<std::int32_t>(it->second.asInt());
        }

        if (width != -1 || height != -1)
        {
          _imageWidget.set_size_request(width, height);
        }

        if (auto const it = node.layout.find("cssClasses"); it != node.layout.end())
        {
          if (auto const* const classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
          {
            for (auto const& className : *classes)
            {
              _imageWidget.add_css_class(className);
            }
          }
          else if (auto const className = it->second.asString(); !className.empty())
          {
            _imageWidget.add_css_class(className);
          }
        }

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn =
            ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { updateImage(snap); });
          updateImage(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateImage(rt::TrackDetailSnapshot const& snap)
      {
        if (snap.singleCoverArtId == kInvalidResourceId)
        {
          _imageWidget.clearImage();
          _imageWidget.set_visible(false);
        }
        else
        {
          _imageWidget.loadImage(snap.singleCoverArtId);
          _imageWidget.set_visible(true);
        }
      }

      Gtk::Box _box;
      ImageWidget _imageWidget;
      sigc::connection _scopeConn;
    };

    class TrackMetadataFieldComponent final : public ILayoutComponent
    {
    public:
      TrackMetadataFieldComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 4}
        , _mutation{ctx.runtime.mutation()}
        , _scope{ctx.track.detailScope}
        , _field{node.getProp<std::string>("field", "title")}
      {
        _titleLabel.set_text(node.getProp<std::string>("label", ""));
        _titleLabel.set_halign(Gtk::Align::START);
        _titleLabel.add_css_class("ao-property-label");
        _titleLabel.set_opacity(kLabelOpacity);

        _editable.set_halign(Gtk::Align::START);
        _editable.set_hexpand(true);
        _editable.set_vexpand(false);
        _editable.add_css_class("ao-property-editable");

        _box.append(_titleLabel);
        _box.append(_editable);

        _editable.property_editing().signal_changed().connect(
          sigc::mem_fun(*this, &TrackMetadataFieldComponent::onEdited));

        auto const keyPtr = Gtk::EventControllerKey::create();
        keyPtr->signal_key_pressed().connect(
          [this](guint keyval, guint, Gdk::ModifierType) -> bool
          {
            if (keyval == GDK_KEY_Escape)
            {
              if (_editable.get_editing())
              {
                _editable.stop_editing(false);
              }

              if (_scope != nullptr)
              {
                updateValue(_scope->snapshot());
              }

              return true;
            }

            return false;
          },
          false);
        _editable.add_controller(keyPtr);

        if (_scope != nullptr)
        {
          _scopeConn = _scope->signalSnapshotChanged().connect([this](auto const& snap) { updateValue(snap); });
          _lockConn = _scope->signalEditLockChanged().connect([this](bool locked) { updateLock(locked); });
          updateValue(_scope->snapshot());
          updateLock(_scope->isEditLocked());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateValue(rt::TrackDetailSnapshot const& snap)
      {
        if (_editable.get_editing())
        {
          return;
        }

        if (_field == "title")
        {
          _editable.set_text(snap.title.mixed ? "<Multiple Values>" : snap.title.optValue.value_or(""));
        }
        else if (_field == "artist")
        {
          _editable.set_text(snap.artist.mixed ? "<Multiple Values>" : snap.artist.optValue.value_or(""));
        }
        else if (_field == "album")
        {
          _editable.set_text(snap.album.mixed ? "<Multiple Values>" : snap.album.optValue.value_or(""));
        }
      }

      void updateLock(bool locked) { _editable.set_editable(!locked); }

      void onEdited()
      {
        if (_editable.get_editing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = _editable.get_text().raw();

        if (newValue == "<Multiple Values>")
        {
          return;
        }

        auto patch = rt::MetadataPatch{};

        if (_field == "title")
        {
          patch.optTitle = newValue;
        }
        else if (_field == "artist")
        {
          patch.optArtist = newValue;
        }
        else if (_field == "album")
        {
          patch.optAlbum = newValue;
        }

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to update {}: {}", _field, result.error().message);
          updateValue(snap);
        }
      }

      Gtk::Box _box;
      Gtk::Label _titleLabel;
      Gtk::EditableLabel _editable;

      rt::LibraryMutationService& _mutation;
      ITrackDetailScope* _scope;
      std::string _field;
      sigc::connection _scopeConn;
      sigc::connection _lockConn;
    };

    class TrackAudioPropertyComponent final : public ILayoutComponent
    {
    public:
      TrackAudioPropertyComponent(LayoutContext& ctx, LayoutNode const& node)
        : _row{Gtk::Orientation::HORIZONTAL, 8}, _property{node.getProp<std::string>("property", "")}
      {
        _title.set_text(node.getProp<std::string>("label", ""));
        _title.set_halign(Gtk::Align::START);
        _title.set_opacity(kLabelOpacity);
        _title.add_css_class("ao-technical-label");

        _value.set_halign(Gtk::Align::END);
        _value.set_hexpand(true);
        _value.add_css_class("ao-technical-value");

        _row.append(_title);
        _row.append(_value);

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn =
            ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { updateValue(snap); });
          updateValue(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _row; }

    private:
      void updateValue(rt::TrackDetailSnapshot const& snap)
      {
        auto const setLabel = [this](auto const& property, auto const& formatter)
        {
          if (property.optValue && !property.mixed)
          {
            _value.set_text(formatter(*property.optValue));
          }
          else
          {
            _value.set_text(property.mixed ? "Mixed" : "Unknown");
          }
        };

        if (_property == "format")
        {
          setLabel(snap.audio.codecId, [](auto id) { return formatCodecId(id); });
        }
        else if (_property == "sampleRate")
        {
          setLabel(snap.audio.sampleRate, [](auto rate) { return formatSampleRate(rate); });
        }
        else if (_property == "channels")
        {
          setLabel(snap.audio.channels, [](auto ch) { return std::format("{} Ch", ch); });
        }
        else if (_property == "duration")
        {
          setLabel(snap.audio.durationMs, [](auto ms) { return formatDuration(std::chrono::milliseconds{ms}); });
        }
        else if (_property == "bitDepth")
        {
          setLabel(snap.audio.bitDepth, [](auto depth) { return std::format("{} bit", depth); });
        }
      }

      Gtk::Box _row;
      Gtk::Label _title;
      Gtk::Label _value;
      std::string _property;
      sigc::connection _scopeConn;
    };

    class TrackEditLockComponent final : public ILayoutComponent
    {
    public:
      TrackEditLockComponent(LayoutContext& ctx, LayoutNode const& node)
        : _scope{ctx.track.detailScope}
      {
        _lockedIcon = node.getProp<std::string>("lockedIcon", "changes-prevent-symbolic");
        _unlockedIcon = node.getProp<std::string>("unlockedIcon", "changes-allow-symbolic");

        _button.set_has_frame(false);
        _button.add_css_class("ao-icon-button");
        _button.signal_clicked().connect(
          [this]
          {
            if (_scope != nullptr)
            {
              _scope->setEditLocked(!_scope->isEditLocked());
            }
          });

        if (_scope != nullptr)
        {
          _lockConn = _scope->signalEditLockChanged().connect([this](bool locked) { updateIcon(locked); });
          updateIcon(_scope->isEditLocked());
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      void updateIcon(bool locked)
      {
        _button.set_icon_name(locked ? _lockedIcon : _unlockedIcon);
        _button.set_tooltip_text(locked ? "Unlock to Edit" : "Lock Editing");
      }

      Gtk::Button _button;
      ITrackDetailScope* _scope;
      std::string _lockedIcon;
      std::string _unlockedIcon;
      sigc::connection _lockConn;
    };
  }

  void registerTrackDetailComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.detailScope",
       .displayName = "Detail Scope",
       .category = "Tracks",
       .container = true,
       .props = {},
       .layoutProps = {{.name = "cssClasses", .kind = PropertyKind::String, .label = "CSS Classes"}},
       .minChildren = 1,
       .optMaxChildren = 0},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<TrackDetailScopeComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "track.selectionRegion",
       .displayName = "Selection Region",
       .category = "Tracks",
       .container = true,
       .props =
         {{.name = "showWhen", .kind = PropertyKind::String, .label = "Show When", .defaultValue = LayoutValue{"any"}}},
       .layoutProps = {},
       .minChildren = 1,
       .optMaxChildren = 0},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<TrackSelectionRegionComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "track.coverArt",
       .displayName = "Cover Art",
       .category = "Tracks",
       .container = false,
       .props = {},
       .layoutProps = {{.name = "widthRequest", .kind = PropertyKind::Int, .label = "Width Request"},
                       {.name = "heightRequest", .kind = PropertyKind::Int, .label = "Height Request"},
                       {.name = "cssClasses", .kind = PropertyKind::String, .label = "CSS Classes"}},
       .minChildren = 0,
       .optMaxChildren = 0},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<TrackCoverArtComponent>(ctx, node); });

    registry.registerComponent({.type = "track.metadataField",
                                .displayName = "Metadata Field",
                                .category = "Tracks",
                                .container = false,
                                .props = {{.name = "field", .kind = PropertyKind::String, .label = "Field"},
                                          {.name = "label", .kind = PropertyKind::String, .label = "Label"}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TrackMetadataFieldComponent>(ctx, node); });

    registry.registerComponent({.type = "track.audioProperty",
                                .displayName = "Audio Property",
                                .category = "Tracks",
                                .container = false,
                                .props = {{.name = "property", .kind = PropertyKind::String, .label = "Property"},
                                          {.name = "label", .kind = PropertyKind::String, .label = "Label"}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TrackAudioPropertyComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "track.editLock",
       .displayName = "Edit Lock",
       .category = "Tracks",
       .container = false,
       .props = {{.name = "lockedIcon", .kind = PropertyKind::String, .label = "Locked Icon"},
                 {.name = "unlockedIcon", .kind = PropertyKind::String, .label = "Unlocked Icon"}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<TrackEditLockComponent>(ctx, node); });
  }
}
