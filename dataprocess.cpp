#include "DataProcess.h"
#include "Logger.h"
#include <QtConcurrent/QtConcurrent>
#include <sstream>
#include "sqlconnectionpool.h"
extern std::tuple<std::string, std::string, int> splitUdpMessage(const std::string& msg);
extern std::string getCurrentTime();
DataProcess::DataProcess()                      //开服务,并初始化程序中的资源以及设置
{
    try {
        _udpSupplyReceiver = new UdpReceiver(3011, "0.0.0.0");             //接收拱包信息
        _udpSupplyReceiver->setCallback([this](const std::vector<uint8_t>& data, const std::string& ip, uint16_t port)
                                        {
                                            const std::string& text = std::string(data.begin(), data.end());
                                            supplyRaw r{text};
                                            if(!supplyRing.try_push(r)){
                                                supplyRingDrops.fetch_add(1,std::memory_order_relaxed);
                                            }
                                        });
        startSupplyWorker();
        m_recvPdaServer = new QtTcpServer(this);                                        //接收PDA消息
        if (!m_recvPdaServer->start(m_recvPdaPort)) {
            Logger::getInstance().Log("----[DataProcess] DataProcess() Failed to start pda TCP server on port " + std::to_string(m_recvPdaPort));
        }
        connect(m_recvPdaServer, &QtTcpServer::messageReceived, this, &DataProcess::onPdaTCPServerRecv, Qt::QueuedConnection);
        m_supplyIDToOrder.resize(12);                       //初始化每个供包台的序列号
        for(int i = 0; i<12;++i){
            m_supplyIDToOrder[i] = 0;
        }
        dbInit();
        m_plc_unloadClient.onRawData = [this](const QByteArray& data){
            receiveRaw r{data};
            if(!unloadRing.try_push(r)){
                unloadRingDrops.fetch_add(1,std::memory_order_relaxed);
            }
        };
        m_plc_slotStatusClient.onRawData = [this](const QByteArray& data){
            receiveRaw r{data};
            if(!slotStatusRing.try_push(r)){
                slotStatusRingDrops.fetch_add(1,std::memory_order_relaxed);
            }
        };
    }
    catch (...) {}
}
void DataProcess::dbInit(){
    auto _sql = SqlConnectionPool::instance().acquire();
    if(!_sql){
        Logger::getInstance().Log("----[DataProcess] dbInit() failed to acquire database connection");
        return;
    }
    auto slot_config = _sql->readTable("slot_config");
    if(!slot_config.empty()){
        for (const auto& row: slot_config){
            if(row.size()<1) continue;
            int slot_id = std::stoi(row[0]);
            m_slotStatus[slot_id] = 0;                      //初始化格口, 格口都正常
            m_slotToPackage[slot_id] = "";                  //初始化格口对应的包牌号
        }
    }
    auto terminal_to_slot = _sql->readTable("terminal_to_slot");
    if(!terminal_to_slot.empty()){
        for(const auto& row:terminal_to_slot){
            if(row.size()<2) continue;
            std::string terminal_code = row[0];
            int slot_id = std::stoi(row[1]);
            m_terminalCodeToSlot[terminal_code] = slot_id;
        }
    }
}
void DataProcess::startSupplyWorker(){                                                  //接收供包信息
    supplyWorkerRunning = true;
    supplyWorkerThread = std::thread([this](){
        const size_t BATCH = 256;
        supplyRaw batch[BATCH];
        while (supplyWorkerRunning) {
            size_t n = supplyRing.pop_bulk(batch, BATCH);
            if(n == 0){                                         //空闲
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            for (size_t i =0;i<n;++i){
                const std::string& data = batch[i].data;
                onSupplyUDPServerRecv(data);
            }
        }
    });
}
void DataProcess::stopSupplyWorker(){                                                   //停止接收
    supplyWorkerRunning = false;
    if(supplyWorkerThread.joinable()) supplyWorkerThread.join();
}
void DataProcess::dataProInit()
{
    // connect(&m_plc_supplyClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSupplyRecv);
    // connect(&m_plc_sendSlotClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSendSlotRecv);
    tcpConnect();
    startUnloadWorker();
    startSlotStatusWorker();
}
void DataProcess::startUnloadWorker(){
    unloadWorkerRunning = true;
    unloadWorkerThread = std::thread([this](){
        const size_t BATCH = 256;
        receiveRaw batch[BATCH];
        while(unloadWorkerRunning){
            size_t n = unloadRing.pop_bulk(batch,BATCH);
            if(n==0){
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            for(size_t i = 0; i<n; ++i){
                const QByteArray& data = batch[i].data;
                onPLCUnLoadRecv(data);
            }
        }
    });
}
void DataProcess::stopUnloadWorker(){
    unloadWorkerRunning = false;
    if(unloadWorkerThread.joinable()) unloadWorkerThread.join();
}
void DataProcess::startSlotStatusWorker(){
    slotStatusWorkerRunning = true;
    slotStatusWorkerThread = std::thread([this](){
        const size_t BATCH = 256;
        receiveRaw batch[BATCH];
        while (slotStatusWorkerRunning) {
            size_t n = slotStatusRing.pop_bulk(batch,BATCH);
            if(n==0){
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            for(size_t i = 0; i<n;++i){
                const QByteArray& data = batch[i].data;
                onPLCSlotStatusRecv(data);
            }
        }
    });
}
void DataProcess::stopSlotStatusWorker(){
    slotStatusWorkerRunning = false;
    if(slotStatusWorkerThread.joinable()) slotStatusWorkerThread.join();
}
void DataProcess::dataProCleanUp()                      //清理所有资源
{
    try {
        stopSupplyWorker();
        stopUnloadWorker();
        stopSlotStatusWorker();
        _udpSupplyReceiver->stop();
        if (m_recvPdaServer) {
            m_recvPdaServer->stop();
            delete m_recvPdaServer;
            m_recvPdaServer = nullptr;
        }
        tcpDisconnect();
    }
    catch (...) {}
}
void DataProcess::tcpConnect() {                        //tcp连接
    plcSupplyTcpConnect(false);
    plcSendSlotTcpConnect(false);
    plcRecvUnloadTcpConnect(false);
    plcRecvSlotStatusTcpConnect(false);

}
void DataProcess::tcpDisconnect() {                     //tcp断开连接
    try {
        m_plc_supplyClient.disconnect();
        m_plc_sendSlotClient.disconnect();
        m_plc_unloadClient.disconnect();
        m_plc_slotStatusClient.disconnect();
        stopTcpCheckThread();
    }
    catch (...) {}

}
void DataProcess::startTcpCheckThread() {               //开启tcp连接的检查线程
    try {
        if (m_tcpChecking.load()) return;
        m_tcpChecking.store(true);
        m_tcpCheckThread = std::thread(&DataProcess::checkTcpConn, this);
    }
    catch (...) {}
}
void DataProcess::checkTcpConn() {                      //检查tcp连接
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
void DataProcess::stopTcpCheckThread() {                //停止检查线程
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
        if (m_plcSupplyMutex.try_lock()) {				//避免死锁
            if (!m_plc_supplyClient.connectStatus("0000000000000000")) {
                if (destorySock) {
                    Logger::getInstance().Log("----[DataProcess] plcSupplyTcpConnect() Start reconnect to supply port...");
                    m_plc_supplyClient.disconnect();
                    Sleep(200);
                }
                SOCKET ok = m_plc_supplyClient.connectTo(m_plc_ip, m_plc_supply,false);                     //不开启接收线程
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
                SOCKET ok = m_plc_sendSlotClient.connectTo(m_plc_ip, m_plc_sendSlot,false);                     //不开启接收线程
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
// void DataProcess::onPLCSupplyRecv(const QByteArray& data) {											//plc中的供包信息返回, 用于判断是否已经上传成功, AKD01ID000100000#AKD02ID000100000#
//     std::string dataStr = data.toStdString();
//     Logger::getInstance().Log("----[DataProcess] onPLCSupplyRecv() recv data: [" + dataStr + "]");

// }
// void DataProcess::onPLCSendSlotRecv(const QByteArray& data) {                                       //plc中的单号格口信息返回
//     std::string dataStr = data.toStdString();
//     Logger::getInstance().Log("----[DataProcess] onPLCSendSlotRecv() recv data: [" + dataStr + "]");
// }
void DataProcess::onPLCUnLoadRecv(const QByteArray& data) {                                             //plc中的下件发送,SU代表正常下件, FA代表未成功并且需要删除该包裹的集包记录
    try{
        std::string dataStr = data.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPLCUnLoadRecv() recv data: [" + dataStr + "]");
        std::string id_msg = "";                                                                        //供包台号以及序列号
        if(dataStr.find(unload_fail) == std::string::npos){                                             //下件成功, 进行集包请求
            const std::string& code = m_msgToCodeMap[id_msg];                                           //获取单号
            int slot_id = m_codeToSlotMap[code];                                                        //获取格口号
            const std::string& packageNume = m_slotToPackage[slot_id];                                  //获取包牌号
            // QMetaObject::invokeMethod(&m_requestAPI,
            //                           "")
        }
    }
    catch(...){}
}
void DataProcess::onPLCSlotStatusRecv(const QByteArray& data) {                                     //plc中的格口状态返回, 用于判断pda巴枪绑定包牌
    try{
        std::string dataStr = data.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPLCSlotStatusRecv() recv data: [" + dataStr + "]");
        if(dataStr.find("SK") !=std::string::npos){                                                 //格口信息, 需要实际查看

        }
    }
    catch(...){}
}
void DataProcess::onSupplyUDPServerRecv(const std::string& message) {							//接收供包台消息, 发送给PLC, 并写入数据库, 判断是否有请求格口, 若没有进行格口请求.(同时需要判断进出港件)
    try {
        Logger::getInstance().Log("----[DataProcess] onSupplyUDPServerRecv() message: [" + message + "]");
        auto [code, weight, supply_id] = splitUdpMessage(message);
        if (supply_id <= 0 || supply_id > 12) {
            return;
        }
        int supply_order = updateSupplyOrder(supply_id);
        std::ostringstream oss;
        oss << "D"
            << std::setw(2) << std::setfill('0') << supply_id
            << "ID"
            << std::setw(4) << std::setfill('0') << supply_order;
        std::string order_msg = oss.str();
        m_msgToCodeMap[order_msg] = code;                                                                   //写入队列中
        sendSupplyDataToPLC(supply_id,supply_order);
        QtConcurrent::run([this, code, weight, supply_id, supply_order]() {                                 //异步执行
            int slot_id = insertSupplyDataToDB(code, weight, supply_id, supply_order);
            requestAndSendToPLC(code, weight, slot_id);
        });
    }
    catch (...) {}
}
void DataProcess::requestAndSendToPLC(const std::string& code,const std::string& weight, int slot_id){            //若没请求一段码则进行请求, 若请求过则发送到plc中, 需要判断是进港还是出港
    try{
        if(m_operateType == 1){                                             //进港
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
        }else if(m_operateType == 2){                                       //出港

        }

    }
    catch(...){}
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
std::string DataProcess::takeMsgFromPLCSupplyQueue(size_t n) {                              //从待供包队列中提取最多五个并删除
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
std::string DataProcess::takeMsgFromPLCSendSlotQueue(size_t n) {                            //从待发送格口队列中提取最多五个并删除
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
int DataProcess::insertSupplyDataToDB(const std::string& code, const std::string weight, int supply_id, int supply_order){              //上件单号插入数据库, 返回格口号
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
void DataProcess::sendSupplyDataToPLC(int supply_id, int supply_order) {                            //STD+供包台+ID+供包台序列号+00000
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
void DataProcess::sendSlotToPLC(const std::string code, int slot_id){                           //发送格口信息给plc
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
void DataProcess::onPdaTCPServerRecv(int clientId, const QString& message) {                        //接收pda巴枪, 需要测试pda巴枪数据,使用connect信号槽
    try {
        int slot_id = -1;
        std::string new_package = "";
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPdaTCPServerRecv() clientId: " + std::to_string(clientId) + ", message: [" + msgStr + "]");
        //m_slotToPackage
        if(m_slotStatus[slot_id] == 1)                                                              //锁格状态
        {
            m_slotToPackage[slot_id] = new_package;                                                 //更新包牌号码
        }
    }
    catch (...) {}
}















