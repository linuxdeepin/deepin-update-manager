// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "abstracttask.h"

#include <PolkitQt1/Authority>

AbstractTask::AbstractTask(const QDBusConnection &bus, QObject *parent)
    : QObject{parent}
    , m_bus(bus)
    , m_systemdManager(new org::freedesktop::systemd1::Manager(
          SYSTEMD1_SERVICE, SYSTEMD1_MANAGER_PATH, bus, this))    
{
}

AbstractTask::~AbstractTask()
{
}

bool AbstractTask::checkAuthorization(const QString &actionId, const QString &service) const
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
