# macos specific target definitions
if(SUNSHINE_PACKAGE_MACOS)
    set_target_properties(sunshine PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/Info.plist.in")
    target_compile_definitions(sunshine PUBLIC SUNSHINE_MACOS_BUNDLE=1)
    add_dependencies(sunshine vd_helper)
else()
    target_link_options(sunshine PRIVATE LINKER:-sectcreate,__TEXT,__info_plist,${APPLE_PLIST_FILE})
endif()
# Tell linker to dynamically load these symbols at runtime, in case they're unavailable:
target_link_options(sunshine PRIVATE -Wl,-U,_CGPreflightScreenCaptureAccess -Wl,-U,_CGRequestScreenCaptureAccess)
