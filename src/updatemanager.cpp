// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updatemanager.h"

#include "Branch.h"

#include <PolkitQt1/Authority>

#include <QDBusObjectPath>
#include <QLocalSocket>

static const QString SYSTEMD1_SERVICE = "org.freedesktop.systemd1";
static const QString SYSTEMD1_MANAGER_PATH = "/org/freedesktop/systemd1";

static const QString STATE_IDEL = "idle";
static const QString STATE_UPGRADING = "upgrading";
static const QString STATE_FAILED = "failed";
static const QString STATE_SUCCESS = "success";

static const QString OSTREE_REPO = "/sysroot/ostree/repo";
static const QByteArray OSTREE_DEFAULT_REMOTE_NAME = "default";

static const QString ACTION_ID_CHECK_UPGRADE = "org.deepin.UpdateManager.check-upgrade";
static const QString ACTION_ID_UPGRADE = "org.deepin.UpdateManager.upgrade";

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

#define DUM_LIST_REMOTE_REFS_STDOUT_SOCKET "dum-list-remote-refs-stdout"
#define DUM_UPGRADE_STDOUT_SOCKET "dum-upgrade-stdout"

UpdateManager::UpdateManager(int listRemoteRefsFd, int upgradeStdoutFd,QObject *parent)
    : QObject(parent)
    , m_listRemoteRefsStdoutServer(new QLocalServer(this))
    , m_upgradeStdoutServer(new QLocalServer(this))
    , m_dumUpgradeUnit(nullptr)
    , m_state(STATE_IDEL)
    , m_idle(Idle())
    , m_checker(nullptr)
    , m_downloader(nullptr)
    , m_installer(nullptr)
{
    qRegisterMetaType<Progress>("Progress");
    qDBusRegisterMetaType<Progress>();

    m_systemdManager = new org::freedesktop::systemd1::Manager(SYSTEMD1_SERVICE, SYSTEMD1_MANAGER_PATH, QDBusConnection::systemBus(), this);
    auto ok = m_listRemoteRefsStdoutServer->listen(listRemoteRefsFd);
    if (!ok) {
        qWarning() << "Failed to list Remote References." << m_listRemoteRefsStdoutServer->errorString();
    }
    ok = m_upgradeStdoutServer->listen(upgradeStdoutFd);
    if (!ok) {
        qWarning() << "Failed to list Upgrade References." << m_listRemoteRefsStdoutServer->errorString();
    }

    connect(m_upgradeStdoutServer, &QLocalServer::newConnection, this, [this] {
        auto *socket = m_upgradeStdoutServer->nextPendingConnection();
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            while (socket->canReadLine()) {
                auto line = socket->readLine();
                parseUpgradeStdoutLine(line);
            }
        });
    });
}

UpdateManager::~UpdateManager() = default;

void UpdateManager::setPropertyUpgradable(bool upgradable)
{
    if (m_upgradable != upgradable) {
        m_upgradable = upgradable;
        sendPropertyChanged("upgradable",m_upgradable);
    }
}

void UpdateManager::setPropertyState(const QString &state)
{
    if (m_state != state) {
        m_state = state;
        sendPropertyChanged("state",m_state);
    }
}

void UpdateManager::setPropertyCurrentBranch(const QString &branch)
{
    m_currentBranch = branch;
    sendPropertyChanged("currentBranch",m_currentBranch);
}

void UpdateManager::setPropertyAllBranches(const QStringList &branches)
{
    m_allBranches = branches;
    sendPropertyChanged("allBranches", m_allBranches);
}

void UpdateManager::setCheckUpdateMode(uint16_t checkUpdateMode) { }

void UpdateManager::setUpgradeMode(uint16_t upgradeMode) { }

