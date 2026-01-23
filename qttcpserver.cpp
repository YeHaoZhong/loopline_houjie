#include "QtTcpServer.h"
#include <QDebug>
#include "logger.h"
QtTcpServer::QtTcpServer(QObject* parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &QtTcpServer::onNewConnection);
}

QtTcpServer::~QtTcpServer()
{
    stop();
    // m_server 由 parent (this) 管理，会自动 delete
}

bool QtTcpServer::start(quint16 port, const QHostAddress& address)
{
    if (!m_server) return false;
    if (m_server->isListening()) m_server->close();
    bool ok = m_server->listen(address, port);
    if (!ok) {
        qWarning() << "TcpServer listen failed:" << m_server->errorString();
    }
    else {
        // qInfo() << "TcpServer listening on" << address.toString() << ":" << port;
        Logger::getInstance().Log("----[QtTcpServer] start() TcpServer listening on: ["+address.toString().toStdString()+"], port: ["+std::to_string(port)+"]");
    }
    return ok;
}

void QtTcpServer::stop()
{
    if (m_server && m_server->isListening()) {
        m_server->close();
    }

    QMutexLocker lk(&m_mutex);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->socket) {
            it->socket->disconnectFromHost();
            it->socket->close();
            it->socket->deleteLater();
        }
    }
    m_clients.clear();
}

void QtTcpServer::onNewConnection()
{
    QTcpSocket* sock = nullptr;
    while (m_server->hasPendingConnections()) {
        sock = m_server->nextPendingConnection();
        if (!sock) continue;

        // set socket options if needed
        sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        sock->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

        // create client entry
        int cid;
        {
            QMutexLocker lk(&m_mutex);
            cid = m_nextClientId++;
            Client c;
            c.id = cid;
            c.socket = sock;
            c.buffer.clear();
            m_clients.insert(cid, c);
        }

        // connect signals
        connect(sock, &QTcpSocket::readyRead, this, &QtTcpServer::onSocketReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, &QtTcpServer::onSocketDisconnected);
        emit clientConnected(cid);
        qInfo() << "Client connected, id=" << cid << "from" << sock->peerAddress().toString() << ":" << sock->peerPort();
    }
}

