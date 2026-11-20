#pragma once

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>

namespace Appearance
{
inline void prepareApplication()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
}

inline void apply(QApplication &app)
{
    QFont font = app.font();
#ifdef Q_OS_WIN
    // Use the Windows UI family for Chinese text when it is installed.
    if (QFontDatabase().families().contains(QStringLiteral("Microsoft YaHei UI"), Qt::CaseInsensitive)) {
        font.setFamily(QStringLiteral("Microsoft YaHei UI"));
    }
#endif
    font.setPointSize(10);
    app.setFont(font);

    QFile stylesheet(":/interface.qss");
    if (stylesheet.open(QIODevice::ReadOnly)) {
        app.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
    }
}
}
