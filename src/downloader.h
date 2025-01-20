//
// Created by uos on 25-1-16.
//

#ifndef DOWNLOADER_H
#define DOWNLOADER_H
#include "Branch.h"

#include <QObject>

// Downloader: 下载器，下载更新数据
class Downloader
{
public:
    Downloader();
    ~Downloader();
    void doDownload();
    void stopDownload();
private:
    void downloadCheck();
Q_SIGNALS:
    void finished();
private:
    uint64_t rate;
    Branch *m_branch;

};
#endif //DOWNLOADER_H
