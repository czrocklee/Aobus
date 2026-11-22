// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "common/AccessibleLabel.h"
#include "common/UiWorkflow.h"
#include "completion/EntryCompletionController.h"
#include "i18n/GtkText.h"
#include "layout/LayoutConstants.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/property/TrackPropertiesPreparation.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/adaptors/track_obj.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  using i18n::MessageId;
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

    void revealWithdrawnInputPlaceholder(Gtk::Entry& entry, uimodel::TrackPropertiesFormEditorKind const kind)
    {
      auto const text = entry.get_text().raw();

      if (text.empty() || !entry.get_editable())
      {
        return;
      }

      // Zero (including a partial leading zero while typing) withdraws a batch number.
      // Wait for focus to leave before showing the baseline placeholder again.
      if (auto const inputRes = uimodel::parseTrackPropertiesFieldInput(kind, text);
          inputRes && uimodel::isEmptyTrackFieldEdit(*inputRes))
      {
        entry.set_text("");
      }
    }
  } // namespace

  TrackPropertiesDialog::TrackPropertiesDialog(Gtk::Window& parent,
                                               async::Runtime& asyncRuntime,
                                               rt::Library& library,
                                               rt::CompletionService& completion,
                                               i18n::MessageCatalog textCatalog,
                                               TrackRowCache& rowCache,
                                               std::span<TrackId const> const trackIds)
    : AppDialog{}
    , _library{library}
    , _asyncRuntime{asyncRuntime}
    , _completion{completion}
    , _textCatalog{std::move(textCatalog)}
    , _rowCache{rowCache}
    , _formModel{_textCatalog}
    , _formSpec{uimodel::buildTrackPropertiesFormSpec(_textCatalog)}
    , _creditsEditor{_completion, _textCatalog, _formModel, [this] { updateSaveEnabled(); }}
  {
    auto const prepareRes = prepareEditing(trackIds);
    // A failed open still identifies the requested selection, but offers no batch edit actions.
    auto const titleCount = prepareRes ? _formModel.targetCount() : trackIds.size();
    auto const title =
      titleCount > 1
        ? i18n::requiredFormat(_textCatalog, MessageId::GtkTrackPropertiesSelectedTitle, {{"count", titleCount}})
        : gtkText(_textCatalog, MessageId::GtkTrackPropertiesTitle);

    set_title(title);
    configureForParent(parent);
    set_default_size(-1, -1);

    buildUi();

    if (prepareRes)
    {
      applyLoadedFields();
    }
    else
    {
      _sessionErrorLabel.set_text(i18n::requiredFormat(
        _textCatalog, MessageId::GtkTrackEditingUnavailable, {{"detail", prepareRes.error().message}}));
      _sessionErrorLabel.set_visible(true);
    }

    updateEditorSensitivity();
    updateSaveEnabled();

    signal_response().connect(
      [this](std::int32_t const responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          handleSaveClicked();
          return;
        }

        if (responseId == Gtk::ResponseType::CLOSE || responseId == Gtk::ResponseType::CANCEL)
        {
          close();
        }
      });
    signal_close_request().connect(
      [this]
      {
        // Every close route arrives here, including Close, Escape, and the
        // window manager. A Credits draft is the one unsaved state worth a
        // question, because rebuilding an edited list is laborious.
        if (_interactionState == InteractionState::Editing && !_discardConfirmed && hasUnsavedCredits())
        {
          promptDiscardCredits();
          return true;
        }

        if (_interactionState != InteractionState::Closing)
        {
          // A native close need not retire a retained C++ owner. Cancel its
          // presentation delivery, not the independently admitted command.
          _tasks.cancelAll();
        }

        return false;
      },
      false);
  }

  TrackPropertiesDialog::~TrackPropertiesDialog() = default;

  void TrackPropertiesDialog::buildUi()
  {
    addCancelAction(gtkText(_textCatalog, MessageId::GtkCommonClose), Gtk::ResponseType::CLOSE);
    _saveButton = addPrimaryAction(gtkText(_textCatalog, MessageId::GtkCommonSave), Gtk::ResponseType::OK);
    _saveButton->set_sensitive(false);

    _notebook.add_css_class("ao-properties-notebook");
    _notebook.set_vexpand(true);

    _sessionErrorLabel.set_halign(Gtk::Align::START);
    _sessionErrorLabel.set_xalign(0.0F);
    _sessionErrorLabel.set_wrap(true);
    _sessionErrorLabel.set_visible(false);
    _sessionErrorLabel.add_css_class("ao-status-error");
    _sessionErrorLabel.add_css_class("ao-properties-session-error");

    _contentBox.set_spacing(kSectionSpacing);
    _contentBox.append(_sessionErrorLabel);
    _contentBox.append(_notebook);

    buildMetadataTab();
    buildPropertiesTab();

    setContentWidget(_contentBox);
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
    auto const targetCount = _formModel.targetCount();

    for (auto const& row : _formSpec.metadataRows)
    {
      // The Credits surface owns these scoped previews and their edit actions.
      if (rt::creditKindForTrackField(row.field))
      {
        continue;
      }

      auto* const widget = createEditorWidget(row.field, row.editorKind);
      setAccessibleLabel(*widget, row.label);
      auto editor = FieldEditor{.field = row.field, .widget = widget, .kind = row.editorKind};

      if (row.editorKind != uimodel::TrackPropertiesFormEditorKind::ReadonlyText &&
          (targetCount > 1 || (targetCount == 1 && row.field == rt::TrackField::Title)))
      {
        auto* const controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingSmall);
        controls->append(*widget);
        auto const clearLabel = i18n::requiredFormat(
          _textCatalog, MessageId::TrackClearFieldForTracks, {{"field", row.label}, {"count", targetCount}});
        editor.clearButton = Gtk::make_managed<Gtk::ToggleButton>(clearLabel);

        if (auto* const label = dynamic_cast<Gtk::Label*>(editor.clearButton->get_child()); label != nullptr)
        {
          label->set_wrap(true);
          label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
        }

        setAccessibleLabel(*editor.clearButton, clearLabel);
        controls->append(*editor.clearButton);
        list->addRow(row.label, *controls);
      }
      else
      {
        list->addRow(row.label, *widget);
      }

      _editors.push_back(editor);
    }

    _metadataBox.append(*list);
    _metadataBox.append(_creditsEditor);
    _notebook.append_page(_metadataScroll, gtkText(_textCatalog, MessageId::TrackMetadataHeading));
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

    for (auto const& row : _formSpec.propertyRows)
    {
      auto* const widget = createReadonlyWidget(row.field);
      list->addRow(std::string{row.label}, *widget);

      _readonlyRows.push_back(FieldEditor{.field = row.field, .widget = widget});
    }

    _propertiesBox.append(*list);
    _notebook.append_page(_propertiesScroll, gtkText(_textCatalog, MessageId::GtkTrackPropertiesTitle));
  }

  Gtk::Widget* TrackPropertiesDialog::createEditorWidget(rt::TrackField field,
                                                         uimodel::TrackPropertiesFormEditorKind editorKind)
  {
    if (editorKind == uimodel::TrackPropertiesFormEditorKind::ReadonlyText)
    {
      return createReadonlyWidget(field);
    }

    if (editorKind == uimodel::TrackPropertiesFormEditorKind::Number && _formModel.targetCount() == 1)
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

    if (editorKind == uimodel::TrackPropertiesFormEditorKind::Number)
    {
      // Batch input must retain empty and out-of-range drafts across focus changes, not clamp them.
      entry->set_input_purpose(Gtk::InputPurpose::DIGITS);
    }

    if (rt::supportsTrackFieldValueCompletion(field))
    {
      _completionControllers.push_back(CompletionControllerBinding{
        .entry = entry,
        .controllerPtr = std::make_unique<EntryCompletionController>(
          *entry, _textCatalog, rt::MetadataValueCompleter{_completion, field}.asProvider()),
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

  Result<> TrackPropertiesDialog::prepareEditing(std::span<TrackId const> const trackIds)
  {
    auto baseline = uimodel::TrackPropertiesFormModel{_textCatalog};
    auto sessionRes = [&] -> Result<uimodel::TrackAuthoringSession>
    {
      auto snapshot = _library.snapshot();
      return uimodel::prepareTrackProperties(_library, snapshot, trackIds, _formSpec, baseline);
    }();

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    auto session = std::move(*sessionRes);

    auto invalidatedSub = session.onInvalidated(
      [this]
      {
        updateEditorSensitivity();
        updateSaveEnabled();
      });

    if (!session.isCurrent())
    {
      return makeError(Error::Code::InvalidState, "The library changed while Track Properties was opening");
    }

    _optEditSession.emplace(std::move(session));
    _editSessionInvalidatedSubscription = std::move(invalidatedSub);
    _formModel = std::move(baseline);
    return {};
  }

  void TrackPropertiesDialog::applyLoadedFields()
  {
    _creditsEditor.refresh();

    for (auto& editor : _editors)
    {
      applyRowView(editor.widget, _formModel.rowView(editor.field));
      // Only input after hydration can stage a field edit.
      bindEditor(editor);
    }

    for (auto& row : _readonlyRows)
    {
      applyRowView(row.widget, _formModel.rowView(row.field));
    }

    if (!_formModel.segmentRows().empty())
    {
      auto* const list = Gtk::make_managed<FormBoxedList>();

      for (auto const& row : _formModel.segmentRows())
      {
        auto* const label = Gtk::make_managed<Gtk::Label>(row.text);
        label->set_selectable(true);
        label->set_xalign(0.0F);
        label->set_hexpand(true);
        label->add_css_class("ao-property-value");
        list->addRow(row.label, *label);
      }

      _propertiesBox.append(*list);
    }
  }

  void TrackPropertiesDialog::handleSaveClicked()
  {
    if (_interactionState != InteractionState::Editing || !_optEditSession || _formModel.creditsEditor().isEditing())
    {
      return;
    }

    if (hasInvalidInput())
    {
      return;
    }

    auto const patch = _formModel.buildPatch();

    auto submission = _optEditSession->submitMetadataAsync(std::move(patch));
    setInteractionState(InteractionState::Submitting);
    spawnUiTask(
      _asyncRuntime,
      _tasks,
      *this,
      "track properties save",
      std::move(submission),
      [](TrackPropertiesDialog* owner, Result<uimodel::TrackMetadataSubmitResult> replyRes)
      {
        if (!replyRes)
        {
          owner->setInteractionState(InteractionState::Editing);
          AppDialog::presentMessage(*owner,
                                    gtkText(owner->_textCatalog, MessageId::GtkTrackSaveFailed),
                                    replyRes.error().message,
                                    {AppDialogAction{.label = gtkText(owner->_textCatalog, MessageId::GtkCommonClose),
                                                     .responseId = Gtk::ResponseType::CLOSE,
                                                     .role = AppDialogActionRole::Cancel}},
                                    Gtk::ResponseType::CLOSE);
          return;
        }

        if (replyRes->status == rt::AuthoringStatus::Busy)
        {
          owner->setInteractionState(InteractionState::Editing);
          AppDialog::presentMessage(*owner,
                                    gtkText(owner->_textCatalog, MessageId::GtkTrackSaveFailed),
                                    gtkText(owner->_textCatalog, MessageId::LibraryBusyTryAgain),
                                    {AppDialogAction{.label = gtkText(owner->_textCatalog, MessageId::GtkCommonClose),
                                                     .responseId = Gtk::ResponseType::CLOSE,
                                                     .role = AppDialogActionRole::Cancel}},
                                    Gtk::ResponseType::CLOSE);
          return;
        }

        if (replyRes->status != rt::AuthoringStatus::Applied && replyRes->status != rt::AuthoringStatus::NoOp)
        {
          owner->setInteractionState(InteractionState::Editing);
          AppDialog::presentMessage(*owner,
                                    gtkText(owner->_textCatalog, MessageId::GtkTrackSaveStaleTitle),
                                    gtkText(owner->_textCatalog, MessageId::GtkTrackSaveStale),
                                    {AppDialogAction{.label = gtkText(owner->_textCatalog, MessageId::GtkCommonClose),
                                                     .responseId = Gtk::ResponseType::CLOSE,
                                                     .role = AppDialogActionRole::Cancel}},
                                    Gtk::ResponseType::CLOSE);
          return;
        }

        owner->setInteractionState(InteractionState::Closing);

        for (auto const& change : replyRes->reply.changes)
        {
          owner->_rowCache.invalidate(change.trackId);
        }

        // Return from the task completion before closing the native window. A
        // managed toplevel may retire its C++ owner as part of close handling;
        // the tracked idle therefore owns no unguarded post-close access.
        Glib::signal_idle().connect_once(sigc::track_object([owner] { owner->close(); }, *owner));
      });
  }

  bool TrackPropertiesDialog::hasUnsavedCredits() const noexcept
  {
    return _formModel.creditsEditor().hasDraftChanges() || _formModel.pendingCredits().has_value();
  }

  void TrackPropertiesDialog::promptDiscardCredits()
  {
    // A repeated close request returns to the live question instead of stacking another.
    if (_discardPrompt != nullptr)
    {
      _discardPrompt->present();
      return;
    }

    _discardPrompt =
      AppDialog::presentMessage(*this,
                                gtkText(_textCatalog, MessageId::GtkTrackCreditsDiscardTitle),
                                gtkText(_textCatalog, MessageId::GtkTrackCreditsDiscardMessage),
                                {AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkTrackCreditsKeepEditing),
                                                 .responseId = Gtk::ResponseType::CANCEL,
                                                 .role = AppDialogActionRole::Cancel},
                                 AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkTrackCreditsDiscard),
                                                 .responseId = Gtk::ResponseType::REJECT,
                                                 .role = AppDialogActionRole::Primary}},
                                Gtk::ResponseType::CANCEL,
                                _callbackScope.guard(
                                  [this](std::int32_t const responseId)
                                  {
                                    _discardPrompt = nullptr;

                                    if (responseId != Gtk::ResponseType::REJECT)
                                    {
                                      return;
                                    }

                                    _discardConfirmed = true;
                                    // Leave the prompt's response handler before closing this window.
                                    Glib::signal_idle().connect_once(sigc::track_object([this] { close(); }, *this));
                                  }));
  }

  void TrackPropertiesDialog::setInteractionState(InteractionState const state)
  {
    _interactionState = state;

    if (state != InteractionState::Editing)
    {
      for (auto const& binding : _completionControllers)
      {
        binding.controllerPtr->hide();
      }
    }

    updateEditorSensitivity();
    updateSaveEnabled();
  }

  void TrackPropertiesDialog::updateEditorSensitivity()
  {
    auto const sessionCanEdit = _optEditSession && _optEditSession->isCurrent();
    auto const interactionCanEdit = _interactionState == InteractionState::Editing;
    _creditsEditor.set_sensitive(sessionCanEdit && interactionCanEdit);

    for (auto const& editor : _editors)
    {
      auto const view = _formModel.rowView(editor.field);
      auto const writable = sessionCanEdit && interactionCanEdit && view.editable;

      if (editor.clearButton != nullptr)
      {
        editor.clearButton->set_sensitive(writable);
      }

      auto const canEdit = writable && (editor.clearButton == nullptr || !editor.clearButton->get_active());
      editor.widget->set_sensitive(canEdit);

      if (auto* const editable = dynamic_cast<Gtk::Editable*>(editor.widget); editable != nullptr)
      {
        // Also reject delayed editable operations, such as a clipboard reply.
        editable->set_editable(canEdit);
      }
    }
  }

  void TrackPropertiesDialog::updateSaveEnabled()
  {
    if (_saveButton != nullptr)
    {
      auto const sessionCanSave = _optEditSession && _optEditSession->isCurrent();
      _saveButton->set_sensitive(_interactionState == InteractionState::Editing && sessionCanSave &&
                                 !hasInvalidInput() && _formModel.canSave());
    }
  }

  bool TrackPropertiesDialog::hasInvalidInput() const
  {
    return std::ranges::any_of(_editors, &FieldEditor::invalid);
  }

  void TrackPropertiesDialog::bindEditor(FieldEditor& editor)
  {
    if (auto* const entry = dynamic_cast<Gtk::Entry*>(editor.widget); entry != nullptr)
    {
      _editorConnections.emplace_back(
        entry->signal_changed().connect([this, &editor, entry] { updateEntryValue(editor, *entry); }));

      if (_formModel.targetCount() > 1 && editor.kind != uimodel::TrackPropertiesFormEditorKind::Text)
      {
        auto focusPtr = Gtk::EventControllerFocus::create();
        _editorConnections.emplace_back(focusPtr->signal_leave().connect(
          [this, entry, kind = editor.kind]
          {
            if (_interactionState == InteractionState::Editing)
            {
              revealWithdrawnInputPlaceholder(*entry, kind);
            }
          }));
        entry->add_controller(focusPtr);
      }

      if (editor.clearButton != nullptr)
      {
        _editorConnections.emplace_back(
          editor.clearButton->signal_toggled().connect([this, &editor, entry] { toggleFieldClear(editor, *entry); }));
      }
    }
    else if (auto* const spin = dynamic_cast<Gtk::SpinButton*>(editor.widget); spin != nullptr)
    {
      _editorConnections.emplace_back(spin->signal_value_changed().connect(
        [this, &editor, spin]
        {
          if (_interactionState == InteractionState::Editing && spin->get_editable())
          {
            _formModel.setFieldInput(editor.field, static_cast<std::uint16_t>(spin->get_value_as_int()));
            updateSaveEnabled();
          }
        }));
    }
  }

  void TrackPropertiesDialog::updateEntryValue(FieldEditor& editor, Gtk::Entry& entry)
  {
    if (_interactionState != InteractionState::Editing || !entry.get_editable())
    {
      return;
    }

    auto const text = entry.get_text().raw();
    auto inputRes = uimodel::parseTrackPropertiesFieldInput(editor.kind, text);
    editor.invalid = !inputRes;

    if (inputRes)
    {
      entry.remove_css_class("error");
      entry.set_tooltip_text("");
      _formModel.setFieldInput(editor.field, std::move(*inputRes));
    }
    else
    {
      entry.add_css_class("error");
      entry.set_tooltip_text(std::string{uimodel::trackPropertiesInputHint(_textCatalog, editor.kind)});
      _formModel.restoreFieldEdit(editor.field);
    }

    updateSaveEnabled();
  }

  void TrackPropertiesDialog::toggleFieldClear(FieldEditor& editor, Gtk::Entry& entry)
  {
    if (_interactionState != InteractionState::Editing || !editor.clearButton->get_sensitive())
    {
      return;
    }

    if (editor.clearButton->get_active())
    {
      updateEditorSensitivity();
      editor.invalid = false;
      entry.remove_css_class("error");
      entry.set_tooltip_text("");
      entry.set_placeholder_text("");
      setWidgetValue(&entry, "");
      _formModel.clearFieldEdit(editor.field);
    }
    else
    {
      _formModel.restoreFieldEdit(editor.field);
      applyRowView(&entry, _formModel.rowView(editor.field));
      updateEditorSensitivity();
    }

    updateSaveEnabled();
  }

  void TrackPropertiesDialog::applyRowView(Gtk::Widget* widget, uimodel::TrackPropertiesFormRowView const& view)
  {
    if (_formModel.targetCount() > 1)
    {
      if (auto* const entry = dynamic_cast<Gtk::Entry*>(widget); entry != nullptr)
      {
        // Empty ordinary input preserves the baseline; only a checked clear action removes it.
        entry->set_placeholder_text(view.text);
        setWidgetValue(entry, view.mixed ? std::string_view{} : std::string_view{view.text});
        return;
      }
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
} // namespace ao::gtk
