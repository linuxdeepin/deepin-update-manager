//
// Created by uos on 25-1-16.
//

#ifndef CHECKER_H
#define CHECKER_H
#include "Branch.h"

#include <qtmetamacros.h>

#include <QString>

// 检查器: 检查系统更新
class Checker
{
public:
    Checker();
    ~Checker();
    static void doCheck();
Q_SIGNALS:
    static void finished();
    static void progress(int percentage);
private:
    QString m_remote;
    Branch m_current;
    QList<Branch> m_branches;
    QMap<QString,Branch> m_availableBranches;
};

#endif //CHECKER_H
