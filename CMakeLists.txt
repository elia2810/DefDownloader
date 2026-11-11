cmake_minimum_required(VERSION 3.21)
project(ytmlp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# App icon: resources/ytmlp.icns is generated from resources/icon.svg.
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/ytmlp.icns)
set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

qt_add_executable(ytmlp WIN32 MACOSX_BUNDLE
    ${APP_ICON}
    src/main.cpp
    src/tools.h
    src/tools.cpp
    src/tooltab.h
    src/tooltab.cpp
)

if(WIN32)
    # Icon of the .exe; resources/ytmlp.ico is generated from resources/icon.svg.
    target_sources(ytmlp PRIVATE resources/ytmlp.rc)
endif()

target_link_libraries(ytmlp PRIVATE Qt6::Widgets)

set_target_properties(ytmlp PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "ytmlp"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.ytmlp"
    MACOSX_BUNDLE_ICON_FILE "ytmlp.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
