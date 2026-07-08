// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/component/track/TrackFieldGridCustomControls.h"
#include "layout/component/track/TrackFieldGridRows.h"
#include "layout/component/track/TrackFieldGridTextUtils.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackFieldUi.h"
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/field/TrackInlineEditWorkflow.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/detail/TrackCustomMetadataWorkflow.h>
#include <ao/uimodel/library/detail/TrackFieldGridPolicy.h>
#include <ao/uimodel/library/detail/TrackFieldGridSchema.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr float kLabelOpacity = 0.6F;

    class ColumnWidthAnchor final : public Gtk::Widget
    {
    public:
      explicit ColumnWidthAnchor(std::string_view cssClass)
      {
        set_halign(Gtk::Align::FILL);
        set_hexpand(false);
        set_can_target(false);
        set_focusable(false);
        set_overflow(Gtk::Overflow::HIDDEN);
        set_size_request(0, -1);
        set_visible(true);
        add_css_class(std::string{cssClass});
      }

      void setWidgets(std::vector<Gtk::Widget*> widgets)
      {
        _widgets = std::move(widgets);
        queue_resize();
      }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimumBaseline = -1;
        naturalBaseline = -1;
        minimum = 0;
        natural = 0;

        if (orientation != Gtk::Orientation::HORIZONTAL)
        {
          return;
        }

        for (auto* const widget : _widgets)
        {
          if (widget == nullptr)
          {
            continue;
          }

          std::int32_t labelMinimum = 0;
          std::int32_t labelNatural = 0;
          std::int32_t labelMinimumBaseline = -1;
          std::int32_t labelNaturalBaseline = -1;
          widget->measure(
            Gtk::Orientation::HORIZONTAL, -1, labelMinimum, labelNatural, labelMinimumBaseline, labelNaturalBaseline);
          natural = std::max(natural, labelNatural);
        }
      }

      void size_allocate_vfunc(int /*width*/, int /*height*/, int /*baseline*/) override {}

    private:
      std::vector<Gtk::Widget*> _widgets;
    };

    using namespace track_field_grid;

    class TrackFieldGridComponent final : public ILayoutComponent
    {
    public:
      TrackFieldGridComponent(LayoutContext& ctx, LayoutNode const& node)
        : _editCoordinator{ctx.parentWindow}
        , _writer{ctx.runtime.library().writer()}
        , _completion{ctx.runtime.completion()}
        , _scope{ctx.track.detailScope}
        , _detailUndo{ctx.track.detailUndo}
        , _compositePrimarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
        , _compositeSecondarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
      {
        _showAllFieldsButton.set_has_frame(false);
        _showAllFieldsButton.add_css_class("ao-detail-show-all-button");
        _showAllFieldsButton.add_css_class("ao-clickable");
        _showAllFieldsButton.set_halign(Gtk::Align::START);
        _showAllFieldsButton.set_size_request(0, -1);

        if (auto* const labelWidget = _showAllFieldsButton.get_child(); labelWidget != nullptr)
        {
          if (auto* const label = dynamic_cast<Gtk::Label*>(labelWidget); label != nullptr)
          {
            label->set_ellipsize(Pango::EllipsizeMode::END);
            label->set_size_request(0, -1);
          }
        }

        _showAllFieldsButton.signal_clicked().connect([this] { onToggleShowEmptyMetadata(); });

        _metadataHeader.button.signal_clicked().connect([this] { onToggleMetadata(); });
        _technicalHeader.button.signal_clicked().connect([this] { onToggleTechnical(); });

        _metadataHeader.addCssClass("ao-track-detail-section-meta");
        _technicalHeader.addCssClass("ao-track-detail-section-tech");

        _metadataActionBox.set_hexpand(true);
        _metadataActionBox.add_css_class("ao-detail-field-action-row");
        _metadataActionSpacer.set_hexpand(true);
        _metadataActionSpacer.set_size_request(0, -1);
        _metadataActionBox.append(_showAllFieldsButton);
        _metadataActionBox.append(_metadataActionSpacer);
        _metadataActionBox.append(_addMetadataButton.button());

        updateHeaderStyles();

        _wrapper.setGrid(_grid);
        _wrapper.set_vexpand(true);
        _grid.set_hexpand(true);
        _grid.set_column_spacing(kGridColumnSpacing);
        _grid.set_row_spacing(4);
        _grid.set_valign(Gtk::Align::START);
        _grid.set_vexpand(true);
        _valueColumnWidthAnchor.set_hexpand(true);

        auto const requestedCategories =
          node.getProp<std::vector<std::string>>("categories", {"metadata", "technical"});
        auto const includesCategory = [&requestedCategories](std::string_view const category)
        {
          return std::ranges::any_of(
            requestedCategories, [category](std::string const& item) { return item == category; });
        };

        _metadataCategoryEnabled = includesCategory("metadata");

        auto const projection = uimodel::buildTrackFieldGridSchema(uimodel::TrackFieldGridSchemaRequest{
          .includeMetadata = _metadataCategoryEnabled,
          .includeTechnical = includesCategory("technical"),
        });

        for (auto const field : projection.metadataFields)
        {
          _metadataRows.emplace_back(field);
          setupBuiltInRow(_metadataRows.back());
        }

        for (auto const fields : projection.compositeMetadataFields)
        {
          _compositeRows.emplace_back(fields.primaryField, fields.secondaryField);
          setupCompositeRow(_compositeRows.back());
        }

        for (auto const field : projection.technicalFields)
        {
          _technicalRows.emplace_back(field);
          setupBuiltInRow(_technicalRows.back(), true);
        }

        setupAddMetadataUi();
        buildGrid();

        if (_scope != nullptr)
        {
          _scopeConn = _scope->signalSnapshotChanged().connect([this](auto const& snap) { onSnapshot(snap); });
          onSnapshot(_scope->snapshot());
        }
      }

      ~TrackFieldGridComponent() override = default;

      TrackFieldGridComponent(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent(TrackFieldGridComponent&&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent&&) = delete;

      Gtk::Widget& widget() override { return _wrapper; }

    private:
      static constexpr std::int32_t kGridColumnSpacing = 12;
      static constexpr std::int32_t kValueColWidth = 3;
      static constexpr std::int32_t kFieldRowHeight = 28;

      static uimodel::TrackFieldEditValue editValueFromSnapshot(rt::TrackField field,
                                                                rt::TrackDetailSnapshot const& snap)
      {
        auto const& aggregate = rt::trackFieldArrayAt(snap.fields, field);

        if (!aggregate.optValue)
        {
          return uimodel::TrackFieldEditValue{};
        }

        if (auto const* text = std::get_if<std::string>(&*aggregate.optValue); text != nullptr)
        {
          return uimodel::TrackFieldEditValue{std::in_place_type<std::string>, *text};
        }

        if (auto const* number = std::get_if<std::uint16_t>(&*aggregate.optValue); number != nullptr)
        {
          return uimodel::TrackFieldEditValue{std::in_place_type<std::uint16_t>, *number};
        }

        return uimodel::TrackFieldEditValue{};
      }

      bool shouldShowRow(BuiltInRow const& row, rt::TrackDetailSnapshot const& snap) const
      {
        auto const text = uimodel::displayTextForTrackField(row.field, snap, uimodel::kMultipleTrackValuesText, true);
        return shouldShowTrackFieldGridMetadataFieldRow(
          TrackFieldGridMetadataFieldVisibility{.metadataExpanded = _metadataExpanded,
                                                .showEmptyMetadata = _showEmptyMetadata,
                                                .editorEditing = row.valueEditor.getEditing(),
                                                .hasDisplayText = !text.empty()});
      }

      bool shouldShowCompositeRow(CompositeBuiltInRow const& row, rt::TrackDetailSnapshot const& snap) const
      {
        auto const primText =
          uimodel::displayTextForTrackField(row.primaryField, snap, uimodel::kCompositeMixedTrackText, false);
        auto const secText =
          uimodel::displayTextForTrackField(row.secondaryField, snap, uimodel::kCompositeMixedTrackText, false);
        return shouldShowCompositeMetadataRow(CompositeMetadataVisibility{
          .metadataExpanded = _metadataExpanded,
          .showEmptyMetadata = _showEmptyMetadata,
          .primaryEditorEditing = row.primaryEditor.getEditing(),
          .secondaryEditorEditing = row.secondaryEditor.getEditing(),
          .hasPrimaryDisplayText = !primText.empty(),
          .hasSecondaryDisplayText = !secText.empty(),
        });
      }

      void updateHeaderLabels(rt::TrackDetailSnapshot const& snap)
      {
        if (_metadataExpanded)
        {
          _metadataHeader.label.set_text("Metadata");
        }
        else
        {
          auto const titleText = validUtf8Text(
            uimodel::displayTextForTrackField(rt::TrackField::Title, snap, uimodel::kMultipleTrackValuesText, true));
          auto const artistText = validUtf8Text(
            uimodel::displayTextForTrackField(rt::TrackField::Artist, snap, uimodel::kMultipleTrackValuesText, true));

          _metadataHeader.label.set_text(uimodel::formatMetadataHeader(titleText, artistText));
        }

        if (_technicalExpanded)
        {
          _technicalHeader.label.set_text("Audio Properties");
        }
        else
        {
          auto const codec = validUtf8Text(uimodel::displayTextForTrackField(rt::TrackField::Codec, snap, "", false));
          auto const sampleRate =
            validUtf8Text(uimodel::displayTextForTrackField(rt::TrackField::SampleRate, snap, "", false));
          auto const bitDepth =
            validUtf8Text(uimodel::displayTextForTrackField(rt::TrackField::BitDepth, snap, "", false));

          _technicalHeader.label.set_text(uimodel::formatTechnicalHeader(codec, sampleRate, bitDepth));
        }
      }

      void updateMetadataVisibility()
      {
        for (auto& row : _metadataRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        for (auto& row : _compositeRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        _metadataActionSlot.set_visible(_metadataExpanded);

        for (auto& row : _customRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        _metadataHeader.label.set_text("Metadata");
      }

      void updateMetadataVisibility(rt::TrackDetailSnapshot const& snap)
      {
        for (auto& row : _metadataRows)
        {
          bool const show = shouldShowRow(row, snap);
          row.labelSlot.set_visible(show);
          row.valueSlot.set_visible(show);
        }

        for (auto& row : _compositeRows)
        {
          bool const show = shouldShowCompositeRow(row, snap);
          row.labelSlot.set_visible(show);
          row.valueSlot.set_visible(show);
        }

        _metadataActionSlot.set_visible(_metadataExpanded);

        for (std::uint32_t i = 0U; i < snap.customMetadata.size() && i < _customRows.size(); ++i)
        {
          updateCustomRow(_customRows[i], snap.customMetadata[i]);
        }

        updateHeaderLabels(snap);
      }

      void onToggleShowEmptyMetadata()
      {
        _showEmptyMetadata = !_showEmptyMetadata;
        _showAllFieldsButton.set_label(_showEmptyMetadata ? "Hide empty fields" : "Show empty fields");

        if (_scope != nullptr)
        {
          updateMetadataVisibility(_scope->snapshot());
        }
        else
        {
          updateMetadataVisibility();
        }
      }

      void onToggleMetadata()
      {
        _metadataExpanded = !_metadataExpanded;
        _metadataHeader.setExpanded(_metadataExpanded);
        updateHeaderStyles();

        if (_scope != nullptr)
        {
          updateMetadataVisibility(_scope->snapshot());
        }
        else
        {
          updateMetadataVisibility();
        }
      }

      void onToggleTechnical()
      {
        _technicalExpanded = !_technicalExpanded;
        _technicalHeader.setExpanded(_technicalExpanded);
        updateHeaderStyles();

        if (_scope != nullptr)
        {
          updateHeaderLabels(_scope->snapshot());
        }

        for (auto& row : _technicalRows)
        {
          row.labelSlot.set_visible(_technicalExpanded);
          row.valueSlot.set_visible(_technicalExpanded);
        }
      }

      void updateHeaderStyles()
      {
        auto toggleClass = [](auto& header, bool const expanded)
        {
          if (expanded)
          {
            header.button.remove_css_class("is-collapsed");
          }
          else
          {
            header.button.add_css_class("is-collapsed");
          }
        };

        toggleClass(_metadataHeader, _metadataExpanded);
        toggleClass(_technicalHeader, _technicalExpanded);
      }

      void onSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        for (auto& row : _metadataRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _technicalRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _compositeRows)
        {
          updateCompositeRow(row, snap);
        }

        updateCustomRows(snap);
        updateMetadataVisibility(snap);
      }

      void setupBuiltInRow(BuiltInRow& row, bool isTechnical = false)
      {
        auto const* def = rt::trackFieldDefinition(row.field);
        row.label.set_text(std::string{def != nullptr ? def->label : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.valueEditor);
        row.valueEditor.add_css_class("ao-property-value");
        _editCoordinator.registerEditor(row.valueEditor);

        if (isTechnical)
        {
          row.label.add_css_class("ao-detail-field-technical");
          row.valueEditor.add_css_class("ao-detail-field-technical-value");
        }

        if (row.editable)
        {
          if (rt::trackFieldSupportsValueCompletion(row.field))
          {
            row.valueEditor.setCompletionProvider(rt::MetadataValueCompleter{_completion, row.field}.asProvider());
          }

          row.valueEditor.add_css_class("ao-property-editable");
          row.valueEditor.signalCommitted().connect([this, field = row.field] { onBuiltInEdited(field); });
          row.valueEditor.signalCanceled().connect(
            [this, field = row.field]
            {
              if (auto* row = findBuiltInRow(field); row != nullptr)
              {
                if (_scope != nullptr)
                {
                  updateBuiltInRow(*row, _scope->snapshot());
                }
              }
            });
        }
      }

      void setupCompositeRow(CompositeBuiltInRow& row)
      {
        auto const* def = rt::trackFieldDefinition(row.primaryField);
        row.label.set_text(std::string{def != nullptr ? def->label : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        row.separatorLabel.set_opacity(kLabelOpacity);

        configureValueEditor(row.primaryEditor);
        row.primaryEditor.add_css_class("ao-property-value");
        row.primaryEditor.removeMaxWidthConstraint();
        row.primaryEditor.set_hexpand(false);
        row.primaryEditor.set_halign(Gtk::Align::START);
        _compositePrimarySizeGroupPtr->add_widget(row.primaryEditor);

        configureValueEditor(row.secondaryEditor);
        row.secondaryEditor.add_css_class("ao-property-value");
        row.secondaryEditor.removeMaxWidthConstraint();
        row.secondaryEditor.set_hexpand(false);
        row.secondaryEditor.set_halign(Gtk::Align::START);
        _compositeSecondarySizeGroupPtr->add_widget(row.secondaryEditor);

        auto bindEditor = [this](DetailFieldEditor& editor, bool isEditable, rt::TrackField field)
        {
          _editCoordinator.registerEditor(editor);

          if (isEditable)
          {
            if (rt::trackFieldSupportsValueCompletion(field))
            {
              editor.setCompletionProvider(rt::MetadataValueCompleter{_completion, field}.asProvider());
            }

            editor.add_css_class("ao-property-editable");
            editor.signalCommitted().connect([this, field] { onCompositeEdited(field); });
            editor.signalCanceled().connect(
              [this, field]
              {
                if (auto* row = findCompositeBuiltInRow(field); row != nullptr)
                {
                  if (_scope != nullptr)
                  {
                    updateCompositeRow(*row, _scope->snapshot());
                  }
                }
              });
          }
        };

        bindEditor(row.primaryEditor, row.primaryEditableFlag, row.primaryField);
        bindEditor(row.secondaryEditor, row.secondaryEditableFlag, row.secondaryField);
      }

      void updateBuiltInRow(BuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        if (row.valueEditor.getEditing())
        {
          return;
        }

        auto const displayText =
          validUtf8Text(uimodel::displayTextForTrackField(row.field, snap, uimodel::kMultipleTrackValuesText, true));
        row.valueEditor.setText(displayText);
        row.valueEditor.set_tooltip_text(displayText);
      }

      void updateCompositeRow(CompositeBuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        auto updateField = [](DetailFieldEditor& editor, rt::TrackField field, rt::TrackDetailSnapshot const& snap)
        {
          if (editor.getEditing())
          {
            return;
          }

          auto const displayText =
            validUtf8Text(uimodel::displayTextForTrackField(field, snap, uimodel::kCompositeMixedTrackText, false));
          editor.setText(displayText);
          editor.set_tooltip_text(displayText);
        };

        updateField(row.primaryEditor, row.primaryField, snap);
        updateField(row.secondaryEditor, row.secondaryField, snap);
      }

      bool applyFieldEdit(rt::TrackField field,
                          DetailFieldEditor& editor,
                          std::string_view newValue,
                          rt::TrackDetailSnapshot const& snap,
                          std::string_view mixedText,
                          bool showTechnicalUnknown)
      {
        auto const* uiDef = trackFieldUiDefinition(field);

        if (uiDef == nullptr || uiDef->parseInlineEdit == nullptr || !uimodel::trackFieldCanWritePatch(field))
        {
          return false;
        }

        auto const oldText =
          validUtf8Text(uimodel::displayTextForTrackField(field, snap, mixedText, showTechnicalUnknown));
        auto const newText = std::string{newValue};
        bool firstApply = true;
        auto const result = uimodel::TrackInlineEditWorkflow::apply(
          uimodel::TrackInlineEditRequest{.field = field, .oldText = oldText, .newText = newText},
          uimodel::TrackInlineEditHooks{
            .parse = [uiDef](std::string_view text) { return uiDef->parseInlineEdit(text); },
            .readCurrentValue = [&snap, field] { return editValueFromSnapshot(field, snap); },
            .applyValue =
              [&editor, &firstApply, &newText, &oldText](uimodel::TrackFieldEditValue const&)
            {
              editor.setText(firstApply ? newText : oldText);
              firstApply = false;
            },
            .writePatch = [field](rt::MetadataPatch& patch, uimodel::TrackFieldEditValue const& value)
            { std::ignore = uimodel::writeTrackFieldPatch(patch, field, value); },
            .commitPatch = [this, &snap](rt::MetadataPatch const& patch) -> Result<rt::UpdateTrackMetadataReply>
            { return _writer.updateMetadata(snap.trackIds, patch); },
          });

        switch (result.outcome)
        {
          case uimodel::TrackInlineEditOutcome::NoChange:
          case uimodel::TrackInlineEditOutcome::Applied: return true;
          case uimodel::TrackInlineEditOutcome::NotEditable: return false;
          case uimodel::TrackInlineEditOutcome::ParseRejected:
            APP_LOG_ERROR("Failed to parse edit value for {}: {}", rt::trackFieldId(field), result.statusMessage);
            return false;
          case uimodel::TrackInlineEditOutcome::MutationRejected:
            APP_LOG_ERROR("Metadata update failed: {}", result.statusMessage);
            return false;
        }

        return false;
      }

      void onBuiltInEdited(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditor.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = row->valueEditor.getText().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, false))
        {
          return;
        }

        if (!applyFieldEdit(field, row->valueEditor, newValue, snap, uimodel::kMultipleTrackValuesText, true))
        {
          updateBuiltInRow(*row, snap);
        }
      }

      void onCompositeEdited(rt::TrackField field)
      {
        auto* row = findCompositeBuiltInRow(field);

        if (row == nullptr || _scope == nullptr)
        {
          return;
        }

        bool const isPrimary = (row->primaryField == field);
        bool const editable = isPrimary ? row->primaryEditableFlag : row->secondaryEditableFlag;
        auto& editor = isPrimary ? row->primaryEditor : row->secondaryEditor;

        if (!editable || editor.getEditing())
        {
          return;
        }

        auto const& snap = _scope->snapshot();
        auto const newValue = editor.getText().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, true))
        {
          return;
        }

        if (!applyFieldEdit(field, editor, newValue, snap, uimodel::kCompositeMixedTrackText, false))
        {
          updateCompositeRow(*row, snap);
        }
      }

      void updateCustomRows(rt::TrackDetailSnapshot const& snap)
      {
        bool const selectedTracksChanged = _customSectionHasSelectedTracks != !snap.trackIds.empty();
        bool keysChanged = (snap.customMetadata.size() != _customRows.size());

        if (!keysChanged)
        {
          for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
          {
            if (snap.customMetadata[i].key != _customRows[i].key)
            {
              keysChanged = true;
              break;
            }
          }
        }

        if (keysChanged || selectedTracksChanged)
        {
          for (auto& row : _customRows)
          {
            _editCoordinator.forgetEditor(row.editor);
          }

          _customRows.clear();

          for (auto const& item : snap.customMetadata)
          {
            _customRows.emplace_back(item.key, kGridColumnSpacing);
            setupCustomRow(_customRows.back());
          }

          buildGrid();
        }

        for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
        {
          updateCustomRow(_customRows[i], snap.customMetadata[i]);
        }
      }

      void setupCustomRow(CustomRow& row)
      {
        row.label.set_text(validUtf8Text(row.key));
        configureKeyLabel(row.label);
        row.label.set_ellipsize(Pango::EllipsizeMode::END);
        row.label.set_width_chars(0);
        row.label.set_max_width_chars(24);
        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.editor);
        row.editor.add_css_class("ao-property-value");
        row.editor.add_css_class("ao-property-editable");
        _editCoordinator.registerEditor(row.editor);
        row.editor.signalCommitted().connect([this, key = row.key] { onCustomEdited(key); });
        row.editor.signalCanceled().connect(
          [this, key = row.key]
          {
            if (auto* row = findCustomRow(key); row != nullptr)
            {
              if (_scope != nullptr)
              {
                updateCustomRows(_scope->snapshot());
              }
            }
          });

        row.deleteButton.set_icon_name("user-trash-symbolic");
        row.deleteButton.set_halign(Gtk::Align::END);
        row.deleteButton.set_hexpand(false);
        row.deleteButton.set_has_frame(false);
        row.deleteButton.add_css_class("ao-icon-button");
        row.deleteButton.add_css_class("ao-detail-field-delete");
        row.deleteButton.set_tooltip_text("Delete Custom Metadata");
        row.deleteButton.signal_clicked().connect([this, key = row.key] { onCustomDeleted(key); });

        row.partialIcon.set_from_icon_name("dialog-warning-symbolic");
        row.partialIcon.set_opacity(kLabelOpacity);
        row.partialIcon.set_tooltip_text("Missing on some tracks");
      }

      void updateCustomRow(CustomRow& row, rt::CustomMetadataItem const& item)
      {
        auto const displayText = validUtf8Text(uimodel::displayTextForTrackCustomMetadata(item));
        auto const show = shouldShowTrackFieldGridMetadataFieldRow(
          TrackFieldGridMetadataFieldVisibility{.metadataExpanded = _metadataExpanded,
                                                .showEmptyMetadata = _showEmptyMetadata,
                                                .editorEditing = row.editor.getEditing(),
                                                .hasDisplayText = !displayText.empty()});
        row.labelSlot.set_visible(show);
        row.valueSlot.set_visible(show);

        if (row.editor.getEditing())
        {
          return;
        }

        row.editor.setText(displayText);
        row.editor.set_tooltip_text(displayText);
        row.partialIcon.set_visible(!item.presentOnAll);
      }

      void onCustomEdited(std::string key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editor.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();
        auto const newValue = row->editor.getText().raw();

        if (uimodel::isProtectedTrackCustomMetadataEditText(newValue))
        {
          return;
        }

        auto const replyResult =
          _writer.updateMetadata(snap.trackIds, uimodel::makeCustomMetadataUpdatePatch(key, newValue));

        if (!replyResult)
        {
          APP_LOG_ERROR("Custom metadata update failed: {}", replyResult.error().message);
          return;
        }

        if (!replyResult->mutatedIds.empty() && _detailUndo != nullptr)
        {
          _detailUndo->clearIfAffectsCustomMetadata(key, snap.trackIds);
        }
      }

      void onCustomDeleted(std::string key)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        auto const optPrevValue = uimodel::undoValueForDeletedTrackCustomMetadata(snap, key);
        auto const replyResult = _writer.updateMetadata(snap.trackIds, uimodel::makeCustomMetadataDeletePatch(key));

        if (!replyResult)
        {
          APP_LOG_ERROR("Custom metadata delete failed: {}", replyResult.error().message);
          return;
        }

        if (!replyResult->mutatedIds.empty() && optPrevValue && _detailUndo != nullptr)
        {
          _detailUndo->showCustomMetadataDeleted(std::move(key), snap.trackIds, *optPrevValue);
        }
      }

      void onCustomAdded(std::string key, std::string value)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const& snap = _scope->snapshot();

        if (uimodel::validateCustomMetadataAddition(snap, key) != uimodel::CustomMetadataAddValidation::Accepted)
        {
          _addMetadataButton.markKeyError();
          return;
        }

        _addMetadataButton.popdown();

        auto const replyResult =
          _writer.updateMetadata(snap.trackIds, uimodel::makeCustomMetadataUpdatePatch(key, value));

        if (!replyResult)
        {
          APP_LOG_ERROR("Custom metadata add failed: {}", replyResult.error().message);
          return;
        }

        if (!replyResult->mutatedIds.empty() && _detailUndo != nullptr)
        {
          _detailUndo->clearIfAffectsCustomMetadata(key, snap.trackIds);
        }

        _addMetadataButton.clearInputs();
      }

      void buildGrid()
      {
        _addMetadataButton.popdown();
        clearGrid();
        std::int32_t rowIndex = 0;

        refreshColumnWidthAnchors();
        _grid.attach(_keyColumnWidthAnchor, 0, 0, 1, 1);
        _grid.attach(_valueColumnWidthAnchor, 1, 0, kValueColWidth, 1);

        auto const sectionAvailability = TrackFieldGridSectionAvailability{
          .metadataCategoryEnabled = _metadataCategoryEnabled,
          .hasMetadataFields = !_metadataRows.empty() || !_compositeRows.empty(),
          .hasSelectedTracks = (_scope != nullptr && !_scope->snapshot().trackIds.empty()),
          .hasTechnicalFields = !_technicalRows.empty(),
        };
        _customSectionHasSelectedTracks = shouldRenderCustomMetadataArea(sectionAvailability);

        if (shouldRenderMetadataSection(sectionAvailability))
        {
          _metadataHeader.setExpanded(_metadataExpanded);
          _grid.attach(_metadataHeader.button, 0, rowIndex++, 1 + kValueColWidth, 1);

          attachBuiltInGroup(_metadataRows, rowIndex, _metadataExpanded);

          for (auto& row : _compositeRows)
          {
            attachCompositeRow(row, rowIndex++);
            row.labelSlot.set_visible(_metadataExpanded);
            row.valueSlot.set_visible(_metadataExpanded);
          }

          for (auto& row : _customRows)
          {
            _grid.attach(row.labelSlot, 0, rowIndex, 1, 1);
            _grid.attach(row.valueSlot, 1, rowIndex, kValueColWidth, 1);
            row.labelSlot.set_visible(_metadataExpanded);
            row.valueSlot.set_visible(_metadataExpanded);
            rowIndex++;
          }

          _showAllFieldsButton.set_visible(sectionAvailability.hasMetadataFields);
          _addMetadataButton.button().set_visible(shouldRenderCustomMetadataArea(sectionAvailability));
          _grid.attach(_metadataActionSlot, 0, rowIndex++, 1 + kValueColWidth, 1);
          _metadataActionSlot.set_visible(_metadataExpanded);
        }

        if (shouldRenderTechnicalSection(sectionAvailability))
        {
          _technicalHeader.setExpanded(_technicalExpanded);
          _grid.attach(_technicalHeader.button, 0, rowIndex++, 1 + kValueColWidth, 1);
          attachBuiltInGroup(_technicalRows, rowIndex, _technicalExpanded);
        }
      }

      void clearGrid()
      {
        while (auto* child = _grid.get_first_child())
        {
          _grid.remove(*child);
        }
      }

      void attachBuiltInRow(BuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachCompositeRow(CompositeBuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachBuiltInGroup(std::deque<BuiltInRow>& rows, std::int32_t& rowIndex, bool const expanded)
      {
        for (auto& row : rows)
        {
          attachBuiltInRow(row, rowIndex++);
          row.labelSlot.set_visible(expanded);
          row.valueSlot.set_visible(expanded);
        }
      }

      void refreshColumnWidthAnchors()
      {
        auto keyWidgets = std::vector<Gtk::Widget*>{};
        auto valueWidgets = std::vector<Gtk::Widget*>{};
        auto const rowCount = _metadataRows.size() + _compositeRows.size() + _technicalRows.size() + _customRows.size();
        keyWidgets.reserve(rowCount);
        valueWidgets.reserve(rowCount + 1);

        for (auto& row : _metadataRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _compositeRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _technicalRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _customRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        _keyColumnWidthAnchor.setWidgets(std::move(keyWidgets));
        _valueColumnWidthAnchor.setWidgets(std::move(valueWidgets));
      }

      void configureValueBox(Gtk::Widget& box)
      {
        box.set_halign(Gtk::Align::FILL);
        box.set_hexpand(true);
        box.set_overflow(Gtk::Overflow::HIDDEN);
        box.set_size_request(0, -1);
      }

      void configureKeyLabel(Gtk::Label& label)
      {
        label.set_halign(Gtk::Align::END);
        label.set_xalign(1.0F);
        label.set_hexpand(false);
        label.set_overflow(Gtk::Overflow::HIDDEN);
        label.set_size_request(0, -1);
        label.set_ellipsize(Pango::EllipsizeMode::NONE);
        label.set_wrap(false);
        label.set_lines(1);
      }

      void configureValueEditor(DetailFieldEditor& editor)
      {
        editor.set_halign(Gtk::Align::FILL);
        editor.set_hexpand(true);
        editor.set_overflow(Gtk::Overflow::HIDDEN);
        editor.set_size_request(0, -1);
      }

      void setupAddMetadataUi()
      {
        _addMetadataButton.signalAddRequested().connect([this](std::string key, std::string value)
                                                        { onCustomAdded(std::move(key), std::move(value)); });
      }

      BuiltInRow* findBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _metadataRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        for (auto& row : _technicalRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CompositeBuiltInRow* findCompositeBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _compositeRows)
        {
          if (row.primaryField == field || row.secondaryField == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CustomRow* findCustomRow(std::string const& key)
      {
        for (auto& row : _customRows)
        {
          if (row.key == key)
          {
            return &row;
          }
        }

        return nullptr;
      }

      Gtk::Grid _grid;
      DetailEditCoordinator _editCoordinator;
      rt::LibraryWriter& _writer;
      rt::CompletionService& _completion;
      ITrackDetailScope* _scope;
      TrackDetailUndoController* _detailUndo;
      std::deque<BuiltInRow> _metadataRows;
      std::deque<CompositeBuiltInRow> _compositeRows;
      std::deque<BuiltInRow> _technicalRows;
      std::deque<CustomRow> _customRows;

      bool _metadataExpanded = true;
      bool _technicalExpanded = false;
      bool _metadataCategoryEnabled = true;
      bool _customSectionHasSelectedTracks = false;

      SectionHeaderRow _metadataHeader{"Metadata"};
      SectionHeaderRow _technicalHeader{"Audio Properties"};

      Gtk::Button _showAllFieldsButton{"Show empty fields"};
      Gtk::Label _metadataActionSpacer;
      AddCustomMetadataButton _addMetadataButton;
      Gtk::Box _metadataActionBox{Gtk::Orientation::HORIZONTAL, 4};
      FixedHeightWidgetSlot _metadataActionSlot{_metadataActionBox, false, false, kFieldRowHeight};
      bool _showEmptyMetadata = false;

      Glib::RefPtr<Gtk::SizeGroup> _compositePrimarySizeGroupPtr;
      Glib::RefPtr<Gtk::SizeGroup> _compositeSecondarySizeGroupPtr;

      ColumnWidthAnchor _keyColumnWidthAnchor{"ao-key-column-width-anchor"};
      ColumnWidthAnchor _valueColumnWidthAnchor{"ao-value-column-width-anchor"};

      sigc::connection _scopeConn;

      ConstrainedGridBox _wrapper;
    };

    std::unique_ptr<ILayoutComponent> createTrackFieldGrid(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackFieldGridComponent>(ctx, node);
    }
  } // namespace

  void registerTrackFieldGridComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.fieldGrid",
       .displayName = "Field Grid",
       .category = LayoutComponentCategory::Track,
       .props = {{.name = "categories", .kind = LayoutPropertyKind::StringList, .label = "Categories"}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackFieldGrid);
  }
} // namespace ao::gtk::layout
