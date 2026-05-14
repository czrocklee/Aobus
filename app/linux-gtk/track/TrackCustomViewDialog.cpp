// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include <format>
#include <random>

namespace ao::gtk
{
  namespace
  {
    std::string generateId()
    {
      static std::random_device rd;
      static std::mt19937_64 gen(rd());
      static std::uniform_int_distribution<std::uint64_t> dis;
      return std::format("{:016x}", dis(gen));
    }

    std::string_view groupKeyName(rt::TrackGroupKey key)
    {
      switch (key)
      {
        case rt::TrackGroupKey::None: return "None";
        case rt::TrackGroupKey::Artist: return "Artist";
        case rt::TrackGroupKey::Album: return "Album";
        case rt::TrackGroupKey::AlbumArtist: return "Album Artist";
        case rt::TrackGroupKey::Genre: return "Genre";
        case rt::TrackGroupKey::Composer: return "Composer";
        case rt::TrackGroupKey::Work: return "Work";
        case rt::TrackGroupKey::Year: return "Year";
      }
      return "None";
    }

    Glib::RefPtr<Gtk::StringList> createSortFieldsModel()
    {
      auto model = Gtk::StringList::create({});
      model->append("Artist");
      model->append("Album");
      model->append("Album Artist");
      model->append("Genre");
      model->append("Composer");
      model->append("Work");
      model->append("Year");
      model->append("Disc Number");
      model->append("Track Number");
      model->append("Title");
      model->append("Duration");
      return model;
    }

    Glib::RefPtr<Gtk::StringList> createVisibleFieldsModel()
    {
      auto model = Gtk::StringList::create({});
      model->append("Title");
      model->append("Artist");
      model->append("Album");
      model->append("Album Artist");
      model->append("Genre");
      model->append("Composer");
      model->append("Work");
      model->append("Year");
      model->append("Disc Number");
      model->append("Track Number");
      model->append("Duration");
      model->append("Tags");
      return model;
    }
  }

  TrackCustomViewDialog::TrackCustomViewDialog(Gtk::Window& parent,
                                               rt::TrackPresentationSpec const& initialSpec,
                                               std::string_view initialLabel)
  {
    set_title("Edit Custom View");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(500, 600);

    setupUi();
    populateFromSpec(initialSpec, initialLabel);
  }

