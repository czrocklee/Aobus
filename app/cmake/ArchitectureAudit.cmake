# Application architecture rules are source policy, not target implementation.
# Keep them in one table and scan each governed source once so a completion gate
# reports the complete set of violations instead of stopping at the first rule.

function(aobus_add_architecture_audit)
  add_custom_target(ao_application_architecture_audit
    COMMAND ${CMAKE_COMMAND}
            "-DAOBUS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    COMMAND ${CMAKE_COMMAND}
            "-DPUBLIC_ROOT=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/app/uimodel"
            "-DTEST_ROOT=${CMAKE_SOURCE_DIR}/test/unit/uimodel"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertUimodelOrganization.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DPUBLIC_ROOT=${CMAKE_SOURCE_DIR}/app/include/ao/uimodel"
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/app/uimodel"
            "-DTEST_ROOT=${CMAKE_SOURCE_DIR}/test/unit/uimodel"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertUimodelFrontendNeutrality.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertWinUiEventRevokers.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertWinUiStateSubscriptions.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DROOT=${CMAKE_SOURCE_DIR}/app/windows-winui"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertWinUiLeafCapabilities.cmake"
    COMMAND ${CMAKE_COMMAND}
            "-DROOT=${CMAKE_SOURCE_DIR}/app/linux-gtk"
            -P "${CMAKE_SOURCE_DIR}/cmake/AssertGtkLeafCapabilities.cmake"
    COMMENT "Auditing application architecture boundaries"
    VERBATIM
  )
endfunction()

function(_aobus_register_architecture_rule name)
  cmake_parse_arguments(PARSE_ARGV 1 RULE "" "FORBIDDEN;ALLOWED;EXCLUDE" "ROOTS")
  if(NOT RULE_ROOTS OR NOT RULE_FORBIDDEN)
    message(FATAL_ERROR "Architecture rule '${name}' requires ROOTS and FORBIDDEN")
  endif()

  list(APPEND _ao_rule_names "${name}")
  set(_ao_rule_names "${_ao_rule_names}" PARENT_SCOPE)
  set("_ao_rule_${name}_roots" "${RULE_ROOTS}" PARENT_SCOPE)
  set("_ao_rule_${name}_forbidden" "${RULE_FORBIDDEN}" PARENT_SCOPE)
  set("_ao_rule_${name}_allowed" "${RULE_ALLOWED}" PARENT_SCOPE)
  set("_ao_rule_${name}_exclude" "${RULE_EXCLUDE}" PARENT_SCOPE)
endfunction()

function(_aobus_filter_architecture_sample rule sample output)
  set(_allowed_var "_ao_rule_${rule}_allowed")
  set(_filtered "${sample}")
  if(NOT "${${_allowed_var}}" STREQUAL "")
    string(REGEX REPLACE "${${_allowed_var}}" "" _filtered "${_filtered}")
  endif()
  set(${output} "${_filtered}" PARENT_SCOPE)
endfunction()

function(_aobus_assert_architecture_rejects rule sample)
  set(_forbidden_var "_ao_rule_${rule}_forbidden")
  _aobus_filter_architecture_sample("${rule}" "${sample}" _filtered)
  if(NOT _filtered MATCHES "${${_forbidden_var}}")
    message(FATAL_ERROR "Architecture rule '${rule}' does not reject '${sample}'")
  endif()
endfunction()

function(_aobus_assert_architecture_allows rule sample)
  set(_forbidden_var "_ao_rule_${rule}_forbidden")
  _aobus_filter_architecture_sample("${rule}" "${sample}" _filtered)
  if(_filtered MATCHES "${${_forbidden_var}}")
    message(FATAL_ERROR "Architecture rule '${rule}' rejects '${sample}'")
  endif()
endfunction()

function(_aobus_adjudicate_architecture_rule rule rejected_sample allowed_sample)
  _aobus_assert_architecture_rejects("${rule}" "${rejected_sample}")
  _aobus_assert_architecture_allows("${rule}" "${allowed_sample}")
  set(_adjudicated_rules "${_aobus_adjudicated_rules}")
  list(APPEND _adjudicated_rules "${rule}")
  set(_aobus_adjudicated_rules "${_adjudicated_rules}" PARENT_SCOPE)
endfunction()

