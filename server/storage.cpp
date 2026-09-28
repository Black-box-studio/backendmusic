#include "storage.h"
#include <QFile>
#include <QProcess>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QByteArray>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <memory>
#include <sstream>
#include <stdexcept>
Storage::Storage(const std::filesystem::path &directory) : root(directory)
{
    std::filesystem::create_directories(root);
    QFile file(QString::fromStdString((root / "storage.key").string()));
    if (!file.exists()) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw std::runtime_error("Cannot create storage key");
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        QByteArray bytes(32, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), 32)!=1 || file.write(bytes)!=32) throw std::runtime_error("Cannot initialize storage key");
        file.close();
    }
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open storage key");
    key=file.readAll().toStdString();
    if (key.size()!=32) throw std::runtime_error("Invalid storage key; restore the original key from backup");
}
std::string Storage::seal(const Json::Value &value) const {
    const auto plain=Json::writeString(Json::StreamWriterBuilder(),value);
    unsigned char iv[12],tag[16]; if(RAND_bytes(iv,12)!=1) throw std::runtime_error("Encryption failed");
    auto ctx=std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)>(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    QByteArray ciphertext(int(plain.size())+16,'\0'); int n=0,last=0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,reinterpret_cast<const unsigned char *>(key.data()),iv)!=1 || EVP_EncryptUpdate(ctx.get(),reinterpret_cast<unsigned char *>(ciphertext.data()),&n,reinterpret_cast<const unsigned char *>(plain.data()),int(plain.size()))!=1 || EVP_EncryptFinal_ex(ctx.get(),reinterpret_cast<unsigned char *>(ciphertext.data())+n,&last)!=1 || EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,tag)!=1) throw std::runtime_error("Encryption failed");
    ciphertext.resize(n+last);
    return (QByteArray(reinterpret_cast<char *>(iv),12)+QByteArray(reinterpret_cast<char *>(tag),16)+ciphertext).toBase64().toStdString();
}
Json::Value Storage::open(const std::string &value) const {
    auto bytes=QByteArray::fromBase64(QByteArray::fromStdString(value)); if(bytes.size()<28) throw std::runtime_error("Invalid encrypted configuration");
    auto ctx=std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)>(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    QByteArray plain(bytes.size(),'\0'); int n=0,last=0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,reinterpret_cast<const unsigned char *>(key.data()),reinterpret_cast<unsigned char *>(bytes.data()))!=1 || EVP_DecryptUpdate(ctx.get(),reinterpret_cast<unsigned char *>(plain.data()),&n,reinterpret_cast<unsigned char *>(bytes.data()+28),int(bytes.size()-28))!=1 || EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,bytes.data()+12)!=1 || EVP_DecryptFinal_ex(ctx.get(),reinterpret_cast<unsigned char *>(plain.data())+n,&last)!=1) throw std::runtime_error("Cannot decrypt storage configuration");
    plain.resize(n+last); Json::Value result;std::istringstream stream(plain.toStdString());stream>>result;return result;
}
std::string Storage::sign(const std::string &value) const {
    unsigned char result[32];unsigned int n=0;
    if(!HMAC(EVP_sha256(),key.data(),int(key.size()),reinterpret_cast<const unsigned char *>(value.data()),value.size(),result,&n)) throw std::runtime_error("Cannot sign media ticket");
    return QByteArray(reinterpret_cast<char *>(result),int(n)).toHex().toStdString();
}
Json::Value Storage::run(Json::Value request) const {
    auto python=qEnvironmentVariable("GLASS_STORAGE_PYTHON",QStandardPaths::findExecutable("python3"));
    auto helper=qEnvironmentVariable("GLASS_STORAGE_HELPER",QCoreApplication::applicationDirPath()+"/storage_provider.py");
    QProcess process; process.start(python,{helper});
    if(!process.waitForStarted(3000)) throw std::runtime_error("Cannot start storage helper");
    process.write(QByteArray::fromStdString(Json::writeString(Json::StreamWriterBuilder(),request))); process.closeWriteChannel();
    if(!process.waitForFinished(90000)) { process.kill();process.waitForFinished(1000);throw std::runtime_error("Storage request timed out"); }
    const auto output=process.readAllStandardOutput();
    if(process.exitCode()!=0 || output.size()>2*1024*1024) throw std::runtime_error("Storage helper failed");
    Json::Value result;std::istringstream stream(output.toStdString());stream>>result;
    if(!result.isObject()) throw std::runtime_error("Invalid storage response");
    return result;
}
