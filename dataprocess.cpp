#include "DataProcess.h"
#include "Logger.h"
#include "UdpReceiver.h"
#include <QtConcurrent/QtConcurrent>
#include <sstream>
#include "sqlconnectionpool.h"
extern std::tuple<std::string, std::string, int> splitUdpMessage(const std::string& msg);
extern std::string getCurrentTime();
DataProcess::DataProcess()
{
    //开服务
    try {
        UdpReceiver* _udpSupplyReceiver = new UdpReceiver(3011, "0.0.0.0");             //接收拱包信息
        _udpSupplyReceiver->setCallback([this](const std::vector<uint8_t>& data, const std::string& ip, uint16_t port)
                                        {
                                            QString qtext = QString::fromStdString(std::string(data.begin(), data.end()));
                                            emit onUDPReceived(qtext);
                                        });
        connect(this, &DataProcess::onUDPReceived, this, &DataProcess::onSupplyUDPServerRecv, Qt::QueuedConnection);
        m_recvPdaServer = new QtTcpServer(this);                                        //接收PDA消息
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
    m_slotToPackageNum.resize(100);                     //格口号对应的包牌号初始化

}
void DataProcess::dataProCleanUp()
{
    try {
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
void DataProcess::onSupplyUDPServerRecv(const QString& message) {							//接收供包台消息, 发送给PLC, 并写入数据库
    try {
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onSupplyUDPServerRecv() message: [" + msgStr + "]");
        auto [code, weight, supply_id] = splitUdpMessage(msgStr);
        if (supply_id <= 0 || supply_id > 12) {
            return;
        }
        int supply_order = updateSupplyOrder(supply_id);
        QtConcurrent::run([this, code, weight, supply_id, supply_order]() {         //异步执行
            sendSupplyDataToPLC(supply_id,supply_order);
            int slot_id = insertSupplyDataToDB(code, weight, supply_id, supply_order);

        });
    }
    catch (...) {}
}
void DataProcess::requestAndSendToPLC(const std::string code,const std::string weight, int slot_id){            //若没请求一段码则进行请求, 若请求过则发送到plc中
    try{
        if(slot_id<=0){                                                    //未请求,进行请求
            QMetaObject::invokeMethod(&m_requestAPI,
                                      "requestUploadData",                 //四合一
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(code)), Q_ARG(QString, QString::fromStdString(weight))
                                      );
            QMetaObject::invokeMethod(&m_requestAPI,
                                      "requestTerminalCode",               //一段码
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(code))
                                      );
        }
        else{                                                                   //已请求, 发送至plc
            sendSlotToPLC(code,slot_id);
        }
    }
}
int DataProcess::updateSupplyOrder(int supply_id){                                         //更新每个供包台对应的序列号
    int supply_id_vector = supply_id - 1;
    int supply_order = m_supplyIDToOrder[supply_id_vector];
    if(supply_order==9999){
        supply_order = 1;
    }
    else{
        supply_order +=1;
    }
    m_supplyIDToOrder[supply_id_vector] = supply_order;
    return supply_order;
}
std::string DataProcess::takeMsgFromPLCSupplyQueue(size_t n) {			//提取并删除
    std::string msg = "";
    try{
        size_t _n = std::min(n, m_sendPLCSlotDataQueue.size());
        for (size_t i = 0; i < _n; ++i) {
            std::string message = std::move(m_sendPLCSlotDataQueue.front());
            msg += message + "#";
            m_sendPLCSlotDataQueue.pop_front();
        }
    }catch(...){}
    return msg;
}
std::string DataProcess::takeMsgFromPLCSendSlotQueue(size_t n) {			//提取并删除
    std::string msg = "";
    try{
        size_t _n = std::min(n, m_sendPLCSupplyDataQueue.size());
        for (size_t i = 0; i < _n; ++i) {
            std::string message = std::move(m_sendPLCSupplyDataQueue.front());
            msg += message + "#";
            m_haveSentPLCSupplyDataQueue.push_back(message);
            m_sendPLCSupplyDataQueue.pop_front();
        }
    }catch(...){}
    return msg;
}
int DataProcess::insertSupplyDataToDB(const std::string& code, const std::string weight, int supply_id, int supply_order){      //供包台数据插入数据库
    int slot_id = -1;
    std::string now_time = getCurrentTime();
    try{
        auto _mysql = SqlConnectionPool::instance().acquire();
        if(!_mysql){
            Logger::getInstance().Log("----[DataProcess] insertSupplyDataToDB() sql pool no free connect!");
            return slot_id;
        }
        auto check = _mysql->queryString("supply_data", "code", code, "slot_id");
        const std::vector<std::string> column = {"code",
            "weight",
            "scan_time",
            "supply_id",
            "supply_order",
            "is_supply_to_plc",
            "slot_id",
            "operate_type"
        };
        if(check&&check.has_value()){                                                                  //已存在数据库, 更新数据
            slot_id = std::stoi(*check);
            const std::vector<std::string> value = {code,
                weight,
                now_time,
                std::to_string(supply_id),
                std::to_string(supply_order),
                "0",
                std::to_string(slot_id),
                std::to_string(m_operateType)
            };
            bool ok = _mysql->updateRow("supply_data",column,value,"code",code);
            if(!ok){
                Logger::getInstance().Log("----[DataProcess] insertSupplyDataToDB() update row failed! code: ["+code+"]");
            }
        }
        else{                                                                       //未写入, 写入数据库
            const std::vector<std::string> value = {code,
                                                    weight,
                                                    now_time,
                                                    std::to_string(supply_id),
                                                    std::to_string(supply_order),
                                                    "0",
                                                    std::to_string(slot_id),
                                                    std::to_string(m_operateType)
            };
            bool ok = _mysql->insertRow("supply_data",column,value);
            if(!ok){
                Logger::getInstance().Log("----[DataProcess] insertSupplyDataToDB() insert row failed! code: ["+code+"]");
            }
        }
    }catch(...){}
    return slot_id;
}
void DataProcess::sendSupplyDataToPLC(int supply_id, int supply_order) {			//STD+供包台+ID+供包台序列号+00000
    try{
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
                Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() store m_sendPLCSupplyDataQueue data: ["+message+"]");
                return;
            }
        }
        else {														//队列不为空, 先存储到队列中
            m_sendPLCSupplyDataQueue.push_back(message);
            if(m_plcSupplyMutex.try_lock()) {
                send_msg = takeMsgFromPLCSupplyQueue(5);				//每次最多发送5条
                m_plc_supplyClient.sendData(send_msg);
                m_plcSupplyMutex.unlock();
            }
            else{
                Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() store m_sendPLCSupplyDataQueue data: ["+message+"]");
                return;
            }
        }
        Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() send message: ["+send_msg+"]");
    }catch(...){}
}
void DataProcess::sendSlotToPLC(const std::string code, int slot_id){                           //发送格口信息
    try{
        int supply_id = -1;
        int supply_order = -1;

        auto _mysql = SqlConnectionPool::instance().acquire();
        if(!_mysql){
            Logger::getInstance().Log("----[DataProcess] sendSlotToPLC() sql pool no free connect!");
            return;
        }
        auto query_id = _mysql->queryString("supply_data","code",code,"supply_id");
        if(query_id){
            supply_id = std::stoi(*query_id);
        }
        auto query_order = _mysql->queryString("supply_data","code",code,"supply_order");
        if(query_order){
            supply_order = std::stoi(*query_order);
        }
        if(supply_id!=-1 &&supply_order!=-1){       //发送plc格口信息
            std::string supply_idStr = std::to_string(supply_id);
            std::string supply_orderStr = std::to_string(supply_order);
            std::string send_msg = "";
            std::ostringstream oss;
            oss << "GKD"
                << std::setw(2) << std::setfill('0') << supply_id
                << "ID"
                << std::setw(4) << std::setfill('0') << supply_order
                << "G"
                << std::to_string(slot_id);                                             //
            std::string message = oss.str();
            if(m_sendPLCSlotDataQueue.size() == 0){                                     //队列为空
                if(m_plcSendSlotMutex.try_lock()){                                      //直接发送
                    send_msg = message+"#";
                    m_plc_sendSlotClient.sendData(send_msg);
                    m_plcSendSlotMutex.unlock();
                }
                else{                                                                   //写入到队列中
                    m_sendPLCSlotDataQueue.push_back(message);
                    Logger::getInstance().Log("----[DataProcess] sendSlotToPLC() store m_sendPLCSlotDataQueue data: ["+message+"]");
                    return;
                }
            }
            else{
                m_sendPLCSlotDataQueue.push_back(message);
                if(m_plcSendSlotMutex.try_lock()){
                    send_msg = takeMsgFromPLCSendSlotQueue(5);
                    m_plc_sendSlotClient.sendData(send_msg);
                    m_plcSendSlotMutex.unlock();
                }
                else{
                    Logger::getInstance().Log("----[DataProcess] sendSlotToPLC() store m_sendPLCSlotDataQueue data: ["+message+"]");
                    return;
                }
            }
            Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() send message: ["+send_msg+"]");
        }
    }
    catch(...){}
}
void DataProcess::onPdaTCPServerRecv(int clientId, const QString& message) {
    try {
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPdaTCPServerRecv() clientId: " + std::to_string(clientId) + ", message: [" + msgStr + "]");
    }
    catch (...) {}
}















