// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusArgument>
#include <QString>

static const QString PERIOD_DEVELOP = "develop";
static const QString PERIOD_RELEASE = "release";

static const QString COMPONENT_BASE = "base";
static const QString COMPONENT_SECURITY = "security";

class Branch
{
public:
    Branch() = default;
    explicit Branch(const QString &str);

    const QString &period() const { return m_period; }

    const QString &version() const { return m_version; }

    bool valid() const;
    bool canUpgradeTo(const Branch &dest) const;

    QString toString() const;

private:
    QString m_codeName;
    QString m_period;
    QString m_version;
    QString m_project;
    QString m_component;
    QString m_revision;
};
