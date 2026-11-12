cmake_minimum_required(VERSION 3.16)

project(RemoteReleaseSync VERSION 1.0.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_executable(RemoteReleaseSync
    src/appconfig.h
    src/configmanager.cpp
    src/configmanager.h
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/syncplan.cpp
    src/syncplan.h
    src/mainwindow.ui
    src/syncruledialog.ui
    src/usagetipsdialog.ui
    src/recentcopypreview.ui
    src/updatedialog.ui
    src/syncworker.cpp
    src/syncworker.h
    resources/app.qrc
)

target_include_directories(RemoteReleaseSync PRIVATE src)

target_link_libraries(RemoteReleaseSync PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)

if (WIN32)
    target_sources(RemoteReleaseSync PRIVATE resources/app.rc)
    target_link_libraries(RemoteReleaseSync PRIVATE Mpr Shell32)
endif()

if (QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(RemoteReleaseSync)
endif()

option(SYNC_BUILD_TESTS "Build synchronization and UI regression tests" OFF)
if (SYNC_BUILD_TESTS)
    enable_testing()
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_executable(SyncToolTests
        tests/sync_tests.cpp
        src/configmanager.cpp src/configmanager.h src/appconfig.h
        src/syncplan.cpp src/syncplan.h
        src/syncworker.cpp src/syncworker.h
        src/mainwindow.cpp src/mainwindow.h
        src/mainwindow.ui src/syncruledialog.ui src/usagetipsdialog.ui
        src/recentcopypreview.ui src/updatedialog.ui
        resources/app.qrc)
    target_include_directories(SyncToolTests PRIVATE src)
    target_link_libraries(SyncToolTests PRIVATE Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    if (WIN32)
        target_link_libraries(SyncToolTests PRIVATE Mpr Shell32)
    endif()
    add_test(NAME SyncToolTests COMMAND SyncToolTests)
    set_tests_properties(SyncToolTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
