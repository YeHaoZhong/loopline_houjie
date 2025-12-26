#include "sqlconnectionpool.h"

SqlConnectionPool& SqlConnectionPool::instance()
{
    static SqlConnectionPool pool;
    return pool;
}

void SqlConnectionPool::init(size_t poolSize)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (initialized) return;

    for (size_t i = 0; i < poolSize; ++i) {
        connections.push(std::make_unique<SqlConnection>());
    }
    initialized = true;
}

SqlConnectionPool::Guard SqlConnectionPool::acquire()
{
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]() { return !connections.empty(); });

    auto conn = connections.front().release();
    connections.pop();

    return Guard(conn, this);
}

void SqlConnectionPool::release(SqlConnection* conn)
{
    std::lock_guard<std::mutex> lock(mtx);
    connections.push(std::unique_ptr<SqlConnection>(conn));
    cv.notify_one();
}

void SqlConnectionPool::shutdown()
{
    std::lock_guard<std::mutex> lock(mtx);
    while (!connections.empty()) {
        connections.pop();
    }
    initialized = false;
}

SqlConnectionPool::~SqlConnectionPool()
{
    shutdown();
}
