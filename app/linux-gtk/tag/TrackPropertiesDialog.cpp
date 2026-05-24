// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "layout/LayoutConstants.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>

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

#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kDialogDefaultWidth = 520;
    constexpr int kDialogDefaultHeight = 580;
    constexpr int kBoxSpacing = layout::kSpacingMedium;
    constexpr int kSectionSpacing = layout::kSpacingLarge;
    constexpr int kFieldRowSpacing = layout::kSpacingSmall;
    constexpr float kLabelOpacity = 0.6F;
    constexpr int kLabelWidthChars = 14;
    constexpr int kSpinButtonStepIncrement = 1;
    constexpr int kSpinButtonPageIncrement = 10;
    constexpr double kSpinMin = 0.0;
    constexpr double kSpinMax = 65535.0;
    constexpr int kSpinDigits = 0;
  } // namespace

  TrackPropertiesDialog::TrackPropertiesDialog(Gtk::Window& parent,
                                               library::MusicLibrary& library,
                                               rt::LibraryMutationService& mutation,
                                               TrackRowCache& rowCache,
                                               std::vector<TrackId> trackIds)
    : Gtk::Dialog{}
    , _library{library}
    , _mutation{mutation}
    , _rowCache{rowCache}
    , _trackIds{std::move(trackIds)}
    , _multipleTracks{_trackIds.size() > 1}
  {
    auto const title =
      _multipleTracks ? std::format("Properties — {} tracks selected", _trackIds.size()) : std::string{"Properties"};

    set_title(title);
    set_transient_for(parent);
    set_modal(true);
    set_default_size(kDialogDefaultWidth, kDialogDefaultHeight);

    setupUi();
    loadData();

    signal_response().connect([this](std::int32_t) { close(); });
    present();
  }

  TrackPropertiesDialog::~TrackPropertiesDialog() = default;

  void TrackPropertiesDialog::setupUi()
  {
    auto* const contentArea = get_content_area();
    contentArea->add_css_class("ao-dialog-content");

    _notebook.add_css_class("ao-properties-notebook");
    setupMetadataTab();
    setupPropertiesTab();

    contentArea->append(_notebook);

    auto* const saveButton = Gtk::make_managed<Gtk::Button>("Save");
    saveButton->add_css_class("suggested-action");
    saveButton->signal_clicked().connect([this] { onSave(); });
    add_action_widget(*saveButton, Gtk::ResponseType::OK);

    auto* const closeButton = Gtk::make_managed<Gtk::Button>("Close");
    add_action_widget(*closeButton, Gtk::ResponseType::CLOSE);
  }

  void TrackPropertiesDialog::setupMetadataTab()
  {
    _metadataScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _metadataScroll.set_vexpand(true);
    _metadataScroll.set_child(_metadataBox);

    _metadataBox.set_spacing(kBoxSpacing);
    _metadataBox.set_valign(Gtk::Align::START);
    _metadataBox.set_margin_start(kSectionSpacing);
    _metadataBox.set_margin_end(kSectionSpacing);
    _metadataBox.set_margin_top(kSectionSpacing);
    _metadataBox.set_margin_bottom(kSectionSpacing);

    for (auto const& def : trackFieldUiDefinitions())
    {
      if (!def.propertyDialogEditable || def.field == rt::TrackField::Tags)
      {
        continue;
      }

      auto const* const fieldDef = trackFieldDefinition(def.field);

      if (fieldDef == nullptr)
      {
        continue;
      }

      auto* const widget = createEditorWidget(def.field);

      auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kFieldRowSpacing);
      auto* const label = Gtk::make_managed<Gtk::Label>(std::string{fieldDef->label});

      label->set_halign(Gtk::Align::START);
      label->set_valign(Gtk::Align::CENTER);
      label->set_width_chars(kLabelWidthChars);
      label->set_xalign(0.0F);
      label->set_opacity(kLabelOpacity);
      label->add_css_class("ao-property-label");

      widget->set_halign(Gtk::Align::FILL);
      widget->set_hexpand(true);
      widget->set_valign(Gtk::Align::CENTER);

      row->append(*label);
      row->append(*widget);
      _metadataBox.append(*row);

      _editors.push_back(FieldEditor{.field = def.field, .widget = widget});
    }

    _notebook.append_page(_metadataScroll, "Metadata");
  }

  void TrackPropertiesDialog::setupPropertiesTab()
  {
    _propertiesScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _propertiesScroll.set_vexpand(true);
    _propertiesScroll.set_child(_propertiesBox);

    _propertiesBox.set_spacing(kBoxSpacing);
    _propertiesBox.set_valign(Gtk::Align::START);
    _propertiesBox.set_margin_start(kSectionSpacing);
    _propertiesBox.set_margin_end(kSectionSpacing);
    _propertiesBox.set_margin_top(kSectionSpacing);
    _propertiesBox.set_margin_bottom(kSectionSpacing);

    for (auto const& def : trackFieldUiDefinitions())
    {
      if (!def.propertyDialogReadonly)
      {
        continue;
      }

      auto const* const fieldDef = trackFieldDefinition(def.field);

      if (fieldDef == nullptr)
      {
        continue;
      }

      auto* const widget = createReadonlyWidget(def.field);

      auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kFieldRowSpacing);
      auto* const label = Gtk::make_managed<Gtk::Label>(std::string{fieldDef->label});

      label->set_halign(Gtk::Align::START);
      label->set_valign(Gtk::Align::CENTER);
      label->set_width_chars(kLabelWidthChars);
      label->set_xalign(0.0F);
      label->set_opacity(kLabelOpacity);
      label->add_css_class("ao-property-label");

      widget->set_halign(Gtk::Align::START);
      widget->set_hexpand(true);
      widget->set_valign(Gtk::Align::CENTER);

      row->append(*label);
      row->append(*widget);
      _propertiesBox.append(*row);

      _readonlyRows.push_back(FieldEditor{.field = def.field, .widget = widget});
    }

    _notebook.append_page(_propertiesScroll, "Properties");
  }

  Gtk::Widget* TrackPropertiesDialog::createEditorWidget(rt::TrackField field)
  {
    if (auto const* const fieldDef = trackFieldDefinition(field);
        fieldDef != nullptr && fieldDef->valueKind == rt::TrackFieldValueKind::Number)
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

    auto const txn = _library.readTransaction();
    auto const reader = _library.tracks().reader(txn);
    auto const& dictionary = _library.dictionary();
    auto const manifestReader = _library.manifest().reader(txn);

    bool first = true;

    for (auto const trackId : _trackIds)
    {
      if (auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both); optView)
      {
        if (first)
        {
          loadFirstTrack(*optView, dictionary, &manifestReader);
          first = false;
        }
        else
        {
          loadSubsequentTrack(*optView, dictionary, &manifestReader);
        }
      }
    }
  }

  void TrackPropertiesDialog::loadFirstTrack(library::TrackView const& view,
                                             library::DictionaryStore const& dictionary,
                                             library::FileManifestStore::Reader const* manifestReader)
  {
    for (auto& editor : _editors)
    {
      auto const* const def = trackFieldUiDefinition(editor.field);

      if (def == nullptr || def->readViewRawValue == nullptr || def->formatValue == nullptr)
      {
        continue;
      }

      auto const rawValue = def->readViewRawValue(view, dictionary, manifestReader);
      editor.originalRawValue = rawValue;
      setWidgetValue(editor.field, editor.widget, def->formatValue(rawValue));
    }

    for (auto& row : _readonlyRows)
    {
      auto const* const def = trackFieldUiDefinition(row.field);

      if (def == nullptr || def->readViewRawValue == nullptr || def->formatValue == nullptr)
      {
        continue;
      }

      auto const rawValue = def->readViewRawValue(view, dictionary, manifestReader);
      row.originalRawValue = rawValue;
      setWidgetValue(row.field, row.widget, def->formatValue(rawValue));
    }
  }

  void TrackPropertiesDialog::loadSubsequentTrack(library::TrackView const& view,
                                                  library::DictionaryStore const& dictionary,
                                                  library::FileManifestStore::Reader const* manifestReader)
  {
    for (auto& editor : _editors)
    {
      if (editor.mixed)
      {
        continue;
      }

      auto const* const def = trackFieldUiDefinition(editor.field);

      if (def == nullptr || def->readViewRawValue == nullptr || def->formatValue == nullptr)
      {
        continue;
      }

      auto const rawValue = def->readViewRawValue(view, dictionary, manifestReader);

      if (rawValue != editor.originalRawValue)
      {
        editor.mixed = true;
        setEditorMixed(editor.field, editor.widget);
      }
    }

    for (auto& row : _readonlyRows)
    {
      if (row.mixed)
      {
        continue;
      }

      auto const* const def = trackFieldUiDefinition(row.field);

      if (def == nullptr || def->readViewRawValue == nullptr || def->formatValue == nullptr)
      {
        continue;
      }

      auto const rawValue = def->readViewRawValue(view, dictionary, manifestReader);

      if (rawValue != row.originalRawValue)
      {
        row.mixed = true;
        setEditorMixed(row.field, row.widget);
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
      if (editor.mixed)
      {
        continue;
      }

      auto const* const def = trackFieldUiDefinition(editor.field);

      if (def == nullptr || def->writePatch == nullptr)
      {
        continue;
      }

      auto rawValue = detail::TrackFieldRawValue{};

      if (auto* const entry = dynamic_cast<Gtk::Entry*>(editor.widget))
      {
        rawValue = detail::TrackFieldRawValue{std::in_place_type<std::string>, entry->get_text().raw()};
      }
      else if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(editor.widget))
      {
        rawValue = detail::TrackFieldRawValue{
          std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(spin->get_value_as_int())};
      }
      else
      {
        continue;
      }

      if (rawValue == editor.originalRawValue)
      {
        continue;
      }

      auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = rawValue};
      def->writePatch(ctx);
    }

    auto const result = _mutation.updateMetadata(_trackIds, patch);

    if (result)
    {
      for (auto const trackId : _trackIds)
      {
        _rowCache.invalidate(trackId);
      }
    }
  }

  void TrackPropertiesDialog::setWidgetValue(rt::TrackField /*field*/, Gtk::Widget* widget, std::string_view value)
  {
    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget))
    {
      entry->set_text(std::string{value});
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget))
    {
      auto intValue = 0;

      if (!value.empty())
      {
        std::from_chars(value.data(), value.data() + value.size(), intValue);
      }

      spin->set_value(static_cast<double>(intValue));
      return;
    }

    if (auto* const label = dynamic_cast<Gtk::Label*>(widget))
    {
      label->set_text(std::string{value});
    }
  }

  void TrackPropertiesDialog::setEditorMixed(rt::TrackField /*field*/, Gtk::Widget* widget)
  {
    widget->set_sensitive(false);

    if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget))
    {
      entry->set_placeholder_text("<Multiple Values>");
      return;
    }

    if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(widget))
    {
      spin->set_value(0.0);
      return;
    }

    if (auto* const label = dynamic_cast<Gtk::Label*>(widget))
    {
      label->set_text("<Multiple Values>");
    }
  }
} // namespace ao::gtk
