#include<WS2tcpip.h>
#include<thread>
#include"tcpsocketclient.h"
#pragma comment(lib,"ws2_32.lib")


SOCKET SocketClient::connectTo(const std::string& ip, int port, bool receivce) {

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ipAddress = ip;
    mPort = port;

    if (clientSocket == INVALID_SOCKET) {
        // Logger::getInstance().Log("---- [Error] Port: [" + std::to_string(port) + "] Create socket failed：" + std::to_string(WSAGetLastError()));
        return INVALID_SOCKET;
    }

    char flag = 1;
    if (setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == SOCKET_ERROR) {
        // Logger::getInstance().Log("---- [Error] Failed to set socket options: " + std::to_string(WSAGetLastError()));
        closesocket(clientSocket);
        return INVALID_SOCKET;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
        // Logger::getInstance().Log("---- [Error] Invalid socket address：" + ip);
        closesocket(clientSocket);
        return INVALID_SOCKET;
    }

    int result = ::connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (result == SOCKET_ERROR) {
        //Logger::getInstance().Log("---- [Error] Socket connection failed! IP: ["+ip+"], port: ["+std::to_string(port) + "]");
        closesocket(clientSocket);
        return INVALID_SOCKET;
    }
    else
    {
        mSock = clientSocket;  // 保存连接的套接字
        SocketConnection = true;
        if (receivce) {		// 是否启动接收线程
            startReceiveData(clientSocket);  // 启动接收数据线程
        }
    }
    return clientSocket;
}

bool SocketClient::sendData(const std::string& message)
{
    try
    {
        int result = send(mSock, message.c_str(), message.length(), 0);
        if (result == SOCKET_ERROR)
        {
            // Logger::getInstance().Log("---- [Error] Socket falied to send message：" + message);
            SocketConnection = false;
            return false;
        }
        else {
            //Logger::getInstance().Log("---- [Info] Socket send message：" + message + " successfully!");
            return true;
        }
    }
    catch (const std::exception& e)
    {
        // Logger::getInstance().Log("---- [Error] Socket sendData exception: " + std::string(e.what()));
        SocketConnection = false;
    }
    return false;
}

void SocketClient::receiveData(SOCKET sock) {

    char buffer[1024];
    int result;
    try
    {
        while (receiving)
        {
            result = recv(sock, buffer, sizeof(buffer), 0);
            if (result > 0)
            {
                int len = std::min(result, static_cast<int>(sizeof(buffer) - 1));
                buffer[len] = '\0';
                QByteArray ba(buffer, result);
                if(onRawData){              //设置回调后, 走回调
                    onRawData(ba);
                }else{
                    emit dataReceived(ba);
                }

            }
            else if (result == 0) {
                // Logger::getInstance().Log("---- [Error] IP: [" + ipAddress + "]. Port: [" + std::to_string(mPort) + "]. Socket disconnected!");
                SocketConnection = false;
                break;
            }
            else {
                // Logger::getInstance().Log("---- [Error] IP: [" + ipAddress + "]. Port: [" + std::to_string(mPort) + "]. Socket failed to receive data: " + std::to_string(WSAGetLastError()));
                //SocketConnection = false;
                //break;
            }
        }
    }
    catch (const std::exception& ex)
    {
        // Logger::getInstance().Log(std::string("---- [Error] handleSupplyWorking exception: ") + ex.what());
    }
}

void SocketClient::startReceiveData(SOCKET sock) {
    receiving.store(true);  // 保证循环能跑起来
    receiveThread = std::thread(&SocketClient::receiveData, this, sock);
    receiveThread.detach();  // 放掉线程，让它自己跑
}

void SocketClient::stopReceiveData() {
    if (!receiving.load() && !SocketConnection) return;
    SocketConnection = false;
    receiving.store(false);
    if (receiveThread.joinable()) {
        receiveThread.join();
    }
}
void SocketClient::disconnect() {
    if (mSock != INVALID_SOCKET)
    {
        closesocket(mSock);
        mSock = INVALID_SOCKET;
    }
    stopReceiveData();
}
bool SocketClient::connectStatus(const std::string& text) {
    if (SocketConnection) {
        bool ok = sendData(text);
        if (ok) {
            return true;
        }
    }
    return false;
}
