// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upgradetask.h"
#include "SystemdUnitInterface.h"

#include <QLocalSocket>

UpgradeTask::UpgradeTask(const QDBusConnection &bus, const QDBusMessage &message, QObject *parent)
    : AbstractTask{bus, parent}
    , m_upgradeStdoutServer(new QLocalServer(this))
    , m_message(message)
    , m_unitName(QString())
    , m_fd(-1)
    , m_state(STATE_IDEL)
{

}

UpgradeTask::~UpgradeTask()
{
}

void UpgradeTask::setTaskDate(const QString &unitName, int fd)
{
    m_unitName = unitName;
    m_fd = fd;

    m_upgradeStdoutServer->listen(m_fd);
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

bool UpgradeTask::run()
{
    if (m_unitName.isEmpty() || m_fd == -1)
        return false;

    qDebug() << "upgrade task run...";
    if (!checkAuthorization(ACTION_ID_UPGRADE, m_message.service())) {
        m_bus.send(m_message.createErrorReply(QDBusError::AccessDenied, "Not authorized"));
        return false;
    }

    auto reply = m_systemdManager->LoadUnit(m_unitName);
    reply.waitForFinished();
    if (!reply.isValid()) {
        m_bus.send(m_message.createErrorReply(QDBusError::InternalError,
                                            QString("GetUnit %1 failed").arg(m_unitName)));
        return false;
    }

    auto unitPath = reply.value();
    auto *dumUpgradeUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                            unitPath.path(),
                                                            QDBusConnection::systemBus(),
                                                            this);

    auto activeState = dumUpgradeUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        m_bus.send(m_message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return false;
    }

    m_bus.connect(SYSTEMD1_SERVICE,
                  dumUpgradeUnit->path(),
                  "org.freedesktop.DBus.Properties",
                  "PropertiesChanged",
                  this,
                  SLOT(onDumUpgradeUnitPropertiesChanged(const QString &,
                                                         const QVariantMap &,
                                                         const QStringList &)));    

    auto reply1 = dumUpgradeUnit->Start("replace");
    reply1.waitForFinished();
    if (reply1.isError()) {
        m_bus.send(m_message.createErrorReply(
            QDBusError::InternalError,
            QString("Start %1 failed: %2").arg(m_unitName).arg(reply1.error().message())));
        return false;
    }

    return true;
}

void UpgradeTask::onDumUpgradeUnitPropertiesChanged(const QString &interfaceName,
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
            emit upgradableChanged(false);
        } else if (activeState == "failed") {
            m_state = STATE_FAILED;
            emit stateChanged(m_state);
        } else if (activeState == "inactive") {
            if (m_state == STATE_UPGRADING) {
                m_state = STATE_SUCCESS;
                emit stateChanged(m_state);
                emit upgradableChanged(false);
            }
        } else {
            qWarning() << "unknown activeState:" << activeState;
        }
    }
}

static const QByteArray PROGRESS_PREFIX = "progressRate:";

void UpgradeTask::parseUpgradeStdoutLine(const QByteArray &line)
{
    if (line.startsWith(PROGRESS_PREFIX)) {
        auto tmp = QByteArrayView(line.begin() + PROGRESS_PREFIX.size(), line.end()).trimmed();
        auto colonIdx = tmp.indexOf(':');
        QString percentStr = tmp.sliced(0, colonIdx).trimmed().toByteArray();
        QString stage = tmp.sliced(colonIdx + 1).trimmed().toByteArray();

        emit progressChanged(stage, percentStr.toFloat());
    }
}
