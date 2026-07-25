# Turns a sample executable into a macOS .app bundle.
#
# A plain Mach-O executable has no bundle identity, so macOS gives it no Info.plist to
# read (high-DPI backing store, minimum system version, ...) and treats it as an
# accessory process rather than a foreground UI app. GUI targets therefore have to ship
# as .app bundles; the binary itself still lands at
# ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/<target>.app/Contents/MacOS/<target>.
function(vkm_configure_macos_app_bundle target)
    if (NOT APPLE OR IOS)
        return()
    endif()

    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE                      TRUE
        MACOSX_BUNDLE_INFO_PLIST           "${PROJECT_SOURCE_DIR}/buildscripts/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME          "${target}"
        MACOSX_BUNDLE_GUI_IDENTIFIER       "com.vkm.${target}"
        MACOSX_BUNDLE_BUNDLE_VERSION       "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endfunction()
