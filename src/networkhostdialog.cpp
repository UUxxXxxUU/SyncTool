#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#endif

#include "networkhostdialog.h"
#include "diagnostics.h"
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QGuiApplication>
#include <QJsonArray>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QPushButton>
#include <QRunnable>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QtEndian>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#ifdef _MSC_VER
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ws2_32.lib")
#endif
#endif

namespace {
QString serverKey(const QString &server)
{
    QString value = server.trimmed().toCaseFolded();
    if (value.endsWith('.')) value.chop(1);
    const QHostAddress ip(value);
    return ip.isNull() ? value : ip.toString().toCaseFolded();
}

QString uncServerForAddress(const QHostAddress &address)
{
    QString server = address.toString();
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        server.replace(':', '-').replace('%', 's');
        server += QStringLiteral(".ipv6-literal.net");
    }
    return server;
}

class ProbeTask : public QRunnable
{
public:
    explicit ProbeTask(std::function<void()> work) : m_work(std::move(work)) {}
    void run() override { m_work(); }
private:
    std::function<void()> m_work;
};

bool echo(const QHostAddress &target, const QHostAddress &source)
{
#ifdef Q_OS_WIN
    char payload[] = "RRS";
    alignas(16) char reply[512] = {};
    if (target.protocol() == QAbstractSocket::IPv4Protocol) {
        HANDLE handle = IcmpCreateFile();
        if (handle == INVALID_HANDLE_VALUE) return false;
        const DWORD count = IcmpSendEcho2Ex(handle, nullptr, nullptr, nullptr,
            htonl(source.toIPv4Address()), htonl(target.toIPv4Address()), payload,
            sizeof(payload), nullptr, reply, sizeof(reply), 220);
        const bool ok = count > 0 && reinterpret_cast<ICMP_ECHO_REPLY *>(reply)->Status == IP_SUCCESS;
        IcmpCloseHandle(handle);
        return ok;
    }
    HANDLE handle = Icmp6CreateFile();
    if (handle == INVALID_HANDLE_VALUE) return false;
    sockaddr_in6 from = {}, to = {};
    from.sin6_family = to.sin6_family = AF_INET6;
    const Q_IPV6ADDR sourceBytes = source.toIPv6Address(), targetBytes = target.toIPv6Address();
    std::memcpy(&from.sin6_addr, sourceBytes.c, 16);
    std::memcpy(&to.sin6_addr, targetBytes.c, 16);
    from.sin6_scope_id = source.scopeId().toUInt();
    to.sin6_scope_id = target.scopeId().toUInt();
    const DWORD count = Icmp6SendEcho2(handle, nullptr, nullptr, nullptr, &from, &to,
        payload, sizeof(payload), nullptr, reply, sizeof(reply), 220);
    const bool ok = count > 0 && reinterpret_cast<ICMPV6_ECHO_REPLY *>(reply)->Status == IP_SUCCESS;
    IcmpCloseHandle(handle);
    return ok;
#else
    Q_UNUSED(target);
    Q_UNUSED(source);
    return false;
#endif
}

QByteArray nodeStatusQuery(quint16 id)
{
    QByteArray packet(12, '\0');
    qToBigEndian<quint16>(id, reinterpret_cast<uchar *>(packet.data()));
    packet[5] = 1;
    packet.append(char(32));
    QByteArray name(16, '\0');
    name[0] = '*';
    // NBNS 通配名称由 16 字节编码成 32 个 A～P 字符，用于查询设备自己的 NetBIOS 名称。
    for (uchar c : name) {
        packet.append(char('A' + (c >> 4)));
        packet.append(char('A' + (c & 15)));
    }
    packet.append(QByteArray("\0\0\x21\0\x01", 5));
    return packet;
}

int skipDnsName(const QByteArray &data, int offset)
{
    for (int labels = 0; labels < 128 && offset < data.size(); ++labels) {
        const uchar length = uchar(data.at(offset++));
        if (!length) return offset;
        if ((length & 0xc0) == 0xc0) return offset < data.size() ? offset + 1 : -1;
        if (length > 63 || offset + length > data.size()) return -1;
        offset += length;
    }
    return -1;
}