//void QtTcpServer::onSocketReadyRead()
//{
//    // Sender is the socket that has data
//    QTcpSocket* sock = qobject_cast<QTcpSocket*>(sender());
//    if (!sock) return;
//
//    int clientId = -1;
//    {
//        QMutexLocker lk(&m_mutex);
//        // 找到对应 clientId
//        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
//            if (it->socket == sock) {
//                clientId = it->id;
//                break;
//            }
//        }
//    }
//    if (clientId == -1) {
//        qWarning() << "ReadyRead from unknown socket";
//        return;
//    }
//
//    // 读取所有数据并追加到 buffer
//    QByteArray data = sock->readAll();
//
//    QStringList completeLines;
//    {
//        QMutexLocker lk(&m_mutex);
//        auto& buf = m_clients[clientId].buffer;
//        buf.append(data);
//
//        // 分割以 '\n' 为边界 (保留 CRLF 兼容)
//        int pos = -1;
//        while ((pos = buf.indexOf('\n')) != -1) {
//            QByteArray line = buf.left(pos); // 不含 '\n'
//            // 移除行以及 '\n'
//            buf.remove(0, pos + 1);
//            // 去掉末尾的 '\r'（如果有）
//            if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
//            completeLines << QString::fromUtf8(line);
//        }
//    }
//
//    // 触发回调/信号（在 Qt 事件线程中）
//    for (const QString& ln : completeLines) {
//        emit messageReceived(clientId, ln);
//        if (m_messageHandler) {
//            try {
//                m_messageHandler(clientId, ln);
//            }
//            catch (...) {
//                qWarning() << "Exception in message handler";
//            }
//        }
//    }
//}
void QtTcpServer::onSocketReadyRead()
{
    // Sender is the socket that has data
    QTcpSocket* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;

    int clientId = -1;
    {
        QMutexLocker lk(&m_mutex);
        // 找到对应 clientId
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->socket == sock) {
                clientId = it->id;
                break;
            }
        }
    }
    if (clientId == -1) {
        qWarning() << "ReadyRead from unknown socket";
        return;
    }

    // 读取所有数据并追加到 buffer
    QByteArray data = sock->readAll();

    QStringList completeLines;
    {
        QMutexLocker lk(&m_mutex);
        auto& buf = m_clients[clientId].buffer;
        buf.append(data);

        // 我们支持两种消息终止符：
        //  - ETX (0x03) 设备协议
        //  - LF ('\n') 兼容现有逻辑 (也会去掉 CR '\r')
        while (true) {
            // 找到最近的分隔符位置（ETX 或 '\n'）
            int pos_etx = buf.indexOf(char(0x03));
            int pos_nl = buf.indexOf('\n');

            int pos = -1;
            if (pos_etx >= 0 && pos_nl >= 0) pos = std::min(pos_etx, pos_nl);
            else if (pos_etx >= 0) pos = pos_etx;
            else if (pos_nl >= 0) pos = pos_nl;
            else break; // 没有完整消息

            QByteArray frame = buf.left(pos); // 不含分隔符
            // 移除已消费的数据（包括分隔符）
            buf.remove(0, pos + 1);

            // 如果以 STX (0x02) 开头，去掉它
            if (!frame.isEmpty() && frame.at(0) == char(0x02)) {
                frame.remove(0, 1);
            }
            // 去掉末尾的 CR（如果存在）
            if (!frame.isEmpty() && frame.endsWith('\r')) {
                frame.chop(1);
            }

            // 现在 frame 是纯负载字节序列，将其按 UTF-8 解码为 QString（若不是 UTF-8，视情况改用 fromLocal8Bit）
            QString s = QString::fromUtf8(frame);

            // 去除前后空白（如果需要）
            s = s.trimmed();

            // 如果非空则添加为完整消息
            if (!s.isEmpty()) completeLines << s;
        }
    }

    // 触发回调/信号（在 Qt 事件线程中）
    for (const QString& ln : completeLines) {
        // debug 打点（可选）
        qDebug() << "QtTcpServer: emit messageReceived, clientId=" << clientId << ", payload=" << ln;
        emit messageReceived(clientId, ln);
        if (m_messageHandler) {
            try {
                m_messageHandler(clientId, ln);
            }
            catch (...) {
                qWarning() << "Exception in message handler";
            }
        }
    }
}
void QtTcpServer::onSocketDisconnected()
{
    QTcpSocket* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;

    int clientId = -1;
    {
        QMutexLocker lk(&m_mutex);
        for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
            if (it->socket == sock) {
                clientId = it->id;
                // cleanup socket object
                it->socket->deleteLater();
                m_clients.erase(it);
                break;
            }
        }
    }

    if (clientId != -1) {
        emit clientDisconnected(clientId);
        qInfo() << "Client disconnected id=" << clientId;
    }
    else {
        qWarning() << "Disconnected unknown socket";
    }
}

void QtTcpServer::sendToClient(int clientId, const QByteArray& data)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_clients.find(clientId);
    if (it != m_clients.end() && it->socket && it->socket->isOpen()) {
        it->socket->write(data);
        it->socket->flush();
    }
    else {
        qWarning() << "sendToClient: client not found or closed:" << clientId;
    }
}

void QtTcpServer::sendLineToClient(int clientId, const QString& line)
{
    sendToClient(clientId, line.toUtf8() + "\n");
}

void QtTcpServer::broadcast(const QByteArray& data)
{
    QMutexLocker lk(&m_mutex);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->socket && it->socket->isOpen()) {
            it->socket->write(data);
            it->socket->flush();
        }
    }
}

void QtTcpServer::broadcastLine(const QString& line)
{
    broadcast(line.toUtf8() + "\n");
}

void QtTcpServer::setMessageHandler(MessageHandler h)
{
    m_messageHandler = std::move(h);
}

int QtTcpServer::clientCount() const
{
    QMutexLocker lk(&m_mutex);
    return m_clients.size();
}