void UpdateManager::checkUpgrade()
{
    if (!checkAuthorization(ACTION_ID_CHECK_UPGRADE, message().service())) {
        sendErrorReply(QDBusError::AccessDenied, "Not authorized");
        return;
    }

    if (m_state != STATE_IDEL && m_state != STATE_FAILED) {
       sendErrorReply(QDBusError::AccessDenied, "An upgrade is in progress");
        return;
    }

    QString unit = "dum-list-remote-refs.service";
    auto reply = m_systemdManager->LoadUnit(unit);
    reply.waitForFinished();
    if (!reply.isValid()) {
        sendErrorReply(QDBusError::InternalError,QString("LoadUnit %1 failed").arg(unit));
        return;
    }

    auto unitPath = reply.value();
    auto *dumListRemoteRefsUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                                       unitPath.path(),
                                                                       QDBusConnection::systemBus(),
                                                                       this);
    auto activeState = dumListRemoteRefsUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        sendErrorReply(QDBusError::AccessDenied, "An upgrade is in progress");
        return;
    }

    auto reply1 = dumListRemoteRefsUnit->Start("replace");
    reply1.waitForFinished();
    if (reply1.isError()) {
        sendErrorReply(
            QDBusError::InternalError,
            QString("Start %1 failed: %2").arg(unit).arg(reply1.error().message()));
        return;
    }

    if (!m_listRemoteRefsStdoutServer->waitForNewConnection(5000)) {
        sendErrorReply(QDBusError::InternalError,QString("WaitForNewConnection failed"));
        return;
    }

    auto *socket = m_listRemoteRefsStdoutServer->nextPendingConnection();
    // socket->waitForReadyRead();
    socket->waitForDisconnected();
    auto output = socket->readAll();
    socket->deleteLater();

    auto remoteRefs = output.trimmed().split('\n');
    if (remoteRefs.size() < 1) {
        sendErrorReply(QDBusError::InternalError, "Check upgrade failed: no refs");
        return;
    }

    Branch currentBranchInfo;
    Branch lastBranchInfo;
    QStringList branches;
    for (auto ref : remoteRefs) {
        bool startsWithAsterisk = ref.startsWith('*');
        if (startsWithAsterisk) {
            ref.remove(0, 1);
            ref = ref.trimmed();
        }

        auto colonIdx = ref.indexOf(' ');
        if (colonIdx == -1) {
            qWarning() << "Invalid ref: " << ref;
            continue;
        }

        auto branch = ref.first(colonIdx).trimmed();
        if (!branch.startsWith("default:")) {
            qWarning() << "Invalid branch: " << branch;
            continue;
        }
        branch = branch.sliced(OSTREE_DEFAULT_REMOTE_NAME.length() + 1).trimmed(); // "default:"
        auto commit = ref.sliced(colonIdx + 1).trimmed();

        Branch branchInfo(branch);

        if (!branchInfo.valid()) {
            qWarning() << "Invalid branch: " << branch;
            continue;
        }
        branches.push_back(branchInfo.toString());
        qInfo() << "Branch: " << branch;
        if (startsWithAsterisk) {
            currentBranchInfo = branchInfo;
        }

        if (!lastBranchInfo.valid() || lastBranchInfo.canUpgradeTo(branchInfo)) {
            lastBranchInfo = branchInfo;
        }
    }

    qInfo() << "currentBranchInfo:" << currentBranchInfo.toString();
    qInfo() << "lastBranchInfo:" << lastBranchInfo.toString();
    setPropertyCurrentBranch(currentBranchInfo.toString());
    setPropertyAllBranches(branches);
    if (currentBranchInfo.valid()) {
        if (!currentBranchInfo.canUpgradeTo(lastBranchInfo)) {
            lastBranchInfo = Branch();
        }
    }

    bool upgradable = lastBranchInfo.valid();
    if (upgradable) {
        m_remoteBranch = lastBranchInfo.toString();
    }
    setPropertyUpgradable(upgradable);
}

static QString systemdEscape(const QString &str)
{
    auto tmp = str;
    tmp.replace('-', "\\x2d");
    tmp.replace('/', "-");

    return tmp;
}

