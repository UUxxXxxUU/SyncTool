#pragma once

#include <QProgressBar>
#include <QString>

namespace DownloadProgress
{
inline int percentage(qint64 received, qint64 total)
{
    if (total <= 0) return -1;
    // Convert before multiplying so large byte counts cannot overflow.
    return static_cast<int>(qBound(0.0, double(received) / double(total), 1.0) * 100.0);
}

inline QString size(qint64 bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', unit == 0 ? 0 : 1) + QLatin1Char(' ') + QString::fromLatin1(units[unit]);
}

inline QString text(qint64 received, qint64 total)
{
    const int percent = percentage(received, total);
    return percent < 0
        ? QString::fromUtf8("已下载 %1（总大小未知）").arg(size(received))
        : QString::fromUtf8("%1 / %2（%3%）").arg(size(received), size(total)).arg(percent);
}

inline void apply(QProgressBar *bar, int percent)
{
    if (!bar) return;
    bar->setRange(0, percent < 0 ? 0 : 100);
    if (percent >= 0) bar->setValue(qBound(0, percent, 100));
}
}
