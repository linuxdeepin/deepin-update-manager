#pragma once

#include <QString>

struct OsVersion {
    QString systemName;
    QString productType;
    QString editionName;
    int majorVersion;
    int minorVersion;
    QString osBuild;
};

class Report {
public:
    Report();

private:
    static OsVersion getOsVersion();
    static QString getProcessorModelName();
    static QString getArchitecture();
    static QString getActivationCode();
    static std::tuple<QString, bool> getIsoIdAndCustomizedKernel();
    static bool verifyOemInfoFile();
    static QString getProjectId();
    static QString getProductSKU();
    static QString getMacAddress();
};
