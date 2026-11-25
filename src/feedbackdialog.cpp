#include "feedbackdialog.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {
constexpr int imageLimit = 10 * 1024 * 1024;

QString draftPath()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(root).filePath(QStringLiteral("RemoteReleaseSync/Feedback/Drafts/current.json"));
}

QByteArray credential(const QString &key)
{
#ifdef Q_OS_WIN
    PCREDENTIALW value = nullptr;
    if (CredReadW(reinterpret_cast<LPCWSTR>(key.utf16()), CRED_TYPE_GENERIC, 0, &value)) {
        const QByteArray token(reinterpret_cast<const char *>(value->CredentialBlob), int(value->CredentialBlobSize));
        CredFree(value);
        return token;
    }
#else
    Q_UNUSED(key);
#endif
    return {};
}

bool saveCredential(const QString &key, const QByteArray &token)
{
#ifdef Q_OS_WIN
    CREDENTIALW value{};
    value.Type = CRED_TYPE_GENERIC;
    value.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(key.utf16()));
    value.CredentialBlobSize = DWORD(token.size());
    value.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(token.constData()));
    value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&value, 0) != FALSE;
#else
    Q_UNUSED(key);
    Q_UNUSED(token);
    return false;
#endif
}

QByteArray deviceToken()
{
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    return QByteArray(reinterpret_cast<const char *>(words), sizeof(words)).toHex();
}

QUrl feedbackUrl(const QString &text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    const QString host = url.host().toLower();
    const bool local = host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1");
    const bool validScheme = url.scheme() == QStringLiteral("https") || (local && url.scheme() == QStringLiteral("http"));
    if (!url.isValid() || !validScheme || host.isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) {
        return {};
    }
    return url;
}

QString statusText(const QString &status)
{
    static const QHash<QString, QString> names{
        {"received", QStringLiteral("已收到")}, {"analysing", QStringLiteral("分析中")},
        {"needs_input", QStringLiteral("待你补充")}, {"answered", QStringLiteral("已有回复")},
        {"awaiting_decision", QStringLiteral("待维护者决定")}, {"deferred", QStringLiteral("已暂缓")},
        {"declined", QStringLiteral("未采纳")}, {"developing", QStringLiteral("修复中")},
        {"tracking", QStringLiteral("跟进同类问题")},
        {"awaiting_publish", QStringLiteral("等待发布确认")}, {"ready_to_publish", QStringLiteral("待发布")},
        {"published", QStringLiteral("已发布，待验证")}, {"resolved", QStringLiteral("已解决")},
        {"blocked", QStringLiteral("处理中断")}};
    return names.value(status, status);
}
}

