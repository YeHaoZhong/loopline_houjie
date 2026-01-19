#ifndef DATAPROCESS_H
#define DATAPROCESS_H
#include <QObject>
#include "TcpSocketClient.h"
#include "QtTcpServer.h"
#include "jtrequest.h"
#include <shared_mutex>
#include <deque>
class DataProcess : public QObject
{
    Q_OBJECT
public:
    DataProcess();
    void dataProInit();
    void dataProCleanUp();

private:
    bool plcSupplyTcpConnect(bool destorySock);
    bool plcSendSlotTcpConnect(bool destorySock);
    bool plcRecvUnloadTcpConnect(bool destorySock);
    bool plcRecvSlotStatusTcpConnect(bool destorySock);
    void tcpConnect();
    void tcpDisconnect();
    void checkTcpConn();
    void startTcpCheckThread();
    void stopTcpCheckThread();

    void sendSupplyDataToPLC(int supply_id, int supply_order);              //STD+供包台+ID+供包台序列号+00000
    void sendSlotToPLC(const std::string code,int slot_id);
    int insertSupplyDataToDB(const std::string& code,                       //插入数据库, 若返回-1代表还未请求格口号
                             const std::string weight,
                             int supply_id,
                             int supply_order);
    std::string takeMsgFromPLCSupplyQueue(size_t n);
    std::string takeMsgFromPLCSendSlotQueue(size_t n);
    int updateSupplyOrder(int supply_id);                                  //更新每个供包台对应的序列号
    void requestAndSendToPLC(const std::string code, const std::string weight, int slot_id);

private:
    std::shared_mutex m_plcSupplyMutex;
    SocketClient m_plc_supplyClient;        //2011 socket连接
    std::shared_mutex m_plcSendSlotMutex;
    SocketClient m_plc_sendSlotClient;      //2012 socket连接
    std::shared_mutex m_plcUnloadMutex;
    SocketClient m_plc_unloadClient;        //2013 socket连接
    SocketClient m_plc_slotStatusClient;    //2014 socket连接
    std::string m_plc_ip = "192.168.2.10";
    int m_plc_supply = 2011;					//发送上件端口
    int m_plc_sendSlot = 2012;					//发送格口端口
    int m_plc_unload = 2013;					//接收下件端口
    int m_plc_slotStatus = 2014;				//接收格口状态端口
    std::deque<std::string> m_sendPLCSupplyDataQueue;       //发送供包台数据队列
    std::deque<std::string> m_haveSentPLCSupplyDataQueue;   //已发送供包台数据队列
    std::deque<std::string> m_sendPLCSlotDataQueue;         //发送格口数据队列
    QtTcpServer* m_recvPdaServer = nullptr;					//pda巴枪服务器
    int m_recvPdaPort = 3021;
    std::atomic<bool> m_tcpChecking{ false };				//tcp客户端心跳线程
    std::thread m_tcpCheckThread;

    int m_operateType = 1;                                    //操作类型, 1为进港, 2为出港

    std::atomic<int> m_msgOrder{ 1 };						//消息序列号
    std::vector<int> m_supplyIDToOrder;						//供包台号对应的上件序列号
    std::unordered_map<int, std::string> m_orderToCode;		//序列号对应的单号
    std::vector<std::string> m_slotToPackageNum;			//格口号对应的包号

    JTRequest m_requestAPI;                                 //请求类

signals:
    void onUDPReceived(const QString& message);
private slots:
    void onPLCSupplyRecv(const QByteArray& data);       //2011
    void onPLCSendSlotRecv(const QByteArray& data);     //2012
    void onPLCUnLoadRecv(const QByteArray& data);       //2013
    void onPLCSlotStatusRecv(const QByteArray& data);  //2014
    void onSupplyUDPServerRecv(const QString& message);
    void onPdaTCPServerRecv(int clientId, const QString& message);
};
#endif // DATAPROCESS_H
