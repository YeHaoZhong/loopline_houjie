#include "DataProcess.h"
#include "Logger.h"
#include "UdpReceiver.h"
#include <QtConcurrent/QtConcurrent>
#include <sstream>
extern std::tuple<std::string, std::string, int> splitUdpMessage(const std::string& msg);
DataProcess::DataProcess()
{
    //开服务
    try {
        UdpReceiver* _udpSupplyReceiver = new UdpReceiver(3011, "0.0.0.0");
        _udpSupplyReceiver->setCallback([this](const std::vector<uint8_t>& data, const std::string& ip, uint16_t port)
                                        {
                                            QString qtext = QString::fromStdString(std::string(data.begin(), data.end()));
                                            emit onUDPReceived(qtext);
                                        });
        connect(this, &DataProcess::onUDPReceived, this, &DataProcess::onSupplyUDPServerRecv, Qt::QueuedConnection);
        //m_recvSupplyServer = new QtTcpServer(this);			//接收供包台消息
        //if (!m_recvSupplyServer->start(m_recvSupplyPort)) {
        //	Logger::getInstance().Log("----[DataProcess] dataProInit() Failed to start supply TCP server on port " + std::to_string(m_recvSupplyPort));
        //}
        //connect(m_recvSupplyServer, &QtTcpServer::messageReceived, this, &DataProcess::onSupplyTCPServerRecv, Qt::QueuedConnection);
        m_recvPdaServer = new QtTcpServer(this);			//接收PDA消息
        if (!m_recvPdaServer->start(m_recvPdaPort)) {
            Logger::getInstance().Log("----[DataProcess] dataProInit() Failed to start pda TCP server on port " + std::to_string(m_recvPdaPort));
        }
        connect(m_recvPdaServer, &QtTcpServer::messageReceived, this, &DataProcess::onPdaTCPServerRecv, Qt::QueuedConnection);
    }
    catch (...) {}
}
void DataProcess::dataProInit()
{
    connect(&m_plc_supplyClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSupplyRecv);
    connect(&m_plc_sendSlotClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSendSlotRecv);
    connect(&m_plc_unloadClient, &SocketClient::dataReceived, this, &DataProcess::onPLCUnLoadRecv);
    connect(&m_plc_slotStatusClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSlotStatusRecv);

    m_supplyIDToOrder.resize(12);                       //初始化每个供包台的序列号
    for(int i = 0; i<12;++i){
        m_supplyIDToOrder[i] = 0;
    }

}
void DataProcess::dataProCleanUp()
{
    try {
        /*if (m_recvSupplyServer) {
            m_recvSupplyServer->stop();
            delete m_recvSupplyServer;
            m_recvSupplyServer = nullptr;
        }*/
        if (m_recvPdaServer) {
            m_recvPdaServer->stop();
            delete m_recvPdaServer;
            m_recvPdaServer = nullptr;
        }
        tcpDisconnect();
    }
    catch (...) {}
}
void DataProcess::tcpConnect() {
    plcSupplyTcpConnect(false);
    plcSendSlotTcpConnect(false);
    plcRecvUnloadTcpConnect(false);
    plcRecvSlotStatusTcpConnect(false);

}
void DataProcess::tcpDisconnect() {
    try {
        m_plc_supplyClient.disconnect();
        m_plc_sendSlotClient.disconnect();
        m_plc_unloadClient.disconnect();
        m_plc_slotStatusClient.disconnect();
        stopTcpCheckThread();
    }
    catch (...) {}

}
void DataProcess::startTcpCheckThread() {
    try {
        if (m_tcpChecking.load()) return;
        m_tcpChecking.store(true);
        m_tcpCheckThread = std::thread(&DataProcess::checkTcpConn, this);
    }
    catch (...) {}
}
void DataProcess::checkTcpConn() {
    while (m_tcpChecking.load()) {
        try {
            plcSupplyTcpConnect(true);
            plcSendSlotTcpConnect(true);
            plcRecvUnloadTcpConnect(true);
            plcRecvSlotStatusTcpConnect(true);
        }
        catch (...) {}
    }
}
void DataProcess::stopTcpCheckThread() {
    try {
        if (!m_tcpChecking.load()) return;
        m_tcpChecking.store(false);
        if (m_tcpCheckThread.joinable()) {
            m_tcpCheckThread.join();
        }
    }
    catch (...) {}
}
bool DataProcess::plcSupplyTcpConnect(bool destorySock) {
    try {
        if (m_plcSupplyMutex.try_lock()) {				//避免死锁,
            if (!m_plc_supplyClient.connectStatus("0000000000000000")) {
                if (destorySock) {
                    Logger::getInstance().Log("----[DataProcess] plcSupplyTcpConnect() Start reconnect to supply port...");
                    m_plc_supplyClient.disconnect();
                    Sleep(200);
                }
                SOCKET ok = m_plc_supplyClient.connectTo(m_plc_ip, m_plc_supply);
                if (ok == INVALID_SOCKET) {
                    Logger::getInstance().Log("----[DataProcess] plcSupplyTcpConnect() Connect to supply port failed!");
                    return false;
                }
                m_plcSupplyMutex.unlock();
                Logger::getInstance().Log("----[DataProcess] plcSupplyTcpConnect() Connect to supply port successfully!");
            }
        }
        return true;
    }
    catch (...) {}
    return false;
}
bool DataProcess::plcSendSlotTcpConnect(bool destorySock) {
    try {
        if (m_plcSendSlotMutex.try_lock())		//避免死锁
        {
            if (!m_plc_sendSlotClient.connectStatus("0000000000000000")) {
                if (destorySock) {
                    Logger::getInstance().Log("----[DataProcess] plcSendSlotTcpConnect() Start reconnect to send slot port...");
                    m_plc_sendSlotClient.disconnect();
                    Sleep(200);
                }
                SOCKET ok = m_plc_sendSlotClient.connectTo(m_plc_ip, m_plc_sendSlot);
                if (ok == INVALID_SOCKET) {
                    Logger::getInstance().Log("----[DataProcess] plcSendSlotTcpConnect() Connect to send slot port failed!");
                    return false;
                }
                m_plcSendSlotMutex.unlock();
                Logger::getInstance().Log("----[DataProcess] plcSendSlotTcpConnect() Connect to send slot port successfully!");
            }
        }
        return true;
    }
    catch (...) {}
    return false;
}
bool DataProcess::plcRecvUnloadTcpConnect(bool destorySock) {
    try {
        if (!m_plc_unloadClient.connectStatus("0000000000000000")) {
            if (destorySock) {
                Logger::getInstance().Log("----[DataProcess] plcRecvUnloadTcpConnect() Start reconnect to recv unload port...");
                m_plc_unloadClient.disconnect();
                Sleep(200);
            }
            SOCKET ok = m_plc_unloadClient.connectTo(m_plc_ip, m_plc_unload);
            if (ok == INVALID_SOCKET) {
                Logger::getInstance().Log("----[DataProcess] plcRecvUnloadTcpConnect() Connect to recv unload port failed!");
                return false;
            }
            Logger::getInstance().Log("----[DataProcess] plcRecvUnloadTcpConnect() Connect to recv unload port successfully!");
        }
        return true;
    }
    catch (...) {}
    return false;
}
bool DataProcess::plcRecvSlotStatusTcpConnect(bool destorySock) {
    try {
        if (!m_plc_slotStatusClient.connectStatus("0000000000000000")) {
            if (destorySock) {
                Logger::getInstance().Log("----[DataProcess] plcRecvSlotStatusTcpConnect() Start reconnect to recv slot status port...");
                m_plc_slotStatusClient.disconnect();
                Sleep(200);
            }
            SOCKET ok = m_plc_slotStatusClient.connectTo(m_plc_ip, m_plc_slotStatus);
            if (ok == INVALID_SOCKET) {
                Logger::getInstance().Log("----[DataProcess] plcRecvSlotStatusTcpConnect() Connect to recv slot status port failed!");
                return false;
            }
            Logger::getInstance().Log("----[DataProcess] plcRecvSlotStatusTcpConnect() Connect to recv slot status port successfully!");
        }
        return true;
    }
    catch (...) {}
    return false;
}
void DataProcess::onPLCSupplyRecv(const QByteArray& data) {											//
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCSupplyRecv() recv data: [" + dataStr + "]");
}
void DataProcess::onPLCSendSlotRecv(const QByteArray& data) {
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCSendSlotRecv() recv data: [" + dataStr + "]");
}
void DataProcess::onPLCUnLoadRecv(const QByteArray& data) {
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCUnLoadRecv() recv data: [" + dataStr + "]");
}
void DataProcess::onPLCSlotStatusRecv(const QByteArray& data) {
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCSlotStatusRecv() recv data: [" + dataStr + "]");
}
//void DataProcess::onSupplyTCPServerRecv(int clientId, const QString& message) {							//接收供包台消息, 发送给PLC, 并写入数据库
//	std::string msgStr = message.toStdString();
//	Logger::getInstance().Log("----[DataProcess] onSupplyTCPServerRecv() clientId: " + std::to_string(clientId) + ", message: [" + msgStr + "]");
//}
void DataProcess::onSupplyUDPServerRecv(const QString& message) {							//接收供包台消息, 发送给PLC, 并写入数据库
    try {
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onSupplyUDPServerRecv() message: [" + msgStr + "]");
        auto [code, weight, supply_id] = splitUdpMessage(msgStr);
        if (supply_id <= 0 || supply_id > 12) {
            return;
        }
        int supply_id_vector = supply_id - 1;
        int count = m_supplyIDToOrder[supply_id_vector];
        if(count==9999){
            m_supplyIDToOrder[supply_id_vector] = 0;
        }
        // QtConcurrent::run([this, code, weight, supply_id]() {
        // });


    }
    catch (...) {}

}
std::string DataProcess::takeMsgFromPLCSupplyQueue(size_t n) {			//只提取, 没有删除
    std::string msg = "";
    size_t _n = std::min(n, m_sendPLCSupplyDataQueue.size());
    for (size_t i = 0; i < _n; ++i) {
        std::string message = std::move(m_sendPLCSupplyDataQueue.front());
        msg += message + "#";
        m_haveSentPLCSupplyDataQueue.push_back(message);
        m_sendPLCSupplyDataQueue.pop_front();
    }
    return msg;
}
void DataProcess::sendSupplyDataToPLC(int supply_id, int supply_order) {			//STD+供包台+ID+供包台序列号+00000
    int supply_id_copy = supply_id;
    int supply_order_copy = supply_order;
    if (supply_id_copy > 99) { supply_id_copy %= 100; }
    if (supply_order_copy > 9999) { supply_order_copy %= 100; }
    std::string supply_idStr = std::to_string(supply_id);
    std::string supply_orderStr = std::to_string(supply_order);
    std::string send_msg = "";
    std::ostringstream oss;
    oss << "STD"
        << std::setw(2) << std::setfill('0') << supply_id_copy
        << "ID"
        << std::setw(4) << std::setfill('0') << supply_order_copy
        << "00000";
    std::string message = oss.str();
    if (m_sendPLCSupplyDataQueue.size() == 0) {					//队列为空, 直接发送
        m_haveSentPLCSupplyDataQueue.push_back(message);
        send_msg = message + "#";
        if (m_plcSupplyMutex.try_lock()) {						//直接发送
            m_plc_supplyClient.sendData(send_msg);
            m_plcSupplyMutex.unlock();
        }
        else {													//存储到队列中, 下次发送
            m_sendPLCSupplyDataQueue.push_back(message);
        }
    }
    else {														//队列不为空, 先存储到队列中
        m_sendPLCSupplyDataQueue.push_back(message);
        if(m_plcSupplyMutex.try_lock()) {
            send_msg = takeMsgFromPLCSupplyQueue(5);				//每次最多发送5条
            m_plc_supplyClient.sendData(send_msg);
            m_plcSupplyMutex.unlock();
        }
    }


}
void DataProcess::onPdaTCPServerRecv(int clientId, const QString& message) {
    try {
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPdaTCPServerRecv() clientId: " + std::to_string(clientId) + ", message: [" + msgStr + "]");

    }
    catch (...) {}

}
