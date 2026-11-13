cmake_minimum_required(VERSION 3.21)
project(DefDownloader VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools)
qt_standard_project_setup()

# App icon: resources/DefDownloader.icns is generated from resources/icon.svg.
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/DefDownloader.icns)
set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(DefDownloader WIN32 MACOSX_BUNDLE
    ${APP_ICON}
    src/main.cpp
    src/tools.h
    src/tools.cpp
    src/tooltab.h
    src/tooltab.cpp
    src/convert.h
    src/convert.cpp
    src/simple.h
    src/simple.cpp
    src/widgets.h
    src/widgets.cpp
)

if(WIN32)
    # Icon of the .exe; resources/DefDownloader.ico is generated from resources/icon.svg.
    target_sources(DefDownloader PRIVATE resources/DefDownloader.rc)
endif()

target_link_libraries(DefDownloader PRIVATE Qt6::Widgets)

if(UNIX AND NOT APPLE)
    # Linux: the window icon travels inside the executable (":/DefDownloader.png"),
    # and `cmake --install` adds what a desktop needs to list the app in its menu.
    # resources/DefDownloader.png is generated from resources/icon.svg.
    set(APP_ID io.github.elia2810.DefDownloader)
    qt_add_resources(DefDownloader "icon" PREFIX "/" BASE resources FILES resources/DefDownloader.png)
    install(TARGETS DefDownloader RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES resources/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/DefDownloader.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME ${APP_ID}.png)
    install(FILES resources/icon.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME ${APP_ID}.svg)
endif()

# The texts in the code are Italian. English lives in translations/DefDownloader_en.ts
# and ends up inside the executable (":/i18n/DefDownloader_en.qm").
# After changing a text: cmake --build build --target update_translations, then
# fill in the new entries of the .ts file.
qt_add_translations(TARGETS DefDownloader
    TS_FILES translations/DefDownloader_en.ts
    LUPDATE_OPTIONS -locations none -no-obsolete
)
# Compared with the latest release on GitHub when the app opens.
target_compile_definitions(DefDownloader PRIVATE APP_VERSION="${PROJECT_VERSION}")

set_target_properties(DefDownloader PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "DefDownloader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.defdownloader"
    MACOSX_BUNDLE_ICON_FILE "DefDownloader.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
