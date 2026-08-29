include(cmake/ArchitectureAudit.cmake)

# Shared application targets own their source lists beside their implementations.
# Keep this order: UIModel consumes runtime and i18n, while WinUI consumes the
# resource paths exported by i18n.
add_subdirectory(i18n)
add_subdirectory(desktop)
add_subdirectory(runtime)

if(WIN32)
  add_library(ao_windows_platform STATIC
    windows/MemoryRandomAccessStream.cpp
    windows/include/ao/winui/MemoryRandomAccessStream.h
  )
  target_include_directories(ao_windows_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/windows/include")
  target_link_libraries(ao_windows_platform PUBLIC ole32 runtimeobject shcore)
  if(AOBUS_BUILD_WINUI)
    aobus_enable_cppwinrt(ao_windows_platform)
  endif()
endif()

add_subdirectory(uimodel)

if(AOBUS_BUILD_GTK)
  add_subdirectory(linux-gtk)
endif()

if(AOBUS_BUILD_CLI)
  add_subdirectory(cli)

  # The CLI is intentionally outside interactive localization. Walk its full
  # link closure because a private transitive edge is just as real as a direct
  # one and cannot be expressed through header visibility.
  function(aobus_assert_cli_link_closure root_target)
    set(_queue "${root_target}")
    set(_visited)
    while(_queue)
      list(POP_FRONT _queue _target)
      if(_target IN_LIST _visited OR NOT TARGET "${_target}")
        continue()
      endif()
      list(APPEND _visited "${_target}")

      foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_links "${_target}" "${_property}")
        if(NOT _links OR _links STREQUAL "_links-NOTFOUND")
          continue()
        endif()
        foreach(_link IN LISTS _links)
          if(_link MATCHES "ao_app_i18n(_ordering|_completion)?|ao_catalog_pattern|ICU::i18n")
            message(FATAL_ERROR
              "CLI target '${root_target}' reaches forbidden localization dependency "
              "'${_link}' through '${_target}'.")
          endif()
          string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.:+-]*" _link_tokens "${_link}")
          foreach(_token IN LISTS _link_tokens)
            if(TARGET "${_token}" AND NOT _token IN_LIST _visited)
              list(APPEND _queue "${_token}")
            endif()
          endforeach()
        endforeach()
      endforeach()
    endwhile()
  endfunction()

  aobus_assert_cli_link_closure(aobus)
endif()

if(AOBUS_BUILD_TUI)
  add_subdirectory(tui)
endif()

if(AOBUS_BUILD_WINUI)
  add_subdirectory(windows-winui)
endif()

# The audit scans its declared source roots directly; it is intentionally not
# coupled to whichever platform targets this configure happens to create.
aobus_add_architecture_audit()
