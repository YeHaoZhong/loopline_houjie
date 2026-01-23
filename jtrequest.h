#ifndef JTREQUEST_H
#define JTREQUEST_H

#include "Logger.h"
#include <QDateTime>
#include <QQueue>
#include <QTimer>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QHash>
#include <QMutex>
#include <QJsonObject>
#include <atomic>

struct PendingInfo {
    std::string weight;
    int car_id;
    int supply_id;
    QDateTime ts;   // 请求时间，用于超时
    QNetworkReply* reply; // optional, for correlation
};
struct ReqItem
{
    QNetworkRequest req;
    QByteArray payload;
    QString reqTag;
    int retriesLeft;
};

class JTRequest :public QObject
{
    Q_OBJECT
public:
    explicit JTRequest(QObject* parent = nullptr);
    ~JTRequest() override;
    void log(const std::string msg)
    {
        Logger::getInstance().Log(msg);
    }
    void dbInit();
    void enqueueOrSend(const QNetworkRequest& req, const QByteArray& payload, const QString& reqTag, int maxRetries, bool bypassPause = false);
    void tryStartNext();
    void attachAuthHeader(QNetworkRequest& req) const;
    void debugLog(const QString& s) const;
    QByteArray computeSignatureMd5Base64(const QString& appSecret, const QString& timestamp, const QByteArray& payload) const;

    void requestToken(const QString& account, const QString& password, const QString& appKey, const QString& appSecret);
    void startRefreshIfNeeded();

    //void requestUploadingData(const QString& pakageNum, const QString& weight);	//卸车到件扫描
    //void requestWarehouseScan(const QString& Code);	//入仓扫描
    //void requestLoadCar(const QString& Code);	//装车发件扫描
private:
    mutable QMutex m_mutex;
    QNetworkAccessManager* m_netMgr = nullptr;
    QQueue<ReqItem> m_requestQueue;
    QHash<QNetworkReply*, qint64> m_pending; // reply -> start timestamp (ms)
    int requestTimeoutMs = 5000;    // 超时阈值（毫秒）
    int concurrencyLimit = 6;
    int maxQueueSize = 1000;
    QTimer timeoutTimer;

    QString m_baseUrl;                  //基础接口地址
    QString m_terminalUrl;
    QString m_equipmentID;              //设备编号
    QString m_authToken;                //登录token
    QString m_refreshToken;             //
    QString	m_SortPlanCode;             //格口方案编号
    QString m_account_;                  //账户(网点)
    QString m_password_;                 //密码
    QString m_appKey = "GZJD001231121";
    QString m_appSecret = "kI8gLrUxTSVaRx0ZjhCwkQ==";

    //重新登录机制
    std::atomic_bool m_refreshingToken{ false }; // 正在刷新 token
    QQueue<QPair<QNetworkRequest, QByteArray>> m_pausedRequests; // 在刷新 token 时暂停的请求
    QMutex m_pausedMutex;

    int maxLoginRetries = 3;
    int loginRetries = 0;

private slots:
    void onNetworkFinished(QNetworkReply* reply);
    void checkPendingTimeouts();
    void requestTerminalCode(const QString& code);                                              //请求三段码
    void requestSmallData(const QString& code,
                          const QString& weight,
                          int operateType,
                          int slot_id,
                          int supply_id,
                          const QString& supply_mac);         //小件回传数据

    void requestUploadData(const QString& code, const QString& weight);                         //四合一扫描,补收入发 集散点
    void requestBuild(const QString& packageNum);                                               //建包接口,所有数据从数据库拿
    void requestBuildOneByOne(const QString& code, const QString& packageNum);                  //建包接口，掉一个建一个

    void unloadToPieces(const QString& code, const QString& weight);                            //卸车到件, 进港
    void outboundScanning(const QString& code);                                                 //出仓扫描， 进港

signals:
    void loginSucceeded();
    void loginFailed(const QString& reason);
    void slotResult(const QString& waybill, int terminalCode, int order_type);
    //通用请求失败回调
    void requestFailed(const QString& url, const QString& reason);


};
#endif // JTREQUEST_H