QString nodeName(const QByteArray &data, quint16 id)
{
    if (data.size() < 12 || qFromBigEndian<quint16>(data.constData()) != id
        || !(uchar(data[2]) & 0x80) || (uchar(data[3]) & 15)) return {};
    int offset = 12;
    const int questions = qFromBigEndian<quint16>(data.constData() + 4);
    const int answers = qFromBigEndian<quint16>(data.constData() + 6);
    if (questions > 16 || answers > 32) return {};
    for (int i = 0; i < questions; ++i) {
        offset = skipDnsName(data, offset);
        if (offset < 0 || offset + 4 > data.size()) return {};
        offset += 4;
    }
    for (int i = 0; i < answers; ++i) {
        offset = skipDnsName(data, offset);
        if (offset < 0 || offset + 10 > data.size()) return {};
        const int type = qFromBigEndian<quint16>(data.constData() + offset);
        const int length = qFromBigEndian<quint16>(data.constData() + offset + 8);
        offset += 10;
        if (offset + length > data.size()) return {};
        if (type == 0x21 && length >= 1) {
            const int count = uchar(data[offset]);
            // 每条名称记录为 15 字节名称、1 字节后缀和 2 字节标志；先检查长度，再排除组名称。
            if (1 + count * 18 > length) return {};
            for (int n = 0; n < count; ++n) {
                const char *entry = data.constData() + offset + 1 + n * 18;
                const quint16 flags = qFromBigEndian<quint16>(entry + 16);
                if (uchar(entry[15]) == 0 && !(flags & 0x8000)) {
                    const QString name = QString::fromLatin1(entry, 15).trimmed();
                    static const QRegularExpression validName("^[A-Za-z0-9_.-]{1,15}$");
                    if (validName.match(name).hasMatch()) return name;
                }
            }
        }
        offset += length;
    }
    return {};
}
}

