function(gamehq_add_id_translations target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "TS_FILES;LUPDATE_OPTIONS")

    if(Qt6_VERSION VERSION_LESS "6.10")
        qt_add_translations(${target}
            TS_FILES ${arg_TS_FILES}
            RESOURCE_PREFIX "/i18n"
            LUPDATE_OPTIONS ${arg_LUPDATE_OPTIONS}
            LRELEASE_OPTIONS -idbased)
        return()
    endif()

    # Qt 6.10+ recognizes a message ID as the lookup key only when the
    # context is empty. Keep the tracked catalogs grouped for translators.
    set(runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/id-catalogs/${target}")
    file(MAKE_DIRECTORY "${runtime_dir}")
    foreach(source IN LISTS arg_TS_FILES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
        get_filename_component(filename "${source}" NAME)
        set(runtime_ts "${runtime_dir}/${filename}")
        file(READ "${source}" contents)
        string(REGEX REPLACE "(<context>[ \t\r\n]*<name>)[^<]*(</name>)" "\\1\\2"
            contents "${contents}")
        unset(previous)
        if(EXISTS "${runtime_ts}")
            file(READ "${runtime_ts}" previous)
        endif()
        if(NOT contents STREQUAL previous)
            file(WRITE "${runtime_ts}" "${contents}")
        endif()
        list(APPEND runtime_catalogs "${runtime_ts}")
    endforeach()

    qt_add_lupdate(SOURCE_TARGETS ${target}
        LUPDATE_TARGET ${target}_lupdate
        TS_FILES ${arg_TS_FILES}
        OPTIONS ${arg_LUPDATE_OPTIONS})
    qt_add_lrelease(LRELEASE_TARGET ${target}_lrelease
        TS_FILES ${runtime_catalogs}
        QM_OUTPUT_DIRECTORY "${runtime_dir}"
        QM_FILES_OUTPUT_VARIABLE qm_files)
    qt_add_resources(${target} "${target}_translations"
        PREFIX "/i18n" BASE "${runtime_dir}" FILES ${qm_files})
endfunction()
