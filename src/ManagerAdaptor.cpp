#include "ManagerAdaptor.h"

#include <QDBusObjectPath>
#include <QLocalSocket>

static const QString SYSTEMD1_SERVICE = "org.freedesktop.systemd1";
static const QString SYSTEMD1_MANAGER_PATH = "/org/freedesktop/systemd1";

static const QString STATE_IDEL = "idle";
static const QString STATE_UPGRADING = "upgrading";
static const QString STATE_FAILED = "failed";

static const QString OSTREE_REPO = "/sysroot/ostree/repo";
static const QByteArray OSTREE_DEFAULT_REMOTE_NAME = "default";

ManagerAdaptor::ManagerAdaptor(int upgradeStdoutFd, const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_server(new QLocalServer(this))
    , m_systemdManager(new org::freedesktop::systemd1::Manager(
          SYSTEMD1_SERVICE, SYSTEMD1_MANAGER_PATH, bus, this))
    , m_upgradable(false)
    , m_state(STATE_IDEL)
{
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

    auto reply = m_systemdManager->GetUnit("dum-upgrade.service");
    reply.waitForFinished();
    if (!reply.isValid()) {
        // todo: log
        throw std::runtime_error("GetUnit dum-upgrade.service failed");
    }

    auto unitPath = reply.value();
    m_dumUpgradeUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                            unitPath.path(),
                                                            QDBusConnection::systemBus());
    m_bus.connect(SYSTEMD1_SERVICE,
                  unitPath.path(),
                  "org.freedesktop.DBus.Properties",
                  "PropertiesChanged",
                  this,
                  SLOT(onDumUpgradeUnitPropertiesChanged(const QString &,
                                                         const QVariantMap &,
                                                         const QStringList &)));
}

ManagerAdaptor::~ManagerAdaptor() = default;

void ManagerAdaptor::checkUpgrade(const QDBusMessage &message)
{
    if (m_state != STATE_IDEL || m_state != STATE_FAILED) {
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

    auto remoteRefs = output.split('\n');
    if (remoteRefs.size() < 1) {
        m_bus.send(message.createErrorReply(QDBusError::InternalError, "Check upgrade failed"));
        return;
    }

    QString remoteCommit;
    const auto ref = remoteRefs.last();
    auto colonIdx = ref.indexOf(' ');
    if (colonIdx == -1) {
        return;
    }

    auto branch = ref.sliced(OSTREE_DEFAULT_REMOTE_NAME.length() + 1, // "default:"
                             colonIdx)
                      .trimmed();
    auto commit = ref.sliced(colonIdx + 1).trimmed();
    if (branch == OSTREE_DEFAULT_REMOTE_NAME) {
        remoteCommit = currentCommit;
        return;
    }

    m_upgradable = currentCommit != remoteCommit;
    emit upgradableChanged(m_upgradable);
}

void ManagerAdaptor::upgrade(const QDBusMessage &message)
{
    auto activeState = m_dumUpgradeUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        m_bus.send(message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return;
    }
    m_dumUpgradeUnit->Start("replace");
}

void ManagerAdaptor::onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
                                                       const QVariantMap &changedProperties,
                                                       const QStringList &invalidatedProperties)
{
    if (interfaceName == "org.freedesktop.systemd1.Unit") {
        auto activeState = changedProperties.value("ActiveState").toString();
        if (activeState == "active" || activeState == "activating"
            || activeState == "deactivating") {
            m_state = STATE_UPGRADING;
            emit stateChanged(m_state);
        } else if (activeState == "failed") {
            m_state = STATE_FAILED;
            emit stateChanged(m_state);
        } else {
            m_state = STATE_IDEL;
            emit stateChanged(m_state);
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
