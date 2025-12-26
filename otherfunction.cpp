#include <sstream>
#include <vector>
#include "Logger.h"
#include <iomanip>
#include <cctype>
#include <QCryptographicHash>
#include <chrono>
#include <ctime>

std::tuple<std::string, std::string, int> splitUdpMessage(const std::string& msg)	//单号,重量,供包台号
{
    std::stringstream ss(msg);
    std::string item;
    std::vector<std::string> parts;

    while (std::getline(ss, item, ','))
    {
        parts.push_back(item);
    }
    try
    {
        if (parts.size() >= 3)
        {
            std::string code = parts[0];
            std::string weight = parts[1];
            int supply_id = std::stoi(parts[2]);
            return std::make_tuple(code, weight, supply_id);
        }
    }
    catch (const std::exception& e)
    {
        Logger::getInstance().Log("---- [Error] Function splitUdpMessage error: " + std::string(e.what()));
    }
    return std::make_tuple("", "", -1);
}
std::string currentDateTimeString()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto now_time_t = system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now_time_t);   // Windows 安全版本
#else
    localtime_r(&now_time_t, &tm);   // POSIX 安全版本
#endif
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}


// 将 0-9 a-f A-F 转为 0..15；返回 false 表示非法字符
bool hexCharToVal(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
    return false;
}

// 把 hexStr 转成 bytes，支持 "4A 54 30"、"4A5430"、"0x4A 0x54" 等。
// 返回 false 表示解析失败（非法字符或奇数个有效 hex 字符）
bool hexStringToBytes(const std::string& hexStr, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    std::vector<uint8_t> nibbles;
    nibbles.reserve(hexStr.size());

    for (size_t i = 0; i < hexStr.size(); ++i) {
        char c = hexStr[i];
        // 忽略空格、制表符、逗号等常见分隔符
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') continue;
        // 忽略 0x 前缀
        if (c == '0' && (i + 1) < hexStr.size() && (hexStr[i + 1] == 'x' || hexStr[i + 1] == 'X')) {
            ++i; // skip '0', next char is 'x' or 'X' which also skip by loop increment
            continue;
        }
        if (c == 'x' || c == 'X') continue; // in case leading 'x'
        uint8_t v;
        if (!hexCharToVal(c, v)) {
            return false;
        }
        nibbles.push_back(v);
    }

    if (nibbles.empty()) return true; // empty -> success, outBytes empty
    if ((nibbles.size() & 1) != 0) {
        // 奇数个 nybble，报错以避免含糊情况；如果你想自动补0，请改这里逻辑
        return false;
    }

    outBytes.reserve(nibbles.size() / 2);
    for (size_t i = 0; i < nibbles.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>((nibbles[i] << 4) | nibbles[i + 1]);
        outBytes.push_back(byte);
    }
    return true;
}
QByteArray hmacSha256(const QByteArray& key, const QByteArray& message)
{
    const int blockSize = 64; // SHA-256 block size in bytes
    QByteArray k = key;
    if (k.size() > blockSize)
    {
        k = QCryptographicHash::hash(k, QCryptographicHash::Sha256);
    }
    if (k.size() < blockSize)
    {
        k.append(QByteArray(blockSize - k.size(), '\0'));
    }
    QByteArray o_key_pad(blockSize, '\x5c');
    QByteArray i_key_pad(blockSize, '\x36');
    for (int i = 0; i < blockSize; ++i)
    {
        o_key_pad[i] = o_key_pad[i] ^ k[i];
        i_key_pad[i] = i_key_pad[i] ^ k[i];
    }
    QByteArray inner = QCryptographicHash::hash(i_key_pad + message, QCryptographicHash::Sha256);
    QByteArray hmac = QCryptographicHash::hash(o_key_pad + inner, QCryptographicHash::Sha256);
    return hmac.toHex();
}
QByteArray hmacSha256Raw(const QByteArray& key, const QByteArray& message) {
    const int blockSize = 64;
    QByteArray k = key;
    if (k.size() > blockSize)
        k = QCryptographicHash::hash(k, QCryptographicHash::Sha256);
    if (k.size() < blockSize)
        k.append(QByteArray(blockSize - k.size(), '\0'));

    QByteArray o_key_pad(blockSize, '\x5c');
    QByteArray i_key_pad(blockSize, '\x36');
    for (int i = 0; i < blockSize; ++i) {
        o_key_pad[i] = o_key_pad[i] ^ k[i];
        i_key_pad[i] = i_key_pad[i] ^ k[i];
    }
    QByteArray inner = QCryptographicHash::hash(i_key_pad + message, QCryptographicHash::Sha256);
    QByteArray hmac = QCryptographicHash::hash(o_key_pad + inner, QCryptographicHash::Sha256);
    return hmac; // 返回 raw bytes（32 字节）
}
std::string getCurrentTime()
{
    using namespace std::chrono;
    // 获取当前系统时间点
    auto now = system_clock::now();
    // 转为time_t（秒）
    std::time_t t = system_clock::to_time_t(now);

    // 转为本地时间的tm结构（线程安全做法：Windows用 localtime_s，POSIX用 localtime_r）
    std::tm tm;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm, &t);   // Windows
#else
    localtime_r(&t, &tm);   // POSIX (Linux/macOS)
#endif

    std::ostringstream oss;
    // 格式化输出 yyyy-MM-dd HH:mm:ss
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
// 返回当前时间戳（毫秒），类型为 unsigned long long / uint64_t
std::uint64_t currentTimeMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()
        );
}

//// 将 account 和当前毫秒数拼接成一个字符串（例如用于 HTTP 字段）
// std::string makeAccountWithMillis(const std::string& account) {
//	return account + std::to_string(currentTimeMillis());
//}
