#ifndef SQLCONNECTION_H
#define SQLCONNECTION_H

#include <mysql.h>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <unordered_map>
#include "logger.h"
#include <string>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

struct DbConfig
{
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string dbname;
};

class SqlConnection
{
public:
    SqlConnection();
    ~SqlConnection();

    std::vector<std::vector<std::string>> readTable(const std::string& tableName);
    bool insertRow(const std::string& tableName, const std::vector<std::string>& columnNames, const std::vector<std::string>& values);

    std::optional<std::string> queryString(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn);//查询单行
    std::vector<std::string> queryArray(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn);//查询多值
    bool updateRow(const std::string& tableName, const std::vector<std::string>& columnNames, const std::vector<std::string>& values, const std::string& keyColumn, const std::string& keyValue, bool keyIsNumeric = false);
    bool updateValue(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue, const std::string& targetColumn, const std::string& newValue);
    std::optional<std::unordered_map<std::string, std::string>> queryRowByField(const std::string& tableName, const std::string& keyColumn, const std::string& keyValue);
    void disconnect();
    std::vector<std::unordered_map<std::string, std::string>> queryRowsByField(
        const std::string& tableName,
        const std::string& keyColumn,
        const std::string& keyValue,
        const std::vector<std::string>& selectColumns = {}
        );
private:
    DbConfig db_cfg;
    MYSQL* conn;
    std::mutex mtx;
    bool connect();
    void log(const std::string& Message)
    {
        Logger::getInstance().Log(Message);
    }
};

#endif // SQLCONNECTION_H