FeedbackDialog::FeedbackDialog(QWidget *parent) : QDialog(parent)
{
    setProperty("diagnosticsPrivate", true);
    setWindowTitle(QStringLiteral("反馈中心"));
    resize(760, 690);
    setAcceptDrops(true);
    auto *layout = new QVBoxLayout(this);
    auto *connection = new QFormLayout;
    m_service = new QLineEdit(this);
    m_service->setObjectName(QStringLiteral("feedbackService"));
    m_service->setText(QSettings().value(QStringLiteral("feedback/service")).toString());
    m_service->setPlaceholderText(QStringLiteral("https://反馈服务地址"));
    connection->addRow(QStringLiteral("反馈服务"), m_service);
    auto *activation = new QHBoxLayout;
    m_invite = new QLineEdit(this);
    m_invite->setObjectName(QStringLiteral("feedbackInvite"));
    m_invite->setEchoMode(QLineEdit::Password);
    m_invite->setPlaceholderText(QStringLiteral("由维护者提供，一次使用"));
    m_activate = new QPushButton(QStringLiteral("启用反馈"), this);
    m_activate->setObjectName(QStringLiteral("feedbackActivate"));
    activation->addWidget(m_invite, 1);
    activation->addWidget(m_activate);
    connection->addRow(QStringLiteral("邀请码"), activation);
    layout->addLayout(connection);

    auto *reportBar = new QHBoxLayout;
    reportBar->addWidget(new QLabel(QStringLiteral("我的反馈"), this), 1);
    auto *newReport = new QPushButton(QStringLiteral("新反馈"), this);
    m_refresh = new QPushButton(QStringLiteral("刷新"), this);
    reportBar->addWidget(newReport);
    reportBar->addWidget(m_refresh);
    layout->addLayout(reportBar);
    m_reports = new QTreeWidget(this);
    m_reports->setHeaderLabels({QStringLiteral("标题"), QStringLiteral("状态")});
    m_reports->setRootIsDecorated(false);
    layout->addWidget(m_reports, 1);
    m_thread = new QPlainTextEdit(this);
    m_thread->setObjectName(QStringLiteral("feedbackConversation"));
    m_thread->setReadOnly(true);
    m_thread->setPlaceholderText(QStringLiteral("双击一项反馈打开会话。"));
    layout->addWidget(m_thread, 1);
    m_received = new QListWidget(this);
    m_received->setMaximumHeight(88);
    m_received->hide();
    m_received->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_received);
    connect(m_received, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        openAttachment(item->data(Qt::UserRole).toString());
    });
    connect(m_received, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const auto *item = m_received->itemAt(position);
        if (!item || !item->data(Qt::UserRole + 1).toBool() || m_busy) return;
        const QString id = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        auto *revoke = menu.addAction(QStringLiteral("关闭此图片的后续 AI 访问"));
        if (menu.exec(m_received->viewport()->mapToGlobal(position)) == revoke) {
            const QJsonObject payload;
            request(QStringLiteral("attachments/") + id + QStringLiteral("/revoke-ai"), &payload,
                    [this](const QJsonObject &) { showReport(m_report); });
        }
    });

    auto *compose = new QHBoxLayout;
    m_type = new QComboBox(this);
    m_type->addItem(QStringLiteral("问题"), QStringLiteral("bug"));
    m_type->addItem(QStringLiteral("需求"), QStringLiteral("feature"));
    m_title = new QLineEdit(this);
    m_title->setObjectName(QStringLiteral("feedbackTitle"));
    m_title->setMaxLength(120);
    m_title->setPlaceholderText(QStringLiteral("反馈标题"));
    compose->addWidget(m_type);
    compose->addWidget(m_title, 1);
    layout->addLayout(compose);
    m_body = new QPlainTextEdit(this);
    m_body->setObjectName(QStringLiteral("feedbackBody"));
    m_body->setPlaceholderText(QStringLiteral("描述实际现象、预期表现和复现步骤。"));
    layout->addWidget(m_body, 1);
    m_diagnostics = new QCheckBox(QStringLiteral("附上版本、Windows 版本和架构"), this);
    layout->addWidget(m_diagnostics);
    m_attachments = new QListWidget(this);
    m_attachments->setMaximumHeight(100);
    m_attachments->hide();
    layout->addWidget(m_attachments);
    auto *fileBar = new QHBoxLayout;
    auto *browse = new QPushButton(QStringLiteral("选照片或截图"), this);
    auto *paste = new QPushButton(QStringLiteral("粘贴截图"), this);
    paste->setObjectName(QStringLiteral("feedbackPaste"));
    auto *remove = new QPushButton(QStringLiteral("移除选中图片"), this);
    auto *clear = new QPushButton(QStringLiteral("清空草稿"), this);
    m_submit = new QPushButton(QStringLiteral("发送"), this);
    m_submit->setObjectName(QStringLiteral("feedbackSubmit"));
    fileBar->addWidget(browse);
    fileBar->addWidget(paste);
    fileBar->addWidget(remove);
    fileBar->addWidget(clear);
    fileBar->addStretch();
    fileBar->addWidget(m_submit);
    layout->addLayout(fileBar);
    auto *verify = new QHBoxLayout;
    m_resolved = new QPushButton(QStringLiteral("已解决"), this);
    m_reopen = new QPushButton(QStringLiteral("问题仍在"), this);
    m_resolved->hide();
    m_reopen->hide();
    verify->addWidget(m_resolved);
    verify->addWidget(m_reopen);
    verify->addStretch();
    layout->addLayout(verify);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_endpoint = feedbackUrl(m_service->text());
    if (!m_endpoint.isEmpty()) {
        m_token = credential(credentialKey());
    }
    restoreDraft();

    connect(m_activate, &QPushButton::clicked, this, &FeedbackDialog::activate);
    connect(m_refresh, &QPushButton::clicked, this, &FeedbackDialog::refresh);
    connect(m_submit, &QPushButton::clicked, this, &FeedbackDialog::submit);
    connect(m_resolved, &QPushButton::clicked, this, [this] { markResolution(true); });
    connect(m_reopen, &QPushButton::clicked, this, [this] { markResolution(false); });
    connect(m_reports, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (m_body->toPlainText().trimmed().isEmpty() && m_files.isEmpty() && m_pending.isEmpty()) {
            showReport(item->data(0, Qt::UserRole).toString());
        } else {
            m_status->setText(QStringLiteral("请先发送当前草稿。"));
        }
    });
    connect(newReport, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_pending.isEmpty() || !m_body->toPlainText().trimmed().isEmpty() || !m_files.isEmpty()) {
            m_status->setText(QStringLiteral("请先发送当前草稿。"));
            return;
        }
        m_report.clear();
        m_title->show();
        m_type->show();
        m_thread->clear();
        m_received->clear();
        m_received->hide();
        m_resolved->hide();
        m_reopen->hide();
        saveDraft();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_pending.isEmpty()) return;
        const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择图片"), {}, QStringLiteral("图片 (*.png *.jpg *.jpeg)"));
        for (const QString &name : files) attachFile(name);
    });
    connect(paste, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_pending.isEmpty()) return;
        const QImage image = QApplication::clipboard()->image();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            m_status->setText(QStringLiteral("剪贴板中没有可用图片。"));
            return;
        }
        addAttachment(QStringLiteral("截图.png"), bytes);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_pending.isEmpty()) return;
        const int row = m_attachments->currentRow();
        if (row >= 0) {
            m_files.removeAt(row);
            updateAttachments();
            saveDraft();
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (m_busy || QMessageBox::question(this, QStringLiteral("清空草稿"),
                QStringLiteral("删除这份本机草稿和待传图片副本？"), QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) return;
        m_pending = {};
        m_files = {};
        m_title->clear();
        m_body->clear();
        m_diagnostics->setChecked(false);
        updateAttachments();
        setBusy(false);
        saveDraft();
    });
    connect(m_attachments, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const int row = m_attachments->row(item);
        QJsonObject file = m_files.at(row).toObject();
        file["allow_ai"] = item->checkState() == Qt::Checked;
        m_files.replace(row, file);
        saveDraft();
    });
    connect(m_attachments, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        QImage image;
        image.loadFromData(QByteArray::fromBase64(m_files.at(m_attachments->row(item)).toObject()["data"].toString().toLatin1()));
        preview(image);
    });
    connect(m_body, &QPlainTextEdit::textChanged, this, &FeedbackDialog::saveDraft);
    connect(m_title, &QLineEdit::textChanged, this, &FeedbackDialog::saveDraft);
    connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this, &FeedbackDialog::saveDraft);
    connect(m_diagnostics, &QCheckBox::toggled, this, &FeedbackDialog::saveDraft);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { if (!m_busy && !m_token.isEmpty()) refresh(); });
    timer->start(120000);
    if (!m_token.isEmpty()) QTimer::singleShot(0, this, &FeedbackDialog::refresh);
}

