#include "logger.h"
#include <deque>
#include <filesystem>
#include <sstream>
#include <iostream>

Logger* Logger::instance = nullptr;

static std::tm getLocalTm()
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_time;
#ifdef _WIN32
    localtime_s(&tm_time, &now_time_t);
#else
    localtime_r(&now_time_t, &tm_time);
#endif
    return tm_time;
}

namespace {
// 后台线程与队列（文件作用域静态，避免改动头文件）
std::thread g_bgThread;
std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<std::string> g_msgQueue;
std::atomic<bool> g_stopBackground{ false };
std::atomic<bool> g_bgThreadStarted{ false };
}

Logger::Logger() {
    // 根目录为 ./Logs
    std::filesystem::path rootPath = std::filesystem::current_path() / "Logs";
    logRootDir = rootPath.string();

    // 使用当前本地时间初始化年/月/日目录与 currentDate/currentHour
    auto tm_time = getLocalTm();
    std::ostringstream yyyy, mm, dd;
    yyyy << (1900 + tm_time.tm_year);
    mm << std::setw(2) << std::setfill('0') << (tm_time.tm_mon + 1);
    dd << std::setw(2) << std::setfill('0') << tm_time.tm_mday;

    std::filesystem::path dayPath = rootPath / yyyy.str() / mm.str() / dd.str();
    logDayDir = dayPath.string();

    // 递归创建目录
    try {
        std::filesystem::create_directories(dayPath);
    }
    catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Fail to create log directory: " << e.what() << std::endl;
    }

    // currentDate 格式 "YYYY-MM-DD"
    {
        char buf[20];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_time);
        currentDate = std::string(buf);
    }

    // 当前小时
    currentHour = tm_time.tm_hour;

    // 文件名为 log_YYYY-MM-DD_HH.txt（按小时）
    std::filesystem::path filePath = dayPath / getLogFileName();
    logFilePath = filePath.string();

    try {
        logFile.open(logFilePath, std::ios::out | std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "Failed to open log file: " << logFilePath << std::endl;
        }
    }
    catch (const std::ofstream::failure& e) {
        std::cerr << "File opening failed::" << e.what() << std::endl;
    }

    // 启动后台线程（以 this 为上下文），但确保线程真正启动一次
    g_stopBackground.store(false);
    g_bgThread = std::thread([this]() {
        g_bgThreadStarted.store(true);
        while (true) {
            std::unique_lock<std::mutex> lk(g_queueMutex);
            // 等待新消息或退出信号；也会超时（500ms）以便定期检查退出条件
            g_queueCv.wait_for(lk, std::chrono::milliseconds(500), [] {
                return !g_msgQueue.empty() || g_stopBackground.load();
            });

            // 处理队列中的所有消息
            while (!g_msgQueue.empty()) {
                std::string userMsg = std::move(g_msgQueue.front());
                g_msgQueue.pop_front();
                lk.unlock(); // 在写入时释放队列锁，最小化阻塞

                // 为消息加上时间戳并写入文件/控制台
                try {
                    // 先检查是否需要切换文件（跨日/跨小时）
                    this->rotateLogFileIfNeeded();

                    // 生成带时间戳的最终字符串
                    std::string timeStamp = this->getCurrentTime();
                    std::stringstream logMessage;
                    logMessage << timeStamp << " :" << userMsg;
                    std::string finalMsg = logMessage.str();

                    // 写入文件（若可用）
                    if (this->logFile.is_open()) {
                        this->logFile << finalMsg << std::endl;
                        // 不一定每次都 flush，保持性能（可按需加 flush）
                    }

                    // 输出到控制台
                    std::cout << finalMsg << std::endl;
                }
                catch (const std::exception& ex) {
                    // 后台线程必须稳健，不让异常终止线程
                    std::cerr << "Logger background write error: " << ex.what() << std::endl;
                }
                lk.lock();
            }

            // 如果收到停止信号并且队列已空则退出循环
            if (g_stopBackground.load() && g_msgQueue.empty()) {
                break;
            }
        } // end while
    });
}

Logger& Logger::getInstance() {
    static std::once_flag s_onceFlag;
    std::call_once(s_onceFlag, []() {
        instance = new Logger();
    });
    return *instance;
}

