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

    add_library (Platform::Dependencies ALIAS platform_dependencies)

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/50-joyshockmapper.rules
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

    install (
        FILES
            ${PROJECT_SOURCE_DIR}/dist/linux/systemd/joysockmapper.service
            ${PROJECT_SOURCE_DIR}/dist/linux/systemd/joysockmapper@.service
            ${PROJECT_SOURCE_DIR}/dist/linux/systemd/joysockmapper.socket
        DESTINATION lib/systemd/system
    )

    install (
        FILES ${PROJECT_SOURCE_DIR}/dist/linux/conf.d/joysockmapper.conf
        DESTINATION ../etc/JoyShockMapper/conf.d
    )
endif ()
