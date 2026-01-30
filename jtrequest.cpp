#include "jtrequest.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <QUrl>
#include <QCoreApplication>
#include <QThread>
#include <QJsonArray>
#include "SqlConnectionPool.h"
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
extern QByteArray hmacSha256Raw(const QByteArray& key, const QByteArray& message);
extern std::string getCurrentTime();
extern std::uint64_t currentTimeMillis();
JTRequest::JTRequest(QObject* parent)
    : QObject(parent),
    m_netMgr(new QNetworkAccessManager(this))
{
    dbInit();
    connect(m_netMgr, &QNetworkAccessManager::finished, this, &JTRequest::onNetworkFinished);
    timeoutTimer.setInterval(2000); // 每 2s 检查一次超时
    connect(&timeoutTimer, &QTimer::timeout, this, &JTRequest::checkPendingTimeouts);
    timeoutTimer.start();

}
JTRequest::~JTRequest()
{
    timeoutTimer.stop();
    QList<QNetworkReply*> pendingReplies;
    {
        QMutexLocker l(&m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            pendingReplies.append(it.key());
        }
        m_pending.clear();
    }
    for (QNetworkReply* r : pendingReplies) {
        if (!r) continue;
        disconnect(r, nullptr, this, nullptr);
        r->abort();
        r->deleteLater();
    }
}
void JTRequest::setOperateType(int type){
    m_operateType = type;
    if(type == 1)                       //进港
    {
        m_account = m_account_in;
        m_password = m_password_in;
    } else if(type == 2){               //出港
        m_account = m_account_out;
        m_password = m_password_out;
    }
    requestToken(m_account, m_password, m_appKey, m_appSecret);//请求获取token

}
void JTRequest::attachAuthHeader(QNetworkRequest& req) const
{
    if (!m_authToken.isEmpty()) {
        req.setRawHeader("authToken", QString("%1").arg(m_authToken).toUtf8());
    }
}
void JTRequest::debugLog(const QString& s) const
{
    Logger::getInstance().Log(s.toStdString());
}
void JTRequest::dbInit()
{
    auto _mysql = SqlConnectionPool::instance().acquire();
    if (!_mysql)
    {
        log("----[JTRequest] dbInit() failed to acquire database connection");
        return;
    }
    auto url = _mysql->queryString("request_config", "name", "request_url", "value");
    if (url)
    {
        m_baseUrl = QString::fromStdString(*url);
        log("----[JTRequest] dbInit() query sql for baseUrl: [" + m_baseUrl.toStdString() + "]");
    }
    else
    {
        m_baseUrl = "https://opa.jtexpress.com.cn"; // 默认值
    }
    auto terminalUrl = _mysql->queryString("request_config", "name", "terminal_url", "value");
    if (terminalUrl)
    {
        m_terminalUrl = QString::fromStdString(*terminalUrl);
        log("----[JTRequest] dbInit() query sql for terminalUrl: [" + m_terminalUrl.toStdString() + "]");
    }
    auto ID = _mysql->queryString("request_config", "name", "equipment_id", "value");
    if (ID)
    {
        m_equipmentID = QString::fromStdString(*ID);
        log("----[JTRequest] dbInit() query sql for equipment_id: [" + m_equipmentID.toStdString() + "]");
    }
    auto account_in = _mysql->queryString("request_config", "name", "in_account", "value");                   //进港
    if (account_in)
    {
        m_account_in = QString::fromStdString(*account_in);
        log("----[JTRequest] dbInit() query sql for in account: [" + m_account_in.toStdString() + "]");
    }
    auto password_in = _mysql->queryString("request_config", "name", "in_password", "value");
    if (password_in)
    {
        m_password_in = QString::fromStdString(*password_in);
    }
    auto account_out = _mysql->queryString("request_config", "name", "out_account", "value");
    if(account_out){
        m_account_out = QString::fromStdString(*account_out);
    }
    auto password_out = _mysql->queryString("request_config", "name", "out_password", "value");
    if(password_out){
        m_password_out = QString::fromStdString(*password_out);
    }
}
void JTRequest::enqueueOrSend(const QNetworkRequest& req, const QByteArray& payload, const QString& reqTag, int maxRetries, bool bypassPause)
{
    // 为了避免在锁内进行网络 I/O，先决定是否应该立即发出
    bool shouldSend = false;
    {
        QMutexLocker l(&m_mutex);
        if (m_pending.size() >= concurrencyLimit) {
            if (m_requestQueue.size() < maxQueueSize) {
                ReqItem it{req,payload,reqTag,maxRetries};
                m_requestQueue.enqueue(it);
                log("----[加入请求] 地址:[" + req.url().toString().toStdString() + "], 队列中数量:[" + std::to_string(m_requestQueue.size()) + "]");
            } else {
                log("----[加入请求] 队列已满，正在丢弃请求: [" + req.url().toString().toStdString() + "]");
            }
        } else {
            // 有并发位置，准备直接发送
            shouldSend = true;
        }
    }

    if (!shouldSend) return;

    // 发送必须在锁外进行（避免阻塞其他线程）
    QNetworkReply* reply = m_netMgr->post(req, payload);
    if (reply) {
        QVariantMap hmap;
        for (const QByteArray& hk : req.rawHeaderList())
            hmap.insert(QString::fromUtf8(hk), QString::fromUtf8(req.rawHeader(hk)));

        reply->setProperty("origHeaders", hmap);
        reply->setProperty("retriesLeft", maxRetries);
        reply->setProperty("reqTag", reqTag);
        reply->setProperty("payload", payload);
        reply->setProperty("reqUrl", req.url().toString());
        // 插入 pending 要加锁
        {
            QMutexLocker l(&m_mutex);
            m_pending.insert(reply, QDateTime::currentMSecsSinceEpoch());
        }
        log("----[发送请求] 立即请求地址: [" + req.url().toString().toStdString() + "]");
    }
}
//签名
QByteArray JTRequest::computeSignatureMd5Base64(const QString& appSecret, const QString& timestamp, const QByteArray& payload) const
{
    // 1) 拼接：appSecret + timestamp + body
    QByteArray concat = appSecret.toUtf8();
    concat += timestamp.toUtf8();
    concat += payload; // payload 是紧凑 JSON（QJsonDocument::Compact）

    // 2) MD5 -> 得到 raw md5 bytes，然后转成小写 hex 字符串（32 位）
    QByteArray md5hex = QCryptographicHash::hash(concat, QCryptographicHash::Md5).toHex(); // already lower-case

    // 3) 将 hex 字符串进行 BASE64 编码（注意：是对 hex 字符串进行 base64，而不是对 md5 原始字节）
    QByteArray signatureBase64 = md5hex.toBase64();

    return signatureBase64;
}
void JTRequest::checkPendingTimeouts()
{
    try {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        QList<QNetworkReply*> toAbort;

        { // 把超时的 reply 从 pending 中移出到本地列表（避免重复 abort）
            QMutexLocker l(&m_mutex);
            for (auto it = m_pending.begin(); it != m_pending.end(); ) {
                qint64 start = it.value();
                QNetworkReply* r = it.key();
                if (now - start > requestTimeoutMs) {
                    toAbort.append(r);
                    it = m_pending.erase(it); // 立即从 pending 中移除
                } else {
                    ++it;
                }
            }
        }

        for (QNetworkReply* r : toAbort) {
            if (!r) continue;
            log("---- [请求中止] 正在中止已超时的请求接口: [" + r->property("reqUrl").toString().toStdString() + "]");
            // 标记为 timeoutAbort，后续 finished 时会特别处理（不立即重试）
            r->setProperty("timeoutAbort", true);
            r->abort();
        }
    } catch (const std::exception& e) {
        log("---- [检查超时] 请求超时异常: " + std::string(e.what()));
    }
}
void JTRequest::tryStartNext()
{
    // 我们要在锁外 post，所以每次只 dequeue 一个 item 并在锁外发送
    while (true) {
        ReqItem item;
        {
            QMutexLocker l(&m_mutex);
            if (m_requestQueue.isEmpty() || m_pending.size() >= concurrencyLimit) {
                return;
            }
            item = m_requestQueue.dequeue();
        }

        QNetworkRequest req = item.req;
        QByteArray payload = item.payload;
        QString reqTag = item.reqTag;
        int retries = item.retriesLeft;

        QNetworkReply* reply = m_netMgr->post(req, payload);
        if (reply) {
            QVariantMap hmap;
            for (const QByteArray& hk : req.rawHeaderList()) {
                hmap.insert(QString::fromUtf8(hk), QString::fromUtf8(req.rawHeader(hk)));
            }
            reply->setProperty("origHeaders", hmap);
            // 从队列中出来的任务，默认重试次数设为 3（或者根据你的策略）
            reply->setProperty("retriesLeft", retries);
            reply->setProperty("reqTag", reqTag);                                                               //标签强制需要重新设置
            reply->setProperty("payload", payload);
            reply->setProperty("reqUrl", req.url().toString());
            {
                QMutexLocker l(&m_mutex);
                m_pending.insert(reply, QDateTime::currentMSecsSinceEpoch());
            }
            log("---- [发出请求] 已出列并发送请求: [" + req.url().toString().toStdString() + "]");
        }
    }
}
void JTRequest::startRefreshIfNeeded()
{
    bool expected = false;
    if (!m_refreshingToken.compare_exchange_strong(expected, true)) return; // already refreshing

    // 如果后端提供 refresh 接口，优先使用 refreshToken，示例这里直接重新登录：
    loginRetries = 0;
    requestToken(m_account, m_password, m_appKey, m_appSecret);
}
void JTRequest::onNetworkFinished(QNetworkReply* reply) //所有的回调
{
    if (!reply) return;

    // 先拿出常用 property（不要先 readAll）
    QString reqTag = reply->property("reqTag").toString();
    QString reqUrl = reply->property("reqUrl").toString();
    QNetworkReply::NetworkError netErr = reply->error();

    // 从 pending 移除（不论成功或失败，都先移除）
    {
        QMutexLocker l(&m_mutex);
        if (m_pending.contains(reply)) m_pending.remove(reply);
    }

    // ---------- 1) 网络错误 / abort 处理（先于读取数据）
    bool isTimeoutAbort = reply->property("timeoutAbort").toBool();
    if (netErr != QNetworkReply::NoError) {
        debugLog(QString("----[JTRequest] onNetworkFinished() Network error for %1: code=%2 msg=%3").arg(reqUrl).arg((int)netErr).arg(reply->errorString()));

        int retriesLeft = reply->property("retriesLeft").toInt();
        if (isTimeoutAbort) {
            reply->deleteLater();

            if (retriesLeft > 0) {
                // 延迟重试 1s（避免立即自激），重试走 enqueueOrSend（受并发和队列控制）
                QNetworkRequest newReq(reply->url());
                QVariantMap origHeaders = reply->property("origHeaders").toMap();
                if (!origHeaders.isEmpty()) {
                    for (auto it = origHeaders.begin(); it != origHeaders.end(); ++it) {
                        newReq.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
                    }
                }
                QByteArray payload = reply->property("payload").toByteArray();
                QString reqTagLocal = reqTag;
                int nextRetries = retriesLeft - 1;
                QTimer::singleShot(1000, this, [this, newReq, payload, reqTagLocal, nextRetries]() {
                    enqueueOrSend(newReq, payload, reqTagLocal, nextRetries);
                    // 尝试启动（enqueueOrSend 可能已直接发送）
                    tryStartNext();
                });
            } else {
                emit requestFailed(reqUrl, "请求超时并已耗尽重试次数");
            }
            return;
        }
        if (retriesLeft > 0) {
            QNetworkRequest newReq(reply->url());
            QVariantMap origHeaders = reply->property("origHeaders").toMap();
            if (!origHeaders.isEmpty()) {
                for (auto it = origHeaders.begin(); it != origHeaders.end(); ++it) {
                    newReq.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
                }
            }
            QByteArray payload = reply->property("payload").toByteArray();
            reply->deleteLater();

            // 立即把重试任务入队（enqueueOrSend 会根据并发决定是否立刻发送）
            enqueueOrSend(newReq, payload, reqTag, retriesLeft - 1);
            // 尝试发出队列中的下一个（如果空位）
            tryStartNext();
        } else {
            emit requestFailed(reqUrl, reply->errorString());
            reply->deleteLater();
            tryStartNext();
        }
        return;
    }

    // ---------- 2) 正常情况下再读取数据（reply 没被 abort） 成功路径
    QByteArray data = reply->readAll();
    log("---- [JTRequest] onNetworkFinished（） reqeust tag: [" + reqTag.toStdString() + "], return body: [" + QString::fromUtf8(data.left(512)).toStdString() + "]");
    // ---------- 3) 解析 JSON
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        debugLog(QString("----[JTRequest] onNetworkFinished() Response not JSON for %1: %2; raw=%3")
                     .arg(reqUrl)
                     .arg(parseErr.errorString())
                     .arg(QString::fromUtf8(data.left(512))));
        emit requestFailed(reqUrl, "invalid json");
        reply->deleteLater();
        tryStartNext();
        return;
    }

    QJsonObject obj = doc.object();
    int code = obj.value("code").toInt(-1);
    QString msg = obj.value("msg").toString();
    bool succ = obj.value("succ").toBool();

    // ---------- 4) 处理 token 失效（自动重登）逻辑
    bool tokenExpired = (code == 401) || msg.contains("失效") || msg.contains("expired");
    if (tokenExpired) {
        // 如果是 login 请求本身返回 401 -> 登录失败，直接通知上层
        if (reqTag == "login") {
            debugLog(QString("----[JTRequest] onNetworkFinished() Login returned token-expired/401: url=%1 msg=%2").arg(reqUrl).arg(msg));
            // 确保刷新标志被清除（避免一直 pause 请求）
            m_refreshingToken.store(false);
            emit loginFailed(msg);
            reply->deleteLater();
            tryStartNext();
            return;
        }

        // 对非 login 请求：我们把原始请求压入 paused 队列，触发一次 refresh（如果未在刷新中）
        // 首先检查是否该请求已经是“重试过一次（来自上次刷新）”，防止无限刷新循环
        QVariantMap origHeaders = reply->property("origHeaders").toMap();
        if (!origHeaders.isEmpty() && origHeaders.contains("X-Retried-After-Refresh")) {
            // 已经尝试过 refresh 后重发但仍 401，放弃并通知失败
            debugLog(QString("----[JTRequest] onNetworkFinished()  Request already retried after refresh but still 401: %1").arg(reqUrl));
            emit requestFailed(reqUrl, msg);
            // 同时通知需要重新登录（上层可能想弹窗或人工干预）
            emit loginFailed(msg);
            reply->deleteLater();
            tryStartNext();
            return;
        }

        // 构造要入 paused 队列的原始 QNetworkRequest（保持 headers）
        QUrl qurl(reply->url());
        QNetworkRequest origReq(qurl);
        for (auto it = origHeaders.begin(); it != origHeaders.end(); ++it) {
            origReq.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
        }
        // 标记为将被重试（下次重试时 header 会随 origReq 一起发送）
        origReq.setRawHeader("X-Retried-After-Refresh", "1");

        QByteArray payload = reply->property("payload").toByteArray();

        {
            QMutexLocker pl(&m_pausedMutex);
            m_pausedRequests.enqueue(qMakePair(origReq, payload));
        }
        debugLog(QString("----[JTRequest] onNetworkFinished() Paused request while refreshing token: %1").arg(reqUrl));

        // 发起刷新（如果还未在刷新中）
        startRefreshIfNeeded();

        reply->deleteLater();
        tryStartNext();
        return;
    }

    // ---------- 5) login 请求正常返回（登录成功）: 保存 token，并恢复 paused 请求
    if (reqTag == "login") {
        if (msg == "请求成功" && obj.contains("data") && obj["data"].isObject()) {
            QJsonObject dataObj = obj["data"].toObject();
            if (dataObj.contains("token")) {
                {
                    QMutexLocker l(&m_mutex);
                    m_authToken = dataObj["token"].toString();
                    if (dataObj.contains("refreshToken")) m_refreshToken = dataObj["refreshToken"].toString();
                }
                // 登录成功，停止刷新标志，并把 paused 请求放回主队列（并发控制会在 tryStartNext 处理）
                m_refreshingToken.store(false);
                {
                    QMutexLocker pl(&m_pausedMutex);
                    while (!m_pausedRequests.isEmpty()) {
                        auto p = m_pausedRequests.dequeue();
                        ReqItem _item;
                        _item.payload = p.second;
                        _item.req = p.first;
                        _item.reqTag = "login";
                        _item.retriesLeft = 3;
                        // 这些请求已经带了 X-Retried-After-Refresh 标记（如果本函数入队时设置了）
                        m_requestQueue.enqueue(_item);
                    }
                }
                tryStartNext();
                debugLog(QString("----[JTRequest] onNetworkFinished()  Login succeeded, token = [%1]").arg(m_authToken));
                emit loginSucceeded();
            }
            else {
                debugLog("----[JTRequest] onNetworkFinished() Login success but accessToken missing");
                m_refreshingToken.store(false);
                emit loginFailed("accessToken missing");
            }
        }
        else {
            // 登录接口返回错误：通知失败并清理 paused 队列（或者按策略保留）
            debugLog(QString("----[JTRequest] onNetworkFinished() Login failed: code=%1 msg=%2").arg(code).arg(msg));
            m_refreshingToken.store(false);
            emit loginFailed(msg);

            // 可选策略：把 paused 请求全部 fail 掉，避免一直挂着
            {
                QMutexLocker pl(&m_pausedMutex);
                while (!m_pausedRequests.isEmpty()) {
                    auto p = m_pausedRequests.dequeue();
                    emit requestFailed(p.first.url().toString(), "login failed");
                }
            }
        }
        reply->deleteLater();
        tryStartNext();
        return;
    }

    // ---------- 6) 其它业务请求（如 get_terminalCode / sort_plan 等）按原逻辑处理
    if (reqTag == "get_terminalCode")   // 返回一段码,并返回件的状态
    {

        if (msg == "请求成功") {
            if (obj.contains("data")) {
                QJsonValue dataVal = obj["data"];

                // 情况 A: data 是数组（你给的示例）
                if (dataVal.isArray()) {
                    QJsonArray dataArr = dataVal.toArray();
                    if (!dataArr.isEmpty()) {
                        // 取第一个元素（如果你需要按 waybill 匹配，可以遍历 dataArr）
                        QJsonObject firstObj = dataArr.at(0).toObject();
                        // firstDispatchCode 可能是字符串也可能是数字，处理都兼容
                        if(firstObj.contains("waybillNo")){
                            QJsonValue v = firstObj.value("waybillNo");
                            auto _sql = SqlConnectionPool::instance().acquire();
                            if(_sql){
                                _sql->updateValue("terminal_request_data","code",v.toString().toStdString(),"answer_body", QString::fromUtf8(data.left(512)).toStdString());
                            }
                        }
                        std::string terminal_code = "";                                         //一段码
                        if (firstObj.contains("firstDispatchCode")) {
                            QJsonValue v = firstObj.value("firstDispatchCode");
                            if (v.isString()) {
                                terminal_code = v.toString().toStdString();
                            }/*
                            else if (v.isDouble()) {
                                terminal_code = v.toInt(-1);
                            }
                            else {
                                // 其它类型，尝试转字符串再转 int
                                terminal_code = QString(v.toVariant().toString()).toInt();
                            }*/
                        }
                        else {
                            debugLog("----[JTRequest] onNetworkFinished() firstDispatchCode not found in first data element");
                        }
                        std::string thirdTerminalCode = "";                                 //第三段码
                        if(firstObj.contains("thirdlyDispatchCode")){
                            QJsonValue v = firstObj.value("thirdlyDispatchCode");
                            if(v.isString()){
                                thirdTerminalCode = v.toString().toStdString();
                            }
                        }
                        int order_type = -1;
                        if (firstObj.contains("orderType"))
                        {
                            QJsonValue v = firstObj.value("orderType");
                            if (v.isString())
                            {
                                order_type = v.toString().toInt();
                            }
                            else if (v.isDouble())
                            {
                                order_type = v.toInt(-1);
                            }
                            else
                            {
                                // 其它类型，尝试转字符串再转 int
                                order_type = QString(v.toVariant().toString()).toInt();
                            }
                        }
                        int interceptor = 2;                    //是否拦截件, 1=是 2=否
                        if (firstObj.contains("interceptor"))
                        {
                            QJsonValue v = firstObj.value("interceptor");
                            if (v.isString())
                            {
                                interceptor = v.toString().toInt();
                            }
                            else if (v.isDouble())
                            {
                                interceptor = v.toInt(-1);
                            }
                            else
                            {
                                interceptor = QString(v.toVariant().toString()).toInt();
                            }
                        }
                        else {
                            debugLog("----[JTRequest] onNetworkFinished() orderType/interceptor not found in first data element");
                        }

                        // waybill 字段在示例里是 waybillNo
                        QString waybill = firstObj.value("waybillNo").toString();
                        if (waybill.isEmpty()) {
                            // 有时候字段名可能不同，尝试 fallback 查找 "waybill"
                            waybill = firstObj.value("waybill").toString();
                        }
                        if(m_operateType == 1){                             //进港, 使用第三段码
                            emit slotResult(waybill, thirdTerminalCode, order_type, interceptor);
                        }
                        else{                                               //出港， 使用一段码
                            emit slotResult(waybill, terminal_code, order_type,interceptor);
                        }
                        // emit slotResult(waybill, terminal_code, order_type);
                    }
                    else {
                        debugLog("----[JTRequest] onNetworkFinished() data array is empty");
                        // emit requestFailed(reqUrl, "get_terminalCode: data array empty");
                    }
                }
                // 情况 B: 兼容旧代码，data 直接是对象
                else if (dataVal.isObject()) {
                    QJsonObject dataObj = dataVal.toObject();

                    std::string terminal_code = "";
                    if (dataObj.contains("firstDispatchCode")) {
                        QJsonValue v = dataObj.value("firstDispatchCode");
                        if (v.isString()) terminal_code = v.toString().toStdString();
                        // else if (v.isDouble()) terminal_code = v.toInt(-1);
                        // else terminal_code = QString(v.toVariant().toString()).toInt();
                    }
                    else {
                        debugLog("----[JTRequest] onNetworkFinished() firstDispatchCode not found in data object");
                    }

                    QString waybill = dataObj.value("waybillNo").toString();
                    if (waybill.isEmpty()) waybill = dataObj.value("waybill").toString();

                    emit slotResult(waybill, terminal_code, 1, 2);
                }
                else {
                    debugLog("----[JTRequest] onNetworkFinished()  data is neither array nor object");
                    emit requestFailed(reqUrl, "get_terminalCode: invalid data type");
                }
            }
            else {
                debugLog("----[JTRequest] onNetworkFinished() no data!");
                emit requestFailed(reqUrl, "get_terminalCode: no data");
            }
        }
        else {
            // 非成功：如果后端返回的 code/ msg 表明 token 问题，这里也可以检测（但上面已经统一处理过）
            if (code == 401 || msg.contains("失效") || msg.contains("expired")) {
                debugLog("----[JTRequest] onNetworkFinished() Token expired detected in get_terminalCode branch");
                emit requestFailed(reqUrl, msg);
            }
            else {
                emit requestFailed(reqUrl, msg);
            }
        }

        reply->deleteLater();
        tryStartNext();
        return;
    }
    if (reqTag == "upload")                         //四合一
    {
        if (msg != "请求成功" || succ == false)
        {
            debugLog(QString("---- [Arrival request] something get wrong! msg: %1").arg(msg));
        }
        reply->deleteLater();
        tryStartNext();
        return;
    }
    if (reqTag == "build")                          //建包
    {
        if (msg != "请求成功" || succ == false)
        {
            debugLog(QString("---- [Build request] something get wrong! msg: %1").arg(msg));
        }
        reply->deleteLater();
        tryStartNext();
        return;
    }
    if(reqTag == "smallItem"){                      //小件回传
        if(msg!="请求成功" || succ == false){
            debugLog(QString("---- [smallItem request]something get wrong! msg: %1").arg(msg));
        }
        reply->deleteLater();
        tryStartNext();
        return;
    }

    reply->deleteLater();
    tryStartNext();
}
/* ----------------------
   公共 API：登录、查格口
   ---------------------- */