QString FeedbackDialog::credentialKey() const
{
    QUrl origin = m_endpoint;
    origin.setPath({});
    return QStringLiteral("RemoteReleaseSync.Feedback.")
        + QString::fromLatin1(QCryptographicHash::hash(origin.toEncoded(), QCryptographicHash::Sha256).toHex());
}

void FeedbackDialog::request(const QString &route, const QJsonObject *payload, Response handler, bool authenticated)
{
    if (m_busy || m_endpoint.isEmpty() || (authenticated && m_token.isEmpty())) {
        if (!m_busy) m_status->setText(QStringLiteral("请先配置反馈地址并启用反馈。"));
        return;
    }
    QUrl url = m_endpoint;
    url.setPath(QStringLiteral("/v1/") + route);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (authenticated) request.setRawHeader("Authorization", "Bearer " + m_token);
    auto *reply = payload ? m_network.post(request, QJsonDocument(*payload).toJson(QJsonDocument::Compact)) : m_network.get(request);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(90000);
    setBusy(true);
    m_status->setText(QStringLiteral("正在连接反馈服务…"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, handler = std::move(handler)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument result = QJsonDocument::fromJson(reply->readAll());
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 && result.isObject();
        reply->deleteLater();
        setBusy(false);
        if (!ok) {
            m_status->setText(status ? QStringLiteral("反馈请求未成功（HTTP %1）。草稿会保留，可重试。").arg(status)
                                     : QStringLiteral("无法连接反馈服务。草稿会保留，可在联网后重试。"));
            return;
        }
        m_status->clear();
        handler(result.object());
    });
}

