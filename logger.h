#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>
class Logger {
public:

    static Logger& getInstance();
    ~Logger();

    void Log(const std::string& message);
    void close();

private:
    Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string getLogFileName();            // "YYYY-MM-DD_HH.txt"
    std::string getCurrentTime();            // "YYYY-MM-DD HH:MM:SS.mmm"
    std::string getCurrentDate();            // "YYYY-MM-DD"
    void rotateLogFileIfNeeded();
    void ensureLogDirectory();

    static Logger* instance;
    std::mutex logMutex;
    std::ofstream logFile;
    std::string logRootDir;   // base ".../logs"
    std::string logDayDir;    // ".../logs/YYYY-MM-DD"
    std::string logFilePath;
    std::string currentDate;
    int currentHour;
};

#endif // LOGGER_H