void JTRequest::requestToken(const QString& account, const QString& password, const QString& appKey, const QString& appSecret)
{
    QJsonObject body;
    body["account"] = account;
    body["password"] = password;
    body["appKey"] = appKey;
    body["appSecret"] = appSecret;

    QJsonDocument doc(body);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);
    // Logger::getInstance().Log("----[JTRequest] requestToken() request body: "+QString::fromUtf8(payload).toStdString());

    QUrl url(m_baseUrl + "/opa/smartLogin");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(5000); // 例如 5s

    enqueueOrSend(req, payload, "login", 1, true);
}
void JTRequest::requestTerminalCode(const QString& Code)                                //请求一段码
{
    QJsonObject body;
    body["waybillNo"] = Code;
    QJsonDocument doc(body);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);
    Logger::getInstance().Log("----[JTRequest] requestTerminalCode() request body: "+QString::fromUtf8(payload).toStdString());

    QUrl url(m_terminalUrl);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");

    // --------- Redirect attribute: only when available ----------
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
#  ifdef QNetworkRequest::RedirectPolicyAttribute
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
#  endif
#elif QT_VERSION >= QT_VERSION_CHECK(5,6,0)
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

    // --------- Transfer timeout: Qt 5.15+ ----------
#if QT_VERSION >= QT_VERSION_CHECK(5,15,0)
    req.setTransferTimeout(5000); // 5s
