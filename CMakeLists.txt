cmake_minimum_required(VERSION 3.16)

project(RemoteReleaseSync VERSION 1.0.0 LANGUAGES CXX)

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
    src/syncworker.cpp
    src/syncworker.h
)

target_include_directories(RemoteReleaseSync PRIVATE src)

target_link_libraries(RemoteReleaseSync PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)

if (WIN32)
    target_link_libraries(RemoteReleaseSync PRIVATE Mpr)
endif()

if (QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(RemoteReleaseSync)
endif()
