QT += widgets network

CONFIG += c++17
TEMPLATE = app
TARGET = RemoteReleaseSync

SOURCES += \
    src/configmanager.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/syncplan.cpp \
    src/syncworker.cpp

HEADERS += \
    src/appconfig.h \
    src/configmanager.h \
    src/mainwindow.h \
    src/syncplan.h \
    src/syncworker.h

RESOURCES += resources/app.qrc

FORMS += \
    src/mainwindow.ui \
    src/syncruledialog.ui \
    src/usagetipsdialog.ui \
    src/recentcopypreview.ui \
    src/updatedialog.ui

win32:RC_FILE = resources/app.rc
win32:LIBS += -lMpr -lShell32
