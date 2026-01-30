#ifndef DATAPROCESS_H
#define DATAPROCESS_H
#include <QObject>
#include "TcpSocketClient.h"
#include "QtTcpServer.h"
#include "jtrequest.h"
#include <shared_mutex>
#include <deque>
#include "spsc_ring.h"
#include "UdpReceiver.h"
#include "unordered_map"
class DataProcess : public QObject
{
    Q_OBJECT
public:
    DataProcess();
    void dataProInit();
    void dataProCleanUp();                                                  //关闭所有资源
    void tcpDisconnect();                                                   //断开连接
    void setOperateType(int type);
    int getOperateType();                                                  //得到当前操作模式

private:
    bool plcSupplyTcpConnect(bool destorySock);
    bool plcSendSlotTcpConnect(bool destorySock);
    bool plcRecvUnloadTcpConnect(bool destorySock);
    bool plcRecvSlotStatusTcpConnect(bool destorySock);
    void dbInit();
    void tcpConnect();
    void checkTcpConn();
    void startTcpCheckThread();
    void stopTcpCheckThread();

    void sendSupplyDataToPLC(int supply_id, int supply_order);              //STD+供包台+ID+供包台序列号+00000
    void sendSlotToPLC(const std::string& code, const std::string& order_msg, int slot_id);
    int insertSupplyDataToDB(const std::string& code,                       //插入数据库, 若返回-1代表还未请求格口号
                             const std::string& weight,
                             int supply_id,
                             int supply_order);
    std::string takeMsgFromPLCSupplyQueue(size_t n);
    std::string takeMsgFromPLCSendSlotQueue(size_t n);
    int updateSupplyOrder(int supply_id);                                  //更新每个供包台对应的序列号
    void requestAndSendToPLC(const std::string& code, const std::string& weight, int slot_id);

private:
    std::shared_mutex m_plcSupplyMutex;
    SocketClient m_plc_supplyClient;        //2061 socket连接
    std::shared_mutex m_plcSendSlotMutex;
    SocketClient m_plc_sendSlotClient;      //2062 socket连接
    std::shared_mutex m_plcUnloadMutex;
    SocketClient m_plc_unloadClient;        //2063 socket连接
    SocketClient m_plc_slotStatusClient;    //2064 socket连接
    // std::string m_plc_ip = "192.168.2.10";
    std::string m_plc_ip = "192.168.2.98";
    int m_plc_supply = 2061;                                                //发送上件端口
    int m_plc_sendSlot = 2062;                                              //发送格口端口
    int m_plc_unload = 2063;                                                //接收下件端口
    int m_plc_slotStatus = 2064;                                            //接收格口状态端口

    std::atomic<bool> isRecvAndSend{true};                                  //发送拱包信息到plc后, 变为false, 需要等到接收回plc的信息后才可以再次发送
    std::deque<std::string> m_sendPLCSupplyDataQueue;                       //发送供包台数据队列
    std::deque<std::string> m_haveSentPLCSupplyDataQueue;                   //已发送供包台数据队列
    std::deque<std::string> m_sendPLCSlotDataQueue;                         //发送格口数据队列
    QtTcpServer* m_recvPdaServer = nullptr;                                 //pda巴枪服务器
    int m_recvPdaPort = 3021;                                               //pda巴枪端口
    std::atomic<bool> m_tcpChecking{ false };                               //tcp客户端心跳线程
    std::thread m_tcpCheckThread;

    int m_operateType = 0;                                                  //操作类型, 1为进港, 2为出港

    std::atomic<int> m_msgOrder{ 1 };                                       //消息序列号
    std::vector<int> m_supplyIDToOrder;                                     //供包台号对应的上件序列号

    JTRequest m_requestAPI;                                                 //请求类

    //格口配置
    std::unordered_map<std::string, int> depature_terminalCodeToSlotMap;      //一段码对应的格口号，出港
    std::unordered_map<std::string, int> arrival_terminalCodeToSlotMap;       //三段码对应格口号， 进港
    std::unordered_map<int, int> m_slotStatus;                      //格口状态, 0 = 正常, 1 = 锁格
    std::unordered_map<int, std::string> m_slotToPackage;           //格口对应的包牌号
    std::unordered_map<int, std::string> m_slotTodeliveryCodeMap;               //格口对应派件员编码， 进港


    //接收读码平台消息
    UdpReceiver* _udpSupplyReceiver;
    std::unordered_map<std::string, std::string> m_msgToCodeMap;               //货物在线体上的周期, 使用供包台号以及序列号对应上单号
    std::unordered_map<std::string, std::string> m_codeToMsgMap;               //单号对应供包台以及序列号
    std::unordered_map<std::string, int> m_codeToSlotMap;                       //单号所对应的格口号
    struct supplyRaw {std::string data;};
    SpscRing<supplyRaw> supplyRing{1<<14};
    std::atomic<uint64_t> supplyRingDrops{0};
    std::thread supplyWorkerThread;
    bool supplyWorkerRunning = false;
    void startSupplyWorker();
    void stopSupplyWorker();
    void onSupplyUDPServerRecv(const std::string& message);

    struct receiveRaw
    {
        QByteArray data;
    };
    //下件接收
    SpscRing<receiveRaw> unloadRing{1<<14};
    std::atomic<uint64_t> unloadRingDrops{0};
    std::thread unloadWorkerThread;
    bool unloadWorkerRunning = false;
    void startUnloadWorker();
    void stopUnloadWorker();
    void onPLCUnLoadRecv(const QByteArray& data);       //2013
    void sendUnloadRecvToPLC(const std::string& data);
    std::string unload_fail = "FA";                     //失败字样

    //格口状态接收, 用于更换包牌
    SpscRing<receiveRaw> slotStatusRing{1<<14};
    std::atomic<uint64_t> slotStatusRingDrops{0};
    std::thread slotStatusWorkerThread;
    bool slotStatusWorkerRunning = false;
    void startSlotStatusWorker();
    void stopSlotStatusWorker();
    void onPLCSlotStatusRecv(const QByteArray& data);  //2014

    std::atomic<bool> m_deviceRunning{false};                       //开启运行后， 代表线体起来
    std::vector<std::string> m_supplyMacVector;

signals:
    void onUDPReceived(const QString& message);
private slots:
    void onPLCSupplyRecv(const QByteArray& data);       //2011
    void onPLCSendSlotRecv(const QByteArray& data);     //2012
    void onPdaTCPServerRecv(int clientId, const QString& message);
    void onTerminalCodeRecv(const QString& code, const std::string& terminalCode, int order_type, int interceptor);
};
#endif // DATAPROCESS_H
