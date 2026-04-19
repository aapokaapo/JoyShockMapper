if (UNIX AND NOT APPLE)
    set (LINUX ON)

    find_package (PkgConfig QUIET REQUIRED)

    pkg_search_module (Gtkmm REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_search_module (appindicator REQUIRED IMPORTED_TARGET appindicator3-0.1)
    pkg_search_module (evdev REQUIRED IMPORTED_TARGET libevdev)

    # libmanette – optional; provides better Steam/SDL compatibility on modern
    # Linux distributions (e.g. Fedora 43).  When found, HAVE_LIBMANETTE is
    # defined so GamepadLibmanette.cpp can enable the extra integration code.
    pkg_search_module (manette QUIET IMPORTED_TARGET manette-0.2)
    if (manette_FOUND)
        message (STATUS "libmanette found – enabling XDG Gamepad backend")
    else ()
        message (STATUS "libmanette not found – using uinput+BUS_USB fallback")
    endif ()

    # GIO/D-Bus – used to send desktop notifications via the XDG Desktop Portal
    # (org.freedesktop.portal.Notification), the modern standard on GNOME 45+
    # and Fedora 43.  gio-2.0 ships with GLib and is already a transitive
    # dependency of GTK+, so it will virtually always be present.  When found,
    # HAVE_XDG_NOTIFICATIONS is defined and LinuxNotificationManager.cpp is
    # compiled with full portal-based notification support.  When absent, the
    # header provides inline no-op stubs so the rest of the code compiles
    # unchanged.
    pkg_search_module (gio QUIET IMPORTED_TARGET gio-2.0)
    if (gio_FOUND)
        message (STATUS "gio-2.0 found – enabling XDG desktop notifications")
    else ()
        message (STATUS "gio-2.0 not found – desktop notifications disabled")
    endif ()

    add_library (
        platform_dependencies INTERFACE
    )

    target_link_libraries (
	platform_dependencies INTERFACE
        PkgConfig::Gtkmm
        PkgConfig::appindicator
        PkgConfig::evdev
        pthread
        dl
    )

    if (manette_FOUND)
        target_link_libraries (
            platform_dependencies INTERFACE
            PkgConfig::manette
        )
        target_compile_definitions (
            platform_dependencies INTERFACE
            HAVE_LIBMANETTE
        )
    endif ()

    if (gio_FOUND)
        target_link_libraries (
            platform_dependencies INTERFACE
            PkgConfig::gio
        )
        target_compile_definitions (
            platform_dependencies INTERFACE
            HAVE_XDG_NOTIFICATIONS
        )
    endif ()

    # Release-only: enable fast floating-point math and SSE4.2 SIMD intrinsics.
    # -ffast-math enables aggressive floating-point optimisations (e.g. reassociation,
    # reciprocal approximation) that are safe for game-pad processing but may break
    # IEEE-754 edge cases.  Remove or restrict these flags if strict FP accuracy is needed.
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options (
            platform_dependencies INTERFACE
            -ffast-math
            -msse4.2
        )
    endif ()

    add_library (Platform::Dependencies ALIAS platform_dependencies)

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/50-joyshockmapper.rules
        DESTINATION lib/udev/rules.d
    )
    
    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/50-sony-gamepad.rules
        DESTINATION lib/udev/rules.d
    )
    
    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/99-disable-gamepad-touchpad-mouse.rules
        DESTINATION lib/udev/rules.d
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/JoyShockMapper/gyro_icon.png
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME JoyShockMapper.png
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/JoyShockMapper.desktop
        DESTINATION share/applications
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status.svg
        DESTINATION share/icons/hicolor/16x16/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status-dark.svg
        DESTINATION share/icons/hicolor/16x16/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status.svg
        DESTINATION share/icons/hicolor/22x22/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status-dark.svg
        DESTINATION share/icons/hicolor/22x22/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status.svg
        DESTINATION share/icons/hicolor/24x24/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status-dark.svg
        DESTINATION share/icons/hicolor/24x24/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status.svg
        DESTINATION share/icons/hicolor/scalable/status
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/jsm-status-dark.svg
        DESTINATION share/icons/hicolor/scalable/status
    )

    install (
        DIRECTORY ${PROJECT_SOURCE_DIR}/dist/GyroConfigs
        DESTINATION ../etc/JoyShockMapper/
    )

    # User-level systemd service (graphical session, no root required)
    install (
        FILES
            ${PROJECT_SOURCE_DIR}/dist/linux/systemd/joyshockmapper@.service
            ${PROJECT_SOURCE_DIR}/dist/linux/systemd/joyshockmapper.socket
        DESTINATION lib/systemd/user
    )

endif ()
