//
//Copyright (C) 2019 Intel Corporation
//
//SPDX-License-Identifier: MIT
//
#ifndef HDDLUNITE_IPC_H
#define HDDLUNITE_IPC_H

#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <condition_variable>

#ifdef WIN32
#include <windows.h>
typedef HANDLE RPCIPCHandle;
#else
#define RPCIPCHandle int
#endif
#ifdef ERROR
#undef ERROR
#endif

namespace rpc {
class Mutex {
public:
    void lock() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }
    bool tryLock() { return m_mutex.try_lock(); }

private:
    std::mutex m_mutex;

    friend class Condition;
};

class AutoMutex {
public:
    explicit AutoMutex(Mutex& mutex)
        : m_mutex(mutex)
    {
        m_mutex.lock();
    };

    ~AutoMutex()
    {
        m_mutex.unlock();
    };

private:
    Mutex& m_mutex;
};

class Condition {
public:
    void wait(Mutex& mutex);
    bool waitFor(Mutex& mutex, long milliseconds);
    void signal();
    void broadcast();

private:
    std::condition_variable m_condition;
};

inline void Condition::wait(Mutex& mutex)
{
    std::unique_lock<std::mutex> lock(mutex.m_mutex, std::defer_lock);
    m_condition.wait(lock);
}

inline bool Condition::waitFor(Mutex& mutex, long milliseconds)
{
    std::unique_lock<std::mutex> lock(mutex.m_mutex, std::defer_lock);

    if (milliseconds < 0) {
        m_condition.wait(lock);
        return true;
    }

    auto status = m_condition.wait_for(lock, std::chrono::milliseconds(milliseconds));

    if (status == std::cv_status::timeout) {
        errno = ETIME;
        return false;
    }

    return true;
}

inline void Condition::signal()
{
    m_condition.notify_one();
}

inline void Condition::broadcast()
{
    m_condition.notify_all();
}

class Poller;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Ptr = std::shared_ptr<Connection>;
    using WPtr = std::weak_ptr<Connection>;

    enum class State : uint8_t {
        NONE = 0,
        LISTENING = 1,
        CONNECTED = 2,
        ERROR = 3
    };

    explicit Connection(std::weak_ptr<Poller> poller);
    virtual ~Connection();

    State getState() const;
    uint64_t getId() const;
    RPCIPCHandle getHandle() const;

    bool listen(const std::string& serverName, int numListen = 5);
    bool connect(const std::string& serverName);
    Connection::Ptr accept();

    Mutex& getMutex() const noexcept;

    int read(void* buffer, int bufferSize) const;
    int write(const void* buffer, int bufferSize) const;

    bool read(RPCIPCHandle& handle) const;
    bool write(RPCIPCHandle handle) const;

    static Ptr create(const std::weak_ptr<Poller>& poller);
    static void setAccessAttributes(std::tuple<std::string, std::string, int> accessAttrs);

private:
    friend std::enable_shared_from_this<Connection>;
    friend Poller;

private:
    class Impl;
    bool m_isInPoller;
    mutable Mutex m_mutex;
    std::weak_ptr<Poller> m_poller;
    std::unique_ptr<Impl> m_impl;
};

class Event {
public:
    enum class Type : uint8_t {
        NONE = 0,
        CONNECTION_IN = 1,
        CONNECTION_OUT = 2,
        MESSAGE_IN = 3
    };

    Connection::Ptr connection;
    Type type;

    static std::string getType(Event& event)
    {
        switch (event.type) {
        case Type::NONE:
            return "NONE";
        case Type::CONNECTION_IN:
            return "CONNECTION_IN";
        case Type::CONNECTION_OUT:
            return "CONNECTION_OUT";
        case Type::MESSAGE_IN:
            return "MESSAGE_IN";
        default:
            return "INVALID";
        }
    }
};

class Poller {
public:
    using Ptr = std::shared_ptr<Poller>;

    explicit Poller(std::string& name);
    virtual ~Poller();

    static Ptr create(std::string name = {});

    bool isOK() const;
    RPCIPCHandle getHandle() const;
    size_t getTotalConnections() const;
    Event waitEvent(long milliseconds) const;
    bool addConnection(Connection::Ptr spConnection) const;
    void removeConnection(uint64_t connectionId) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}

#endif //HDDLUNITE_IPC_H
