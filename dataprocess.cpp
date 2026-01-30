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
        _udpSupplyReceiver = new UdpReceiver(3011, "192.168.2.98");             //接收拱包信息
        _udpSupplyReceiver->setCallback([this](const std::vector<uint8_t>& data, const std::string& ip, uint16_t port)
                                        {
                                            const std::string& text = std::string(data.begin(), data.end());
                                            supplyRaw r{text};
                                            if(!supplyRing.try_push(r)){
                                                supplyRingDrops.fetch_add(1,std::memory_order_relaxed);
                                            }
                                        });
        _udpSupplyReceiver->start();
        startSupplyWorker();
        m_recvPdaServer = new QtTcpServer(this);                                        //接收PDA消息
        if (!m_recvPdaServer->start(m_recvPdaPort)) {
            Logger::getInstance().Log("----[DataProcess] DataProcess() Failed to start pda TCP server on port " + std::to_string(m_recvPdaPort));
        }
        connect(m_recvPdaServer, &QtTcpServer::messageReceived, this, &DataProcess::onPdaTCPServerRecv, Qt::QueuedConnection);
        connect(&m_requestAPI,&JTRequest::slotResult,this, &DataProcess::onTerminalCodeRecv, Qt::QueuedConnection);
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
        connect(&m_plc_supplyClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSupplyRecv);
    }
    catch (...) {}
}
void DataProcess::dbInit(){
    try{
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
                m_slotStatus[slot_id] = 0;                      //初始化格口状态, 格口都正常
                m_slotToPackage[slot_id] = "";                  //初始化格口对应的包牌号
            }
        }
        auto terminal_to_slot_arrival = _sql->readTable("terminal_to_slot_arrival");
        if(!terminal_to_slot_arrival.empty()){
            for(const auto& row:terminal_to_slot_arrival){
                if(row.size()<2) continue;
                std::string terminal_code = row[0];
                int slot_id = std::stoi(row[1]);
                arrival_terminalCodeToSlotMap[terminal_code] = slot_id;
            }
        }
        auto terminal_to_slot_depature = _sql->readTable("terminal_to_slot_depature");
        if(!terminal_to_slot_depature.empty()){
            for(const auto& row:terminal_to_slot_depature){
                if(row.size()<2) continue;
                std::string terminal_code = row[0];
                int slot_id = std::stoi(row[1]);
                depature_terminalCodeToSlotMap[terminal_code] = slot_id;
            }
        }
        m_supplyMacVector.resize(12);
        auto supply_mac = _sql->readTable("supply_config");
        if(!supply_mac.empty()){
            for (const auto& row: supply_mac){
                if(row.size()<3) continue;
                int vector_supply_id = std::stoi(row[0]) - 1;
                m_supplyMacVector[vector_supply_id] = row[2];           //初始化供包台mac地址
            }
        }
        auto slot_to_deliveryCode = _sql->readTable("slot_to_delivery");
        if(!slot_to_deliveryCode.empty()){
            for (const auto& row:slot_to_deliveryCode){
                if(row.size()<2) continue;
                int slot_id = std::stoi(row[0]);
                m_slotTodeliveryCodeMap[slot_id] = row[1];              //初始化格口对应的派件员编码， 进港
            }
        }
    }catch(...){}
}
void DataProcess::setOperateType(int type){                                             //设置操作模式
    m_operateType = type;
    m_requestAPI.setOperateType(m_operateType);
}
int DataProcess::getOperateType(){
    return m_operateType;
}
void DataProcess::startSupplyWorker(){                                                  //接收供包信息
    supplyWorkerRunning = true;
    supplyWorkerThread = std::thread([this](){
        const size_t BATCH = 256;
        supplyRaw batch[BATCH];
        Logger::getInstance().Log("----[DataProcess] startSupplyWorker() start receive supply thread!");
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
void DataProcess::dataProInit()                             //点击运行按钮
{

    // connect(&m_plc_sendSlotClient, &SocketClient::dataReceived, this, &DataProcess::onPLCSendSlotRecv);

    m_deviceRunning.store(true);
    isRecvAndSend.store(true);
    tcpConnect();
    startUnloadWorker();                        //接收下件信息线程
    startSlotStatusWorker();                    //接收格口状态线程

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
    startTcpCheckThread();

}
void DataProcess::tcpDisconnect() {                     //tcp断开连接,即点击了停止按钮
    try {
        stopUnloadWorker();
        stopSlotStatusWorker();
        m_deviceRunning.store(false);
        stopTcpCheckThread();
        m_plc_supplyClient.disconnect();
        m_plc_sendSlotClient.disconnect();
        m_plc_unloadClient.disconnect();
        m_plc_slotStatusClient.disconnect();
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
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));            //三秒检查一次
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
                    m_plcSupplyMutex.unlock();
                    return false;
                }
                Logger::getInstance().Log("----[DataProcess] plcSupplyTcpConnect() Connect to supply port successfully!");
            }
        }
        m_plcSupplyMutex.unlock();
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
                    m_plcSendSlotMutex.unlock();
                    return false;
                }
                Logger::getInstance().Log("----[DataProcess] plcSendSlotTcpConnect() Connect to send slot port successfully!");
            }
        }
        m_plcSendSlotMutex.unlock();
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
void DataProcess::onPLCSupplyRecv(const QByteArray& data) {											//plc中的供包信息返回, 用于判断是否已经上传成功, AKD01ID000100000#AKD02ID000100000#
    // isRecvAndSend.store(true);
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCSupplyRecv() recv data: [" + dataStr + "]");

}
void DataProcess::onPLCSendSlotRecv(const QByteArray& data) {                                       //plc中的单号格口信息返回
    std::string dataStr = data.toStdString();
    Logger::getInstance().Log("----[DataProcess] onPLCSendSlotRecv() recv data: [" + dataStr + "]");
}
std::vector<std::string> extract_by_split(const std::string &s) {                                   //切割字符串GKD01ID0001G1001#GKD02ID0001G1020#SU213D04ID0001G2008
    std::vector<std::string> results;
    size_t start = 0;
    while (start < s.size()) {
        size_t hash = s.find('#', start);
        if (hash == std::string::npos) hash = s.size();
        if (hash > start) {
            std::string seg = s.substr(start, hash - start); // e.g. "GKD01ID0001G1001"
            size_t posD = seg.find('D');                      // 找 D 的位置
            if (posD != std::string::npos) {
                // 找 D 后面的下一个 'G'（若无则取到段尾）
                size_t posNextG = seg.find('G', posD + 1);
                size_t len = (posNextG == std::string::npos) ? seg.size() - posD : posNextG - posD;
                results.push_back(seg.substr(posD, len));
            }
        }
        start = hash + 1;
    }
    return results;
}
void DataProcess::onPLCUnLoadRecv(const QByteArray& data) {                                             //plc中的下件发送,SU代表正常下件, FA代表未成功并且需要删除该包裹的集包记录
    try{
        std::string dataStr = data.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPLCUnLoadRecv() recv data: [" + dataStr + "]");
        sendUnloadRecvToPLC(dataStr);
        QtConcurrent::run([this,dataStr]() {                                 //异步执行
            auto msgs = extract_by_split(dataStr);                                                              //切割字符串得到序列号
            for(auto &msg : msgs){
                int supply_id = -1;                                                                             //从消息中获取供包台号
                if(msg.size()>=3 && msg[0] == 'D'){
                    std::string num = msg.substr(1,2);
                    supply_id = std::stoi(num);
                }
                if(supply_id<1||supply_id>12) return;
                std::string supply_mac = m_supplyMacVector[supply_id - 1];
                auto it = m_msgToCodeMap.find(msg);
                if(it!=m_msgToCodeMap.end())
                {
                    const std::string& code = m_msgToCodeMap[msg];                                              //获取单号
                    //清除队列中存储的序列号与单号的键值对
                    auto _sql = SqlConnectionPool::instance().acquire();
                    std::string weight = "0.5";                                                                 //初始化重量
                    if(_sql){
                        auto db_weight = _sql->queryString("supply_data","code",code,"weight");
                        if(db_weight)
                        {
                            weight = *db_weight;
                        }
                    }

                    // int slot_id = m_codeToSlotMap[code];                                                        //获取格口号
                    int slot_id = -1;
                    auto slot_id_map = m_codeToSlotMap.find(code);
                    if(slot_id_map!=m_codeToSlotMap.end()){                                                     //存在列表中
                        slot_id = slot_id_map->second;
                        m_codeToSlotMap.erase(code);                                                            //清除队列中单号与格口号的键值对
                    }
                    else{                                                                                       //队列中不存在
                        if(_sql){
                            auto db_slot_id = _sql->queryString("supply_data","code",code,"slot_id");
                            if(db_slot_id){
                                slot_id = std::stoi(*db_slot_id);
                            }
                        }
                    }
                    if(m_operateType == 1)                                                                      //进港,不需要集包，只需要出仓扫描
                    {
                        auto it = m_slotTodeliveryCodeMap.find(slot_id);
                        if(it!=m_slotTodeliveryCodeMap.end()){
                            const std::string& deliveryCode = it->second;
                            QMetaObject::invokeMethod(&m_requestAPI,                                                    //出仓扫描
                                                      "outboundScanning",
                                                      Qt::QueuedConnection,
                                                      Q_ARG(QString, QString::fromStdString(code)), Q_ARG(QString, QString::fromStdString(deliveryCode)));
                        }
                    }
                    else if(m_operateType == 2){                                                                        //出港，需要集包
                        auto _it = m_slotToPackage.find(slot_id);
                        if(_it!=m_slotToPackage.end()){
                            const std::string& packageNum = _it->second;                                                //获取包牌号
                            QMetaObject::invokeMethod(&m_requestAPI,                                                    //建包
                                                      "requestBuildOneByOne",
                                                      Qt::QueuedConnection,
                                                      Q_ARG(QString, QString::fromStdString(code)), Q_ARG(QString, QString::fromStdString(packageNum)));
                        }
                    }
                    QMetaObject::invokeMethod(&m_requestAPI,                                                    //小件回传，进出港都需要
                                              "requestSmallData",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString,QString::fromStdString(code)),
                                              Q_ARG(QString,QString::fromStdString(weight)),
                                              Q_ARG(int,m_operateType),
                                              Q_ARG(int, slot_id),
                                              Q_ARG(int, supply_id),
                                              Q_ARG(QString,QString::fromStdString(supply_mac)));
                    m_msgToCodeMap.erase(msg);
                }
            }
        });
                                                                                //发送回复消息
    }
    catch(...){}
}
void DataProcess::onPLCSlotStatusRecv(const QByteArray& data) {                                     //plc中的格口状态返回, 把格口号对应的包号置为空
    try{
        std::string dataStr = data.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPLCSlotStatusRecv() recv data: [" + dataStr + "]");
    }
    catch(...){}
}
void DataProcess::onSupplyUDPServerRecv(const std::string& message) {							//接收供包台消息, 发送给PLC, 并写入数据库, 判断是否有请求格口, 若没有进行格口请求.(同时需要判断进出港件)
    try {
        Logger::getInstance().Log("----[DataProcess] onSupplyUDPServerRecv() message: [" + message + "]");
        if(!m_deviceRunning.load()) return;                                                     //设备停止状态
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
        m_msgToCodeMap[order_msg] = code;                                                                   //写入队列中，序列号对应单号
        m_codeToMsgMap[code] = order_msg;                                                                   //单号对应序列号
        QtConcurrent::run([this, code, weight, supply_id, supply_order]() {                                 //异步执行
            sendSupplyDataToPLC(supply_id,supply_order);
            int slot_id = insertSupplyDataToDB(code, weight, supply_id, supply_order);
            requestAndSendToPLC(code, weight, slot_id);
        });
    }
    catch (...) {}
}
void DataProcess::requestAndSendToPLC(const std::string& code,const std::string& weight, int slot_id){            //若没请求一段码则进行请求, 若请求过则发送到plc中, 需要判断是进港还是出港
    try{
        if(m_operateType == 1){                                                 //进港
            if(slot_id<=0){                                                     //格口未请求
                QMetaObject::invokeMethod(&m_requestAPI,
                                          "requestTerminalCode",                //一段码
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, QString::fromStdString(code))
                                          );
            }else{                                                               //已请求, 发送至plc
                m_codeToSlotMap[code] = slot_id;                                    //写入队列

                auto it = m_codeToMsgMap.find(code);
                if(it!=m_codeToMsgMap.end()){
                    const std::string& order_msg = it->second;
                    sendSlotToPLC(code,order_msg,slot_id);
                    m_codeToMsgMap.erase(code);
                }else{
                    sendSlotToPLC(code,"",slot_id);
                }
            }
            QMetaObject::invokeMethod(&m_requestAPI,
                                      "unloadToPieces",                     //卸车到件
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(code)),
                                      Q_ARG(QString, QString::fromStdString(weight))
                                      );
        }else if(m_operateType == 2){                                           //出港
            QMetaObject::invokeMethod(&m_requestAPI,
                                      "requestUploadData",                  //四合一
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(code)), Q_ARG(QString, QString::fromStdString(weight))
                                      );
            if(slot_id<=0){                                                     //未请求,进行请求
                QMetaObject::invokeMethod(&m_requestAPI,
                                          "requestTerminalCode",                //一段码
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, QString::fromStdString(code))
                                          );
            }
            else{                                                               //已请求, 发送至plc
                m_codeToSlotMap[code] = slot_id;                                //写入队列, 供onPLCUnLoadRecv 寻找格口号使用
                auto it = m_codeToMsgMap.find(code);
                if(it!=m_codeToMsgMap.end()){
                    const std::string& order_msg = it->second;
                    sendSlotToPLC(code,order_msg,slot_id);
                    m_codeToMsgMap.erase(code);
                }else{
                    sendSlotToPLC(code,"",slot_id);
                }
            }
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
std::string DataProcess::takeMsgFromPLCSendSlotQueue(size_t n) {                              //从待发送格口队列中提取最多五个并删除
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

std::string DataProcess::takeMsgFromPLCSupplyQueue(size_t n) {                                  //从待供包队列中提取最多五个并删除
    std::string msg = "";
    try{
        size_t _n = std::min(n, m_sendPLCSupplyDataQueue.size());
        for (size_t i = 0; i < _n; ++i) {
            std::string message = std::move(m_sendPLCSupplyDataQueue.front());
            msg += message + "#";
            Logger::getInstance().Log("----[test] message : ["+msg+"]");
            m_sendPLCSupplyDataQueue.pop_front();
        }
        return msg;
    }catch(...){}
    return "0000000000000000";
}
int DataProcess::insertSupplyDataToDB(const std::string& code, const std::string& weight, int supply_id, int supply_order){              //上件单号插入数据库, 返回格口号
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
        else{                                                                                           //未写入, 写入数据库
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
void DataProcess::onTerminalCodeRecv(const QString& code, const std::string& terminalCode, int order_type, int interceptor){             //接收到格口信息,发送给plc， 同时写入数据库
    try{
        std::string code_copy = code.toStdString();
        int slot_id = -1;
        Logger::getInstance().Log("----[DataProcess] onTerminalCodeRecv() operate type:["+std::to_string(m_operateType)
                                  +"],code,["+code_copy
                                  +"],terminal_code:["+terminalCode
                                  +"],order_type:["+std::to_string(order_type)+"]");
        if(interceptor == 2)                                                            //不是拦截件，是否拦截件，1-是 2-否
        {
            if(order_type == 1)                                                       //正常件,发送对应格口号至plc中
            {
                if(m_operateType == 1)                                                  //进港
                {
                    auto it = arrival_terminalCodeToSlotMap.find(terminalCode);                     //查找map中的格口号， 并写入数据库
                    if(it!=arrival_terminalCodeToSlotMap.end()){
                        slot_id = it->second;
                    }
                    else{
                        slot_id = arrival_terminalCodeToSlotMap["异常格"];
                    }
                }
                else{                                                                   //出港
                    auto it = depature_terminalCodeToSlotMap.find(terminalCode);
                    if(it!=depature_terminalCodeToSlotMap.end()){
                        slot_id = it->second;
                    }
                    else{
                        slot_id = depature_terminalCodeToSlotMap["异常格"];
                    }
                }

            }else{                                                                      //异常件,发送异常格口至plc中
                if(m_operateType == 1)                                                  //进港
                {
                    slot_id = arrival_terminalCodeToSlotMap["异常格"];
                }
                else                                                                    //出港
                {
                    slot_id = depature_terminalCodeToSlotMap["异常格"];
                }
            }
        }
        else if(interceptor == 1)                                                       //拦截
        {
            if(m_operateType == 1) slot_id = arrival_terminalCodeToSlotMap["拦截件"];
            else slot_id = depature_terminalCodeToSlotMap["拦截件"];
        }
        m_codeToSlotMap[code_copy] = slot_id;                                               //写入队列，单号对应的格口号
        auto _it = m_codeToMsgMap.find(code_copy);
        if(_it!=m_codeToMsgMap.end()){
            const std::string& order_msg = _it->second;
            sendSlotToPLC(code_copy, order_msg, slot_id);
            m_codeToMsgMap.erase(code_copy);                                                //清除队列中存储的单号与序列号键值对
        }
        else{
            sendSlotToPLC(code_copy,"",slot_id);
        }
        QtConcurrent::run([this, code_copy,slot_id]() {                                     //异步执行,写入数据库
            auto _sql = SqlConnectionPool::instance().acquire();
            if(_sql){
                _sql->updateValue("supply_data","code",code_copy,"slot_id",std::to_string(slot_id));
            }
        });
    }catch(...){}
}
void DataProcess::sendUnloadRecvToPLC(const std::string& data){
    try{
        if(m_plcUnloadMutex.try_lock()){
            m_plc_unloadClient.sendData(data);
            m_plcUnloadMutex.unlock();
            Logger::getInstance().Log("----[DataProcess] sendUnloadRecvToPLC() send message: ["+data+"]");
        }
    }catch(...){}
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
            send_msg = message + "#";
            if (m_plcSupplyMutex.try_lock()) {						//直接发送
                m_plc_supplyClient.sendData(send_msg);
                m_plcSupplyMutex.unlock();
                Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() send message: ["+send_msg+"]");
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
                Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() send message: ["+send_msg+"]");
            }
            else{
                Logger::getInstance().Log("----[DataProcess] sendSupplyDataToPLC() store m_sendPLCSupplyDataQueue data: ["+message+"]");
                return;
            }
        }

    }catch(...){}
}
void DataProcess::sendSlotToPLC(const std::string& code, const std::string& order_msg, int slot_id){                           //发送格口信息给plc
    try{
        std::string send_msg = "";
        std::string message = "";
        if(order_msg != ""){                    //有现成序列号
            std::ostringstream oss;
            oss << "GK"
                << order_msg
                << "G"
                << std::to_string(slot_id);
            message = oss.str();
        }
        else{                                   //若没有， 使用数据库中序列号
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
            message = oss.str();
        }
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
        Logger::getInstance().Log("----[DataProcess] sendSlotToPLC() send message: ["+send_msg+"]");
    }
    catch(...){}
}

static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
}
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
}
static inline void trim(std::string &s) { ltrim(s); rtrim(s); }

