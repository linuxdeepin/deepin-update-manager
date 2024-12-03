// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ManagerAdaptor.h"

#include <QDBusObjectPath>
#include <QLocalSocket>

static const QString DBUS_PATH = "/org/freedesktop/UpdateManager1";

static const QString SYSTEMD1_SERVICE = "org.freedesktop.systemd1";
static const QString SYSTEMD1_MANAGER_PATH = "/org/freedesktop/systemd1";

static const QString STATE_IDEL = "idle";
static const QString STATE_UPGRADING = "upgrading";
static const QString STATE_FAILED = "failed";
static const QString STATE_SUCCESS = "success";

static const QString OSTREE_REPO = "/sysroot/ostree/repo";
static const QByteArray OSTREE_DEFAULT_REMOTE_NAME = "default";

QDBusArgument &operator<<(QDBusArgument &argument, const Progress &progress)
{
    argument.beginStructure();
    argument << progress.stage << progress.percent;
    argument.endStructure();

    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Progress &progress)
{
    argument.beginStructure();
    argument >> progress.stage;
    double d;
    argument >> d;
    progress.percent = d;
    argument.endStructure();

    return argument;
}

ManagerAdaptor::ManagerAdaptor(int upgradeStdoutFd, const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_server(new QLocalServer(this))
    , m_systemdManager(new org::freedesktop::systemd1::Manager(
          SYSTEMD1_SERVICE, SYSTEMD1_MANAGER_PATH, bus, this))
    , m_dumUpgradeUnit(nullptr)
    , m_upgradable(false)
    , m_state(STATE_IDEL)
{
    qRegisterMetaType<Progress>("Progress");
    qDBusRegisterMetaType<Progress>();

    m_server->listen(upgradeStdoutFd);
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        auto *socket = m_server->nextPendingConnection();
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            while (socket->canReadLine()) {
                auto line = socket->readLine();
                parseUpgradeStdoutLine(line);
            }
        });
    });

    connect(this, &ManagerAdaptor::stateChanged, this, [this](const QString &state) {
        sendPropertyChanged("state", state);
    });
    connect(this, &ManagerAdaptor::upgradableChanged, this, [this](bool upgradable) {
        sendPropertyChanged("upgradable", upgradable);
    });
}

ManagerAdaptor::~ManagerAdaptor() = default;

void ManagerAdaptor::checkUpgrade(const QDBusMessage &message)
{
    if (m_state != STATE_IDEL && m_state != STATE_FAILED) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }

    QFile file("/usr/ostree-parent");
    if (!file.open(QIODevice::ReadOnly)) {
        m_bus.send(
            message.createErrorReply(QDBusError::InternalError, "Failed to get current commit"));
        return;
    }

    auto content = file.readAll().trimmed();
    file.close();

    QString currentCommit = content.first(content.indexOf('.'));

    {
        QProcess listRemoteProcess;
        listRemoteProcess.start("/usr/bin/ostree", { "remote", "list", "--repo", OSTREE_REPO });
        listRemoteProcess.waitForFinished();
        const auto output = listRemoteProcess.readAllStandardOutput();
        if (output.isEmpty() || !output.contains(OSTREE_DEFAULT_REMOTE_NAME)) {
            m_bus.send(message.createErrorReply(QDBusError::InternalError, "No default remote"));
            return;
        }
    }

    QProcess remoteRefsProcess;
    remoteRefsProcess.start(
        "/usr/bin/ostree",
        { "remote", "refs", OSTREE_DEFAULT_REMOTE_NAME, "--repo", OSTREE_REPO, "--revision" });
    remoteRefsProcess.waitForFinished();
    const auto output = remoteRefsProcess.readAllStandardOutput();
    if (output.isEmpty()) {
        m_bus.send(message.createErrorReply(QDBusError::InternalError, "Check upgrade failed"));
        return;
    }

    auto remoteRefs = output.trimmed().split('\n');
    if (remoteRefs.size() < 1) {
        m_bus.send(
            message.createErrorReply(QDBusError::InternalError, "Check upgrade failed: no refs"));
        return;
    }

    const auto lastRef = remoteRefs.last();
    qWarning() << "lastRef" << lastRef;
    auto colonIdx = lastRef.indexOf('\t');
    if (colonIdx == -1) {
        m_bus.send(message.createErrorReply(QDBusError::InternalError,
                                            "Check upgrade failed: refs format error"));
        return;
    }

    auto branch = lastRef.first(colonIdx)
                      .sliced(OSTREE_DEFAULT_REMOTE_NAME.length() + 1) // "default:"
                      .trimmed();
    auto commit = lastRef.sliced(colonIdx + 1).trimmed();
    qWarning() << "branch: " << branch << "commit: " << commit
               << "currentCommit: " << currentCommit;
    if (currentCommit == commit) {
        if (m_upgradable == false) {
            return;
        }

        m_upgradable = false;
        emit upgradableChanged(m_upgradable);

        return;
    }

    m_remoteBranch = branch;
    if (m_upgradable == true) {
        return;
    }
    m_upgradable = true;
    emit upgradableChanged(m_upgradable);
}

