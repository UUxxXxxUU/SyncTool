QT += widgets network

CONFIG += c++17
TEMPLATE = app
TARGET = RemoteReleaseSync

SOURCES += \
    src/configmanager.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/syncworker.cpp

HEADERS += \
    src/appconfig.h \
    src/configmanager.h \
    src/mainwindow.h \
    src/syncworker.h

win32:LIBS += -lMpr
