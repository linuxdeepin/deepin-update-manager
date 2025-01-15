//
// Created by uos on 25-1-16.
//
#include "idle.h"

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>

#include <regex>

Idle::Idle(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &Idle::onTimeout);
    connect(this, &Idle::inhibit, this, &Idle::handleInhibit);
    m_timer->start(DUM_AUTO_IDLE_TIMEOUT);
}

void Idle::onTimeout() const
{
    if (m_reasons.isEmpty()) {
        QCoreApplication::quit();
    }
}

void Idle::Inhibit(const QString& task)
{
    m_reasons.append(task);
    Q_EMIT inhibit();
}

void Idle::UnInhibit(const QString& task)
{
    m_reasons.removeOne(task);
    Q_EMIT inhibit();
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