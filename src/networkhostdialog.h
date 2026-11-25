#pragma once

#include "roundedwindow.h"
#include <QHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QVector>
#include <atomic>

class QLabel;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QTabBar;
class QTableWidget;
class QUdpSocket;

class NetworkHostDialog : public RoundedDialog
{
    Q_OBJECT
public:
    explicit NetworkHostDialog(QWidget *parent = nullptr, bool autoScan = true);
    ~NetworkHostDialog() override;
    QString selectedServer() const;
    void setConfiguredServers(const QStringList &servers);

signals:
    void browseSharesRequested(const QString &uncRoot);

private slots:
    void resolveNameResult(const QHostInfo &info);

private:
    friend class SyncTests;
    enum ResultRole {
        Server = Qt::UserRole,
        KnownNames,
        InterfaceIndex,
        SmbAvailable,
        Configured,
        DnsPending,
        NetbiosPending
    };
    struct Range {
        QHostAddress source;
        quint64 next = 0;
        quint64 last = 0;
        QString label;
        int interfaceIndex = 0;
    };
    struct Candidate {
        QHostAddress address;
        QHostAddress source;
        QString label;
        int interfaceIndex = 0;
    };
    void startScan();
    void cancelScan();
    void pump();
    void recordHost(const Candidate &candidate, bool online, bool smb, quint64 generation);
    int insertHost(const Candidate &candidate, bool smb);
    void resolveName(int row, const Candidate &candidate, quint64 generation);
    void finishNameLookup(int row, int pendingRole);
    void setResolvedName(int row, const QString &name);
    void updateConfiguredState(int row);
    void applyRowFilter(int row);
    void applyFilters();
    void browseShares(int row, int column);
    void updateProgress();

    QTableWidget *m_table = nullptr;
    QTabBar *m_adapterTabs = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_hideUnnamed = nullptr;
    QCheckBox *m_onlySmb = nullptr;
    QCheckBox *m_onlyConfigured = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_interfaces = nullptr;
    QPushButton *m_scanButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_selectButton = nullptr;
    QThreadPool m_pool;
    QVector<Range> m_ranges;
    QVector<Candidate> m_neighbors;
    QHash<QString, int> m_rows;
    QSet<QString> m_localAddresses;
    QSet<QString> m_configuredServers;
    QVector<int> m_lookups;
    QHash<int, int> m_lookupRows;
    QVector<QUdpSocket *> m_nameSockets;
    std::atomic<bool> m_cancelled{true};
    quint64 m_generation = 0;
    quint64 m_total = 0;
    quint64 m_completed = 0;
    int m_inFlight = 0;
    int m_rangeIndex = 0;
    int m_visibleCount = 0;
    bool m_scanning = false;
    QString m_operationId;
    QElapsedTimer m_elapsed;
    bool m_loggedFinished = false;
};
