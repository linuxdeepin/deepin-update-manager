#include "Report.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QSettings>

#include <filesystem>
#include <memory>

static const QString CPUINFO_PATH = "/proc/cpuinfo";
static const QByteArray CPUINFO_MODEL_NAME_KEY = "model name";
static const QByteArray CPUINFO_MODEL_NAME_KEY_SW = "cpu";
static const QByteArray CPUINFO_MODEL_NAME_KEY_KIRIN = "Hardware";
static const QByteArray CPUINFO_MODEL_NAME_KEY_ARM = "Processor";

static const QByteArray LSCPU_MODEL_NAME_KEY = "Model name";

static const std::string DMI_ID_DIR = "/sys/class/dmi/id";
static const std::vector<std::string> DMI_ID_FILENAMES = {
    "product_family",
    "product_name",
    "product_sku",
};

static const QString OEM_INFO_FILE_PATH = "/etc/oem-info";
static const QString OEM_SHADOW_FILE_PATH = "/var/uos/.oem-shadow";
static const std::string OEM_PUB_KEY = R"(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAwzVS35kJl3mhSJssD3S5
EzjJbFoAD+VsMSy2nS7WQA2XH0aPAWjgCeU+1ScYdBOWz+zWsnK77fGm96HueAuT
hQEJ9J+ISJUuYBYCc6ovc35gxnhCmP2Qof+/vw98+uKnf1aTDI1imNCWOd/shSbL
OBn5xFXPsQld1HJqahOuQZOguNIWvrvT7RtmQb77iu576gVLc948HreXKOPD57uK
JoA2KcoUt95hd94wYyphCuE4onjPcIlpJQfda6PP+HO2Xwze3ltIG6hJSSAEK4R9
8GnaOTqvslWVI9QFLCIyQ63dbbnASYFTWpDXTlPJsss64vfWOuEjwIyzzQDJNOzN
FQIDAQAB
-----END PUBLIC KEY-----
)";

static const QString CUSTOM_INFO_PATH = "/usr/share/deepin/custom-note/info.json";

Report::Report() { }

OsVersion Report::getOsVersion()
{
    QSettings settings("/etc/os-version", QSettings::IniFormat);
    settings.beginGroup("Version");

    // clang-format off
    OsVersion version{
        settings.value("SystemName").toString(),
        settings.value("ProductType").toString(),
        settings.value("EditionName").toString(),
        settings.value("MajorVersion").toInt(),
        settings.value("MinorVersion").toInt(),
        settings.value("OsBuild").toString(),
    };
    // clang-format on

    settings.endGroup();

    return version;
}

QString Report::getProcessorModelName()
{
    QFile file(CPUINFO_PATH);
    if (!file.open(QIODevice::ReadOnly)) {
        // todo: log
    }

    while (file.canReadLine()) {
        auto line = file.readLine();
        if (line.startsWith(CPUINFO_MODEL_NAME_KEY) || line.startsWith(CPUINFO_MODEL_NAME_KEY_SW)
            || line.startsWith(CPUINFO_MODEL_NAME_KEY_KIRIN)
            || line.startsWith(CPUINFO_MODEL_NAME_KEY_ARM)) {
            return line.mid(line.indexOf(":") + 1).trimmed();
        }
    }

    QProcess process;
    process.start("lscpu");
    process.waitForFinished();
    if (process.exitStatus() != QProcess::NormalExit) {
        // todo: log
        return "";
    }

    auto lscpuStdout = process.readAllStandardOutput();
    auto lines = lscpuStdout.split('\n');
    for (auto &line : lines) {
        line = line.trimmed();
        if (line.startsWith(LSCPU_MODEL_NAME_KEY)) {
            return line.mid(line.indexOf(":") + 1).trimmed();
        }
    }

    return "";
}

QString Report::getArchitecture()
{
    QProcess process;
    process.start("dpkg", { "--print-architecture" });
    process.waitForFinished();
    if (process.exitStatus() != QProcess::NormalExit) {
        // todo: log
        return "";
    }

    return process.readAllStandardOutput();
}