void UpdateManager::upgrade()
{
    if (!checkAuthorization(ACTION_ID_UPGRADE, message().service())) {
        sendErrorReply(QDBusError::AccessDenied, "Not authorized");
        return;
    }

    if (m_state != STATE_IDEL && m_state != STATE_FAILED) {
        sendErrorReply(QDBusError::AccessDenied, "An upgrade is in progress");
        return;
    }

    if (!m_upgradable) {
        sendErrorReply(QDBusError::AccessDenied, "No upgrade available");
        return;
    }

    QString version = OSTREE_DEFAULT_REMOTE_NAME + ':' + m_remoteBranch;
    QString unit = QString("dum-upgrade@%1.service").arg(systemdEscape(version));

    auto reply = m_systemdManager->LoadUnit(unit);
    reply.waitForFinished();
    if (!reply.isValid()) {
        sendErrorReply(QDBusError::InternalError,QString("GetUnit %1 failed").arg(unit));
        return;
    }

    if (m_dumUpgradeUnit) {
        connection().disconnect(SYSTEMD1_SERVICE,
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
        sendErrorReply(QDBusError::AccessDenied, "An upgrade is in progress");
        return;
    }

    connection().connect(SYSTEMD1_SERVICE,
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
        sendErrorReply(QDBusError::InternalError,QString("Start %1 failed: %2").arg(unit).arg(reply1.error().message()));
        return;
    }
}

bool UpdateManager::upgradable() const
{
    return m_upgradable;
}

QString UpdateManager::state() const
{
    return m_state;
}

QString UpdateManager::currentBranch() const
{
    return m_currentBranch;
}

QStringList UpdateManager::allBranches() const
{
    return m_allBranches;
}

uint16_t UpdateManager::checkUpdateMode() const
{
    return m_checkUpdateMode;
}

uint16_t UpdateManager::upgradeMode() const
{
    return m_upgradeMode;
}

void UpdateManager::onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                                       const QVariantMap &changedProperties,
                                                       const QStringList &invalidatedProperties)
{
    if (interfaceName == "org.freedesktop.systemd1.Unit") {
        auto activeState = changedProperties.value("ActiveState").toString();
        qWarning() << "activeState:" << activeState;
        if (activeState == "active" || activeState == "activating") {
            setPropertyState(STATE_UPGRADING);
        } else if (activeState == "deactivating") {
            setPropertyState(STATE_SUCCESS);
            setPropertyUpgradable(false);
        } else if (activeState == "failed") {
            setPropertyState(STATE_FAILED);
        } else if (activeState == "inactive") {
            if (m_state == STATE_UPGRADING) {
                setPropertyState(STATE_SUCCESS);
                setPropertyUpgradable(false);
            }
        } else {
            qWarning() << "unknown activeState:" << activeState;
        }
    }
}

static const QByteArray PROGRESS_PREFIX = "progressRate:";

void UpdateManager::parseUpgradeStdoutLine(const QByteArray &line)
{
    if (line.startsWith(PROGRESS_PREFIX)) {
        auto tmp = QByteArrayView(line.begin() + PROGRESS_PREFIX.size(), line.end()).trimmed();
        auto colonIdx = tmp.indexOf(':');
        QString percentStr = tmp.sliced(0, colonIdx).trimmed().toByteArray();
        QString stage = tmp.sliced(colonIdx + 1).trimmed().toByteArray();

        emit progress({ stage, percentStr.toFloat() });
    }
}

void UpdateManager::sendPropertyChanged(const QString &property, const QVariant &value)
{
    auto msg = QDBusMessage::createSignal(DBUS_PATH,
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged");
    msg << DBUS_SERVICE_NAME;
    msg << QVariantMap{ { property, value } };
    msg << QStringList{};

    auto res = connection().send(msg);
    if (!res) {
        qWarning() << "sendPropertyChanged failed";
    }
}

bool UpdateManager::checkAuthorization(const QString &actionId, const QString &service) const
{
    auto pid = connection().interface()->servicePid(service).value();
    auto authority = PolkitQt1::Authority::instance();
    auto result = authority->checkAuthorizationSync(actionId,
                                                    PolkitQt1::UnixProcessSubject(pid),
                                                    PolkitQt1::Authority::AllowUserInteraction);
    if (authority->hasError()) {
        qWarning() << "checkAuthorizationSync failed:" << authority->lastError()
                   << authority->errorDetails();
        return false;
    }

    return result == PolkitQt1::Authority::Result::Yes;
}
