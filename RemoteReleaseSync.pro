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
    src/networkhostdialog.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/shortcutmanager.cpp \
    src/synccompletiontoast.cpp \
    src/syncplan.cpp \
    src/syncworker.cpp

HEADERS += \
    src/appearance.h \
    src/roundedwindow.h \
    src/downloadprogress.h \
    third_party/qt-frameless-window/FramelessWindow.hpp \
    src/appconfig.h \
    src/configmanager.h \
    src/diagnostics.h \
    src/remotedebugger.h \
    src/networkhostdialog.h \
    src/mainwindow.h \
    src/shortcutmanager.h \
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
win32:LIBS += -lMpr -lShell32 -lUser32 -lVersion -lWintrust -lCrypt32 -lIphlpapi -lWs2_32