  void TrackCustomViewDialog::setupUi()
  {
    auto* const contentArea = get_content_area();
    contentArea->add_css_class("custom-view-editor");

    auto mainBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    mainBox->add_css_class("custom-view-main-box");

    // Name
    auto nameBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    nameBox->add_css_class("custom-view-row");
    auto nameLabel = Gtk::make_managed<Gtk::Label>("Name");
    nameLabel->set_halign(Gtk::Align::START);
    nameBox->append(*nameLabel);
    _nameEntry.set_hexpand(true);
    nameBox->append(_nameEntry);
    mainBox->append(*nameBox);

    // Group By
    auto groupBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    groupBox->add_css_class("custom-view-row");
    auto groupLabel = Gtk::make_managed<Gtk::Label>("Group By");
    groupLabel->set_halign(Gtk::Align::START);
    groupBox->append(*groupLabel);

    auto groupModel = Gtk::StringList::create({});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::None)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Artist)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Album)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::AlbumArtist)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Genre)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Composer)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Work)});
    groupModel->append(std::string{groupKeyName(rt::TrackGroupKey::Year)});

    _groupDropdown.set_model(groupModel);
    groupBox->append(_groupDropdown);
    mainBox->append(*groupBox);

    // Sort Terms (using boxed list style)
    auto sortLabel = Gtk::make_managed<Gtk::Label>("Sort Order");
    sortLabel->set_halign(Gtk::Align::START);
    sortLabel->add_css_class("custom-view-section-title");
    mainBox->append(*sortLabel);

    _sortTermsList.add_css_class("boxed-list");
    _sortTermsList.add_css_class("custom-view-list");
    _sortTermsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_sortTermsList);

    auto addSortBtn = Gtk::make_managed<Gtk::Button>("Add Sort Field");
    addSortBtn->set_halign(Gtk::Align::START);
    addSortBtn->signal_clicked().connect(
      [this]
      {
        _sortState.push_back({static_cast<std::uint8_t>(rt::TrackSortField::Title), true});
        rebuildSortList();
      });
    mainBox->append(*addSortBtn);

    // Visible Fields
    auto fieldsLabel = Gtk::make_managed<Gtk::Label>("Visible Columns");
    fieldsLabel->set_halign(Gtk::Align::START);
    fieldsLabel->add_css_class("custom-view-section-title");
    mainBox->append(*fieldsLabel);

    _visibleFieldsList.add_css_class("boxed-list");
    _visibleFieldsList.add_css_class("custom-view-list");
    _visibleFieldsList.set_selection_mode(Gtk::SelectionMode::NONE);
    mainBox->append(_visibleFieldsList);

    auto addVisibleBtn = Gtk::make_managed<Gtk::Button>("Add Column");
    addVisibleBtn->set_halign(Gtk::Align::START);
    addVisibleBtn->signal_clicked().connect(
      [this]
      {
        _visibleFieldsState.push_back(static_cast<std::uint8_t>(rt::TrackPresentationField::Title));
        rebuildVisibleFieldsList();
      });
    mainBox->append(*addVisibleBtn);

    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    scroll->set_child(*mainBox);
    contentArea->append(*scroll);

    // Buttons
    _saveButton.add_css_class("suggested-action");
    add_action_widget(_saveButton, Gtk::ResponseType::OK);

    auto cancelButton = Gtk::make_managed<Gtk::Button>("Cancel");
    add_action_widget(*cancelButton, Gtk::ResponseType::CANCEL);
  }

  void TrackCustomViewDialog::rebuildSortList()
  {
    while (auto* child = _sortTermsList.get_first_child())
    {
      _sortTermsList.remove(*child);
    }

    for (std::size_t i = 0; i < _sortState.size(); ++i)
    {
      auto const& term = _sortState[i];
      auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

      auto* const dropdown = Gtk::make_managed<Gtk::DropDown>(createSortFieldsModel());

      dropdown->set_selected(term.field);
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown] { _sortState[i].field = static_cast<std::uint8_t>(dropdown->get_selected()); });
      box->append(*dropdown);

      auto ascBtn = Gtk::make_managed<Gtk::ToggleButton>("Ascending");
      ascBtn->set_active(term.ascending);

      ascBtn->signal_toggled().connect([this, i, ascBtn] { _sortState[i].ascending = ascBtn->get_active(); });
      box->append(*ascBtn);

      auto spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto upBtn = Gtk::make_managed<Gtk::Button>("Up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_sortState[i], _sortState[i - 1]);
          rebuildSortList();
        });
      box->append(*upBtn);

      auto downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i < _sortState.size() - 1);

      downBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_sortState[i], _sortState[i + 1]);
          rebuildSortList();
        });
      box->append(*downBtn);

      auto removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _sortState.erase(_sortState.begin() + i);
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

    for (std::size_t i = 0; i < _visibleFieldsState.size(); ++i)
    {
      auto const field = _visibleFieldsState[i];
      auto row = Gtk::make_managed<Gtk::ListBoxRow>();
      auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

      auto dropdown = Gtk::make_managed<Gtk::DropDown>(createVisibleFieldsModel());

      dropdown->set_selected(field);
      dropdown->property_selected().signal_changed().connect(
        [this, i, dropdown] { _visibleFieldsState[i] = static_cast<std::uint8_t>(dropdown->get_selected()); });
      box->append(*dropdown);

      auto spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      box->append(*spacer);

      auto upBtn = Gtk::make_managed<Gtk::Button>("Up");
      upBtn->set_sensitive(i > 0);

      upBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_visibleFieldsState[i], _visibleFieldsState[i - 1]);
          rebuildVisibleFieldsList();
        });
      box->append(*upBtn);

      auto downBtn = Gtk::make_managed<Gtk::Button>("Down");
      downBtn->set_sensitive(i < _visibleFieldsState.size() - 1);

      downBtn->signal_clicked().connect(
        [this, i]
        {
          std::swap(_visibleFieldsState[i], _visibleFieldsState[i + 1]);
          rebuildVisibleFieldsList();
        });
      box->append(*downBtn);

      auto removeBtn = Gtk::make_managed<Gtk::Button>("Remove");
      removeBtn->signal_clicked().connect(
        [this, i]
        {
          _visibleFieldsState.erase(_visibleFieldsState.begin() + i);
          rebuildVisibleFieldsList();
        });
      box->append(*removeBtn);

      row->set_child(*box);
      _visibleFieldsList.append(*row);
    }
  }

  void TrackCustomViewDialog::populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label)
  {
    _nameEntry.set_text(std::string{label});

    int groupIndex = 0;

    switch (spec.groupBy)
    {
      case rt::TrackGroupKey::None: groupIndex = 0; break;
      case rt::TrackGroupKey::Artist: groupIndex = 1; break;
      case rt::TrackGroupKey::Album: groupIndex = 2; break;
      case rt::TrackGroupKey::AlbumArtist: groupIndex = 3; break;
      case rt::TrackGroupKey::Genre: groupIndex = 4; break;
      case rt::TrackGroupKey::Composer: groupIndex = 5; break;
      case rt::TrackGroupKey::Work: groupIndex = 6; break;
      case rt::TrackGroupKey::Year: groupIndex = 7; break;
    }
    _groupDropdown.set_selected(groupIndex);

    _sortState.clear();
    for (auto const& term : spec.sortBy)
    {
      _sortState.push_back({static_cast<std::uint8_t>(term.field), term.ascending});
    }
    rebuildSortList();

    _visibleFieldsState.clear();
    for (auto const field : spec.visibleFields)
    {
      _visibleFieldsState.push_back(static_cast<std::uint8_t>(field));
    }
    rebuildVisibleFieldsList();
  }

  CustomTrackPresentationState TrackCustomViewDialog::collectState() const
  {
    auto state = CustomTrackPresentationState{};
    state.id = generateId();
    state.label = _nameEntry.get_text();

    auto const groupIndex = _groupDropdown.get_selected();
    switch (groupIndex)
    {
      case 0: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::None); break;
      case 1: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Artist); break;
      case 2: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Album); break;
      case 3: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::AlbumArtist); break;
      case 4: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Genre); break;
      case 5: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Composer); break;
      case 6: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Work); break;
      case 7: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::Year); break;
      default: state.groupBy = static_cast<std::uint8_t>(rt::TrackGroupKey::None); break;
    }

    state.sortBy = _sortState;
    state.visibleFields = _visibleFieldsState;

    return state;
  }

  std::optional<TrackCustomViewDialog::Result> TrackCustomViewDialog::runDialog()
  {
    show();

    auto loop = Glib::MainLoop::create(false);
    auto response = Gtk::ResponseType::CANCEL;

    signal_response().connect(
      [&loop, &response](int resp)
      {
        response = static_cast<Gtk::ResponseType>(resp);
        loop->quit();
      });

    loop->run();
    hide();

    if (response == Gtk::ResponseType::OK)
    {
      return Result{.state = collectState(), .deleted = false};
    }

    return std::nullopt;
  }
} // namespace ao::gtk
