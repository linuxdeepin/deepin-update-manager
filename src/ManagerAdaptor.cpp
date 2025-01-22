// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ManagerAdaptor.h"

#include "Branch.h"

#include <PolkitQt1/Authority>

#include <QDBusObjectPath>
#include <QLocalSocket>

static const QString SYSTEMD1_SERVICE = "org.freedesktop.systemd1";
static const QString SYSTEMD1_MANAGER_PATH = "/org/freedesktop/systemd1";

static const QString STATE_IDEL = "idle";
static const QString STATE_CHECKING = "checking";
static const QString STATE_UPGRADING = "upgrading";
static const QString STATE_FAILED = "failed";
static const QString STATE_SUCCESS = "success";

static const QString OSTREE_REPO = "/sysroot/ostree/repo";
static const QByteArray OSTREE_DEFAULT_REMOTE_NAME = "default";

static const QString ACTION_ID_CHECK_UPGRADE = "org.deepin.UpdateManager.check-upgrade";
static const QString ACTION_ID_UPGRADE = "org.deepin.UpdateManager.upgrade";

static const QString DUM_STATE_FILE = "/tmp/dum-status.ini";

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

ManagerAdaptor::ManagerAdaptor(int listRemoteRefsFd,
                               int upgradeStdoutFd,
                               const QDBusConnection &bus,
                               QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_listRemoteRefsStdoutServer(new QLocalServer(this))
    , m_upgradeStdoutServer(new QLocalServer(this))
    , m_systemdManager(new org::freedesktop::systemd1::Manager(
          SYSTEMD1_SERVICE, SYSTEMD1_MANAGER_PATH, bus, this))
    , m_dumUpgradeUnit(nullptr)
    , m_state(STATE_IDEL)
    , m_upgradable(false)
    , m_idle(new Idle)
{
    qRegisterMetaType<Progress>("Progress");
    qDBusRegisterMetaType<Progress>();

    m_settings = new QSettings(DUM_STATE_FILE, QSettings::IniFormat);
    QFile stateFile(DUM_STATE_FILE);
    if (stateFile.exists()) {
        // 如果文件存在，则说明是空闲退出且没有重启过。读取settings配置恢复之前的状态
        loadStatus();
    }
    m_listRemoteRefsStdoutServer->listen(listRemoteRefsFd);

    m_upgradeStdoutServer->listen(upgradeStdoutFd);
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

    connect(this, &ManagerAdaptor::stateChanged, this, [this](const QString &state) {
        m_settings->setValue("state", state);
        sendPropertyChanged("state", state);
    });
    connect(this, &ManagerAdaptor::upgradableChanged, this, [this](bool upgradable) {
        m_settings->setValue("upgradable", upgradable);
        sendPropertyChanged("upgradable", upgradable);
    });
}

ManagerAdaptor::~ManagerAdaptor() = default;

void ManagerAdaptor::checkUpgrade(const QDBusMessage &message)
{
    if (!checkAuthorization(ACTION_ID_CHECK_UPGRADE, message.service())) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "Not authorized"));
        return;
    }

    if (m_state == STATE_UPGRADING) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }

    QString unit = "dum-list-remote-refs.service";
    auto reply = m_systemdManager->LoadUnit(unit);
    reply.waitForFinished();
    if (!reply.isValid()) {
        m_bus.send(message.createErrorReply(QDBusError::InternalError,
                                            QString("LoadUnit %1 failed").arg(unit)));
        return;
    }

    auto unitPath = reply.value();
    auto *dumListRemoteRefsUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                                       unitPath.path(),
                                                                       QDBusConnection::systemBus(),
                                                                       this);
    auto activeState = dumListRemoteRefsUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }
    m_idle->Inhibit(STATE_CHECKING);
    auto reply1 = dumListRemoteRefsUnit->Start("replace");
    reply1.waitForFinished();
    if (reply1.isError()) {
        m_idle->UnInhibit(STATE_CHECKING);
        m_bus.send(message.createErrorReply(
            QDBusError::InternalError,
            QString("Start %1 failed: %2").arg(unit).arg(reply1.error().message())));
        return;
    }

    if (!m_listRemoteRefsStdoutServer->waitForNewConnection(5000)) {
        m_idle->UnInhibit(STATE_CHECKING);
        m_bus.send(message.createErrorReply(QDBusError::InternalError,
                                            QString("WaitForNewConnection failed")));
        return;
    }

    auto *socket = m_listRemoteRefsStdoutServer->nextPendingConnection();
    // socket->waitForReadyRead();
    socket->waitForDisconnected();
    auto output = socket->readAll();
    socket->deleteLater();
    m_idle->UnInhibit(STATE_CHECKING);

    auto remoteRefs = output.trimmed().split('\n');
    if (remoteRefs.size() < 1) {
        m_bus.send(
            message.createErrorReply(QDBusError::InternalError, "Check upgrade failed: no refs"));
        return;
    }

    Branch currentBranchInfo;
    Branch lastBranchInfo;
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

        qInfo() << "Branch: " << branch;
        if (startsWithAsterisk) {
            currentBranchInfo = branchInfo;
            continue;
        }

        if (!lastBranchInfo.valid() || lastBranchInfo.canUpgradeTo(branchInfo)) {
            lastBranchInfo = branchInfo;
            continue;
        }
    }

    qInfo() << "currentBranchInfo:" << currentBranchInfo.toString();
    qInfo() << "lastBranchInfo:" << lastBranchInfo.toString();
    if (currentBranchInfo.valid()) {
        if (!currentBranchInfo.canUpgradeTo(lastBranchInfo)) {
            lastBranchInfo = Branch();
        }
    }

    bool upgradable = lastBranchInfo.valid();
    if (upgradable) {
        m_remoteBranch = lastBranchInfo.toString();
        m_settings->setValue("remoteBranch", m_remoteBranch);
    }
    if (m_upgradable != upgradable) {
        m_upgradable = upgradable;
        emit upgradableChanged(m_upgradable);
    }
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
    if (!checkAuthorization(ACTION_ID_UPGRADE, message.service())) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "Not authorized"));
        return;
    }

   if (m_state == STATE_SUCCESS) {
        qInfo() << "Upgrade success, need reboot";
        return;
    } else if (m_state == STATE_CHECKING || m_state == STATE_UPGRADING) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }

    if (!m_upgradable) {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "No upgrade available"));
        return;
    }

    QString version = OSTREE_DEFAULT_REMOTE_NAME + ':' + m_remoteBranch;
    QString unit = QString("dum-upgrade@%1.service").arg(systemdEscape(version));

    auto reply = m_systemdManager->LoadUnit(unit);
    reply.waitForFinished();
    if (!reply.isValid()) {
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

    m_idle->Inhibit(STATE_UPGRADING);
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
        m_idle->UnInhibit(STATE_UPGRADING);
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

            m_upgradable = false;
            emit upgradableChanged(m_upgradable);
        } else if (activeState == "failed") {
            m_state = STATE_FAILED;
            emit stateChanged(m_state);
        } else if (activeState == "inactive") {
            if (m_state == STATE_UPGRADING) {
                m_state = STATE_SUCCESS;
                emit stateChanged(m_state);

                m_upgradable = false;
                emit upgradableChanged(m_upgradable);
            }
        } else {
            qWarning() << "unknown activeState:" << activeState;
        }
    }
    if (m_state == STATE_SUCCESS || m_state == STATE_FAILED) {
        m_idle->UnInhibit(STATE_UPGRADING);
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

bool ManagerAdaptor::checkAuthorization(const QString &actionId, const QString &service) const
{
    auto pid = m_bus.interface()->servicePid(service).value();
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

void ManagerAdaptor::loadStatus()
{
    if (m_settings) {
        m_state = m_settings->value("state",STATE_IDEL).toString();
        m_upgradable = m_settings->value("upgradable",false).toBool();
        m_remoteBranch = m_settings->value("remoteBranch","").toString();
    }
}