void FeedbackDialog::activate()
{
    const QUrl selected = feedbackUrl(m_service->text());
    if (selected.isEmpty()) {
        m_status->setText(QStringLiteral("请输入 HTTPS 反馈地址。仅本机测试可使用 HTTP。"));
        return;
    }
    if (selected != m_endpoint) {
        if (!m_endpoint.isEmpty() && (!m_files.isEmpty() || !m_body->toPlainText().trimmed().isEmpty()
                || !m_title->text().trimmed().isEmpty())) {
            m_status->setText(QStringLiteral("请先发送或清空原服务的草稿，再切换反馈地址。"));
            return;
        }
        m_activationToken.clear();
        m_token.clear();
        m_report.clear();
        m_thread->clear();
        m_reports->clear();
        m_received->clear();
        m_received->hide();
        m_title->show();
        m_type->show();
    }
    m_endpoint = selected;
    if (m_invite->text().trimmed().isEmpty()) {
        m_token = credential(credentialKey());
        if (!m_token.isEmpty()) refresh();
        else m_status->setText(QStringLiteral("请输入邀请码。"));
        return;
    }
    if (m_activationToken.isEmpty()) m_activationToken = deviceToken();
    const QJsonObject payload{{"invite", m_invite->text().trimmed()}, {"token", QString::fromLatin1(m_activationToken)}};
    request(QStringLiteral("activate"), &payload, [this](const QJsonObject &) {
        m_token = m_activationToken;
        m_activationToken.clear();
        m_invite->clear();
        QSettings().setValue(QStringLiteral("feedback/service"), m_endpoint.toString());
        const bool saved = saveCredential(credentialKey(), m_token);
        refresh();
        if (!saved) m_status->setText(QStringLiteral("本次已启用，但 Windows 凭据保存失败。"));
    }, false);
}

void FeedbackDialog::refresh()
{
    request(QStringLiteral("reports"), nullptr, [this](const QJsonObject &result) {
        m_reports->clear();
        int unread = 0;
        QSettings settings;
        for (const QJsonValue &value : result["reports"].toArray()) {
            const QJsonObject report = value.toObject();
            const QString id = report["id"].toString();
            const QString key = QStringLiteral("feedback/read/") + id;
            const qint64 updated = report["updated"].toVariant().toLongLong();
            if (isVisible() && id == m_report) settings.setValue(key, updated);
            const bool newActivity = updated > settings.value(key).toLongLong();
            if (newActivity) ++unread;
            auto *item = new QTreeWidgetItem(m_reports, {(newActivity ? QStringLiteral("未读 · ") : QString()) + report["title"].toString(),
                                                       statusText(report["status"].toString())});
            item->setData(0, Qt::UserRole, id);
        }
        emit unreadChanged(unread);
        if (isVisible() && !m_report.isEmpty()) showReport(m_report);
    });
}

