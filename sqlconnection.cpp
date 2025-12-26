#include "sqlconnection.h"
#include <stdexcept> // 如果还没包含

bool load_DbConfig(const std::string& path, DbConfig& cfg) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    json j;
    try {
        ifs >> j;
        cfg.host = j.at("database").value("host", "127.0.0.1");
        cfg.port = j.at("database").value("port", 3306);
        cfg.user = j.at("database").value("user", "");
        cfg.dbname = j.at("database").value("dbname", "");
    }
    catch (const std::exception& e) {
        Logger::getInstance().Log("Database configuration parse error: " + std::string(e.what()));
        return false;
    }
    return true;
}

SqlConnection::SqlConnection()
{
    conn = mysql_init(nullptr);
    if (load_DbConfig("config.json", db_cfg))
    {
        connect();
    }
    else
    {
        log("----[错误] 无法读取 config.json 文件");
    }
}
SqlConnection::~SqlConnection()
{
    disconnect();
}
bool SqlConnection::connect()
{
    if (!conn) return false;
    //   if (!mysql_real_connect(conn, db_cfg.host.c_str(), db_cfg.user.c_str(), "123456", db_cfg.dbname.c_str(), db_cfg.port, nullptr, 0))
    if (!mysql_real_connect(conn, db_cfg.host.c_str(), db_cfg.user.c_str(), "jdzn123456!@", db_cfg.dbname.c_str(), db_cfg.port, nullptr, 0))		//正式环境密码: jdzn123456!@
    {
        log("----[数据库] 连接失败: " + std::string(mysql_error(conn)));
        return false;
    }
    return true;
}
std::vector<std::vector<std::string>> SqlConnection::readTable(const std::string& tableName)
{
    std::vector<std::vector<std::string>> result;
    std::string query = "SELECT * FROM `" + tableName + "`;";
    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return result;
    if (mysql_query(conn, query.c_str()) != 0)
    {
        //std::cerr << "Query failed: " << mysql_error(conn) << std::endl;
        return result;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res)
    {
        //std::cerr << "Store result failed: " << mysql_error(conn) << std::endl;
        return result;
    }
    int num_fields = mysql_num_fields(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
    {
        std::vector<std::string> vec;
        for (int i = 0; i < num_fields; ++i)
        {
            vec.emplace_back(row[i] ? row[i] : "NULL");
        }
        result.push_back(vec);
    }
    mysql_free_result(res);
    return result;
}

bool SqlConnection::insertRow(const std::string& tableName, const std::vector<std::string>& columnNames, const std::vector<std::string>& values)
{
    if (columnNames.size() != values.size()) return false;
    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return false;
    std::string cols;
    for (size_t i = 0; i < columnNames.size(); ++i) {
        cols += "`" + columnNames[i] + "`";
        if (i + 1 < columnNames.size()) cols += ", ";
    }
    // 构建值部分，并对特殊字符进行转义
    std::string vals;
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string& src = values[i];
        // 转义后最大长度为 src.length() * 2 + 1
        std::string buf(src.length() * 2 + 1, '\0');
        unsigned long escaped_len = mysql_real_escape_string(
            conn,
            &buf[0],
            src.c_str(),
            static_cast<unsigned long>(src.length())
            );
        vals += "'" + buf.substr(0, escaped_len) + "'";
        if (i + 1 < values.size()) vals += ", ";
    }
    std::string query = "INSERT INTO `" + tableName + "` (" + cols + ") VALUES (" + vals + ");";
    log("----[写入数据库] sql = " + query);
    if (mysql_query(conn, query.c_str()) != 0) {
        //std::cerr << "Insert failed: " << mysql_error(conn) << std::endl;
        log("----[写入数据] 失败: [" + std::to_string(mysql_errno(conn)) + "] " + mysql_error(conn));
        return false;
    }
    return true;
}

