cmake_minimum_required(VERSION 3.21)
project(DefDownloader VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
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

set_target_properties(DefDownloader PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "DefDownloader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.defdownloader"
    MACOSX_BUNDLE_ICON_FILE "DefDownloader.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
