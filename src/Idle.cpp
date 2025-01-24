// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Idle.h"
#include "common.h"

#include <QCoreApplication>

Idle::Idle(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(DUM_AUTO_IDLE_TIMEOUT);

    connect(m_timer,  &QTimer::timeout, this, [this] {
        if (m_reasons.isEmpty()) {
            QCoreApplication::quit();
        }
    });

    handleInhibit();
}

void Idle::Inhibit(const QString& task)
{
    m_reasons.append(task);
    handleInhibit();
}

void Idle::UnInhibit(const QString& task)
{
    m_reasons.removeOne(task);
    handleInhibit();
}

void Idle::handleInhibit() const
{
    if (m_reasons.isEmpty()) {
        if (!m_timer->isActive()) {
            m_timer->start(DUM_AUTO_IDLE_TIMEOUT);
            qInfo() << "dum idle on, will be exiting in 60s";
        }
    } else {
        if (m_timer->isActive()) {
            m_timer->stop();
        }
        qInfo() << "dum inhibited, tasks on handling:" << m_reasons;
    }
}