QString Report::getActivationCode()
{
    QDBusInterface interface("com.deepin.license",
                             "/com/deepin/license/Info",
                             "org.freedesktop.DBus.Properties",
                             QDBusConnection::systemBus());
    if (!interface.isValid()) {
        // todo: log
        return "";
    }

    QDBusReply<QDBusVariant> reply = interface.call("Get", "com.deepin.license.Info", "ActiveCode");
    if (!reply.isValid()) {
        // todo: log
        return "";
    }

    return reply.value().variant().toString();
}

std::tuple<QString, bool> Report::getIsoIdAndCustomizedKernel()
{
    if (!verifyOemInfoFile()) {
        return { "", false };
    }

    QFile oemInfoFile(OEM_INFO_FILE_PATH);
    if (!oemInfoFile.open(QIODevice::ReadOnly)) {
        // todo: log
        return { "", false };
    }

    auto doc = QJsonDocument::fromJson(oemInfoFile.readAll());
    auto oemInfoObj = doc.object();
    auto isoId = oemInfoObj.take("basic").toObject().take("iso_id").toString();
    auto customizedKernel =
        oemInfoObj.take("custom_info").toObject().take("customized_kernel").toBool();

    return { isoId, customizedKernel };
}

bool Report::verifyOemInfoFile()
{
    QFile oemInfoFile(OEM_INFO_FILE_PATH);
    if (!oemInfoFile.open(QIODevice::ReadOnly)) {
        // todo: log
        return false;
    }

    QFile oemShadowFile(OEM_SHADOW_FILE_PATH);
    if (!oemShadowFile.open(QIODevice::ReadOnly)) {
        // todo: log
        return false;
    }

    auto oemInfo = oemInfoFile.readAll();
    auto oemSign = oemShadowFile.readAll();

    std::shared_ptr<BIO> bio(BIO_new_mem_buf(OEM_PUB_KEY.c_str(), OEM_PUB_KEY.length()), &BIO_free);

    uint8_t *data = nullptr;
    long dataLen = 0;
    if (PEM_read_bio(bio.get(), nullptr, nullptr, &data, &dataLen) != 1) {
        // todo: log
        return false;
    }

    EVP_PKEY *pkey =
        d2i_PublicKey(EVP_PKEY_RSA, nullptr, const_cast<const uint8_t **>(&data), dataLen);

    {
        std::shared_ptr<EVP_MD_CTX> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx) {
            // todo: log
            return false;
        }

        if (EVP_DigestInit(ctx.get(), EVP_sha256()) != 1) {
            // todo: log
            return false;
        }

        if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey) != 1) {
            // todo: log
            return false;
        }

        if (EVP_DigestVerifyUpdate(ctx.get(), oemInfo.constData(), oemInfo.size()) != 1) {
            // todo: log
            return false;
        }

        if (EVP_DigestVerifyFinal(ctx.get(),
                                  reinterpret_cast<const uint8_t *>(oemSign.constData()),
                                  oemSign.size())
            != 1) {
            // todo: log
            return false;
        }
    }

    if (data != NULL) {
        OPENSSL_free(data);
    }

    return true;
}

QString Report::getProjectId()
{
    QFile file(CUSTOM_INFO_PATH);
    if (!file.open(QIODevice::ReadOnly)) {
        // todo: log
        return "";
    }

    auto doc = QJsonDocument::fromJson(file.readAll());
    auto obj = doc.object();
    return obj.take("id").toString();
}

QString Report::getProductSKU()
{
    QStringList list;
    list.reserve(DMI_ID_FILENAMES.size());

    std::filesystem::path dir(DMI_ID_DIR);
    for (const auto &filename : DMI_ID_FILENAMES) {
        auto path = dir / filename;
        if (!std::filesystem::is_regular_file(path)) {
            // todo: log
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            // todo: log
        }

        list << file.readAll();
    }

    return list.join(" ");
}

QString Report::getMacAddress()
{
    for (auto &interface : QNetworkInterface::allInterfaces()) {
        auto flags = interface.flags();
        if (flags & QNetworkInterface::IsLoopBack) {
            continue;
        }

        auto mac = interface.hardwareAddress();
        if (mac.isEmpty()) {
            continue;
        }

        return mac;
    }

    return "";
}
