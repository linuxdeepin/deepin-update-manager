//
// Created by uos on 25-1-16.
//

#ifndef UPDATEPLATFORM_H
#define UPDATEPLATFORM_H
#include <QObject>

// updatePlatform: 处理更新平台的数据
class updatePlatform
{
public:
    updatePlatform();
    ~updatePlatform();
    void checkRemoteVersion();
private:
    QString m_baseline;
    QString m_machineID;
};
#endif //UPDATEPLATFORM_H
