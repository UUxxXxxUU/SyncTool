#pragma once

#include "appconfig.h"

#include <QColor>
#include <QHash>
#include <QMainWindow>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QCloseEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QThread;
class QTimer;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void browseReleaseDir();
    void addOrUpdateHost(bool updateExisting);
    void removeSelectedHost();
    void clearHostEditor();
    void loadSelectedHostToEditor();
    void saveConfiguration();
    void syncNow();
    void addFileType();
    void removeSelectedFileType();
    void handleTypeItemChanged(QListWidgetItem *item);
    void handleHostTableItemChanged(QTableWidgetItem *item);
    void handleHostTableCellClicked(int row, int column);
    void handleWorkerProgress(const QString &hostId, int percent, const QString &detail);
    void handleWorkerState(const QString &hostId,
                           int state,
                           int mismatchPercent,
                           const QString &statusText,
                           const QString &detail,
                           const QString &detailTooltip);
    void handleWorkerFinished(const QString &hostId);
    void pollForChanges();

private:
    struct RowWidgets
    {
        QProgressBar *progressBar = nullptr;
    };

    void setupUi();
    void loadConfiguration();
    void applyConfigToUi();
    void collectUiToConfig();
    void refreshTypeList();
    void refreshHostTable();
    void updateHostRow(int row,
                       int state,
                       int mismatchPercent,
                       const QString &statusText,
                       const QString &detail,
                       const QString &detailTooltip,
                       int progressPercent);
    QColor colorForState(int state, int mismatchPercent) const;
    QString currentLocalFingerprint() const;
    QString normalizedExtension(const QString &value) const;
    QVector<int> targetRows() const;
    void requestAutoSync(const QString &statusMessage);
    void startOperations(bool performCopy);
    bool hasRunningWorkers() const;
    void updateTimerInterval();

    AppConfig m_config;
    QString m_configPath;
    QString m_lastFingerprint;
    bool m_isRefreshingTable = false;
    bool m_pendingSyncAfterCurrentRun = false;
    QString m_pendingSyncStatusMessage;

    QLineEdit *m_releaseDirEdit = nullptr;
    QListWidget *m_typeList = nullptr;
    QLineEdit *m_newTypeEdit = nullptr;
    QCheckBox *m_autoSyncCheck = nullptr;
    QSpinBox *m_intervalSpin = nullptr;

    QCheckBox *m_hostEnabledCheck = nullptr;
    QLineEdit *m_hostNameEdit = nullptr;
    QLineEdit *m_hostPathEdit = nullptr;
    QLineEdit *m_hostUserEdit = nullptr;
    QLineEdit *m_hostPasswordEdit = nullptr;

    QTableWidget *m_hostTable = nullptr;
    QTimer *m_pollTimer = nullptr;

    QHash<QString, RowWidgets> m_rowWidgets;
    QHash<QString, QThread *> m_runningThreads;
};
