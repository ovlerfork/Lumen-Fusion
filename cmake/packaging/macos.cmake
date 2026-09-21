# macOS-specific packaging

if(SUNSHINE_PACKAGE_MACOS)
    set(_app "Lumen Fusion.app")
    set(_resources "${_app}/Contents/Resources")
    install(TARGETS sunshine BUNDLE DESTINATION . COMPONENT Runtime)
    install(PROGRAMS "$<TARGET_FILE:vd_helper>"
            DESTINATION "${_app}/Contents/MacOS" COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/sunshine.icns"
            DESTINATION "${_resources}" COMPONENT Runtime)
    install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets/"
            DESTINATION "${_resources}" COMPONENT Runtime
            PATTERN "Info.plist" EXCLUDE)

    # Keep resources current even when only web assets or the helper change.
    add_custom_target(macos-bundle-resources
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/MacOS"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${CMAKE_BINARY_DIR}/assets" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/Resources"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/Resources"
            COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/Resources/Info.plist"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${PROJECT_SOURCE_DIR}/sunshine.icns" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/Resources/sunshine.icns"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:vd_helper>" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/MacOS/vd_helper"
            DEPENDS vd_helper web-ui
            VERBATIM)
    add_dependencies(sunshine macos-bundle-resources)

    set(LUMINA_QT_PLUGINS "")
    set(LUMINA_QT_RUNTIME_DIRS "${MACOS_LINK_DIRECTORIES}")
    if(SUNSHINE_ENABLE_TRAY)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qt.conf"
                CONTENT "[Paths]\nPlugins = PlugIns\n")
        get_filename_component(LUMINA_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
        get_filename_component(LUMINA_QTSVG_PREFIX "${Qt6Svg_DIR}/../../.." ABSOLUTE)
        list(APPEND LUMINA_QT_RUNTIME_DIRS "${LUMINA_QT_PREFIX}/lib" "${LUMINA_QTSVG_PREFIX}/lib")
        foreach(_plugin IN ITEMS platforms/libqcocoa.dylib imageformats/libqsvg.dylib iconengines/libqsvgicon.dylib)
            find_file(_plugin_file NAMES "${_plugin}"
                    PATHS "${LUMINA_QT_PREFIX}/share/qt/plugins" "${LUMINA_QT_PREFIX}/plugins"
                          "${LUMINA_QTSVG_PREFIX}/share/qt/plugins" "${LUMINA_QTSVG_PREFIX}/plugins"
                    NO_DEFAULT_PATH REQUIRED)
            get_filename_component(_plugin_dir "${_plugin}" DIRECTORY)
            install(FILES "${_plugin_file}" DESTINATION "${_app}/Contents/PlugIns/${_plugin_dir}" COMPONENT Runtime)
            add_custom_command(TARGET macos-bundle-resources POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/PlugIns/${_plugin_dir}"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "${_plugin_file}" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/PlugIns/${_plugin}"
                    VERBATIM)
            list(APPEND LUMINA_QT_PLUGINS "${_plugin}")
            unset(_plugin_file CACHE)
        endforeach()
        install(FILES "${CMAKE_BINARY_DIR}/qt.conf" DESTINATION "${_resources}" COMPONENT Runtime)
        add_custom_command(TARGET macos-bundle-resources POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${CMAKE_BINARY_DIR}/qt.conf" "$<TARGET_BUNDLE_CONTENT_DIR:sunshine>/Resources/qt.conf"
                VERBATIM)
    endif()

    install(CODE "set(LUMINA_QT_RUNTIME_DIRS \"${LUMINA_QT_RUNTIME_DIRS}\")\nset(LUMINA_QT_PLUGINS \"${LUMINA_QT_PLUGINS}\")"
            COMPONENT Runtime)
    install(CODE [[
        set(_app "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Lumen Fusion.app")
        set(_contents "${_app}/Contents")
        set(_extra_binaries "${_contents}/MacOS/vd_helper")
        foreach(_plugin IN LISTS LUMINA_QT_PLUGINS)
            list(APPEND _extra_binaries "${_contents}/PlugIns/${_plugin}")
        endforeach()

        include(BundleUtilities)
        set(BU_CHMOD_BUNDLE_ITEMS TRUE)
        set(BU_COPY_FULL_FRAMEWORK_CONTENTS FALSE)
        function(gp_item_default_embedded_path_override item path_var)
            if(item MATCHES "\\.framework/" OR item MATCHES "\\.dylib$")
                set(${path_var} "@executable_path/../Frameworks" PARENT_SCOPE)
            endif()
        endfunction()
        fixup_bundle("${_app}" "${_extra_binaries}" "${LUMINA_QT_RUNTIME_DIRS}")

        # Copied framework resources may need owner-write for signing.
        # Add owner-write only within the staged app; -P keeps symlinks unfollowed.
        if(IS_SYMLINK "${_app}")
            message(FATAL_ERROR "Cannot change permissions of a symlinked app: ${_app}")
        endif()
        execute_process(COMMAND /bin/chmod -R -P u+w "${_app}" RESULT_VARIABLE _chmod_result)
        if(NOT _chmod_result STREQUAL "0")
            message(FATAL_ERROR "Cannot make staged app writable: ${_app} (${_chmod_result})")
        endif()

        # All relocation and stripping precede signing. Sign Mach-O files first,
        # then enclosing frameworks, and finally seal the outer app resources.
        file(GLOB_RECURSE _bundle_items LIST_DIRECTORIES true "${_contents}/*")
        set(_machos "")
        set(_frameworks "")
        foreach(_item IN LISTS _bundle_items)
            if(IS_SYMLINK "${_item}")
                continue()
            elseif(IS_DIRECTORY "${_item}")
                if(_item MATCHES "\\.framework$")
                    list(APPEND _frameworks "${_item}")
                endif()
                continue()
            endif()
            execute_process(COMMAND /usr/bin/file -b "${_item}"
                    OUTPUT_VARIABLE _type RESULT_VARIABLE _file_result)
            if(NOT _file_result EQUAL 0)
                message(FATAL_ERROR "Cannot inspect packaged file: ${_item}")
            endif()
            if(_type MATCHES "Mach-O")
                execute_process(COMMAND /usr/bin/strip -x "${_item}" RESULT_VARIABLE _strip_result)
                if(NOT _strip_result EQUAL 0)
                    message(FATAL_ERROR "Cannot strip packaged binary: ${_item}")
                endif()
                list(APPEND _machos "${_item}")
            endif()
        endforeach()
        list(SORT _frameworks ORDER DESCENDING)
        foreach(_item IN LISTS _machos _frameworks)
            execute_process(COMMAND /usr/bin/codesign --sign - --force "${_item}" RESULT_VARIABLE _sign_result)
            if(NOT _sign_result EQUAL 0)
                message(FATAL_ERROR "Cannot sign packaged runtime: ${_item}")
            endif()
        endforeach()
        execute_process(COMMAND /usr/bin/codesign --sign - --force "${_app}" RESULT_VARIABLE _sign_result)
        if(NOT _sign_result EQUAL 0)
            message(FATAL_ERROR "Cannot sign app: ${_app}")
        endif()
        execute_process(COMMAND /usr/bin/codesign --verify --deep --strict "${_app}" RESULT_VARIABLE _verify_result)
        if(NOT _verify_result EQUAL 0)
            message(FATAL_ERROR "App signature verification failed: ${_app}")
        endif()
    ]] COMPONENT Runtime)

    set(CPACK_STRIP_FILES OFF)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_DMG_VOLUME_NAME "Lumen Fusion")
    set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
    set(CPACK_PACKAGE_FILE_NAME "Lumina")
else()
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/misc/uninstall_pkg.sh"
            DESTINATION "${SUNSHINE_ASSETS_DIR}")
    install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets/"
            DESTINATION "${SUNSHINE_ASSETS_DIR}")
    # copy assets to build directory, for running without install
    file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets/"
            DESTINATION "${CMAKE_BINARY_DIR}/assets")
endif()
