#ifndef TCPSOCKETCLIENT_H
#define TCPSOCKETCLIENT_H
#include<string>
#include<WinSock2.h>
#include<thread>
#include<QObject>
#include <atomic>
class SocketClient :public QObject {

    Q_OBJECT
public:
    SOCKET connectTo(const std::string& ip, int port, bool recevice = true);
    bool sendData(const std::string& message);
    void startReceiveData(SOCKET sock);
    void stopReceiveData();
    bool connectStatus(const std::string& text);
    void disconnect();
    bool SocketConnection = false;

public slots:

    bool sendComand(const QByteArray& command) {
        if (SocketConnection)
        {
            bool ok = sendData(command.toStdString());
            return ok;
        }
        return false;
    }
signals:

    void dataReceived(const QByteArray& data);

private:
    SOCKET mSock = INVALID_SOCKET;
    void receiveData(SOCKET sock);
    std::thread receiveThread;
    std::atomic<bool> receiving{ false }; // class member: replace bool receiving
    std::string ipAddress;
    int mPort;
};

#endif // TCPSOCKETCLIENT_H