/*
 * 解析函数：
 * 返回 true 表示解析成功，out_code/out_area 会被填充。
 * 返回 false 表示解析失败（格式不符合预期）。
 */
bool parse_cb_line(const std::string &input, std::string &out_code, std::string &out_area) {
    out_code.clear();
    out_area.clear();

    // 找到第一个 ':'（例如 CB-1:...）
    size_t pos = input.find(':');
    if (pos == std::string::npos) return false;

    // 取 ':' 之后的子串
    size_t start = pos + 1;
    if (start >= input.size()) return false;

    // 第一个字段到第一个逗号
    size_t comma1 = input.find(',', start);
    if (comma1 == std::string::npos) return false;
    out_code = input.substr(start, comma1 - start);
    trim(out_code);

    // 第二个字段（逗号1之后到逗号2）
    size_t next = comma1 + 1;
    size_t comma2 = input.find(',', next);
    if (comma2 == std::string::npos) {
        // 如果没有第二个逗号，就取到末尾
        out_area = input.substr(next);
    } else {
        out_area = input.substr(next, comma2 - next);
    }
    trim(out_area);

    // 简单校验非空
    return !out_code.empty() && !out_area.empty();
}
void DataProcess::onPdaTCPServerRecv(int clientId, const QString& message) {                        //接收pda巴枪, 需要测试pda巴枪数据,使用connect信号槽
    try {
        std::string slot_id = "";
        std::string new_package = "";
        std::string msgStr = message.toStdString();
        Logger::getInstance().Log("----[DataProcess] onPdaTCPServerRecv() clientId: " + std::to_string(clientId) + ", message: [" + msgStr + "]");
        if(!m_deviceRunning.load())                                                                 //运行为停止状态
        {
            return;
        }
        if(parse_cb_line(msgStr,new_package,slot_id))                                               //解析成功
        {
            int slot_id_int = std::stoi(slot_id);
            m_slotToPackage[slot_id_int] = new_package;

        }
    }
    catch (...) {}
}