function(_aobus_architecture_audit_self_test)
  set(_aobus_adjudicated_rules)
  _aobus_adjudicate_architecture_rule(application_signal "rt::Signal" "rt::SignalRouter")
  foreach(_sample IN ITEMS "rt::Signal" "ao::rt::Subscription" "::ao::rt::Signal")
    _aobus_assert_architecture_rejects(application_signal "${_sample}")
  endforeach()
  foreach(_sample IN ITEMS
      "rt::SignalRouter"
      "rt::SubscriptionScope"
      "Chart::Signal"
      "other::rt::Signal")
    _aobus_assert_architecture_allows(application_signal "${_sample}")
  endforeach()

  _aobus_adjudicate_architecture_rule(
    runtime_write_transaction "library.writeTransaction()" "library.readTransaction()")

  _aobus_adjudicate_architecture_rule(
    playback_internal "PlaybackTransport" "PlaybackTransportSnapshot")
  foreach(_sample IN ITEMS
      "PlaybackTransport"
      "ao::rt::PlaybackSuccession"
      "runtime/playback/PlaybackTransport.h")
    _aobus_assert_architecture_rejects(playback_internal "${_sample}")
  endforeach()
  foreach(_sample IN ITEMS "PlaybackTransportSnapshot" "PlaybackSuccessionSnapshot")
    _aobus_assert_architecture_allows(playback_internal "${_sample}")
  endforeach()

  _aobus_adjudicate_architecture_rule(
    lower_presentation_vocabulary "\"folder-symbolic\"" "\"folder\"")
  _aobus_adjudicate_architecture_rule(
    track_field_copy "std::string_view label;" "MessageId label;")
  _aobus_adjudicate_architecture_rule(
    track_preset_copy "std::string_view description{};" "MessageId description{};")
  _aobus_adjudicate_architecture_rule(
    backend_descriptor_copy "std::string iconName;" "std::string id;")
  _aobus_adjudicate_architecture_rule(
    completion_detail_copy "std::string detail;" "std::string display;")
  _aobus_adjudicate_architecture_rule(
    managed_state_mechanism "#include <ao/yaml/Reflect.h>" "#include <ao/yaml/RymlAdapter.h>")
  _aobus_adjudicate_architecture_rule(
    core_yaml_domain "#include <ao/CoreIds.h>" "#include <ao/utility/Path.h>")
  _aobus_adjudicate_architecture_rule(
    runtime_public "#include <ao/library/MusicLibrary.h>" "#include <ao/rt/LibrarySnapshot.h>")
  _aobus_adjudicate_architecture_rule(
    runtime_mechanism_surface "class SmartListEvaluator;" "class TrackSourceCache;")
  _aobus_adjudicate_architecture_rule(
    uimodel_platform "#include <gtkmm/widget.h>" "#include <ao/uimodel/layout/LayoutSchema.h>")
  _aobus_adjudicate_architecture_rule(
    uimodel_core "#include <ao/library/MusicLibrary.h>" "#include <ao/rt/LibrarySnapshot.h>")

  _aobus_adjudicate_architecture_rule(
    frontend_core "runtime.library().commands()" "runtime.playback().commands()")
  foreach(_sample IN ITEMS
      "runtime.library().commands()"
      "_library.commands()"
      "libraryPtr->commands()"
      "lib.commands()"
      "playbackLibrary.commands()")
    _aobus_assert_architecture_rejects(frontend_core "${_sample}")
  endforeach()
  foreach(_sample IN ITEMS
      "runtime.playback().commands()"
      "_playback.commands()"
      "playbackPtr->commands()")
    _aobus_assert_architecture_allows(frontend_core "${_sample}")
  endforeach()

  _aobus_assert_architecture_rejects(frontend_core "#include <ao/rt/CoreRuntime.h>")
  _aobus_assert_architecture_rejects(frontend_core "#include \"ao/rt/CoreRuntime.h\"")
  _aobus_assert_architecture_allows(frontend_core "#include <ao/rt/AppRuntime.h>")

  _aobus_adjudicate_architecture_rule(frontend_library_path "\"data.mdb\"" "\"library.db\"")
  _aobus_adjudicate_architecture_rule(
    cli_localization "#include <ao/i18n/MessageCatalog.h>" "#include <ao/rt/CoreRuntime.h>")
  _aobus_adjudicate_architecture_rule(
    tui_keymap_load_only "saveKeymap(store, keymap)" "loadKeymap(store, defaults)")

  set(_expected_rules "${_ao_rule_names}")
  list(REMOVE_DUPLICATES _aobus_adjudicated_rules)
  list(SORT _aobus_adjudicated_rules)
  list(SORT _expected_rules)
  if(NOT "${_aobus_adjudicated_rules}" STREQUAL "${_expected_rules}")
    message(FATAL_ERROR
      "Architecture rule self-test coverage differs from registered rules:\n"
      "  registered: ${_expected_rules}\n"
      "  adjudicated: ${_aobus_adjudicated_rules}")
  endif()
  message(STATUS "Application architecture rule self-test passed")
