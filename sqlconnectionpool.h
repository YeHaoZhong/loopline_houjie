#ifndef SQLCONNECTIONPOOL_H
#define SQLCONNECTIONPOOL_H
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include "sqlconnection.h"

class SqlConnectionPool {
public:
    // RAII 句柄
    class Guard {
    public:
        Guard(SqlConnection* c, SqlConnectionPool* p)
            : conn(c), pool(p) {
        }
        ~Guard() {
            if (conn && pool) pool->release(conn);
        }

        SqlConnection* operator->() { return conn; }
        SqlConnection& operator*() { return *conn; }
        explicit operator bool() const { return conn != nullptr; }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        SqlConnection* conn;
        SqlConnectionPool* pool;
    };

    static SqlConnectionPool& instance();

    void init(size_t poolSize);
    Guard acquire();      // 阻塞获取
    void shutdown();

private:
    SqlConnectionPool() = default;
    ~SqlConnectionPool();

    void release(SqlConnection* conn);

    std::queue<std::unique_ptr<SqlConnection>> connections;
    std::mutex mtx;
    std::condition_variable cv;
    bool initialized = false;
};
#endif // SQLCONNECTIONPOOL_H
