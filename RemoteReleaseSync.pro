QT += widgets network

CONFIG += c++17
TEMPLATE = app
TARGET = RemoteReleaseSync
INCLUDEPATH += third_party/spdlog/include
msvc:QMAKE_CXXFLAGS += /utf-8

SOURCES += \
    src/configmanager.cpp \
    src/diagnostics.cpp \
    src/remotedebugger.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/synccompletiontoast.cpp \
    src/syncplan.cpp \
    src/syncworker.cpp

HEADERS += \
    src/appearance.h \
    src/appconfig.h \
    src/configmanager.h \
    src/diagnostics.h \
    src/remotedebugger.h \
    src/mainwindow.h \
    src/synccompletiontoast.h \
    src/syncplan.h \
    src/syncworker.h

RESOURCES += resources/app.qrc

FORMS += \
    src/mainwindow.ui \
    src/preferencesdialog.ui \
    src/remotedebugger.ui \
    src/synccompletiontoast.ui \
    src/syncruledialog.ui \
    src/usagetipsdialog.ui \
    src/recentcopypreview.ui \
    src/updatedialog.ui

win32:RC_FILE = resources/app.rc
win32:LIBS += -lMpr -lShell32 -lUser32 -lVersion -lWintrust -lCrypt32