#endif

    // appKey
    if (!m_appKey.isEmpty()) {
        req.setRawHeader("appKey", m_appKey.toUtf8());
    }

    QString timestamp = QString::fromStdString(getCurrentTime());
    req.setRawHeader("timestamp", timestamp.toUtf8());

    {
        QMutexLocker l(&m_mutex); // 读取 m_authToken 线程安全保护
        if (!m_authToken.isEmpty()) {
            req.setRawHeader("token", m_authToken.toUtf8());
        }
    }

    enqueueOrSend(req, payload, "get_terminalCode", 5);
    auto _sql = SqlConnectionPool::instance().acquire();
    if(_sql){
        const std::vector<std::string> column = {"code","request_body"};
        const std::vector<std::string> values = {Code.toStdString(),QString::fromUtf8(payload).toStdString()};
        _sql->insertRow("terminal_request_data",column,values);
    }
}

void JTRequest::requestUploadData(const QString& Code, const QString& weight)                           // 四合一 到件补收入发，出港，扫描后直接使用
{
    QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));

    // 单条数据对象
    QJsonObject item;
    item["listId"] = m_account + time_mill;     // 网点编码+当前时间毫秒数
    item["waybillId"] = Code;
    item["arriveScanType"] = "1";               // ⚠️ 建议保持字符串/数字一致性
    item["scanTime"] = QString::fromStdString(getCurrentTime());
    item["weight"] = weight;
    item["scanPda"] = m_equipmentID;
    item["weightFlag"] = "2";
    item["scanTypeCode"] = "91";

    // 放入数组
    QJsonArray bodyArr;
    bodyArr.append(item);

    // 序列化为 JSON 数组
    QJsonDocument doc(bodyArr);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    // 请求配置
    QUrl url(m_baseUrl + "/opa/smart/scan/uploadArrivalCRLSData");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(5000); // 例如 5s

    QString timestamp = QString::fromStdString(getCurrentTime());
    req.setRawHeader("timestamp", timestamp.toUtf8());

    {   // 加 token
        QMutexLocker l(&m_mutex);
        if (!m_authToken.isEmpty()) {
            req.setRawHeader("token", m_authToken.toUtf8());
        }
    }

    attachAuthHeader(req);
    enqueueOrSend(req, payload, "upload", 5);
}
void JTRequest::requestBuildOneByOne(const QString& code, const QString& packageNum){                       //单个件建包接口， 在掉格口的时候使用

    auto _sql = SqlConnectionPool::instance().acquire();
    QString scanTime = "";
    if(_sql){
        auto scan_time = _sql->queryString("supply_data","code",code.toStdString(),"scan_time");
        if(scan_time){
            scanTime = QString::fromStdString(*scan_time);
        }
        else{
            scanTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        }
    }
    else{
        scanTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    }

    QString listId = m_account;
    if (listId.isEmpty()) listId = "opa"; // 防御性处理
    listId += QString::number(QDateTime::currentMSecsSinceEpoch());
    QJsonArray detailArr;
    QJsonObject d;
    d["listId"] = listId;
    d["waybillId"] = code;

    d["scanTime"] = scanTime.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") : scanTime;
    d["packageNumber"] = packageNum;
    d["scanPda"] = m_equipmentID;
    detailArr.append(d);

    // master 对象
    QJsonObject masterObj;
    masterObj["listId"] = listId;
    // 若没有任何 scanTime，则使用当前时间
    masterObj["scanTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    masterObj["packageNumber"] = packageNum;
    masterObj["scanPda"] = m_equipmentID;
    QJsonObject rootObj;
    rootObj["detailList"] = detailArr;
    rootObj["master"] = masterObj;
    QJsonArray outer;
    outer.append(rootObj);
    QJsonDocument doc(outer);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);
    Logger::getInstance().Log("----[JTRequest] requestBuildOneByOne() request body: "+ QString::fromUtf8(payload).toStdString());

    QUrl url("https://assscan.jtexpress.com.cn/opa/smart/scan/uploadPackData");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    // req.setRawHeader("appKey", m_appKey.toUtf8());
    {   // 加 token
        QMutexLocker l(&m_mutex);
        if (!m_authToken.isEmpty()) {
            req.setRawHeader("token", m_authToken.toUtf8());
        }
    }
    attachAuthHeader(req);
    enqueueOrSend(req, payload, "build", 3);
}
QString makeToken(const QString &appSecret, const QString &timestamp, const QString &body) {
    // 1) 拼接
    QByteArray concat = appSecret.toUtf8();
    concat += timestamp.toUtf8();
    concat += body.toUtf8();

    // 2) MD5 -> 十六进制字符串（小写）；Java 里通常返回小写 hex
    QByteArray md5hex = QCryptographicHash::hash(concat, QCryptographicHash::Md5).toHex(); // e.g. "a1b2c3..."

    // 如果服务端期望大写，可以使用 .toUpper()
    // QByteArray md5hex = QCryptographicHash::hash(concat, QCryptographicHash::Md5).toHex().toUpper();

    // 3) 把 hex 字符串的 bytes 做 Base64
    QByteArray tokenBase64 = md5hex.toBase64();

    return QString::fromUtf8(tokenBase64);
}
void JTRequest::requestSmallData(const QString& code,
                                 const QString& weight,
                                 int operateType,
                                 int slot_id,
                                 int supply_id,
                                 const QString& supply_mac) {                       //小件回传与建包同时使用,落格口的时候使用
    QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
    // 单条数据对象
    QJsonObject item;

    item["waybillNo"] = code;
    item["networkCode"] = m_account;                                //网点编码
    item["scanTime"] = QString::fromStdString(getCurrentTime());    //扫描时间
    item["userNum"] = m_account;                                    //登陆人账号
    item["weight"] = weight;                                        //重量
    item["uploadResult"] = 1;                                       //上件扫描识别结果1 成功 2失败
    item["crossBeltMac"] = "00-1B-21-CA-3F-67";                     //交叉带MAC地址
    item["supplyDeskCode"] = supply_id;                             //供包台编号
    item["supplyDeskMac"] = supply_mac;                             //供包台MAC地址
    item["uploadTime"] = QString::fromStdString(getCurrentTime());  //上传时间
    item["sortingPlanCode"] = "1";                                  //分拣方案编码
    item["operateType"] = operateType;                              //操作模式 1.出港 2.进港
    item["equipmentCode"] = "JDZN00001";                            //设备编号
    item["equipmentLayer"] = 1;                                     //设备层数
    item["gridNo"] = slot_id;                                       //格口号
    item["fallTime"] = QString::fromStdString(getCurrentTime());    //落格时间
    item["cyclesNum"] = 1;                                          //循环圈数
    item["carNum"] = 100;                                           //小车编号
    item["gridCode"] = 111;                                         //格口编码类型 :（
    /*正常读码：111 ；
    超重件 : 990;
    欠费拦截口 : 991;
    综合异常口：992 ；
    未识别条码 ：993 ；
    多条码：994 ；
    三段码请求超时：995 ；
    三段码未配置：996 ；
    超最大循环：997 ；
    取消件：998 ；
    拦截件：999)*/

    // 放入数组
    QJsonArray bodyArr;
    bodyArr.append(item);

    // 序列化为 JSON 数组
    QJsonDocument doc(bodyArr);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);
    Logger::getInstance().Log("----[JTRequest] requestSmallData() request body: "+ QString::fromUtf8(payload).toStdString());

    QUrl url("https://assscan.jtexpress.com.cn/assscanface/face/assScanSmallUpper/smallUpperDataUpload");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("appKey", m_appKey.toUtf8());
    req.setTransferTimeout(5000); // 例如 5s

    QString timestamp = QString::number(QDateTime::currentSecsSinceEpoch());
    req.setRawHeader("timestamp", timestamp.toUtf8());

    QString token = makeToken(m_appSecret,timestamp,QString::fromUtf8(payload));
    req.setRawHeader("token",token.toUtf8());
    // attachAuthHeader(req);
    enqueueOrSend(req, payload, "smallItem", 3);
}
void JTRequest::unloadToPieces(const QString& code, const QString& weight){                            //卸车到件, 进港,需要添加图片
    const QString codeCopy = code;
    const QString weightCopy = weight;
    // 后台任务：阻塞方式查询 short_url（在工作线程里，不会阻塞主线程）
    auto future = QtConcurrent::run([codeCopy]() -> QString {
        QString shortUrlResult;
        auto _sql = SqlConnectionPool::instance().acquire();
        if (_sql)
        {
            const std::string db_code = codeCopy.toStdString();
            constexpr int maxTries = 15;                                        //延迟15秒
            constexpr std::chrono::milliseconds interval(1000);

            for (int i = 0; i < maxTries; ++i)
            {
                auto db_short_url = _sql->queryString("pic", "code", db_code, "short_url");
                if (db_short_url && !db_short_url->empty())
                {
                    shortUrlResult = QString::fromStdString(*db_short_url);
                    break;
                }
                if (i + 1 < maxTries)
                    std::this_thread::sleep_for(interval);
            }
        }
        return shortUrlResult; // 可能为空
    });
    QFutureWatcher<QString>* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, codeCopy, weightCopy]() {
        QString short_url = watcher->result(); // 从后台任务拿到的结果（主线程）
        watcher->deleteLater();

        // 以下为你原来的构造请求代码，把 short_url 插入：
        QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
        QJsonObject item;
        item["listId"] = m_account + time_mill;
        item["waybillId"] = codeCopy;
        item["scanTime"] = QString::fromStdString(getCurrentTime());
        item["scanTypeCode"] = 92;
        item["weight"] = weightCopy;
        item["transportTypeCode"] = 02;
        item["scanPda"] = "JDZN00001";
        item["scanType"] = 1;
        item["weightFlag"] = 2;
        item["sortingPictureUrl"] = short_url;

        QJsonArray bodyArr;
        bodyArr.append(item);
        QJsonDocument doc(bodyArr);
        QByteArray payload = doc.toJson(QJsonDocument::Compact);
        Logger::getInstance().Log("----[JTRequest] unloadToPieces() request body: "+QString::fromUtf8(payload).toStdString());

        QUrl url("https://opa.jtexpress.com.cn/opa/smart/scan/uploadUnloadingArrivalData");
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setTransferTimeout(5000);
        QString timestamp = QString::fromStdString(getCurrentTime());
        req.setRawHeader("timestamp", timestamp.toUtf8());
        attachAuthHeader(req);
        enqueueOrSend(req, payload, "unloadToPieces", 3);
    });

    watcher->setFuture(future);
    // QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
    // QJsonObject item;
    // item["listId"] = m_account + time_mill;
    // item["waybillId"] = code;
    // item["scanTime"] = QString::fromStdString(getCurrentTime());
    // item["scanTypeCode"] = 92;                                                                          //到件扫描，集散进港
    // item["weight"] = weight;
    // item["transportTypeCode"] = 02;
    // item["scanPda"] = "JDZN00001";                                                                               //设备编号
    // item["scanType"] = 1;                                                                               //运单
    // item["weightFlag"] = 2;                                                                             //称重
    // item["sortingPictureUrl"] = short_url;                                                                      //从数据库中获取

    // QJsonArray bodyArr;
    // bodyArr.append(item);
    // QJsonDocument doc(bodyArr);
    // QByteArray payload = doc.toJson(QJsonDocument::Compact);
    // Logger::getInstance().Log("----[JTRequest] unloadToPieces() request body: "+QString::fromUtf8(payload).toStdString());

    // QUrl url("https://opa.jtexpress.com.cn/opa/smart/scan/uploadUnloadingArrivalData");
    // QNetworkRequest req(url);
    // req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // req.setTransferTimeout(5000); // 例如 5s
    // QString timestamp = QString::fromStdString(getCurrentTime());
    // req.setRawHeader("timestamp", timestamp.toUtf8());
    // attachAuthHeader(req);
    // enqueueOrSend(req, payload, "unloadToPieces", 3);
}
// void JTRequest::outboundScanning(const QString& code,const QString& deliveryCode){                                                 //出仓扫描， 进港，延迟10秒
//     QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
//     QJsonObject item;
//     item["listId"] = m_account + time_mill;     // 网点编码+当前时间毫秒数
//     item["waybillId"] = code;
//     item["deliveryCode"] = deliveryCode;        //派件员code
//     item["scanTime"] = QString::fromStdString(getCurrentTime());
//     item["scanPda"] = m_equipmentID;                       //设备编号

