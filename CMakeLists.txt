cmake_minimum_required(VERSION 3.16)

project(RemoteReleaseSync VERSION 1.1.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
if (MSVC)
    add_compile_options(/utf-8)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_executable(RemoteReleaseSync
    src/appearance.h
    src/roundedwindow.h
    src/downloadprogress.h
    third_party/qt-frameless-window/FramelessWindow.hpp
    src/appconfig.h
    src/configmanager.cpp
    src/configmanager.h
    src/diagnostics.cpp
    src/diagnostics.h
    src/feedbackdialog.cpp src/feedbackdialog.h
    src/remotedebugger.cpp
    src/remotedebugger.h
    src/networkhostdialog.cpp
    src/networkhostdialog.h
    src/remotedebugger.ui
    src/main.cpp
    src/mainwindow.cpp
    src/shortcutmanager.cpp src/shortcutmanager.h
    src/mainwindow.h
    src/synccompletiontoast.cpp
    src/synccompletiontoast.h
    src/synccompletiontoast.ui
    src/syncplan.cpp
    src/syncplan.h
    src/mainwindow.ui
    src/preferencesdialog.ui
    src/syncruledialog.ui
    src/usagetipsdialog.ui
    src/recentcopypreview.ui
    src/updatedialog.ui
    src/syncworker.cpp
    src/syncworker.h
    resources/app.qrc
)

target_include_directories(RemoteReleaseSync PRIVATE src third_party/spdlog/include)

target_link_libraries(RemoteReleaseSync PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)

if (WIN32)
    target_sources(RemoteReleaseSync PRIVATE resources/app.rc)
    target_link_libraries(RemoteReleaseSync PRIVATE Mpr Shell32 User32 Version Wintrust Crypt32 Iphlpapi Ws2_32 Advapi32)
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
        src/configmanager.cpp src/configmanager.h src/appconfig.h src/appearance.h
        src/diagnostics.cpp src/diagnostics.h
        src/feedbackdialog.cpp src/feedbackdialog.h
        src/remotedebugger.cpp src/remotedebugger.h src/remotedebugger.ui
        src/networkhostdialog.cpp src/networkhostdialog.h
        src/syncplan.cpp src/syncplan.h
        src/syncworker.cpp src/syncworker.h
        src/mainwindow.cpp src/mainwindow.h
        src/shortcutmanager.cpp src/shortcutmanager.h
        src/synccompletiontoast.cpp src/synccompletiontoast.h src/synccompletiontoast.ui
        src/mainwindow.ui src/preferencesdialog.ui src/syncruledialog.ui src/usagetipsdialog.ui
        src/recentcopypreview.ui src/updatedialog.ui
        resources/app.qrc)
    target_include_directories(SyncToolTests PRIVATE src third_party/spdlog/include)
    target_link_libraries(SyncToolTests PRIVATE Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    if (WIN32)
        target_link_libraries(SyncToolTests PRIVATE Mpr Shell32 User32 Imm32 Version Wintrust Crypt32 Iphlpapi Ws2_32 Advapi32)
    endif()
    add_test(NAME SyncToolTests COMMAND SyncToolTests
        -o "${CMAKE_CURRENT_BINARY_DIR}/SyncToolTests-$<CONFIG>.txt,txt"
        -o "${CMAKE_CURRENT_BINARY_DIR}/SyncToolTests-$<CONFIG>.xml,xunitxml")
    if (WIN32)
        # Qt 5.12 message boxes access native Windows resources when shown.
        set_tests_properties(SyncToolTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    else()
        set_tests_properties(SyncToolTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
