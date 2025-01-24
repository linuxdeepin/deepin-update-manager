// SPDX-FileCopyrightText: 2025 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ABSTRACTTASK_H
#define ABSTRACTTASK_H

#include "common.h"
#include "SystemdManagerInterface.h"

#include <QObject>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

class AbstractTask : public QObject
{
    Q_OBJECT
public:
    explicit AbstractTask(const QDBusConnection &bus, QObject *parent = nullptr);
    virtual ~AbstractTask();

    virtual bool run() = 0;

protected:
    bool checkAuthorization(const QString &actionId, const QString &service) const;

protected:
    QDBusConnection m_bus;
    org::freedesktop::systemd1::Manager *m_systemdManager;

};

#endif // ABSTRACTTASK_H