void FeedbackDialog::showReport(const QString &report)
{
    if (report.isEmpty()) return;
    request(QStringLiteral("reports/") + report, nullptr, [this, report](const QJsonObject &result) {
        m_report = report;
        m_title->hide();
        m_type->hide();
        const QString status = result["status"].toString();
        m_resolved->setVisible(status == QStringLiteral("published"));
        m_reopen->setVisible(status == QStringLiteral("published") || status == QStringLiteral("resolved"));
        QStringList lines{result["title"].toString(), report + QStringLiteral("  ·  ") + statusText(status), {}};
        m_received->clear();
        QSettings().setValue(QStringLiteral("feedback/read/") + report, result["updated"].toVariant().toLongLong());
        for (const QJsonValue &value : result["messages"].toArray()) {
            const QJsonObject message = value.toObject();
            const QString sender = message["sender"].toString();
            lines << (sender == QStringLiteral("user") ? QStringLiteral("你") : sender == QStringLiteral("ai") ? QStringLiteral("AI") : QStringLiteral("维护方"));
            lines << message["body"].toString();
            for (const QJsonValue &file : message["attachments"].toArray()) {
                auto *item = new QListWidgetItem(QStringLiteral("双击查看附件  %1").arg(file.toObject()["name"].toString()), m_received);
                item->setData(Qt::UserRole, file.toObject()["id"].toString());
                item->setData(Qt::UserRole + 1, file.toObject()["ai"].toInt() == 1);
            }
            lines << QString();
        }
        m_thread->setPlainText(lines.join(QLatin1Char('\n')));
        m_received->setVisible(m_received->count() > 0);
        saveDraft();
    });
}

void FeedbackDialog::submit()
{
    if (m_busy || m_token.isEmpty()) {
        m_status->setText(QStringLiteral("请先启用反馈。"));
        return;
    }
    if (m_pending.isEmpty()) {
        QString body = m_body->toPlainText().trimmed();
        if (body.isEmpty() || body.size() > 8000 || (m_report.isEmpty() && m_title->text().trimmed().isEmpty())) {
            m_status->setText(QStringLiteral("请填写标题和正文；正文最多 8000 字。"));
            return;
        }
        if (m_diagnostics->isChecked()) {
            body += QStringLiteral("\n\n版本 %1，%2（%3）").arg(QCoreApplication::applicationVersion(),
                                            QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
        }
        if (body.size() > 8000) {
            m_status->setText(QStringLiteral("正文和诊断信息总计最多 8000 字。"));
            return;
        }
        m_pending = {{"request", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"body", body}, {"attachments", m_files}};
        if (m_report.isEmpty()) {
            m_pending["title"] = m_title->text().trimmed();
            m_pending["kind"] = m_type->currentData().toString();
            m_pending["version"] = QCoreApplication::applicationVersion();
        }
        saveDraft();
    }
    const QString route = m_report.isEmpty() ? QStringLiteral("reports") : QStringLiteral("reports/") + m_report + QStringLiteral("/messages");
    request(route, &m_pending, [this](const QJsonObject &result) {
        m_pending = {};
        m_files = {};
        m_report = result["id"].toString();
        m_body->clear();
        m_title->clear();
        m_diagnostics->setChecked(false);
        updateAttachments();
        setBusy(false);
        saveDraft();
        refresh();
    });
}

void FeedbackDialog::saveDraft() const
{
    const QString path = draftPath();
    if (m_files.isEmpty() && m_pending.isEmpty() && m_title->text().isEmpty() && m_body->toPlainText().isEmpty()) {
        QFile::remove(path);
        return;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    const QJsonObject draft{{"title", m_title->text()}, {"body", m_body->toPlainText()}, {"type", m_type->currentIndex()},
                            {"report", m_report}, {"attachments", m_files}, {"pending", m_pending}, {"endpoint", m_endpoint.toString()},
                            {"diagnostics", m_diagnostics->isChecked()}};
    QSaveFile file(path);
    const QByteArray encoded = QJsonDocument(draft).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size()) file.commit();
}

void FeedbackDialog::restoreDraft()
{
    QFile file(draftPath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 150 * 1024 * 1024) return;
    const QJsonObject draft = QJsonDocument::fromJson(file.readAll()).object();
    if (draft.isEmpty()) return;
    m_report = draft["report"].toString();
    m_title->setText(draft["title"].toString());
    m_body->setPlainText(draft["body"].toString());
    m_diagnostics->setChecked(draft["diagnostics"].toBool());
    m_type->setCurrentIndex(qBound(0, draft["type"].toInt(), m_type->count() - 1));
    m_files = draft["attachments"].toArray();
    if (draft["endpoint"].toString() == m_endpoint.toString()) m_pending = draft["pending"].toObject();
    m_title->setVisible(m_report.isEmpty());
    m_type->setVisible(m_report.isEmpty());
    updateAttachments();
    setBusy(false);
}

void FeedbackDialog::updateAttachments()
{
    const QSignalBlocker block(m_attachments);
    m_attachments->clear();
    for (const QJsonValue &value : m_files) {
        const QJsonObject file = value.toObject();
        auto *item = new QListWidgetItem(QStringLiteral("允许 AI 分析  ·  %1").arg(file["name"].toString()), m_attachments);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(file["allow_ai"].toBool() ? Qt::Checked : Qt::Unchecked);
    }
    m_attachments->setVisible(!m_files.isEmpty());
}

void FeedbackDialog::addAttachment(const QString &name, const QByteArray &bytes)
{
    if (m_files.size() >= 5 || bytes.isEmpty() || bytes.size() > imageLimit) {
        m_status->setText(QStringLiteral("每条消息最多 5 张图片，单张不超过 10 MB。"));
        return;
    }
    m_files.append(QJsonObject{{"name", name}, {"data", QString::fromLatin1(bytes.toBase64())}, {"allow_ai", false}});
    updateAttachments();
    saveDraft();
}

void FeedbackDialog::attachFile(const QString &path)
{
    QFile file(path);
    const QFileInfo details(file);
    if (!details.isFile() || file.size() > imageLimit || !file.open(QIODevice::ReadOnly)) {
        m_status->setText(QStringLiteral("图片无法读取或超过 10 MB。"));
        return;
    }
    addAttachment(details.fileName(), file.read(imageLimit + 1));
}

void FeedbackDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (!m_busy && m_pending.isEmpty() && event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void FeedbackDialog::dropEvent(QDropEvent *event)
{
    if (m_busy || !m_pending.isEmpty()) return;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) attachFile(url.toLocalFile());
    }
    event->acceptProposedAction();
}