std::optional<std::string> SqlConnection::queryString(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn)
{
    std::string q = "SELECT `" + targetColumn + "` FROM `" + tableName + "` WHERE `" + keyColumn + "`='" + keyValue + "' LIMIT 1;";
    //log("---- [查询语句] sql = [" + q + "]");
    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return std::nullopt;
    if (mysql_query(conn, q.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    std::optional<std::string> ret;
    if (row && row[0]) ret = std::string(row[0]);
    mysql_free_result(res);
    return ret;
}
bool SqlConnection::updateRow(const std::string& tableName, const std::vector<std::string>& columnNames, const std::vector<std::string>& values, const std::string& keyColumn, const std::string& keyValue, bool keyIsNumeric)
{
    if (columnNames.size() != values.size()) return false;
    if (columnNames.empty()) return false;

    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return false;

    // 构建 SET 子句（跳过 keyColumn，避免修改主键/定位列）
    std::string setClause;
    for (size_t i = 0; i < columnNames.size(); ++i) {
        const std::string& col = columnNames[i];
        if (col == keyColumn) continue; // 跳过 keyColumn
        const std::string& src = values[i];

        // 转义值
        std::string buf(src.length() * 2 + 1, '\0');
        unsigned long escaped_len = mysql_real_escape_string(
            conn,
            &buf[0],
            src.c_str(),
            static_cast<unsigned long>(src.length())
            );
        std::string esc = buf.substr(0, escaped_len);

        if (!setClause.empty()) setClause += ", ";
        setClause += "`" + col + "`";
        setClause += "='";
        setClause += esc;
        setClause += "'";
    }

    if (setClause.empty()) {
        // 没有可更新的列（例如传入的 columnNames 只有 keyColumn）
        return false;
    }

    // 转义 keyValue（除非 keyIsNumeric）
    std::string whereClause;
    if (keyIsNumeric) {
        // 直接不加引号（假定传入的 keyValue 已经是合法数字字符串）
        whereClause = "`" + keyColumn + "`=" + keyValue;
    }
    else {
        std::string keyBuf(keyValue.length() * 2 + 1, '\0');
        unsigned long keyEscLen = mysql_real_escape_string(
            conn,
            &keyBuf[0],
            keyValue.c_str(),
            static_cast<unsigned long>(keyValue.length())
            );
        std::string keyEsc = keyBuf.substr(0, keyEscLen);
        whereClause = "`" + keyColumn + "`='" + keyEsc + "'";
    }

    std::string query = "UPDATE `" + tableName + "` SET " + setClause + " WHERE " + whereClause + ";";
    log("----[更新数据库] sql = " + query);

    if (mysql_query(conn, query.c_str()) != 0) {
        log("----[更新数据库] 失败!");
        return false;
    }
    return true;
}

std::vector<std::string> SqlConnection::queryArray(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn)
{
    std::vector<std::string> results;
    std::string q = "SELECT `" + targetColumn + "` FROM `" + tableName + "` WHERE `" + keyColumn + "`='" + keyValue + "';";  // 去掉 LIMIT
    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return results;
    if (mysql_query(conn, q.c_str()) != 0) {
        throw std::runtime_error(mysql_error(conn));
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        throw std::runtime_error(mysql_error(conn));
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        if (row[0]) {
            results.emplace_back(row[0]);
        }
        else {
            results.emplace_back();  // 如果这一列是 NULL，就插入空串
        }
    }
    mysql_free_result(res);
    return results;
}

bool SqlConnection::updateValue(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn, const std::string& newValue)
{
    // 转义 keyValue 和 newValue
    std::string bufKey(keyValue.length() * 2 + 1, '\0');
    unsigned long keyLen = mysql_real_escape_string(conn, &bufKey[0], keyValue.c_str(), static_cast<unsigned long>(keyValue.length()));
    std::string bufVal(newValue.length() * 2 + 1, '\0');
    unsigned long valLen = mysql_real_escape_string(conn, &bufVal[0], newValue.c_str(), static_cast<unsigned long>(newValue.length()));

    std::string q = "UPDATE `" + tableName + "` SET `" + targetColumn + "`='"
                    + bufVal.substr(0, valLen) + "' WHERE `" + keyColumn + "`='"
                    + bufKey.substr(0, keyLen) + "';";
    std::lock_guard<std::mutex> lock(mtx);
    if (!conn) return false;
    if (mysql_query(conn, q.c_str()) != 0) {
        // std::cerr << "Update failed: " << mysql_error(conn) << std::endl;
        return false;
    }
    return true;
}
void SqlConnection::disconnect()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (conn) {
        mysql_close(conn);
        conn = nullptr;
    }
}
std::optional<std::unordered_map<std::string, std::string>> SqlConnection::queryRowByField(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue)
{
    // 转义 keyValue（防注入）
    if (!conn) return std::nullopt;

    std::string bufKey(keyValue.length() * 2 + 1, '\0');
    unsigned long keyLen = mysql_real_escape_string(
        conn,
        &bufKey[0],
        keyValue.c_str(),
        static_cast<unsigned long>(keyValue.length())
        );
    std::string escapedKey = bufKey.substr(0, keyLen);

    std::string q = "SELECT * FROM `" + tableName + "` WHERE `" + keyColumn + "`='"
                    + escapedKey + "' LIMIT 1;";

    std::lock_guard<std::mutex> lock(mtx);

    if (!conn) return std::nullopt;
    if (mysql_query(conn, q.c_str()) != 0) {
        // 可选：记录错误
        // std::cerr << "Query failed: " << mysql_error(conn) << std::endl;
        return std::nullopt;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        // no resultset or error
        // mysql_store_result returns nullptr also when query has no resultset;
        // std::cerr << "Store result failed: " << mysql_error(conn) << std::endl;
        return std::nullopt;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        // no rows
        mysql_free_result(res);
        return std::nullopt;
    }

    // 获取列信息与长度数组（以支持包含二进制或空字符的列）
    unsigned int num_fields = mysql_num_fields(res);
    unsigned long* lengths = mysql_fetch_lengths(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);

    std::unordered_map<std::string, std::string> resultMap;
    resultMap.reserve(num_fields);

    for (unsigned int i = 0; i < num_fields; ++i) {
        std::string fieldName = fields[i].name ? fields[i].name : ("col" + std::to_string(i));
        if (row[i] == nullptr) {
            // NULL -> 空字符串（你可以改为不插入或使用 optional<string>）
            resultMap[fieldName] = std::string();
        }
        else {
            if (lengths) {
                resultMap[fieldName] = std::string(row[i], lengths[i]);
            }
            else {
                resultMap[fieldName] = std::string(row[i]);
            }
        }
    }

    mysql_free_result(res);
    return resultMap;
}
std::vector<std::unordered_map<std::string, std::string>> SqlConnection::queryRowsByField(
    const std::string& tableName,
    const std::string& keyColumn,
    const std::string& keyValue,
    const std::vector<std::string>& selectColumns)
{
    std::vector<std::unordered_map<std::string, std::string>> results;
    if (!conn) return results;

    // 构建 SELECT 子句
    std::string selectClause;
    if (selectColumns.empty()) {
        selectClause = "*";
    }
    else {
        for (size_t i = 0; i < selectColumns.size(); ++i) {
            selectClause += "`" + selectColumns[i] + "`";
            if (i + 1 < selectColumns.size()) selectClause += ", ";
        }
    }

    // 转义 keyValue
    std::string keyBuf(keyValue.length() * 2 + 1, '\0');
    unsigned long keyLen = mysql_real_escape_string(
        conn,
        &keyBuf[0],
        keyValue.c_str(),
        static_cast<unsigned long>(keyValue.length())
        );
    std::string escapedKey = keyBuf.substr(0, keyLen);

    std::string query = "SELECT " + selectClause + " FROM `" + tableName + "` WHERE `" + keyColumn + "`='" + escapedKey + "';";
    std::lock_guard<std::mutex> lock(mtx);

    if (!conn) return results;
    if (mysql_query(conn, query.c_str()) != 0) {
        // 可选：记录错误信息
        // std::cerr << "[queryRowsByField] Query failed: " << mysql_error(conn) << "\n";
        return results;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        // 没有结果集或出错
        // mysql_store_result 返回 nullptr 也可能是因为该语句不返回结果集
        // std::cerr << "[queryRowsByField] Store result failed: " << mysql_error(conn) << "\n";
        return results;
    }

    unsigned int num_fields = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        unsigned long* lengths = mysql_fetch_lengths(res);
        std::unordered_map<std::string, std::string> rowMap;
        if (selectColumns.empty()) {
            // 返回所有列，使用 fields 中的名字
            for (unsigned int i = 0; i < num_fields; ++i) {
                std::string fieldName = fields[i].name ? fields[i].name : ("col" + std::to_string(i));
                if (row[i] == nullptr) {
                    rowMap[fieldName] = std::string();
                }
                else {
                    if (lengths) rowMap[fieldName] = std::string(row[i], lengths[i]);
                    else rowMap[fieldName] = std::string(row[i]);
                }
            }
        }
        else {
            // 只返回 selectColumns 中指定的列（按顺序）
            for (unsigned int i = 0; i < num_fields && i < selectColumns.size(); ++i) {
                const std::string& fieldName = selectColumns[i];
                if (row[i] == nullptr) {
                    rowMap[fieldName] = std::string();
                }
                else {
                    if (lengths) rowMap[fieldName] = std::string(row[i], lengths[i]);
                    else rowMap[fieldName] = std::string(row[i]);
                }
            }
        }
        results.push_back(std::move(rowMap));
    }

    mysql_free_result(res);
    return results;
}
