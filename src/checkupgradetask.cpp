// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "checkupgradetask.h"
#include "SystemdUnitInterface.h"
#include "Branch.h"

#include <QLocalSocket>

checkUpgradeTask::checkUpgradeTask(const QDBusConnection &bus, const QDBusMessage &message, QObject *parent)
    : AbstractTask{bus, parent}
    , m_listRemoteRefsStdoutServer(new QLocalServer(this))
    , m_message(message)
    , m_unitName(QString())
    , m_fd(-1)
{

}

checkUpgradeTask::~checkUpgradeTask()
{
}

void checkUpgradeTask::setTaskDate(const QString &unitName, int fd)
{
    m_unitName = unitName;
    m_fd = fd;

    if (m_fd != -1) {
        m_listRemoteRefsStdoutServer->listen(m_fd);
    }
}

bool checkUpgradeTask::run()
{
    if (m_unitName.isEmpty() || m_fd == -1)
        return false;

    qDebug() << "check upgrade task run...";
    if (!checkAuthorization(ACTION_ID_CHECK_UPGRADE, m_message.service())) {
        m_bus.send(m_message.createErrorReply(QDBusError::AccessDenied, "Not authorized"));
        return false;
    }

    auto reply = m_systemdManager->LoadUnit(m_unitName);
    reply.waitForFinished();
    if (!reply.isValid()) {
        m_bus.send(m_message.createErrorReply(QDBusError::InternalError,
                                                 QString("LoadUnit %1 failed").arg(m_unitName)));
        return false;
    }

    auto unitPath = reply.value();
    auto *dumListRemoteRefsUnit = new org::freedesktop::systemd1::Unit(SYSTEMD1_SERVICE,
                                                                       unitPath.path(),
                                                                       QDBusConnection::systemBus(),
                                                                       this);
    auto activeState = dumListRemoteRefsUnit->activeState();
    if (activeState == "active" || activeState == "activating" || activeState == "deactivating") {
        m_bus.send(m_message.createErrorReply(QDBusError::AccessDenied, "An upgrade is in progress"));
        return false;
    }

    auto reply1 = dumListRemoteRefsUnit->Start("replace");
    reply1.waitForFinished();
    if (reply1.isError()) {
        m_bus.send(m_message.createErrorReply(QDBusError::InternalError,
                                                 QString("Start %1 failed: %2").arg(m_unitName).arg(reply1.error().message())));
        return false;
    }

    if (!m_listRemoteRefsStdoutServer->waitForNewConnection(5000)) {
        m_bus.send(m_message.createErrorReply(QDBusError::InternalError,
                                            QString("WaitForNewConnection failed")));
        return false;
    }

    auto *socket = m_listRemoteRefsStdoutServer->nextPendingConnection();
    // socket->waitForReadyRead();
    socket->waitForDisconnected();
    auto output = socket->readAll();
    socket->deleteLater();    

    auto remoteRefs = output.trimmed().split('\n');
    if (remoteRefs.size() < 1) {
        m_bus.send(m_message.createErrorReply(QDBusError::InternalError, "Check upgrade failed: no refs"));
        return false;
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
    QString remoteBranch = upgradable ? lastBranchInfo.toString() : "";
    emit checkUpgradeResult(upgradable, remoteBranch);

    return true;
}
