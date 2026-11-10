#pragma once

#include "appconfig.h"

#include <atomic>
#include <memory>
#include <QColor>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QSystemTrayIcon>
#include <QStringList>
#include <QUrl>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialog;
class QEvent;
class QFile;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QNetworkAccessManager;
class QNetworkReply;
class QPoint;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QThread;
class QTimer;
class QVariantAnimation;
QT_END_NAMESPACE

class SyncWorker;
class RecentCopyPreview;

// 更新说明中的一个分类，例如“修复问题”“新增功能”。
struct UpdateNoteSection
{
    QString title;
    QStringList items;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool event(QEvent *event) override;
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
    void startUpdateCheck(bool interactive);
    void requestUpdateInfo(bool useRawFallback);
    void finishUpdateCheck(const QString &errorMessage = QString());
    void showAvailableUpdate();
    void updateCheckButton();
    void stopUpdateChecks();
    void openUpdateUrl(const QUrl &url);
    void showUpdateHistoryLink(const QString &message);
    void startInstallerDownload(bool interactive);
    void requestInstallerChecksum();
    void requestInstallerFile(const QString &expectedSha256);
    bool appendInstallerChunk(QNetworkReply *reply);
    void failInstallerDownload(const QString &errorMessage);
    void abortInstallerDownload();
    void finishInstallerDownload();
    void setUpdateDialogStatus(const QString &text, int percent = -1);
    bool isReadyInstallerCurrent() const;
    bool installSilentUpdateIfIdle();
    bool launchInstaller(bool silent, QString *errorMessage);
    void installUpdateNow();
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
    QCheckBox *m_checkUpdatesOnStartupCheck = nullptr;
    QCheckBox *m_silentUpdateCheck = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QPushButton *m_checkUpdatesButton = nullptr;
    QNetworkAccessManager *m_updateNetworkManager = nullptr;
    QPointer<QNetworkReply> m_updateReply;
    QTimer *m_updateCheckTimer = nullptr;
    QVariantAnimation *m_updatePulse = nullptr;
    bool m_updateCheckInProgress = false;
    bool m_updateCheckInteractive = false;
    bool m_updateDialogOpen = false;
    QString m_availableUpdateVersion;
    QString m_availableUpdateNotes;
    QUrl m_availableUpdateUrl;
    QUrl m_availableReleaseUrl;
    QUrl m_availableInstallerUrl;
    QVector<UpdateNoteSection> m_availableUpdateSections;
    QString m_dismissedUpdateVersion;

    // 安装包下载状态：先下载 .sha256 校验文件，再下载安装包并边下边计算摘要。
    QPointer<QNetworkReply> m_installerReply;
    QFile *m_installerFile = nullptr;
    std::unique_ptr<QCryptographicHash> m_installerHash;
    QString m_installerDownloadVersion;
    QUrl m_installerDownloadUrl;
    bool m_installerDownloadInteractive = false;
    bool m_installAfterDownload = false;
    QString m_readyInstallerPath;
    QString m_readyInstallerVersion;
    bool m_isInstallingUpdate = false;
    QString m_silentInstallFailedVersion;
    QPointer<QDialog> m_updateDialog;
    QPointer<QProgressBar> m_updateDialogProgress;
    QPointer<QLabel> m_updateDialogStatus;
    QPointer<QPushButton> m_updateDialogInstallButton;

    QCheckBox *m_hostEnabledCheck = nullptr;
    QLineEdit *m_hostNameEdit = nullptr;
    QLineEdit *m_hostPathEdit = nullptr;
    QLineEdit *m_hostUserEdit = nullptr;
    QLineEdit *m_hostPasswordEdit = nullptr;

    QTableWidget *m_hostTable = nullptr;
    QTimer *m_pollTimer = nullptr;
    RecentCopyPreview *m_copyPreview = nullptr;

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
