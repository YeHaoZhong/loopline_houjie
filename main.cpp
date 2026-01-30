#include "loopline_houjie.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include "logger.h"
#include <WinSock2.h>
#include "sqlconnectionpool.h"
int main(int argc, char *argv[])
{
#ifdef _WIN32
    WSADATA wsaData;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRet != 0) {
        Logger::getInstance().Log("WSAStartup failed : " + std::to_string(wsaRet));
    }
#endif

    SqlConnectionPool::instance().init(30);                                         //数据库连接池初始化30个连接
    QApplication a(argc, argv);
    Logger::getInstance().Log("------------------------------------------ window start ------------------------------------------");
    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "loopline_houjie_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    loopline_houjie w;
    w.show();
    return a.exec();
}