//     QJsonArray bodyArr;
//     bodyArr.append(item);
//     QJsonDocument doc(bodyArr);
//     QByteArray payload = doc.toJson(QJsonDocument::Compact);
//     Logger::getInstance().Log("----[JTRequest] outboundScanning() request body: "+QString::fromUtf8(payload).toStdString());

//     QUrl url("https://opa.jtexpress.com.cn/opa/smart/scan/uploadDeliveryOutStockData");
//     QNetworkRequest req(url);
//     req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
//     req.setTransferTimeout(5000); // 例如 5s
//     QString timestamp = QString::fromStdString(getCurrentTime());
//     req.setRawHeader("timestamp", timestamp.toUtf8());
//     {   // 加 token
//         QMutexLocker l(&m_mutex);
//         if (!m_authToken.isEmpty()) {
//             req.setRawHeader("token", m_authToken.toUtf8());
//         }
//     }
//     attachAuthHeader(req);                                                              //请求头加入autoken， 是登录成功后返回的token值
//     enqueueOrSend(req, payload, "outboundScanning", 3);
// }
void JTRequest::outboundScanning(const QString& code, const QString& deliveryCode) {
    // 立即设置一个 10 秒后执行的单次定时器（10000 ms）
    Logger::getInstance().Log("----[JTRequest] outboundScanning() request!");
    QTimer::singleShot(12000, this, [this, code, deliveryCode]() {
        // 这里是在 10 秒后执行，按需重新生成时间戳等
        QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
        QJsonObject item;
        item["listId"] = m_account + time_mill;
        item["waybillId"] = code;
        item["deliveryCode"] = deliveryCode;
        item["scanTime"] = QString::fromStdString(getCurrentTime());
        item["scanPda"] = m_equipmentID;

        QJsonArray bodyArr;
        bodyArr.append(item);
        QJsonDocument doc(bodyArr);
        QByteArray payload = doc.toJson(QJsonDocument::Compact);
        Logger::getInstance().Log("----[JTRequest] outboundScanning() request body: " + QString::fromUtf8(payload).toStdString());

        QUrl url("https://opa.jtexpress.com.cn/opa/smart/scan/uploadDeliveryOutStockData");
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setTransferTimeout(5000);

        QString timestamp = QString::fromStdString(getCurrentTime());
        req.setRawHeader("timestamp", timestamp.toUtf8());

        {   // 加 token
            QMutexLocker l(&m_mutex);
            if (!m_authToken.isEmpty()) {
                req.setRawHeader("token", m_authToken.toUtf8());
            }
        }
        attachAuthHeader(req);
        enqueueOrSend(req, payload, "outboundScanning", 3);
    });
}