static QString systemdEscape(const QString &str)
{
    auto tmp = str;
    tmp.replace('-', "\\x2d");
    tmp.replace('/', "-");

    return tmp;
}

void ManagerAdaptor::upgrade(const QDBusMessage &message)
{
    QString version = OSTREE_DEFAULT_REMOTE_NAME + ':' + m_remoteBranch;
    QString unit = QString("dum-upgrade@%1.service").arg(systemdEscape(version));

    auto reply = m_systemdManager->LoadUnit(unit);
    reply.waitForFinished();
    if (!reply.isValid()) {
        // todo: log
        m_bus.send(message.createErrorReply(QDBusError::InternalError,
                                            QString("GetUnit %1 failed").arg(unit)));
        return;
    }

    if (m_dumUpgradeUnit) {
        m_bus.disconnect(SYSTEMD1_SERVICE,
                         m_dumUpgradeUnit->path(),
                         "org.freedesktop.DBus.Properties",
                         "PropertiesChanged",
                         this,
                         SLOT(onDumUpgradeUnitPropertiesChanged(const QString &,
                                                                const QVariantMap &,
                                                                const QStringList &)));
        m_dumUpgradeUnit->deleteLater();
    }

    auto unitPath = reply.value();
    m_dumUpgradeUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                            unitPath.path(),
                                                            QDBusConnection::systemBus(),
                                                            this);

    auto activeState = m_dumUpgradeUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }

    m_bus.connect(SYSTEMD1_SERVICE,
                  m_dumUpgradeUnit->path(),
                  "org.freedesktop.DBus.Properties",
                  "PropertiesChanged",
                  this,
                  SLOT(onDumUpgradeUnitPropertiesChanged(const QString &,
                                                         const QVariantMap &,
                                                         const QStringList &)));

    auto reply1 = m_dumUpgradeUnit->Start("replace");
    reply1.waitForFinished();
    if (reply1.isError()) {
        m_bus.send(message.createErrorReply(
            QDBusError::InternalError,
            QString("Start %1 failed: %2").arg(unit).arg(reply1.error().message())));
        return;
    }
}

bool ManagerAdaptor::upgradable() const
{
    return m_upgradable;
}

QString ManagerAdaptor::state() const
{
    return m_state;
}

void ManagerAdaptor::onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                                       const QVariantMap &changedProperties,
                                                       const QStringList &invalidatedProperties)
{
    if (interfaceName == "org.freedesktop.systemd1.Unit") {
        auto activeState = changedProperties.value("ActiveState").toString();
        qWarning() << "activeState:" << activeState;
        if (activeState == "active" || activeState == "activating") {
            m_state = STATE_UPGRADING;
            emit stateChanged(m_state);
        } else if (activeState == "deactivating") {
            m_state = STATE_SUCCESS;
            emit stateChanged(m_state);
        } else if (activeState == "failed") {
            m_state = STATE_FAILED;
            emit stateChanged(m_state);
        } else if (activeState == "inactive") {
            if (m_state == STATE_UPGRADING) {
                m_state = STATE_SUCCESS;
                emit stateChanged(m_state);

                QTimer::singleShot(1000, this, [this]() {
                    if (m_state == STATE_SUCCESS) {
                        m_upgradable = false;
                        emit upgradableChanged(m_upgradable);

                        m_state = STATE_IDEL;
                        emit stateChanged(m_state);
                    };
                });
            } else if (m_state != STATE_IDEL) {
                m_upgradable = false;
                emit upgradableChanged(m_upgradable);

                m_state = STATE_IDEL;
                emit stateChanged(m_state);
            }
        } else {
            qWarning() << "unknown activeState:" << activeState;
        }
    }
}

static const QByteArray PROGRESS_PREFIX = "progressRate:";

void ManagerAdaptor::parseUpgradeStdoutLine(const QByteArray &line)
{
    if (line.startsWith(PROGRESS_PREFIX)) {
        auto tmp = QByteArrayView(line.begin() + PROGRESS_PREFIX.size(), line.end()).trimmed();
        auto colonIdx = tmp.indexOf(':');
        QString percentStr = tmp.sliced(0, colonIdx).trimmed().toByteArray();
        QString stage = tmp.sliced(colonIdx + 1).trimmed().toByteArray();

        emit progress({ stage, percentStr.toFloat() });
    }
}

void ManagerAdaptor::sendPropertyChanged(const QString &property, const QVariant &value)
{
    auto msg = QDBusMessage::createSignal(ADAPTOR_PATH,
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged");
    msg << "org.deepin.UpdateManager1";
    msg << QVariantMap{ { property, value } };
    msg << QStringList{};

    auto res = m_bus.send(msg);
    if (!res) {
        qWarning() << "sendPropertyChanged failed";
    }
}
