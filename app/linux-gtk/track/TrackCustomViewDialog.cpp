// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace ao::gtk
{
  namespace
  {
    constexpr int kBoxSpacing = 6;
    constexpr int kRowIconButtonSize = 32;
    constexpr int kSectionActionButtonSize = 32;
    constexpr int kMinScrollContentWidth = 480;
    constexpr int kMaxScrollContentWidth = 700;
    constexpr std::int32_t kMaxScrollContentHeight = 520;

    std::string generateId()
    {
      static std::random_device rd;
      static std::mt19937_64 gen{rd()};
      static std::uniform_int_distribution<std::uint64_t> dis;
      return std::format("{:016x}", dis(gen));
    }

    Glib::RefPtr<Gtk::StringList> createGroupKeysModel(std::span<uimodel::TrackGroupKeyOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }

    Glib::RefPtr<Gtk::StringList> createSortFieldsModel(std::span<uimodel::TrackSortFieldOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }

    Glib::RefPtr<Gtk::StringList> createVisibleFieldsModel(std::span<uimodel::TrackVisibleFieldOption const> options)
    {
      auto modelPtr = Gtk::StringList::create({});

      for (auto const& option : options)
      {
        modelPtr->append(option.label);
      }

      return modelPtr;
    }

    void configureRowIconButton(Gtk::Button& button, std::string_view iconName, std::string_view tooltip)
    {
      button.set_icon_name(std::string{iconName});
      button.set_tooltip_text(std::string{tooltip});
      button.set_size_request(kRowIconButtonSize, kRowIconButtonSize);
      button.add_css_class("flat");
    }

    Gtk::Button* makeRowIconButton(std::string_view iconName, std::string_view tooltip)
    {
      auto* const button = Gtk::make_managed<Gtk::Button>();
      configureRowIconButton(*button, iconName, tooltip);
      return button;
    }

    void updateSortDirectionButton(Gtk::ToggleButton& button, bool ascending)
    {
      configureRowIconButton(button,
                             ascending ? "view-sort-ascending-symbolic" : "view-sort-descending-symbolic",
                             ascending ? "Sort ascending" : "Sort descending");
    }

    void configureSectionAddButton(Gtk::Button& button, std::string_view tooltip)
    {
      button.set_icon_name("list-add-symbolic");
      button.set_tooltip_text(std::string{tooltip});
      button.set_size_request(kSectionActionButtonSize, kSectionActionButtonSize);
      button.add_css_class("flat");
    }

    Gtk::Box* makeSectionHeader(std::string_view title, Gtk::Button& actionButton)
    {
      auto* const header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);
      auto* const label = Gtk::make_managed<Gtk::Label>(std::string{title});
      label->set_halign(Gtk::Align::START);
      label->set_hexpand(true);
      label->set_xalign(0.0F);
      label->add_css_class("ao-section-header");
      header->append(*label);
      header->append(actionButton);
      return header;
    }
  } // namespace

  TrackCustomViewDialog::TrackCustomViewDialog(Gtk::Window& parent,
                                               rt::TrackPresentationSpec const& initialSpec,
                                               std::string_view initialLabel)
    : AppDialog{}
  {
    set_title("Edit Custom View");
    configureForParent(parent);

    set_default_size(-1, -1);

    setupUi();
    populateFromSpec(initialSpec, initialLabel);
  }

  void TrackCustomViewDialog::setupUi()
  {
    addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    addPrimaryAction("Save", Gtk::ResponseType::OK);

    auto* mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, kBoxSpacing * 2);

    // Section 1: Metadata Card
    auto* metaList = Gtk::make_managed<FormBoxedList>();
    _nameEntry.set_placeholder_text("View label");
    metaList->addEntryRow("Name", _nameEntry);

    _groupDropdown.set_model(createGroupKeysModel(_model.groupOptions()));
    _groupDropdown.property_selected().signal_changed().connect(
      [this]
      {
        if (auto const selected = _groupDropdown.get_selected(); selected < _model.groupOptions().size())
        {
          _model.setGroupKeyByOptionIndex(selected);
        }
      });
    metaList->addRow("Group By", _groupDropdown);
    mainBox->append(*metaList);

    auto* addSortBtn = Gtk::make_managed<Gtk::Button>();
    configureSectionAddButton(*addSortBtn, "Add sort field");
    addSortBtn->signal_clicked().connect(
      [this]
      {
        _model.addSortTerm();
        rebuildSortList();
      });

    mainBox->append(*makeSectionHeader("Sort Order", *addSortBtn));

    _sortTermsList.add_css_class("ao-boxed-list");
    _sortTermsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_sortTermsList);

    auto* addVisibleBtn = Gtk::make_managed<Gtk::Button>();
    configureSectionAddButton(*addVisibleBtn, "Add column");
    addVisibleBtn->signal_clicked().connect(
      [this]
      {
        _model.addVisibleField();
        rebuildVisibleFieldsList();
      });

    mainBox->append(*makeSectionHeader("Visible Columns", *addVisibleBtn));

    _visibleFieldsList.add_css_class("ao-boxed-list");
    _visibleFieldsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_visibleFieldsList);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_propagate_natural_width(true);
    scroll->set_propagate_natural_height(true);
    scroll->set_min_content_width(kMinScrollContentWidth);
    scroll->set_max_content_width(kMaxScrollContentWidth);
    scroll->set_max_content_height(kMaxScrollContentHeight);
    scroll->set_child(*mainBox);
    scroll->set_vexpand(true);

    setContentWidget(*scroll);
  }

  void TrackCustomViewDialog::rebuildSortList()
  {
    while (auto* child = _sortTermsList.get_first_child())
    {
      _sortTermsList.remove(*child);
    }

    auto const sortTerms = _model.sortTerms();

    for (std::size_t i = 0; i < sortTerms.size(); ++i)
    {
      auto const& term = sortTerms[i];
      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* const dropdown = Gtk::make_managed<Gtk::DropDown>(createSortFieldsModel(_model.sortFieldOptions()));
      auto const index = _model.optionIndexForSortField(term.field).value_or(0);

      dropdown->set_selected(static_cast<::guint>(index));
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _model.sortFieldOptions().size())
          {
            _model.setSortFieldByOptionIndex(i, selected);
          }
        });
      box->append(*dropdown);

      auto* ascBtn = Gtk::make_managed<Gtk::ToggleButton>();
      ascBtn->set_active(term.ascending);
      updateSortDirectionButton(*ascBtn, term.ascending);

      ascBtn->signal_toggled().connect(
        [this, i, ascBtn]
        {
          auto const ascending = ascBtn->get_active();
          updateSortDirectionButton(*ascBtn, ascending);
          _model.setSortAscending(i, ascending);
        });
      box->append(*ascBtn);

      auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto* upBtn = makeRowIconButton("go-up-symbolic", "Move up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveSortTermUp(i);
          rebuildSortList();
        });
      box->append(*upBtn);

      auto* downBtn = makeRowIconButton("go-down-symbolic", "Move down");
      downBtn->set_sensitive(i + 1 < sortTerms.size());

      downBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveSortTermDown(i);
          rebuildSortList();
        });
      box->append(*downBtn);

      auto* removeBtn = makeRowIconButton("user-trash-symbolic", "Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _model.removeSortTerm(i);
          rebuildSortList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _sortTermsList.append(*row);
    }
  }

  void TrackCustomViewDialog::rebuildVisibleFieldsList()
  {
    while (auto* child = _visibleFieldsList.get_first_child())
    {
      _visibleFieldsList.remove(*child);
    }

    auto const visibleFields = _model.visibleFields();

    for (std::size_t i = 0; i < visibleFields.size(); ++i)
    {
      auto const field = visibleFields[i];
      auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kBoxSpacing);

      auto* dropdown = Gtk::make_managed<Gtk::DropDown>(createVisibleFieldsModel(_model.visibleFieldOptions()));
      auto const index = _model.optionIndexForVisibleField(field).value_or(0);

      dropdown->set_selected(static_cast<::guint>(index));
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown]
        {
          if (auto const selected = dropdown->get_selected(); selected < _model.visibleFieldOptions().size())
          {
            _model.setVisibleFieldByOptionIndex(i, selected);
          }
        });
      box->append(*dropdown);

      auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto* upBtn = makeRowIconButton("go-up-symbolic", "Move up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveVisibleFieldUp(i);
          rebuildVisibleFieldsList();
        });
      box->append(*upBtn);

      auto* downBtn = makeRowIconButton("go-down-symbolic", "Move down");
      downBtn->set_sensitive(i + 1 < visibleFields.size());

      downBtn->signal_clicked().connect(
        [this, i]
        {
          _model.moveVisibleFieldDown(i);
          rebuildVisibleFieldsList();
        });
      box->append(*downBtn);

      auto* removeBtn = makeRowIconButton("user-trash-symbolic", "Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _model.removeVisibleField(i);
          rebuildVisibleFieldsList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _visibleFieldsList.append(*row);
    }
  }

  void TrackCustomViewDialog::populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _model.populate(spec, label);
    _nameEntry.set_text(std::string{_model.label()});

    if (auto const optIndex = _model.groupKeyOptionIndex(); optIndex)
    {
      _groupDropdown.set_selected(static_cast<::guint>(*optIndex));
    }

    rebuildSortList();
    rebuildVisibleFieldsList();
  }

  rt::CustomTrackPresentationPreset TrackCustomViewDialog::collectState()
  {
    _model.setLabel(_nameEntry.get_text().raw());
    return _model.collectState(generateId());
  }

  std::optional<TrackCustomViewDialog::TrackCustomPresentationDialogResult> TrackCustomViewDialog::runDialog()
  {
    show();

    auto loopPtr = Glib::MainLoop::create(false);
    auto response = Gtk::ResponseType::CANCEL;

    signal_response().connect(
      [&loopPtr, &response](std::int32_t resp)
      {
        response = static_cast<Gtk::ResponseType>(resp);
        loopPtr->quit();
      });

    loopPtr->run();
    hide();

    if (response == Gtk::ResponseType::OK)
    {
      return TrackCustomPresentationDialogResult{.preset = collectState(), .deleted = false};
    }

    return std::nullopt;
  }
} // namespace ao::gtk
