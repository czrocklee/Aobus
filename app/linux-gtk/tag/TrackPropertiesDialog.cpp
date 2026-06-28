// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "completion/EntryCompletionController.h"
#include "layout/LayoutConstants.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/tag/TrackPropertiesFormSpec.h>
#include <ao/uimodel/tag/TrackPropertiesFormWorkflow.h>
#include <ao/uimodel/track/TrackFieldEditPolicy.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kSectionSpacing = layout::kSpacingLarge;
    constexpr int kSpinButtonStepIncrement = 1;
    constexpr int kSpinButtonPageIncrement = 10;
    constexpr double kSpinMin = 0.0;
    constexpr int kSpinMax = 65535.0;
    constexpr int kSpinDigits = 0;

    constexpr int kMinScrollContentWidth = 480;
    constexpr int kMaxScrollContentWidth = 640;
    constexpr int kMaxScrollContentHeight = 520;
  } // namespace

  TrackPropertiesDialog::TrackPropertiesDialog(Gtk::Window& parent,
                                               rt::Library const& reads,
                                               rt::LibraryWriter& writer,
                                               rt::CompletionService& completion,
                                               TrackRowCache& rowCache,
                                               std::vector<TrackId> trackIds)
    : AppDialog{}
    , _reads{reads}
    , _writer{writer}
    , _completion{completion}
    , _rowCache{rowCache}
    , _trackIds{std::move(trackIds)}
    , _multipleTracks{_trackIds.size() > 1}
  {
    auto const title =
      _multipleTracks ? std::format("Properties — {} tracks selected", _trackIds.size()) : std::string{"Properties"};

    set_title(title);
    set_transient_for(parent);
    set_default_size(-1, -1);

    setupUi();
    loadData();

    signal_response().connect([this](std::int32_t) { close(); });
  }

  TrackPropertiesDialog::~TrackPropertiesDialog() = default;

  void TrackPropertiesDialog::setupUi()
  {
    addCancelAction("Close", Gtk::ResponseType::CLOSE);
    addPrimaryAction("Save", Gtk::ResponseType::OK)->signal_clicked().connect([this] { onSave(); });

    _notebook.add_css_class("ao-properties-notebook");
    _notebook.set_vexpand(true);

    setupMetadataTab();
    setupPropertiesTab();

    setContentWidget(_notebook);
  }

  void TrackPropertiesDialog::setupMetadataTab()
  {
    _metadataScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _metadataScroll.set_propagate_natural_width(true);
    _metadataScroll.set_propagate_natural_height(true);
    _metadataScroll.set_min_content_width(kMinScrollContentWidth);
    _metadataScroll.set_max_content_width(kMaxScrollContentWidth);
    _metadataScroll.set_max_content_height(kMaxScrollContentHeight);
    _metadataScroll.set_vexpand(true);
    _metadataScroll.set_child(_metadataBox);

    _metadataBox.set_orientation(Gtk::Orientation::VERTICAL);
    _metadataBox.set_spacing(kSectionSpacing);
    _metadataBox.set_valign(Gtk::Align::START);

    auto* const list = Gtk::make_managed<FormBoxedList>();

    auto const spec = uimodel::tag::buildTrackPropertiesFormSpec();

    for (auto const& row : spec.metadataRows)
    {
      auto* const widget = createEditorWidget(row.field, row.editorKind);
      list->addRow(std::string{row.label}, *widget);

      _editors.push_back(FieldEditor{.state = {.field = row.field}, .widget = widget});
    }

    _metadataBox.append(*list);
    _notebook.append_page(_metadataScroll, "Metadata");
  }

  void TrackPropertiesDialog::setupPropertiesTab()
  {
    _propertiesScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _propertiesScroll.set_propagate_natural_width(true);
    _propertiesScroll.set_propagate_natural_height(true);
    _propertiesScroll.set_min_content_width(kMinScrollContentWidth);
    _propertiesScroll.set_max_content_width(kMaxScrollContentWidth);
    _propertiesScroll.set_max_content_height(kMaxScrollContentHeight);
    _propertiesScroll.set_vexpand(true);
    _propertiesScroll.set_child(_propertiesBox);

    _propertiesBox.set_orientation(Gtk::Orientation::VERTICAL);
    _propertiesBox.set_spacing(kSectionSpacing);
    _propertiesBox.set_valign(Gtk::Align::START);

    auto* const list = Gtk::make_managed<FormBoxedList>();

    auto const spec = uimodel::tag::buildTrackPropertiesFormSpec();

    for (auto const& row : spec.propertyRows)
    {
      auto* const widget = createReadonlyWidget(row.field);
      list->addRow(std::string{row.label}, *widget);

      _readonlyRows.push_back(FieldEditor{.state = {.field = row.field}, .widget = widget});
    }

    _propertiesBox.append(*list);
    _notebook.append_page(_propertiesScroll, "Properties");
  }

  Gtk::Widget* TrackPropertiesDialog::createEditorWidget(rt::TrackField field,
                                                         uimodel::tag::TrackPropertiesFormEditorKind editorKind)
  {
    if (editorKind == uimodel::tag::TrackPropertiesFormEditorKind::Number)
    {
      auto* const spin = Gtk::make_managed<Gtk::SpinButton>();
      spin->set_range(kSpinMin, kSpinMax);
      spin->set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
      spin->set_numeric(true);
      spin->set_digits(kSpinDigits);
      spin->set_hexpand(true);
      return spin;
    }

    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_hexpand(true);

    if (rt::trackFieldSupportsValueCompletion(field))
    {
      _completionControllers.push_back(CompletionControllerBinding{
        .entry = entry,
        .controllerPtr = std::make_unique<EntryCompletionController>(
          *entry, rt::MetadataValueCompleter{_completion, field}.asProvider()),
      });
    }

    return entry;
  }

  Gtk::Widget* TrackPropertiesDialog::createReadonlyWidget(rt::TrackField /*field*/)
  {
    auto* const label = Gtk::make_managed<Gtk::Label>();
    label->set_selectable(true);
    label->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    label->set_xalign(0.0F);
    label->set_hexpand(true);
    label->add_css_class("ao-property-value");
    return label;
  }

  void TrackPropertiesDialog::loadData()
  {
    if (_trackIds.empty())
    {
      return;
    }

    auto scope = _reads.reader();

    bool first = true;

    for (auto const trackId : _trackIds)
    {
      if (!scope.trackRow(trackId))
      {
        continue;
      }

      if (first)
      {
        loadFirstTrack(scope, trackId);
        first = false;
      }
      else
      {
        loadSubsequentTrack(scope, trackId);
      }
    }
  }

  void TrackPropertiesDialog::loadFirstTrack(rt::LibraryReader const& scope, TrackId trackId)
  {
    for (auto& editor : _editors)
    {
      auto const rawValue = scope.trackField(trackId, editor.state.field);
      editor.state = uimodel::tag::makeTrackPropertiesFormFieldState(editor.state.field, rawValue);
      setWidgetValue(
        editor.state.field, editor.widget, uimodel::track::formatTrackFieldRawValue(editor.state.field, rawValue));
    }

    for (auto& row : _readonlyRows)
    {
      auto const rawValue = scope.trackField(trackId, row.state.field);
      row.state = uimodel::tag::makeTrackPropertiesFormFieldState(row.state.field, rawValue);
      setWidgetValue(row.state.field, row.widget, uimodel::track::formatTrackFieldRawValue(row.state.field, rawValue));
    }
  }

  void TrackPropertiesDialog::loadSubsequentTrack(rt::LibraryReader const& scope, TrackId trackId)
  {
    for (auto& editor : _editors)
    {
      if (auto const rawValue = scope.trackField(trackId, editor.state.field);
          uimodel::tag::mergeTrackPropertiesFormFieldState(editor.state, rawValue))
      {
        setEditorMixed(editor.state.field, editor.widget);
      }
    }

    for (auto& row : _readonlyRows)
    {
      if (auto const rawValue = scope.trackField(trackId, row.state.field);
          uimodel::tag::mergeTrackPropertiesFormFieldState(row.state, rawValue))
      {
        setEditorMixed(row.state.field, row.widget);
      }
    }
  }

  void TrackPropertiesDialog::onSave()
  {
    if (_trackIds.empty())
    {
      return;
    }

    auto patch = rt::MetadataPatch{};

    for (auto const& editor : _editors)
    {
      auto const* const def = trackFieldUiDefinition(editor.state.field);

      if (def == nullptr || !uimodel::track::trackFieldCanWritePatch(editor.state.field))
      {
        continue;
      }

      auto editValue = TrackFieldEditValue{};

      if (auto* const entry = dynamic_cast<Gtk::Entry*>(editor.widget); entry != nullptr)
      {
        auto const text = entry->get_text().raw();
        editValue = TrackFieldEditValue{std::in_place_type<std::string>, std::move(text)};
      }
      else if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(editor.widget); spin != nullptr)
      {
        auto const value = static_cast<std::uint16_t>(spin->get_value_as_int());
        editValue = TrackFieldEditValue{std::in_place_type<std::uint16_t>, value};
      }
      else
      {
        continue;
      }

      std::ignore = uimodel::tag::writeTrackPropertiesFormEdit(patch, editor.state, editValue);
    }

    auto const reply = _writer.updateMetadata(_trackIds, patch);

    for (auto const trackId : reply.mutatedIds)
    {
      _rowCache.invalidate(trackId);
    }
  }

  void TrackPropertiesDialog::setWidgetValue(rt::TrackField /*field*/, Gtk::Widget* widget, std::string_view value)
  {
    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget); entry != nullptr)
    {
      auto const iter = std::ranges::find_if(
        _completionControllers, [entry](CompletionControllerBinding const& binding) { return binding.entry == entry; });

      if (iter != _completionControllers.end())
      {
        iter->controllerPtr->setTextProgrammatically(std::string{value});
        return;
      }

      entry->set_text(std::string{value});
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget); spin != nullptr)
    {
      std::int32_t intValue = 0;

      if (!value.empty())
      {
        std::from_chars(value.data(), value.data() + value.size(), intValue);
      }

      spin->set_value(static_cast<double>(intValue));
      return;
    }

    if (auto* const label = dynamic_cast<Gtk::Label*>(widget); label != nullptr)
    {
      label->set_text(std::string{value});
    }
  }

  void TrackPropertiesDialog::setEditorMixed(rt::TrackField /*field*/, Gtk::Widget* widget)
  {
    widget->set_sensitive(false);

    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget); entry != nullptr)
    {
      entry->set_placeholder_text("<Multiple Values>");
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget); spin != nullptr)
    {
      spin->set_value(0.0);
      return;
    }

    if (auto* const label = dynamic_cast<Gtk::Label*>(widget); label != nullptr)
    {
      label->set_text("<Multiple Values>");
    }
  }
} // namespace ao::gtk