void FeedbackDialog::preview(const QImage &image)
{
    if (image.isNull()) {
        m_status->setText(QStringLiteral("图片无法预览。"));
        return;
    }
    QDialog window(this);
    window.setWindowTitle(QStringLiteral("附件预览"));
    auto *box = new QVBoxLayout(&window);
    auto *label = new QLabel(&window);
    label->setPixmap(QPixmap::fromImage(image.scaled(760, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    box->addWidget(label);
    window.exec();
}

void FeedbackDialog::openAttachment(const QString &attachment)
{
    if (m_busy || m_token.isEmpty() || attachment.isEmpty()) return;
    QUrl url = m_endpoint;
    url.setPath(QStringLiteral("/v1/attachments/") + attachment);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    auto *reply = m_network.get(request);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(45000);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > imageLimit) reply->abort();
    });
    setBusy(true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        QImage image;
        if (reply->error() == QNetworkReply::NoError) image.loadFromData(reply->read(imageLimit));
        reply->deleteLater();
        setBusy(false);
        preview(image);
    });
}

void FeedbackDialog::markResolution(bool resolved)
{
    if (m_report.isEmpty()) return;
    const QJsonObject payload{{"resolved", resolved}};
    request(QStringLiteral("reports/") + m_report + QStringLiteral("/resolution"), &payload,
            [this](const QJsonObject &) { refresh(); });
}

void FeedbackDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_activate->setEnabled(!busy && m_pending.isEmpty());
    m_service->setEnabled(!busy && m_pending.isEmpty());
    m_refresh->setEnabled(!busy);
    m_submit->setEnabled(!busy);
    m_submit->setText(m_pending.isEmpty() ? QStringLiteral("发送") : QStringLiteral("重试发送"));
    m_body->setReadOnly(busy || !m_pending.isEmpty());
    m_title->setReadOnly(busy || !m_pending.isEmpty());
    m_type->setEnabled(!busy && m_pending.isEmpty());
    m_diagnostics->setEnabled(!busy && m_pending.isEmpty());
    m_attachments->setEnabled(!busy && m_pending.isEmpty());
}