NetworkHostDialog::NetworkHostDialog(QWidget *parent, bool autoScan) : RoundedDialog(parent)
{
    setWindowTitle(QStringLiteral("扫描局域网设备"));
    resize(830, 560);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(QStringLiteral("双击主机名打开共享根目录。选中设备后点击“填入主机”，可带回主机配置。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_interfaces = new QLabel(this);
    m_interfaces->setWordWrap(true);
    layout->addWidget(m_interfaces);
    m_adapterTabs = new QTabBar(this);
    m_adapterTabs->setObjectName("scanAdapterTabs");
    m_adapterTabs->setExpanding(false);
    m_adapterTabs->setUsesScrollButtons(true);
    m_adapterTabs->setAccessibleName(QStringLiteral("按网卡查看扫描结果"));
    layout->addWidget(m_adapterTabs);
    m_search = new QLineEdit(this);
    m_search->setObjectName("scanSearch");
    m_search->setPlaceholderText(QStringLiteral("搜索主机名或 IP 地址"));
    m_search->setAccessibleName(m_search->placeholderText());
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    auto *filters = new QHBoxLayout;
    m_hideUnnamed = new QCheckBox(QStringLiteral("隐藏未获取主机名的设备"), this);
    m_onlySmb = new QCheckBox(QStringLiteral("仅显示文件共享端口可连接"), this);
    m_onlyConfigured = new QCheckBox(QStringLiteral("仅显示已配置主机"), this);
    for (auto *filter : {m_hideUnnamed, m_onlySmb, m_onlyConfigured}) {
        filters->addWidget(filter);
        connect(filter, &QCheckBox::toggled, this, &NetworkHostDialog::applyFilters);
    }
    filters->addStretch();
    layout->addLayout(filters);
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("主机名"), QStringLiteral("IP 地址"),
        QStringLiteral("文件共享端口"), QStringLiteral("网卡"), QStringLiteral("配置")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    layout->addWidget(m_table, 1);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(this);
    m_scanButton = buttons->addButton(QStringLiteral("重新扫描"), QDialogButtonBox::ActionRole);
    m_cancelButton = buttons->addButton(QStringLiteral("取消扫描"), QDialogButtonBox::ActionRole);
    m_selectButton = buttons->addButton(QStringLiteral("填入主机"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    m_selectButton->setEnabled(false);
    m_cancelButton->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (!selectedServer().isEmpty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_scanButton, &QPushButton::clicked, this, &NetworkHostDialog::startScan);
    connect(m_cancelButton, &QPushButton::clicked, this, &NetworkHostDialog::cancelScan);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_selectButton->setEnabled(!selectedServer().isEmpty());
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &NetworkHostDialog::browseShares);
    connect(m_adapterTabs, &QTabBar::currentChanged, this, &NetworkHostDialog::applyFilters);
    connect(m_search, &QLineEdit::textChanged, this, &NetworkHostDialog::applyFilters);
    m_pool.setMaxThreadCount(32);
    QScreen *screen = parent ? QGuiApplication::screenAt(parent->frameGeometry().center()) : QGuiApplication::primaryScreen();
    if (screen) resize(QSize(830, 560).boundedTo(screen->availableGeometry().size() - QSize(32, 48)));
    if (autoScan) QTimer::singleShot(0, this, &NetworkHostDialog::startScan);
}

NetworkHostDialog::~NetworkHostDialog()
{
    if (m_scanning) cancelScan();
    m_cancelled.store(true);
    for (int id : m_lookups) QHostInfo::abortHostLookup(id);
    m_pool.clear();
    // 工作线程只执行有界网络等待；销毁接收对象前先结束探测，避免回调访问已释放对象。
    m_pool.waitForDone();
}

QString NetworkHostDialog::selectedServer() const
{
    const int row = m_table->currentRow();
    if (row < 0 || m_table->isRowHidden(row)) return {};
    const auto *item = m_table->item(row, 0);
    return item && item->isSelected() ? item->data(Server).toString() : QString();
}

void NetworkHostDialog::setConfiguredServers(const QStringList &servers)
{
    m_configuredServers.clear();
    for (const auto &server : servers) {
        const auto key = serverKey(server);
        if (!key.isEmpty()) m_configuredServers.insert(key);
    }
    for (int row = 0; row < m_table->rowCount(); ++row) updateConfiguredState(row);
    applyFilters();
}

void NetworkHostDialog::browseShares(int row, int column)
{
    if (column != 0 || row < 0 || row >= m_table->rowCount() || m_table->isRowHidden(row)) return;
    const auto *item = m_table->item(row, 0);
    if (!item) return;
    const QString server = item->data(Server).toString();
    static const QRegularExpression validServer("^[A-Za-z0-9_.-]{1,253}$");
    if (!validServer.match(server).hasMatch() || server.startsWith('.') || server.contains("..")) return;
    emit browseSharesRequested(QStringLiteral("\\\\%1\\").arg(server));
}

void NetworkHostDialog::startScan()
{
    if (m_inFlight) return;
    m_operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_elapsed.start();
    m_loggedFinished = false;
    for (int id : m_lookups) QHostInfo::abortHostLookup(id);
    m_lookups.clear();
    m_lookupRows.clear();
    qDeleteAll(m_nameSockets);
    m_nameSockets.clear();
    ++m_generation;
    m_ranges.clear();
    m_neighbors.clear();
    m_rows.clear();
    m_localAddresses.clear();
    m_table->setRowCount(0);
    m_selectButton->setEnabled(false);
    m_visibleCount = 0;
    m_total = m_completed = 0;
    m_rangeIndex = 0;
    QStringList labels;
    QHash<int, QStringList> descriptions;
    int preferredTab = -1;
    int previousTab = -1;
    const QVariant previousAdapter = m_adapterTabs->tabData(m_adapterTabs->currentIndex());
    QSet<QString> rangeKeys;
    const auto interfaces = QNetworkInterface::allInterfaces();
    const QSignalBlocker blockTabs(m_adapterTabs);
    while (m_adapterTabs->count()) m_adapterTabs->removeTab(0);
    m_adapterTabs->addTab(QStringLiteral("全部"));
    m_adapterTabs->setTabData(0, -1);
    for (const auto &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::IsRunning)
            || (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
        const int tab = m_adapterTabs->addTab(iface.humanReadableName());
        m_adapterTabs->setTabData(tab, iface.index());
        if (previousAdapter.isValid() && previousAdapter.toInt() == iface.index()) previousTab = tab;
        if (preferredTab < 0 && (iface.type() == QNetworkInterface::Ethernet || iface.type() == QNetworkInterface::Wifi))
            preferredTab = tab;
        for (const auto &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            m_localAddresses.insert(ip.toString());
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || entry.prefixLength() <= 0) continue;
            const quint32 mask = entry.netmask().toIPv4Address(), address = ip.toIPv4Address();
            if (!mask) continue;
            quint64 first = address & mask, last = quint64(address | ~mask);
            // /31、/32 不剔除端点；64 位游标可安全越过 255.255.255.255，避免末尾地址回绕。
            if (entry.prefixLength() < 31) { ++first; --last; }
            const QString key = QString::number(iface.index()) + '/' + ip.toString();
            if (rangeKeys.contains(key)) continue;
            rangeKeys.insert(key);
            m_ranges.append({ip, first, last, iface.humanReadableName(), iface.index()});
            m_total += last - first + 1;
            const QString subnet = QString("%1/%2").arg(QHostAddress(address & mask).toString()).arg(entry.prefixLength());
            labels.append(QString("%1：%2").arg(iface.humanReadableName(), subnet));
            descriptions[iface.index()].append(subnet);
        }
    }
#ifdef Q_OS_WIN
    PMIB_IPNET_TABLE2 neighbors = nullptr;
    if (GetIpNetTable2(AF_INET6, &neighbors) == NO_ERROR && neighbors) {
        QSet<QString> seen;
        for (ULONG n = 0; n < neighbors->NumEntries; ++n) {
            const auto &neighbor = neighbors->Table[n];
            if (neighbor.State == NlnsUnreachable || neighbor.State == NlnsIncomplete) continue;
            for (const auto &iface : interfaces) {
                if (iface.index() != neighbor.InterfaceIndex || !(iface.flags() & QNetworkInterface::IsUp)
                    || !(iface.flags() & QNetworkInterface::IsRunning) || (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
                Q_IPV6ADDR bytes;
                std::memcpy(bytes.c, &neighbor.Address.Ipv6.sin6_addr, 16);
                QHostAddress ip(bytes);
                if (ip.isMulticast() || ip.isNull() || ip.isLoopback()) continue;
                if (ip.isLinkLocal()) ip.setScopeId(QString::number(iface.index()));
                const QString key = QString::number(iface.index()) + '/' + ip.toString();
                if (seen.contains(key)) continue;
                for (const auto &entry : iface.addressEntries()) {
                    if (entry.ip().protocol() != QAbstractSocket::IPv6Protocol
                        || entry.ip().isLinkLocal() != ip.isLinkLocal()) continue;
                    QHostAddress source = entry.ip();
                    if (source.isLinkLocal()) source.setScopeId(QString::number(iface.index()));
                    m_neighbors.append({ip, source, iface.humanReadableName(), iface.index()});
                    seen.insert(key);
                    ++m_total;
                    break;
                }
            }
        }
        FreeMibTable(neighbors);
    }
#endif
    m_adapterTabs->setTabToolTip(0, labels.join('\n'));
    for (int tab = 1; tab < m_adapterTabs->count(); ++tab)
        m_adapterTabs->setTabToolTip(tab, descriptions.value(m_adapterTabs->tabData(tab).toInt()).join('\n'));
    if (previousAdapter.isValid() && previousAdapter.toInt() == -1) previousTab = 0;
    m_adapterTabs->setCurrentIndex(previousTab >= 0 ? previousTab : qMax(0, preferredTab));
    m_interfaces->setText(m_adapterTabs->count() > 1
        ? QStringLiteral("已启用网卡 %1 张，按页签查看结果。").arg(m_adapterTabs->count() - 1)
        : QStringLiteral("未找到可扫描的网卡。"));
    Diagnostics::event("scan.start", {{"run", m_operationId}, {"action", Diagnostics::lastUserAction()},
        {"interfaces", QJsonArray::fromStringList(labels)}, {"addresses", double(m_total)}, {"ipv6_neighbors", m_neighbors.size()}});
    m_cancelled.store(false);
    m_scanning = true;
    m_scanButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    pump();
}

void NetworkHostDialog::cancelScan()
{
    Diagnostics::event("scan.cancel", {{"run", m_operationId}, {"completed", double(m_completed)},
        {"found", m_table->rowCount()}, {"in_flight", m_inFlight}, {"elapsed_ms", double(m_elapsed.isValid() ? m_elapsed.elapsed() : 0)}});
    m_cancelled.store(true);
    m_scanning = false;
    m_cancelButton->setEnabled(false);
    m_scanButton->setEnabled(m_inFlight == 0);
    updateProgress();
}

void NetworkHostDialog::pump()
{
    while (m_scanning && m_inFlight < 32) {
        Candidate candidate;
        if (!m_neighbors.isEmpty()) {
            candidate = m_neighbors.takeLast();
        } else {
            while (m_rangeIndex < m_ranges.size() && m_ranges[m_rangeIndex].next > m_ranges[m_rangeIndex].last) ++m_rangeIndex;
            if (m_rangeIndex >= m_ranges.size()) break;
            auto &range = m_ranges[m_rangeIndex];
            candidate = {QHostAddress(quint32(range.next++)), range.source, range.label, range.interfaceIndex};
        }
        ++m_inFlight;
        const quint64 generation = m_generation;
        m_pool.start(new ProbeTask([this, candidate, generation]() {
            bool online = false, smb = false;
            if (!m_cancelled.load()) {
                online = echo(candidate.address, candidate.source);
                for (quint16 port : {quint16(445), quint16(80), quint16(443)}) {
                    if (m_cancelled.load() || (online && port != 445)) break;
                    QTcpSocket socket;
                    // 局域网探测直接使用指定网卡，系统 HTTP/SOCKS 代理不支持此处的本地绑定。
                    socket.setProxy(QNetworkProxy::NoProxy);
                    if (!socket.bind(candidate.source, 0)) continue;
                    socket.connectToHost(candidate.address, port);
                    const bool connected = socket.waitForConnected(120);
                    if (connected) { online = true; if (port == 445) smb = true; }
                    else if (socket.error() == QAbstractSocket::ConnectionRefusedError) online = true;
                }
            }
            QMetaObject::invokeMethod(this, [this, candidate, online, smb, generation]() {
                recordHost(candidate, online, smb, generation);
            }, Qt::QueuedConnection);
        }));
    }
    if (!m_inFlight) {
        m_scanning = false;
        m_scanButton->setEnabled(true);
        m_cancelButton->setEnabled(false);
        if (!m_loggedFinished) {
            m_loggedFinished = true;
            Diagnostics::event("scan.finished", {{"run", m_operationId}, {"cancelled", m_cancelled.load()},
                {"checked", double(m_completed)}, {"total", double(m_total)}, {"found", m_table->rowCount()}, {"elapsed_ms", double(m_elapsed.elapsed())}});
        }
    }
    updateProgress();
}

void NetworkHostDialog::recordHost(const Candidate &candidate, bool online, bool smb, quint64 generation)
{
    if (generation != m_generation) return;
    --m_inFlight;
    ++m_completed;
    if (online) {
        const QString ip = candidate.address.toString();
        const QString key = QString::number(candidate.interfaceIndex) + '|' + ip;
        if (!m_rows.contains(key)) {
            Diagnostics::event("scan.host_found", {{"run", m_operationId}, {"ip", ip}, {"source", candidate.source.toString()},
                {"interface", candidate.label}, {"smb", smb}});
            const int row = insertHost(candidate, smb);
            m_rows.insert(key, row);
            resolveName(row, candidate, generation);
            applyRowFilter(row);
        } else {
            const int row = m_rows.value(key);
            if (smb && !m_table->item(row, 0)->data(SmbAvailable).toBool()) {
                m_table->item(row, 0)->setData(SmbAvailable, true);
                m_table->item(row, 2)->setText(QStringLiteral("可连接"));
                applyRowFilter(row);
            }
        }
    }
    pump();
}

int NetworkHostDialog::insertHost(const Candidate &candidate, bool smb)
{
    const int row = m_table->rowCount();
    const QString ip = candidate.address.toString();
    m_table->insertRow(row);
    ++m_visibleCount;
    for (int column = 0; column < m_table->columnCount(); ++column)
        m_table->setItem(row, column, new QTableWidgetItem);
    auto *name = m_table->item(row, 0);
    name->setText(QStringLiteral("正在解析…"));
    name->setData(InterfaceIndex, candidate.interfaceIndex);
    name->setData(SmbAvailable, smb);
    name->setData(Server, uncServerForAddress(candidate.address));
    m_table->item(row, 1)->setText(ip);
    m_table->item(row, 2)->setText(smb ? QStringLiteral("可连接") : QStringLiteral("未响应"));
    m_table->item(row, 3)->setText(candidate.label + (m_localAddresses.contains(ip) ? QStringLiteral("（本机）") : QString()));
    updateConfiguredState(row);
    return row;
}

void NetworkHostDialog::resolveName(int row, const Candidate &candidate, quint64 generation)
{
    auto *item = m_table->item(row, 0);
    const QString ip = candidate.address.toString();
    if (m_lookups.size() < 64) {
        // Qt 5.12 的函数式回调会在 DNS 工作线程直接执行，使用队列槽保证表格仅由界面线程更新。
        const int lookup = QHostInfo::lookupHost(ip, this, SLOT(resolveNameResult(QHostInfo)));
        if (lookup >= 0) {
            item->setData(DnsPending, true);
            m_lookupRows.insert(lookup, row);
            m_lookups.append(lookup);
            QTimer::singleShot(5000, this, [this, lookup, row, generation]() {
                if (generation != m_generation || !m_lookups.removeOne(lookup)) return;
                m_lookupRows.remove(lookup);
                QHostInfo::abortHostLookup(lookup);
                finishNameLookup(row, DnsPending);
            });
        }
    }
    if (candidate.address.protocol() != QAbstractSocket::IPv4Protocol || m_nameSockets.size() >= 128) {
        finishNameLookup(row, NetbiosPending);
        return;
    }
    auto *socket = new QUdpSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    m_nameSockets.append(socket);
    if (!socket->bind(candidate.source, 0)) {
        m_nameSockets.removeOne(socket);
        socket->deleteLater();
        finishNameLookup(row, NetbiosPending);
        return;
    }
    item->setData(NetbiosPending, true);
    const quint16 id = quint16((row + 1) ^ (generation & 0xffff));
    connect(socket, &QUdpSocket::readyRead, this, [this, socket, row, candidate, generation, id]() {
        while (socket->hasPendingDatagrams()) {
            const qint64 size = socket->pendingDatagramSize();
            if (size < 0 || size > 4096) { socket->readDatagram(nullptr, 0); continue; }
            QByteArray data(int(size), '\0');
            QHostAddress sender;
            quint16 port = 0;
            if (socket->readDatagram(data.data(), data.size(), &sender, &port) < 0
                || sender != candidate.address || port != 137 || generation != m_generation) continue;
            const QString name = nodeName(data, id);
            if (!name.isEmpty() && row < m_table->rowCount()) {
                setResolvedName(row, name);
                Diagnostics::event("scan.host_name", {{"run", m_operationId}, {"ip", candidate.address.toString()}, {"name", name}, {"method", "netbios"}});
            }
        }
    });
    if (socket->writeDatagram(nodeStatusQuery(id), candidate.address, 137) < 0) {
        m_nameSockets.removeOne(socket);
        socket->deleteLater();
        finishNameLookup(row, NetbiosPending);
        return;
    }
    QTimer::singleShot(900, socket, [this, socket, row, generation]() {
        m_nameSockets.removeOne(socket);
        socket->deleteLater();
        if (generation == m_generation) finishNameLookup(row, NetbiosPending);
    });
}

void NetworkHostDialog::resolveNameResult(const QHostInfo &info)
{
    if (!m_lookupRows.contains(info.lookupId())) return;
    const int row = m_lookupRows.take(info.lookupId());
    m_lookups.removeOne(info.lookupId());
    if (row < 0 || row >= m_table->rowCount()) return;
    const QString name = info.hostName().trimmed();
    const QHostAddress numeric(name);
    static const QRegularExpression validName("^[A-Za-z0-9_.-]{1,253}$");
    if (info.error() == QHostInfo::NoError && numeric.isNull() && validName.match(name).hasMatch()) {
        setResolvedName(row, name);
        Diagnostics::event("scan.host_name", {{"run", m_operationId}, {"ip", m_table->item(row, 1)->text()}, {"name", name}, {"method", "dns"}});
    }
    finishNameLookup(row, DnsPending);
}

void NetworkHostDialog::finishNameLookup(int row, int pendingRole)
{
    auto *item = m_table->item(row, 0);
    if (!item) return;
    item->setData(pendingRole, false);
    if (item->data(KnownNames).toStringList().isEmpty() && !item->data(DnsPending).toBool() && !item->data(NetbiosPending).toBool()) {
        item->setText(QStringLiteral("未获取（使用 IP）"));
        applyRowFilter(row);
        updateProgress();
    }
}

void NetworkHostDialog::setResolvedName(int row, const QString &name)
{
    auto *item = m_table->item(row, 0);
    QStringList names = item->data(KnownNames).toStringList();
    if (!names.contains(name, Qt::CaseInsensitive)) names.append(name);
    item->setData(KnownNames, names);
    item->setData(Server, name);
    item->setText(name);
    updateConfiguredState(row);
    applyRowFilter(row);
    updateProgress();
}

void NetworkHostDialog::updateConfiguredState(int row)
{
    auto *item = m_table->item(row, 0);
    const QString ip = m_table->item(row, 1)->text();
    const QStringList names = item->data(KnownNames).toStringList();
    const bool configured = m_configuredServers.contains(serverKey(ip))
        || m_configuredServers.contains(serverKey(uncServerForAddress(QHostAddress(ip))))
        || std::any_of(names.cbegin(), names.cend(), [this](const QString &name) {
            return m_configuredServers.contains(serverKey(name));
        });
    if (item->data(Configured).isValid() && item->data(Configured).toBool() == configured) return;
    item->setData(Configured, configured);
    m_table->item(row, 4)->setText(configured ? QStringLiteral("已配置") : QString());
    const QBrush background = configured ? QBrush(QColor("#e8f3ff")) : QBrush();
    for (int column = 0; column < m_table->columnCount(); ++column)
        m_table->item(row, column)->setBackground(background);
}

void NetworkHostDialog::applyRowFilter(int row)
{
    const auto *item = m_table->item(row, 0);
    if (!item) return;
    const QVariant adapter = m_adapterTabs->tabData(m_adapterTabs->currentIndex());
    const QString query = m_search->text().trimmed();
    const QStringList names = item->data(KnownNames).toStringList();
    const bool pending = item->data(DnsPending).toBool() || item->data(NetbiosPending).toBool();
    const bool configured = item->data(Configured).toBool();
    const bool hidden = (adapter.isValid() && adapter.toInt() >= 0 && item->data(InterfaceIndex).toInt() != adapter.toInt())
        || (m_hideUnnamed->isChecked() && names.isEmpty() && !pending && !configured)
        || (m_onlySmb->isChecked() && !item->data(SmbAvailable).toBool())
        || (m_onlyConfigured->isChecked() && !configured)
        || (!query.isEmpty() && !m_table->item(row, 1)->text().contains(query, Qt::CaseInsensitive)
            && std::none_of(names.cbegin(), names.cend(), [&](const QString &name) {
                return name.contains(query, Qt::CaseInsensitive);
            }));
    if (hidden == m_table->isRowHidden(row)) return;
    if (hidden && m_table->currentRow() == row) {
        m_table->clearSelection();
        m_table->setCurrentItem(nullptr);
        m_selectButton->setEnabled(false);
    }
    m_table->setRowHidden(row, hidden);
    m_visibleCount += hidden ? -1 : 1;
}

void NetworkHostDialog::applyFilters()
{
    for (int row = 0; row < m_table->rowCount(); ++row) applyRowFilter(row);
    updateProgress();
}

void NetworkHostDialog::updateProgress()
{
    if (!m_elapsed.isValid()) {
        m_status->setText(QStringLiteral("等待扫描"));
        return;
    }
    const QString state = m_cancelled.load() ? QStringLiteral("已取消")
        : m_scanning ? QStringLiteral("正在扫描") : QStringLiteral("扫描完成");
    m_status->setText(QString("%1：已检查 %2 / %3 个地址，显示 %4 条，共发现 %5 条设备记录。")
        .arg(state).arg(m_completed).arg(m_total).arg(m_visibleCount).arg(m_table->rowCount()));
}