endfunction()

function(_aobus_run_architecture_audit)
  if(NOT AOBUS_SOURCE_DIR OR NOT IS_DIRECTORY "${AOBUS_SOURCE_DIR}")
    message(FATAL_ERROR "AOBUS_SOURCE_DIR must name the repository root")
  endif()

  set(_forbidden_audio_control
    "ao/audio/(Player|Backend[.]h|BackendProvider[.]h|Engine|NullBackend|backend/|detail/)")
  set(_forbidden_write_authority
    "(^|[^A-Za-z0-9_])(WriteTransaction|WritableMusicLibrary)([^A-Za-z0-9_]|$)")
  set(_forbidden_frontend_commands "(\\.|->)[ \t\r\n]*commands[ \t\r\n]*\\(")
  set(_allowed_playback_commands
    "((^|[^A-Za-z0-9_])(_?[Pp]layback|[Pp]laybackPtr)|[.>][ \t\r\n]*[Pp]layback[ \t\r\n]*\\([ \t\r\n]*\\))[ \t\r\n]*(\\.|->)[ \t\r\n]*commands[ \t\r\n]*\\(")

  _aobus_register_architecture_rule(application_signal
    ROOTS app test
    FORBIDDEN
      "#[ \t]*include[ \t]*[<\\\"]ao/rt/(Signal|Subscription)[.]h[>\\\"]|(^|[^A-Za-z0-9_:])((::)?ao::rt::|rt::)(Signal|Subscription)([^A-Za-z0-9_]|$)")
  _aobus_register_architecture_rule(runtime_write_transaction
    ROOTS app/runtime
    FORBIDDEN "(^|[^A-Za-z0-9_])writeTransaction[ \t\r\n]*\\("
    EXCLUDE "/app/runtime/library/(LibraryWriteLane|LibraryYamlImporter|ScanApplyOperation)[.]cpp$")
  _aobus_register_architecture_rule(playback_internal
    ROOTS
      app/include/ao/rt
      app/include/ao/uimodel
      app/include/ao/desktop
      app/uimodel
      app/desktop
      app/linux-gtk
      app/tui
      app/windows-winui
      app/cli
    FORBIDDEN "(^|[^A-Za-z0-9_])(PlaybackTransport|PlaybackSuccession)([^A-Za-z0-9_]|$)")
  _aobus_register_architecture_rule(lower_presentation_vocabulary
    ROOTS include lib app/include/ao/rt app/runtime
    FORBIDDEN
      "\"(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic|Unknown (Artist|Album|Year|Genre|Composer|Conductor|Ensemble|Work)|Modern Linux audio server with low latency|Advanced Linux Sound Architecture [(]Direct Hardware Access[)]|Windows Audio Session API|WASAPI render endpoint|Shared Mode|Exclusive Mode|logical operator)\"")
  _aobus_register_architecture_rule(track_field_copy
    ROOTS app/include/ao/rt/TrackField.h
    FORBIDDEN "std::string_view[ \t]+label[ \t]*[;{]")
  _aobus_register_architecture_rule(track_preset_copy
    ROOTS app/include/ao/rt/TrackPresentation.h
    FORBIDDEN "std::string_view[ \t]+(label|description)[ \t]*[;{]")
  _aobus_register_architecture_rule(backend_descriptor_copy
    ROOTS include/ao/audio/BackendProvider.h
    FORBIDDEN "std::string[ \t]+(name|description|iconName)[ \t]*[;{]")
  _aobus_register_architecture_rule(completion_detail_copy
    ROOTS app/include/ao/rt/completion/CompletionItem.h
    FORBIDDEN "std::string[ \t]+detail[ \t]*[;{]")
  _aobus_register_architecture_rule(managed_state_mechanism
    ROOTS
      app/include
      app/runtime
      app/uimodel
      app/desktop
      app/linux-gtk
      app/tui
      app/windows-winui
    FORBIDDEN "ao/yaml/Reflect[.]h|namespace[ \t\r\n]+ao::yaml")
  _aobus_register_architecture_rule(core_yaml_domain
    ROOTS include/ao/yaml
    FORBIDDEN
      "#[ \t]*include[ \t]*[<\\\"]ao/(CoreIds[.]h|rt/|uimodel/)|APP_LOG_|#[ \t]*include[ \t]*[<\\\"]spdlog/")
  _aobus_register_architecture_rule(runtime_public
    ROOTS app/include/ao/rt
    FORBIDDEN
      "(#[ \t]*include[ \t]*[<\\\"](runtime/|ao/(lmdb/|library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|audio/(Player|Backend[.]h|BackendProvider[.]h|Engine|NullBackend|backend/|detail/))))|${_forbidden_write_authority}")
  _aobus_register_architecture_rule(runtime_mechanism_surface
    ROOTS app/include/ao/rt
    FORBIDDEN
      "(^|[^A-Za-z0-9_])(AllTracksSource|IndexedTrackSequence|ListOrderSource|SmartListEvaluator|SmartListSource|LibraryYamlExporter|LibraryYamlImporter)([^A-Za-z0-9_]|$)")
  _aobus_register_architecture_rule(uimodel_platform
    ROOTS app/include/ao/uimodel app/uimodel
    FORBIDDEN
      "(#[ \t]*include[ \t]*[<\\\"](gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib)/)|(\"(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic)\")")
  _aobus_register_architecture_rule(uimodel_core
    ROOTS app/include/ao/uimodel app/uimodel
    FORBIDDEN
      "(#[ \t]*include[ \t]*[<\\\"](ao/(lmdb/|library/)|${_forbidden_audio_control}))|${_forbidden_write_authority}")
  _aobus_register_architecture_rule(frontend_core
    ROOTS app/include/ao/desktop app/desktop app/linux-gtk app/windows-winui app/tui
    FORBIDDEN
      "(#[ \t]*include[ \t]*[<\\\"](ao/rt/CoreRuntime[.]h|ao/lmdb/|ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)))|${_forbidden_write_authority}|(^|[^A-Za-z0-9_])LibraryCommands([^A-Za-z0-9_]|$)|${_forbidden_frontend_commands}"
    ALLOWED "${_allowed_playback_commands}")
  _aobus_register_architecture_rule(frontend_library_path
    ROOTS app/include/ao/desktop app/desktop app/linux-gtk app/windows-winui app/tui app/cli
    FORBIDDEN "\"([.]aobus|data[.]mdb)")
  _aobus_register_architecture_rule(cli_localization
    ROOTS app/cli
    FORBIDDEN "#[ \t]*include[ \t]*[<\"]ao/i18n/")
  _aobus_register_architecture_rule(tui_keymap_load_only
    ROOTS app/tui
    FORBIDDEN "(^|[^A-Za-z0-9_])saveKeymap[ \t\r\n]*\\(")

  if(AOBUS_ARCHITECTURE_AUDIT_SELF_TEST)
    _aobus_architecture_audit_self_test()
    return()
  endif()

  set(_sources)
  foreach(_rule IN LISTS _ao_rule_names)
    set(_roots_var "_ao_rule_${_rule}_roots")
    set(_absolute_roots_var "_ao_rule_${_rule}_absolute_roots")
    foreach(_relative_root IN LISTS ${_roots_var})
      cmake_path(ABSOLUTE_PATH _relative_root
        BASE_DIRECTORY "${AOBUS_SOURCE_DIR}"
        NORMALIZE
        OUTPUT_VARIABLE _root)
      if(IS_DIRECTORY "${_root}")
        file(GLOB_RECURSE _root_sources LIST_DIRECTORIES false
          "${_root}/*.h"
          "${_root}/*.hh"
          "${_root}/*.hpp"
          "${_root}/*.hxx"
          "${_root}/*.c"
          "${_root}/*.cc"
          "${_root}/*.cpp"
          "${_root}/*.cxx"
          "${_root}/*.inl"
          "${_root}/*.ipp"
          "${_root}/*.def")
        list(APPEND _sources ${_root_sources})
      elseif(EXISTS "${_root}")
        list(APPEND _sources "${_root}")
      else()
        message(FATAL_ERROR "Architecture rule '${_rule}' root does not exist: ${_root}")
      endif()
      list(APPEND ${_absolute_roots_var} "${_root}")
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _sources)
  list(SORT _sources)

  set(_findings)
  foreach(_source IN LISTS _sources)
    file(READ "${_source}" _content)
    foreach(_rule IN LISTS _ao_rule_names)
      set(_absolute_roots_var "_ao_rule_${_rule}_absolute_roots")
      set(_applies false)
      foreach(_root IN LISTS ${_absolute_roots_var})
        if(IS_DIRECTORY "${_root}")
          cmake_path(IS_PREFIX _root "${_source}" NORMALIZE _under_root)
          if(_under_root)
            set(_applies true)
            break()
          endif()
        elseif(_source STREQUAL _root)
          set(_applies true)
          break()
        endif()
      endforeach()
      if(NOT _applies)
        continue()
      endif()

      set(_exclude_var "_ao_rule_${_rule}_exclude")
      if(NOT "${${_exclude_var}}" STREQUAL ""
          AND _source MATCHES "${${_exclude_var}}")
        continue()
      endif()

      _aobus_filter_architecture_sample("${_rule}" "${_content}" _scanned_content)
      set(_forbidden_var "_ao_rule_${_rule}_forbidden")
      if(_scanned_content MATCHES "${${_forbidden_var}}")
        string(REGEX MATCH "${${_forbidden_var}}" _match "${_scanned_content}")
        cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${AOBUS_SOURCE_DIR}" OUTPUT_VARIABLE _relative_source)
        list(APPEND _findings "${_rule}: ${_relative_source}: '${_match}'")
      endif()
    endforeach()
  endforeach()

  foreach(_legacy_path IN ITEMS
      app/include/ao/yaml/ConfigTraits.h
      app/include/ao/rt/Signal.h
      app/include/ao/rt/Subscription.h
      app/include/ao/rt/PlaybackService.h
      app/include/ao/rt/PlaybackSequenceService.h)
    if(EXISTS "${AOBUS_SOURCE_DIR}/${_legacy_path}")
      list(APPEND _findings "removed_surface: ${_legacy_path}: file must remain absent")
    endif()
  endforeach()

  # Git discovery, format, naming, and changed-file hygiene share one governed
  # first-party C++ suffix set: .cpp, .h, .hpp, plus .def include fragments.
  # Reject alternate spellings before they can evade part of that toolchain.
  foreach(_source_root IN ITEMS app include lib test tool)
    if(IS_DIRECTORY "${AOBUS_SOURCE_DIR}/${_source_root}")
      file(GLOB_RECURSE _unsupported_cpp_sources LIST_DIRECTORIES false
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.c"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.cc"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.cxx"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.c++"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.hh"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.hxx"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.inl"
        "${AOBUS_SOURCE_DIR}/${_source_root}/*.ipp")
      foreach(_unsupported_source IN LISTS _unsupported_cpp_sources)
        cmake_path(RELATIVE_PATH _unsupported_source
          BASE_DIRECTORY "${AOBUS_SOURCE_DIR}"
          OUTPUT_VARIABLE _unsupported_relative)
        list(APPEND _findings
          "unsupported_cpp_suffix: ${_unsupported_relative}: use .cpp, .h, .hpp, or .def")
      endforeach()
    endif()
  endforeach()

  list(LENGTH _sources _source_count)
  list(LENGTH _ao_rule_names _rule_count)
  list(LENGTH _findings _finding_count)
  if(_findings)
    list(JOIN _findings "\n  " _finding_text)
    message(FATAL_ERROR
      "Application architecture audit found ${_finding_count} violation(s) across "
      "${_source_count} source files and ${_rule_count} rules:\n  ${_finding_text}")
  endif()
  message(STATUS
    "Application architecture audit passed: ${_source_count} source files, ${_rule_count} rules")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  _aobus_run_architecture_audit()
endif()
