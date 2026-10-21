#pragma once

#include "appconfig.h"

#include <atomic>
#include <memory>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QSystemTrayIcon>

QT_BEGIN_NAMESPACE
class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QNetworkAccessManager;
class QPoint;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QThread;
class QTimer;
QT_END_NAMESPACE

class SyncWorker;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

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
    void stopAllOperations();
    void checkForUpdates();
    void addFileType();
    void removeSelectedFileType();
    void handleTypeItemChanged(QListWidgetItem *item);
    void handleHostTableItemChanged(QTableWidgetItem *item);
    void handleHostTableCellClicked(int row, int column);
    void handleHostTableCellDoubleClicked(int row, int column);
    void showHostTableContextMenu(const QPoint &position);
    void handleWorkerProgress(const QString &hostId, int percent, const QString &detail);
    void handleWorkerState(const QString &hostId,
                           int state,
                           int mismatchPercent,
                           const QString &statusText,
                           const QString &detail,
                           const QString &detailTooltip);
    void handleWorkerFinished(const QString &hostId);
    void refreshLastSyncTimes();
    void pollForChanges();
    void trayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void realExit();
    void onReleaseDirEditFinished();

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
    void handleWorkerCopySucceeded(const QString &hostId, const QDateTime &completedUtc, quint64 revision);
    void addToReleaseDirHistory(const QString &path);
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
    void copyHostNameFromRow(int row);
    void requestAutoSync(const QString &statusMessage);
    void startOperations(bool performCopy);
    bool hasRunningWorkers() const;
    void requestWorkerToStop(const QString &hostId);
    void requestWorkersToStop();
    void finishExit();
    void updateTimerInterval();
    bool isSystemAutoStartEnabled() const;
    bool setSystemAutoStartEnabled(bool enabled, QString *errorMessage = nullptr) const;

    AppConfig m_config;
    QString m_configPath;
    QString m_lastFingerprint;
    QString m_lastShareOpenPath;
    QElapsedTimer m_shareOpenClickTimer;
    bool m_isOpeningShare = false;
    bool m_isRefreshingTable = false;
    bool m_pendingSyncAfterCurrentRun = false;
    bool m_backgroundTasksPaused = false;
    bool m_isClosing = false;
    bool m_exitFinished = false;
    QString m_pendingSyncStatusMessage;

    QComboBox *m_releaseDirEdit = nullptr;
    QListWidget *m_typeList = nullptr;
    QLineEdit *m_newTypeEdit = nullptr;
    QCheckBox *m_autoSyncCheck = nullptr;
    QCheckBox *m_startWithSystemCheck = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QPushButton *m_checkUpdatesButton = nullptr;
    QNetworkAccessManager *m_updateNetworkManager = nullptr;

    QCheckBox *m_hostEnabledCheck = nullptr;
    QLineEdit *m_hostNameEdit = nullptr;
    QLineEdit *m_hostPathEdit = nullptr;
    QLineEdit *m_hostUserEdit = nullptr;
    QLineEdit *m_hostPasswordEdit = nullptr;

    QTableWidget *m_hostTable = nullptr;
    QTimer *m_pollTimer = nullptr;

    QHash<QString, RowWidgets> m_rowWidgets;
    QHash<QString, QThread *> m_runningThreads;
    QHash<QString, std::shared_ptr<std::atomic<bool>>> m_cancellationFlags;
    QSet<QString> m_stoppingHostIds;
    QHash<QString, quint64> m_syncContextRevisions;

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_exitAction = nullptr;
};
