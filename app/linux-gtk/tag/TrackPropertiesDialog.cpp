// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "completion/EntryCompletionController.h"
#include "layout/LayoutConstants.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

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
    configureForParent(parent);
    set_default_size(-1, -1);

    buildUi();
    loadSelectedTrackFields();

    signal_response().connect([this](std::int32_t) { close(); });
  }

  TrackPropertiesDialog::~TrackPropertiesDialog() = default;

  void TrackPropertiesDialog::buildUi()
  {
    addCancelAction("Close", Gtk::ResponseType::CLOSE);
    _saveButton = addPrimaryAction("Save", Gtk::ResponseType::OK);
    _saveButton->set_sensitive(false);
    _saveButton->signal_clicked().connect([this] { handleSaveClicked(); });

    _notebook.add_css_class("ao-properties-notebook");
    _notebook.set_vexpand(true);

    buildMetadataTab();
    buildPropertiesTab();

    setContentWidget(_notebook);
  }

  void TrackPropertiesDialog::buildMetadataTab()
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

    auto const spec = uimodel::buildTrackPropertiesFormSpec();

    for (auto const& row : spec.metadataRows)
    {
      _formModel.addField(row.field, true);
      auto* const widget = createEditorWidget(row.field, row.editorKind);
      list->addRow(std::string{row.label}, *widget);

      _editors.push_back(FieldEditor{.field = row.field, .widget = widget});
    }

    _metadataBox.append(*list);
    _notebook.append_page(_metadataScroll, "Metadata");
  }

  void TrackPropertiesDialog::buildPropertiesTab()
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

    auto const spec = uimodel::buildTrackPropertiesFormSpec();

    for (auto const& row : spec.propertyRows)
    {
      _formModel.addField(row.field, false);
      auto* const widget = createReadonlyWidget(row.field);
      list->addRow(std::string{row.label}, *widget);

      _readonlyRows.push_back(FieldEditor{.field = row.field, .widget = widget});
    }

    _propertiesBox.append(*list);
    _notebook.append_page(_propertiesScroll, "Properties");
  }

  Gtk::Widget* TrackPropertiesDialog::createEditorWidget(rt::TrackField field,
                                                         uimodel::TrackPropertiesFormEditorKind editorKind)
  {
    if (editorKind == uimodel::TrackPropertiesFormEditorKind::Number)
    {
      auto* const spin = Gtk::make_managed<Gtk::SpinButton>();
      spin->set_range(kSpinMin, kSpinMax);
      spin->set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
      spin->set_numeric(true);
      spin->set_digits(kSpinDigits);
      spin->set_hexpand(true);
      spin->signal_value_changed().connect([this, field, spin] { updateEditorValue(field, spin); });
      return spin;
    }

    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_hexpand(true);
    entry->signal_changed().connect([this, field, entry] { updateEditorValue(field, entry); });

    if (rt::supportsTrackFieldValueCompletion(field))
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

  void TrackPropertiesDialog::loadSelectedTrackFields()
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
      auto const rawValue = scope.trackField(trackId, editor.field);
      _formModel.loadFirstTrackField(editor.field, rawValue);
      applyRowView(editor.widget, _formModel.rowView(editor.field));
    }

    for (auto& row : _readonlyRows)
    {
      auto const rawValue = scope.trackField(trackId, row.field);
      _formModel.loadFirstTrackField(row.field, rawValue);
      applyRowView(row.widget, _formModel.rowView(row.field));
    }

    updateSaveEnabled();
  }

  void TrackPropertiesDialog::loadSubsequentTrack(rt::LibraryReader const& scope, TrackId trackId)
  {
    for (auto& editor : _editors)
    {
      if (auto const rawValue = scope.trackField(trackId, editor.field);
          _formModel.mergeTrackField(editor.field, rawValue))
      {
        applyRowView(editor.widget, _formModel.rowView(editor.field));
      }
    }

    for (auto& row : _readonlyRows)
    {
      if (auto const rawValue = scope.trackField(trackId, row.field); _formModel.mergeTrackField(row.field, rawValue))
      {
        applyRowView(row.widget, _formModel.rowView(row.field));
      }
    }

    updateSaveEnabled();
  }

  void TrackPropertiesDialog::handleSaveClicked()
  {
    if (_trackIds.empty())
    {
      return;
    }

    for (auto const& editor : _editors)
    {
      updateEditorValue(editor.field, editor.widget);
    }

    auto const patch = _formModel.buildPatch();
    auto const replyResult = _writer.updateMetadata(_trackIds, patch);

    if (!replyResult)
    {
      AppDialog::presentMessage(
        *this,
        "Save failed",
        replyResult.error().message,
        {AppDialogAction{
          .label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
        Gtk::ResponseType::CLOSE);
      return;
    }

    for (auto const trackId : replyResult->mutatedIds)
    {
      _rowCache.invalidate(trackId);
    }
  }

  void TrackPropertiesDialog::updateSaveEnabled()
  {
    if (_saveButton != nullptr)
    {
      _saveButton->set_sensitive(_formModel.canSave());
    }
  }

  void TrackPropertiesDialog::updateEditorValue(rt::TrackField field, Gtk::Widget* widget)
  {
    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget); entry != nullptr)
    {
      _formModel.setEditValue(field, uimodel::makeTextEditValue(entry->get_text().raw()));
      updateSaveEnabled();
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget); spin != nullptr)
    {
      _formModel.setEditValue(
        field,
        TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(spin->get_value_as_int())});
      updateSaveEnabled();
    }
  }

  void TrackPropertiesDialog::applyRowView(Gtk::Widget* widget, uimodel::TrackPropertiesFormRowView const& view)
  {
    if (view.mixed)
    {
      setEditorMixed(widget);
      return;
    }

    setWidgetValue(widget, view.text);
  }

  void TrackPropertiesDialog::setWidgetValue(Gtk::Widget* widget, std::string_view value)
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

  void TrackPropertiesDialog::setEditorMixed(Gtk::Widget* widget)
  {
    widget->set_sensitive(false);

    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget); entry != nullptr)
    {
      entry->set_placeholder_text(std::string{uimodel::kMultipleTrackValuesText});
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget); spin != nullptr)
    {
      spin->set_value(0.0);
      return;
    }

    if (auto* const label = dynamic_cast<Gtk::Label*>(widget); label != nullptr)
    {
      label->set_text(std::string{uimodel::kMultipleTrackValuesText});
    }
  }
} // namespace ao::gtk