Logger::~Logger() {
    // 请求后台线程停止并等待其完成所有待写入消息
    try {
        g_stopBackground.store(true);
        g_queueCv.notify_all();
        if (g_bgThread.joinable()) {
            g_bgThread.join();
        }
    }
    catch (...) {
        // 不要抛出异常
    }

    if (logFile.is_open()) {
        logFile.flush();
        logFile.close();
    }
}

void Logger::Log(const std::string& message) {
    // 只把用户消息放入队列，尽可能短的临界区，不阻塞主线程做 I/O
    {
        std::lock_guard<std::mutex> qlock(g_queueMutex);
        g_msgQueue.emplace_back(message);
    }
    g_queueCv.notify_one();
}

// rotateLogFileIfNeeded 保持原有实现，仅后台线程会调用它（因此不需要额外的锁）
void Logger::rotateLogFileIfNeeded() {
    // 获取当前本地时间信息
    auto tm_time = getLocalTm();

    // 构造新的 date 和 hour
    char dateBuf[20];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm_time);
    std::string newDateStr = std::string(dateBuf);
    int newHour = tm_time.tm_hour;

    // 构造新的 dayPath (Logs/YYYY/MM/DD)
    std::ostringstream yyyy, mm, dd;
    yyyy << (1900 + tm_time.tm_year);
    mm << std::setw(2) << std::setfill('0') << (tm_time.tm_mon + 1);
    dd << std::setw(2) << std::setfill('0') << tm_time.tm_mday;
    std::filesystem::path newDayPath = std::filesystem::path(logRootDir) / yyyy.str() / mm.str() / dd.str();
    std::string newDayDirStr = newDayPath.string();

    bool needReopen = false;

    // 如果跨日，必须创建新目录并打开新文件
    if (newDateStr != currentDate) {
        try {
            std::filesystem::create_directories(newDayPath);
        }
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Fail to create new day directory: " << e.what() << std::endl;
        }
        currentDate = newDateStr;
        currentHour = newHour;
        logDayDir = newDayDirStr;
        needReopen = true;
    }
    else if (newHour != currentHour) {
        // 同一天但小时变化，按小时切换文件
        currentHour = newHour;
        needReopen = true;
    }

    if (needReopen) {
        if (logFile.is_open()) logFile.close();

        // 构造新的文件路径： Logs/YYYY/MM/DD/log_YYYY-MM-DD_HH.txt
        std::filesystem::path newFilePath = newDayPath / getLogFileName();
        logFilePath = newFilePath.string();

        try {
            logFile.open(logFilePath, std::ios::out | std::ios::app);
            if (!logFile.is_open()) {
                std::cerr << "Failed to open log file: " << logFilePath << std::endl;
            }
        }
        catch (const std::ofstream::failure& e) {
            std::cerr << "File opening failed::" << e.what() << std::endl;
        }
    }
}

void Logger::ensureLogDirectory() {
    // 兼容旧调用：尝试创建 logDayDir（如果已设置）
    try {
        if (!logDayDir.empty()) {
            std::filesystem::create_directories(std::filesystem::path(logDayDir));
        }
        else if (!logRootDir.empty()) {
            std::filesystem::create_directories(std::filesystem::path(logRootDir));
        }
    }
    catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Fail to create log directory: " << e.what() << std::endl;
    }
}

std::string Logger::getLogFileName() {
    // 生成 log_YYYY-MM-DD_HH.txt，使用 current time
    auto tm_time = getLocalTm();
    char fileName[64];
    strftime(fileName, sizeof(fileName), "log_%Y-%m-%d_%H.txt", &tm_time);
    return std::string(fileName);
}

std::string Logger::getCurrentDate() {
    // 返回 "YYYY-MM-DD"
    auto tm_time = getLocalTm();
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm_time);
    return std::string(buffer);
}

std::string Logger::getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_time;
#ifdef _WIN32
    localtime_s(&tm_time, &now_time_t);
#else
    localtime_r(&now_time_t, &tm_time);
#endif
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_time);
    std::ostringstream timeStream;
    timeStream << buffer << "." << std::setw(3) << std::setfill('0') << millis;
    return timeStream.str();
}

void Logger::close() {
    // 请求后台线程停止并等待写完队列，然后再关闭文件
    g_stopBackground.store(true);
    g_queueCv.notify_all();
    if (g_bgThread.joinable()) {
        g_bgThread.join();
    }

    if (logFile.is_open()) {
        logFile.close();
    }
}
