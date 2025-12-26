#ifndef QTTCPSERVER_H
#define QTTCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QMutex>
#include <QHostAddress>
#include <functional>

/*
 QtTcpServer
 - 使用 Qt 的 QTcpServer/QTcpSocket
 - 当收到完整的一行消息 (ended with '\n') 时，触发 signal: messageReceived(clientId, QString)
 - 也支持注册 std::function 回调 setMessageHandler(...)
 - 提供 sendToClient, broadcast, stop 等方法
*/

class QtTcpServer : public QObject {
    Q_OBJECT
public:
    explicit QtTcpServer(QObject* parent = nullptr);
    ~QtTcpServer();

    // 启动服务器（绑定到 port）
    bool start(quint16 port, const QHostAddress& address = QHostAddress::Any);

    // 停止服务器并断开所有客户端
    void stop();

    // 发送（加上不自动添加 '\n'）
    void sendToClient(int clientId, const QByteArray& data);
    // 发送并在末尾加 '\n'
    void sendLineToClient(int clientId, const QString& line);

    // 广播
    void broadcast(const QByteArray& data);
    void broadcastLine(const QString& line);

    // 回调 (可选)
    using MessageHandler = std::function<void(int clientId, const QString& message)>;
    void setMessageHandler(MessageHandler h);

    // 查询在线客户端数
    int clientCount() const;

signals:
    // 当接收到一条完整消息(去掉换行)时发出
    void messageReceived(int clientId, const QString& message);
    void clientConnected(int clientId);
    void clientDisconnected(int clientId);

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

private:
    struct Client {
        int id;
        QTcpSocket* socket;
        QByteArray buffer; // 未完整消息的缓存
    };

    QTcpServer* m_server = nullptr;
    mutable QMutex m_mutex; // 保护 clients 映射
    QMap<int, Client> m_clients;
    int m_nextClientId = 1;

    MessageHandler m_messageHandler;
};

#endif // QTTCPSERVER_H
