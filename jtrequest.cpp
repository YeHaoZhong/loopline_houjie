#include "jtrequest.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <QUrl>
#include <QCoreApplication>
#include <QThread>
#include <QJsonArray>
#include "SqlConnectionPool.h"
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
    m_account = "WD01320584";
    m_password = "2971ddce41fd043263898ddbd9a34e3a";
    requestToken(m_account, m_password, m_appKey, m_appSecret);//请求获取token
}
JTRequest::~JTRequest()
{
    timeoutTimer.stop();
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        QNetworkReply* r = it.key();
        if (r)
        {
            disconnect(r, nullptr, this, nullptr);
            r->abort();
            r->deleteLater();
        }
    }
    m_pending.clear();
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
    auto account = _mysql->queryString("request_config", "name", "account", "value");
    if (account)
    {
        m_account = QString::fromStdString(*account);
        log("----[JTRequest] dbInit() query sql for account: [" + m_account.toStdString() + "]");
    }
    auto password = _mysql->queryString("request_config", "name", "password", "value");
    if (password)
    {
        m_password = QString::fromStdString(*password);
    }
}
void JTRequest::enqueueOrSend(const QNetworkRequest& req, const QByteArray& payload, const QString& reqTag, int maxRetries, bool bypassPause)
{
    if (m_refreshingToken.load() && !bypassPause)
    {
        QMutexLocker pl(&m_pausedMutex);
        m_pausedRequests.enqueue(qMakePair(req, payload));
        debugLog(QString("----[JTRequest] enqueueOrSend() Paused request while refreshing token: %1").arg(req.url().toString()));
        return;
    }
    QMutexLocker l(&m_mutex);
    if (m_requestQueue.size() > maxQueueSize) {
        log("----[JTRequest] enqueueOrSend() Request queue full, rejecting request");
        emit requestFailed(QString::fromUtf8(req.url().toString().toUtf8()), "queue full");
        return;
    }
    // 如果当前并发未达上限，直接发送
    if (m_pending.size() < concurrencyLimit) {
        // we will post immediately
        QNetworkReply* reply = m_netMgr->post(req, payload);
        if (reply) {
            QVariantMap hmap;
            for (const QByteArray& hk : req.rawHeaderList())
            {
                hmap.insert(QString::fromUtf8(hk), QString::fromUtf8(req.rawHeader(hk)));
            }
            reply->setProperty("origHeaders", hmap);
            reply->setProperty("reqTag", reqTag);
            reply->setProperty("retriesLeft", maxRetries);
            reply->setProperty("payload", payload);
            reply->setProperty("reqUrl", req.url().toString());
            m_pending.insert(reply, QDateTime::currentMSecsSinceEpoch());
            log("----[JTRequest] enqueueOrSend() Sent request immediate: " + req.url().toString().toStdString() + ", tag = " + reqTag.toStdString());
        }
    }
    else {
        // enqueue
        m_requestQueue.enqueue(qMakePair(req, payload));
        log("----[JTRequest] enqueueOrSend() Enqueued request: " + req.url().toString().toStdString() + ", tag = " + reqTag.toStdString() + " (queue size=" + std::to_string(m_requestQueue.size()) + ")");
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
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<QNetworkReply*> toAbort;
    { // lock scope
        QMutexLocker l(&m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            qint64 start = it.value();
            if (now - start > requestTimeoutMs) {
                toAbort.append(it.key());
            }
        }
    }

    for (QNetworkReply* r : toAbort) {
        if (!r) continue;
        debugLog(QString("----[JTRequest] checkPendingTimeouts() Aborting timed-out request: %1").arg(r->property("reqUrl").toString()));
        r->abort();
    }
}
void JTRequest::tryStartNext()
{
    QMutexLocker l(&m_mutex);
    while (!m_requestQueue.isEmpty() && m_pending.size() < concurrencyLimit) {
        auto item = m_requestQueue.dequeue();
        QNetworkRequest req = item.first;

        QByteArray payload = item.second;
        QNetworkReply* reply = m_netMgr->post(req, payload);
        if (reply) {
            QVariantMap hmap;
            for (const QByteArray& hk : req.rawHeaderList())
            {
                hmap.insert(QString::fromUtf8(hk), QString::fromUtf8(req.rawHeader(hk)));
            }
            reply->setProperty("origHeaders", hmap);
            reply->setProperty("retriesLeft", /*合适的重试次数，比如*/ 3);
            reply->setProperty("reqTag", "queued");
            reply->setProperty("payload", payload);
            reply->setProperty("reqUrl", req.url().toString());
            m_pending.insert(reply, QDateTime::currentMSecsSinceEpoch());
            debugLog(QString("----[JTRequest] tryStartNext() Dequeued and sent request: %1").arg(req.url().toString()));
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
    if (netErr != QNetworkReply::NoError) {
        debugLog(QString("----[JTRequest] onNetworkFinished() Network error for %1: code=%2 msg=%3").arg(reqUrl).arg((int)netErr).arg(reply->errorString()));

        int retriesLeft = reply->property("retriesLeft").toInt();
        if (retriesLeft > 0) {
            // 从 reply property 恢复原始 headers（如果有），以便重试时保持一致
            QVariantMap origHeaders = reply->property("origHeaders").toMap();
            QNetworkRequest newReq(reply->url());
            if (!origHeaders.isEmpty()) {
                for (auto it = origHeaders.begin(); it != origHeaders.end(); ++it) {
                    newReq.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
                }
            }
            else {
                // fallback：如果没有 origHeaders，则至少恢复授权头
                attachAuthHeader(newReq);
            }

            QByteArray payload = reply->property("payload").toByteArray();
            reply->deleteLater();

            // 直接重发（注意设置 properties）
            QNetworkReply* newr = m_netMgr->post(newReq, payload);
            if (newr) {
                // 把原始 headers 也写回到新 reply（便于后续重试/保留信息）
                QVariantMap hmap;
                for (const QByteArray& hk : newReq.rawHeaderList()) {
                    hmap.insert(QString::fromUtf8(hk), QString::fromUtf8(newReq.rawHeader(hk)));
                }
                newr->setProperty("origHeaders", hmap);

                newr->setProperty("reqTag", reqTag);
                newr->setProperty("payload", payload);
                newr->setProperty("reqUrl", newReq.url().toString());
                newr->setProperty("retriesLeft", retriesLeft - 1);

                QMutexLocker l(&m_mutex);
                m_pending.insert(newr, QDateTime::currentMSecsSinceEpoch());
            }
            else {
                emit requestFailed(reqUrl, "retry post failed");
            }
        }
        else {
            emit requestFailed(reqUrl, reply->errorString());
            reply->deleteLater();
            tryStartNext();
        }
        return;
    }

    // ---------- 2) 正常情况下再读取数据（reply 没被 abort）
    QByteArray data = reply->readAll();

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
                        // 这些请求已经带了 X-Retried-After-Refresh 标记（如果本函数入队时设置了）
                        m_requestQueue.enqueue(p);
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
        //test
        //emit slotResult("JT3135044400584", 130);

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
                        int terminal_code = -1;
                        if (firstObj.contains("firstDispatchCode")) {
                            QJsonValue v = firstObj.value("firstDispatchCode");
                            if (v.isString()) {
                                terminal_code = v.toString().toInt();
                            }
                            else if (v.isDouble()) {
                                terminal_code = v.toInt(-1);
                            }
                            else {
                                // 其它类型，尝试转字符串再转 int
                                terminal_code = QString(v.toVariant().toString()).toInt();
                            }
                        }
                        else {
                            debugLog("----[JTRequest] onNetworkFinished() firstDispatchCode not found in first data element");
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
                        else if (firstObj.contains("interceptor"))   //是否拦截件, 1=是 0=否
                        {
                            QJsonValue v = firstObj.value("interceptor");
                            if (v.isString())
                            {
                                if (v.toString().toInt() == 1) //是拦截件
                                    order_type = -1;
                                else
                                    order_type = 1;
                            }
                            else if (v.isDouble())
                            {
                                if (v.toInt(-1) == 1) //是拦截件
                                    order_type = -1;
                                else
                                    order_type = 1;
                            }
                            else
                            {
                                if (QString(v.toVariant().toString()).toInt() == 1) //是拦截件
                                    order_type = -1;
                                else
                                    order_type = 1;
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

                        emit slotResult(waybill, terminal_code, order_type);
                    }
                    else {
                        debugLog("----[JTRequest] onNetworkFinished() data array is empty");
                        emit requestFailed(reqUrl, "get_terminalCode: data array empty");
                    }
                }
                // 情况 B: 兼容旧代码，data 直接是对象
                else if (dataVal.isObject()) {
                    QJsonObject dataObj = dataVal.toObject();

                    int terminal_code = -1;
                    if (dataObj.contains("firstDispatchCode")) {
                        QJsonValue v = dataObj.value("firstDispatchCode");
                        if (v.isString()) terminal_code = v.toString().toInt();
                        else if (v.isDouble()) terminal_code = v.toInt(-1);
                        else terminal_code = QString(v.toVariant().toString()).toInt();
                    }
                    else {
                        debugLog("----[JTRequest] onNetworkFinished() firstDispatchCode not found in data object");
                    }

                    QString waybill = dataObj.value("waybillNo").toString();
                    if (waybill.isEmpty()) waybill = dataObj.value("waybill").toString();

                    emit slotResult(waybill, terminal_code, 1);
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
    if (reqTag == "upload") //四合一
    {
        if (msg != "请求成功" || succ == false)
        {
            debugLog(QString("---- [Arrival request] something get wrong! msg: %1").arg(msg));
        }
        reply->deleteLater();
        tryStartNext();
        return;
    }
    if (reqTag == "build")   //建包
    {
        if (msg != "请求成功" || succ == false)
        {
            debugLog(QString("---- [Build request] something get wrong! msg: %1").arg(msg));
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

    //QString timestamp = QString::number(QDateTime::currentSecsSinceEpoch()); // 如果对方要毫秒请改成 currentMSecsSinceEpoch()
    //body["timestamp"] = timestamp;

    QJsonDocument doc(body);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    QUrl url(m_baseUrl + "/opa/smartLogin");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(5000); // 例如 5s

    //debugLog(QString("requestToken -> URL: %1").arg(url.toString()));
    enqueueOrSend(req, payload, "login", 1, true);
}
void JTRequest::requestTerminalCode(const QString& Code)    //请求一段码
{
    QJsonObject body;
    body["waybillNo"] = Code;
    QJsonDocument doc(body);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    // 构造 URL（你可以继续用 m_baseUrl + m_terminalUrl 的方式，这里按你示例使用固定完整 URL）
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

    // ---- 在 header 中加入 Postman 里看到的字段 ----
    // appKey
    if (!m_appKey.isEmpty()) {
        req.setRawHeader("appKey", m_appKey.toUtf8());
    }

    // timestamp (格式 yyyy-MM-dd HH:mm:ss)
    QString timestamp = QString::fromStdString(getCurrentTime());
    req.setRawHeader("timestamp", timestamp.toUtf8());

    // token (有些接口同时需要 Authorization 和 token header)
    {
        QMutexLocker l(&m_mutex); // 读取 m_authToken 线程安全保护
        if (!m_authToken.isEmpty()) {
            req.setRawHeader("token", m_authToken.toUtf8());
        }
    }
    // 调试日志：输出 URL / payload / headers 以便对比 Postman
    //debugLog(QString("requestTerminalCode -> URL: %1").arg(url.toString()));
    //debugLog(QString("requestTerminalCode -> Payload: %1").arg(QString::fromUtf8(payload)));

    //test
    /*emit slotResult("JT3135044400584", 130);*/

    enqueueOrSend(req, payload, "get_terminalCode", 5);
}

void JTRequest::requestUploadData(const QString& Code, const QString& weight) // 四合一 到件补收入发
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
    // Bearer token
    attachAuthHeader(req);
    //debugLog(QString("requestUploadData -> URL: %1").arg(url.toString()));
    //debugLog(QString("requestUploadData -> Payload: %1").arg(QString::fromUtf8(payload)));
    enqueueOrSend(req, payload, "upload", 5);
}
void JTRequest::requestBuild(const QString& packageNum)
{
    // 1) 从数据库读取用于构建 detail 的行（你已有的接口）
    //    假定 _sqlForBuild.queryRowsByField 返回 vector<map<string,string>>
    auto _mysql = SqlConnectionPool::instance().acquire();
    if (!_mysql) {
        log("----[JTRequest] requestBuild() failed to acquire database connection");
        return;
    }
    auto rows = _mysql->queryRowsByField("supply_data", "package_num", packageNum.toStdString(), { "code", "scan_time" });
    if (rows.empty()) {
        debugLog(QString("requestBuild: no rows found for packageNum=%1").arg(packageNum));
        emit requestFailed("requestBuild", "no data for package");
        return;
    }

    // 2) 生成一个统一的 listId（网点编码 + 毫秒时间）
    //    使用 m_account（假定为网点编码或 similar）
    QString listId = m_account;
    if (listId.isEmpty()) listId = "opa"; // 防御性处理
    listId += QString::number(QDateTime::currentMSecsSinceEpoch());

    // 3) 构造 detailList（所有行使用相同 listId）
    QJsonArray detailArr;
    QString firstScanTime; // 用于 master 的 scanTime（优先第一条）

    for (const auto& rowMap : rows) {
        // rowMap: key->value 的 std::string 到 std::string
        auto itCode = rowMap.find("code");
        auto itScan = rowMap.find("scan_time");

        QString code = (itCode != rowMap.end()) ? QString::fromStdString(itCode->second) : QString();
        QString scanTime = (itScan != rowMap.end()) ? QString::fromStdString(itScan->second) : QString();

        // 首条记录的 scanTime 用作 master.scanTime（如果存在）
        if (firstScanTime.isEmpty() && !scanTime.isEmpty()) firstScanTime = scanTime;

        QJsonObject d;
        d["listId"] = listId;
        d["waybillId"] = code;
        // 保持和接口示例一致的时间格式：如果你的 DB 存储格式不同，可以转换
        d["scanTime"] = scanTime.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") : scanTime;
        d["packageNumber"] = packageNum;
        d["scanPda"] = m_equipmentID;
        detailArr.append(d);
    }

    // 4) 构造 master 对象
    QJsonObject masterObj;
    masterObj["listId"] = listId;
    // 若没有任何 scanTime，则使用当前时间
    masterObj["scanTime"] = !firstScanTime.isEmpty() ? firstScanTime : QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    masterObj["packageNumber"] = packageNum;
    masterObj["scanPda"] = m_equipmentID;

    // 5) 根对象包装：和图片示例一致，外层是数组，元素包含 detailList 与 master
    QJsonObject rootObj;
    rootObj["detailList"] = detailArr;
    rootObj["master"] = masterObj;
    QJsonArray outer;
    outer.append(rootObj);

    QJsonDocument doc(outer);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    // 6) 构造请求并发送（使用已有的 enqueueOrSend / attachAuthHeader）
    QUrl url(m_baseUrl + "/opa/smart/scan/uploadPackData");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");

    // 如果需要 token 授权头
    attachAuthHeader(req);

    // 调试日志（可选）
    debugLog(QString("requestBuild -> URL: %1").arg(url.toString()));
    debugLog(QString("requestBuild -> Payload size: %1 bytes").arg(payload.size()));
    debugLog(QString("requestBuild -> Payload: %1").arg(QString::fromUtf8(payload)));

    // 这里使用 reqTag = "build" 以便在 onNetworkFinished 中区分处理
    enqueueOrSend(req, payload, "build", 3);
}
void JTRequest::requestSmallData(const QString& code) {
    QString time_mill = QString::fromStdString(std::to_string(currentTimeMillis()));
    // 单条数据对象
    QJsonObject item;
    item["waybillNo"] = code;
    item["networkCode"] = m_account;                                //网点编码
    item["scanTime"] = QString::fromStdString(getCurrentTime());    //扫描时间
    item["userNum"] = m_account;                                    //登陆人账号
    item["uploadResult"] = 1;                                       //上件扫描识别结果1 成功 2失败
    item["crossBeltMac"] = "";                                      //交叉带MAC地址
    item["supplyDeskCode"] = "";                                    //供包台编号
    item["supplyDeskMac"] = "";									    //供包台MAC地址
    item["uploadTime"] = QString::fromStdString(getCurrentTime());  //上传时间
    item["sortingPlanCode"] = "";                                   //分拣方案编码
    item["operateType"] = 1;                                        //操作模式 1.出港 2.进港
    item["equipmentCode"] = "";                                     //设备编号
    item["equipmentLayer"] = 1;                                     //设备层数
    item["gridNo"] = "";                                            //格口号
    item["fallTime"] = QString::fromStdString(getCurrentTime());    //落格时间
    item["cyclesNum"] = 1;                                          //循环圈数
    item["carNum"] = "";                                            //小车编号
    item["gridCode"] = "";                                          //格口编码类型 :（
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
    // Bearer token
    attachAuthHeader(req);
    //debugLog(QString("requestUploadData -> URL: %1").arg(url.toString()));
    //debugLog(QString("requestUploadData -> Payload: %1").arg(QString::fromUtf8(payload)));
    enqueueOrSend(req, payload, "upload", 5);
}

