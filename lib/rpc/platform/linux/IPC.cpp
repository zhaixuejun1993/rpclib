// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>

#include "rpc/IPC.h"
#include "rpc/detail/log.h"

namespace rpc {
class Connection::Impl : public std::enable_shared_from_this<Connection::Impl> {
public:
    Impl();
    virtual ~Impl();

    State getState() const;
    uint64_t getId() const;
    RPCIPCHandle getHandle() const;

    bool listen(const std::string& serverName, int numListen = 5);
    bool connect(const std::string& serverName);
    RPCIPCHandle accept();

    int read(void* buffer, int bufferSize);
    int write(const void* buffer, int bufferSize);

    bool read(RPCIPCHandle& handle);
    bool write(RPCIPCHandle handle) const;

    static void setAccessAttributes(std::tuple<std::string, std::string, int>& accessAttrs);

private:
    bool makeBlock();
    void setFd(int socketFd);
    void setState(Connection::State state);

    friend Connection;
    friend std::enable_shared_from_this<Connection::Impl>;

private:
    int m_fd;
    uint64_t m_id;
    std::string m_serverName;
    Connection::State m_state;

    static std::atomic<uint64_t> connectionCounter;
    static std::string m_group;
    static std::string m_user;
    static uint64_t m_mode;
};

class Poller::Impl {
public:
    explicit Impl(std::string& name);
    ~Impl();

    bool isOK();
    size_t getTotalConnections();
    Event waitEvent(int milliseconds);
    bool addConnection(Connection::Ptr spConnection);
    void removeConnection(uint64_t connectionId);

private:
    static void blockPipeSignal();
    void insertConnection(int socketFd, Connection::Ptr& spConnection);
    Connection::Ptr getConnectionBySocket(int socketFd);
    Connection::Ptr getConnectionById(uint64_t connectionId);
    void eraseConnectionById(uint64_t connectionId);

private:
    std::mutex m_mutex;
    int m_epollFd;
    std::string m_name;
    std::unordered_map<int, Connection::Ptr> m_connections;

    static const int m_epollSize = 1000;
};

std::atomic<uint64_t> Connection::Impl::connectionCounter(0);
std::string Connection::Impl::m_group("users");
std::string Connection::Impl::m_user;
uint64_t Connection::Impl::m_mode(0660);

Connection::Impl::Impl()
    : m_fd(-1)
    , m_id(++connectionCounter)
    , m_state(Connection::State::NONE)
{
}

Connection::Impl::~Impl()
{
    if (m_fd > 0) {
        close(m_fd);
    }
    if (!m_serverName.empty()) {
        unlink(m_serverName.c_str());
    }
}

Connection::State Connection::Impl::getState() const
{
    return m_state;
}

uint64_t Connection::Impl::getId() const
{
    return m_id;
}

RPCIPCHandle Connection::Impl::getHandle() const
{
    return m_fd;
}

bool Connection::Impl::listen(const std::string& serverName, int numListen)
{
    if (serverName.empty()) {
        return false;
    }

    if (numListen <= 0) {
        return false;
    }

    m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) {
        return false;
    }

    unlink(serverName.c_str());

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, serverName.c_str());
    socklen_t addrlen = offsetof(sockaddr_un, sun_path) + strlen(addr.sun_path) + 1;
    if (bind(m_fd, (struct sockaddr*)&addr, addrlen) != 0 || ::listen(m_fd, numListen) != 0) {
        close(m_fd);
        unlink(serverName.c_str());
        m_fd = -1;
        m_state = Connection::State::ERROR;
        return false;
    }

    m_state = Connection::State::LISTENING;
    m_serverName = serverName;

    return true;
}

bool Connection::Impl::connect(const std::string& serverName)
{
    if (serverName.empty()) {
        return false;
    }

    m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) {
        return false;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, serverName.c_str());
    socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1;

    if (::connect(m_fd, (struct sockaddr*)&addr, addrLen) != 0) {
        close(m_fd);
        m_fd = -1;
        return false;
    }

    m_state = Connection::State::CONNECTED;

    return true;
}

RPCIPCHandle Connection::Impl::accept()
{
    if (m_state != Connection::State::LISTENING) {
        return {};
    }

    struct sockaddr_un addr = {};
    socklen_t addrLen = sizeof(addr);

    int connectFd = ::accept(m_fd, (struct sockaddr*)&addr, &addrLen);
    if (connectFd <= 0) {
        return {};
    }

    return connectFd;
}

int Connection::Impl::read(void* buffer, const int bufferSize)
{
    if (m_fd < 0) {
        return 0;
    }

    if (!buffer || bufferSize < 0) {
        return 0;
    }

    if (!makeBlock()) {
        return 0;
    }

    char* data = static_cast<char*>(buffer);

    size_t leftBytes = bufferSize;
    while (leftBytes > 0) {
        ssize_t readBytes = ::read(m_fd, data, leftBytes);
        if (readBytes == 0) { // reach EOF
            break;
        } else if (readBytes < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            } else {
                readBytes = 0;
            }
        }

        data += readBytes;
        leftBytes -= readBytes;
    }

    return bufferSize - leftBytes;
}

bool Connection::Impl::read(RPCIPCHandle& handle)
{
    if (m_fd < 0) {
        return false;
    }

    if (!makeBlock()) {
        return false;
    }

    int num_fd = -1;
    struct iovec iov[1];
    iov[0].iov_base = &num_fd;
    iov[0].iov_len = sizeof(int);

    char cmsgBuf[CMSG_LEN(sizeof(int))] = {};
    struct cmsghdr* cmsg;
    cmsg = (struct cmsghdr*)cmsgBuf;

    struct msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_control = cmsg;
    msg.msg_controllen = CMSG_LEN(sizeof(int));

    ssize_t ret = -1;
    while (ret <= 0) {
        ret = ::recvmsg(m_fd, &msg, 0);

        if (!ret) {
            return false;
        }

        if (ret < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        }
    }

    int rfd = -1;
    if (msg.msg_controllen == CMSG_LEN(sizeof(int))) {
        rfd = *(int*)CMSG_DATA(cmsg);
    }

    if (rfd < 0) {
        return false;
    }

    handle = rfd;
    return true;
}

int Connection::Impl::write(const void* buffer, const int bufferSize)
{
    if (m_fd < 0) {
        return 0;
    }

    if (!buffer || bufferSize < 0) {
        return 0;
    }

    const char* data = static_cast<const char*>(buffer);

    size_t leftBytes = bufferSize;
    while (leftBytes > 0) {
        ssize_t writeBytes = ::write(m_fd, data, leftBytes);
        if (writeBytes < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            } else {
                writeBytes = 0;
            }
        }

        data += writeBytes;
        leftBytes -= writeBytes;
    }

    return bufferSize - leftBytes;
}

bool Connection::Impl::write(const RPCIPCHandle handle) const
{
    if (m_fd < 0) {
        return false;
    }

    int fdNum = 1;

    struct iovec iov[1];
    iov[0].iov_base = &fdNum;
    iov[0].iov_len = sizeof(fdNum);

    char cmsgBuf[CMSG_LEN(sizeof(int))];
    struct cmsghdr* cmsg;

    cmsg = reinterpret_cast<struct cmsghdr*>(cmsgBuf);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *(int*)CMSG_DATA(cmsg) = handle;

    struct msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_control = cmsg;
    msg.msg_controllen = CMSG_LEN(sizeof(int));

    ssize_t ret = -1;
    while (ret <= 0) {
        ret = ::sendmsg(m_fd, &msg, 0);
        if (ret < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        }
    }

    return true;
}

bool Connection::Impl::makeBlock()
{
    int flag = fcntl(m_fd, F_GETFL, 0) & (~O_NONBLOCK);
    int status = fcntl(m_fd, F_SETFL, flag);
    if (status < 0) {
        return false;
    }

    return true;
}

void Connection::Impl::setFd(int socketFd)
{
    m_fd = socketFd;
}

void Connection::Impl::setState(Connection::State state)
{
    m_state = state;
}

void Connection::Impl::setAccessAttributes(std::tuple<std::string, std::string, int>& accessAttrs)
{
    m_group = std::get<0>(accessAttrs);
    m_user = std::get<1>(accessAttrs);
    m_mode = std::get<2>(accessAttrs);
}

Poller::Impl::Impl(std::string& name)
    : m_name(std::move(name))
{
    m_epollFd = epoll_create(m_epollSize);
}

Poller::Impl::~Impl()
{
    close(m_epollFd);
}

bool Poller::Impl::isOK()
{
    return m_epollFd >= 0;
}

size_t Poller::Impl::getTotalConnections()
{
    std::lock_guard<std::mutex> autoLock(m_mutex);
    return static_cast<size_t>(m_connections.size());
}

bool Poller::Impl::addConnection(Connection::Ptr spConnection)
{
    if (!spConnection) {
        return false;
    }

    int socketFd = spConnection->getHandle();
    if (socketFd < 0) {
        return false;
    }

    if (getConnectionBySocket(socketFd)) {
        return false;
    }

    // under linux, just add the fd into epoll
    // we choose level-trigger mode, so blocking socket is enough.
    //
    // if we use edge-trigger mode, then we need to drain all available data in cache
    // using non-blocking socket on each epoll-event, and this can bring some difficulty
    // to application parser implementation

    struct epoll_event event = {};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = socketFd;

    auto ret = epoll_ctl(m_epollFd, EPOLL_CTL_ADD, socketFd, &event);
    if (ret) {
        return false;
    }

    insertConnection(socketFd, spConnection);

    return true;
}

void Poller::Impl::removeConnection(uint64_t connectionId)
{
    auto spConnection = getConnectionById(connectionId);
    if (!spConnection) {
        return;
    }

    int socketFd = spConnection->getHandle();
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, socketFd, nullptr);

    eraseConnectionById(connectionId);
}

Event Poller::Impl::waitEvent(int milliseconds)
{
    blockPipeSignal();

    struct epoll_event event = {};

    int numEvents = epoll_wait(m_epollFd, &event, 1, milliseconds);

    if (numEvents < 0) {
        return { nullptr, Event::Type::NONE };
    }

    if (!numEvents) {
        return { nullptr, Event::Type::NONE };
    }

    int socketFd = event.data.fd;
    auto connection = getConnectionBySocket(socketFd);

    if (!connection) {
        return { nullptr, Event::Type::NONE };
    }

    if (event.events & EPOLLIN) {
        auto state = connection->getState();
        switch (state) {
        case Connection::State::LISTENING:
            return { connection, Event::Type::CONNECTION_IN };

        case Connection::State::CONNECTED:
            // EPOLLIN and EPOLLHUP may arrive simultaneously
            if (event.events & EPOLLRDHUP || event.events & EPOLLHUP) {
                removeConnection(connection->getId());
                return { connection, Event::Type::CONNECTION_OUT };
            } else {
                return { connection, Event::Type::MESSAGE_IN };
            }

        default:
            return { nullptr, Event::Type::NONE };
        }
    }

    return { nullptr, Event::Type::NONE };
}

void Poller::Impl::blockPipeSignal()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, []() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    });
}

void Poller::Impl::insertConnection(int socketFd, Connection::Ptr& spConnection)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections.insert(std::make_pair(socketFd, spConnection));
}

Connection::Ptr Poller::Impl::getConnectionBySocket(int socketFd)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_connections.count(socketFd)) {
        return m_connections[socketFd];
    }

    return {};
}

Connection::Ptr Poller::Impl::getConnectionById(uint64_t connectionId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& entry : m_connections) {
        const auto& connection = entry.second;
        if (connection->getId() == connectionId) {
            return connection;
        }
    }

    return {};
}

void Poller::Impl::eraseConnectionById(uint64_t connectionId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& entry : m_connections) {
        const auto& connection = entry.second;
        if (connection->getId() == connectionId) {
            m_connections.erase(entry.first);
            break;
        }
    }
}

Connection::Connection(std::weak_ptr<Poller> poller)
    : m_isInPoller(false)
    , m_poller(std::move(poller))
    , m_impl(new Connection::Impl())
{
}

Connection::~Connection()
{
    if (m_isInPoller) {
        auto spPoller = m_poller.lock();
        if (spPoller) {
            spPoller->removeConnection(getId());
        }
    }
    m_impl.reset();
}

uint64_t Connection::getId() const
{
    return m_impl->getId();
}

RPCIPCHandle Connection::getHandle() const
{
    return m_impl->getHandle();
}

Connection::State Connection::getState() const
{
    return m_impl->getState();
}

bool Connection::listen(const std::string& serverName, int numListen)
{
    if (!m_impl->listen(serverName, numListen)) {
        return false;
    }

    auto spPoller = m_poller.lock();
    if (spPoller) {
        m_isInPoller = spPoller->addConnection(shared_from_this());
        if (!m_isInPoller) {
            return false;
        }
    }

    return true;
}

bool Connection::connect(const std::string& serverName)
{
    if (!m_impl->connect(serverName)) {
        return false;
    }

    auto spPoller = m_poller.lock();
    if (spPoller) {
        m_isInPoller = spPoller->addConnection(shared_from_this());
        if (!m_isInPoller) {
            return false;
        }
    }

    return true;
}

Connection::Ptr Connection::accept()
{
    auto connectFd = m_impl->accept();
    if (connectFd < 0) {
        return {};
    }

    Connection::Ptr spConnection;

    spConnection = Connection::create(m_poller);
    if (!spConnection) {
        close(connectFd);
        return {};
    }

    spConnection->m_impl->setFd(connectFd);
    spConnection->m_impl->setState(Connection::State::CONNECTED);

    auto spPoller = m_poller.lock();
    if (spPoller) {
        if (!spPoller->addConnection(spConnection)) {
            if (!spConnection) {
                close(connectFd);
            }
            return {};
        }
    }

    return spConnection;
}

Mutex& Connection::getMutex() const noexcept
{
    return m_mutex;
}

int Connection::read(void* buffer, const int bufferSize) const
{
    return m_impl->read(buffer, bufferSize);
}

int Connection::write(const void* buffer, const int bufferSize) const
{
    return m_impl->write(buffer, bufferSize);
}

bool Connection::read(RPCIPCHandle& handle) const
{
    return m_impl->read(handle);
}

bool Connection::write(const RPCIPCHandle handle) const
{
    return m_impl->write(handle);
}

Connection::Ptr Connection::create(const std::weak_ptr<Poller>& poller)
{
    return std::make_shared<Connection>(poller);
}

void Connection::setAccessAttributes(std::tuple<std::string, std::string, int> accessAttrs)
{
    Connection::Impl::setAccessAttributes(accessAttrs);
}

Poller::Poller(std::string& name)
    : m_impl(new Poller::Impl(name))
{
}

Poller::~Poller()
{
    m_impl.reset();
}

bool Poller::isOK() const
{
    return m_impl->isOK();
}

size_t Poller::getTotalConnections() const
{
    return m_impl->getTotalConnections();
}

Event Poller::waitEvent(long milliseconds) const
{
    return m_impl->waitEvent(milliseconds);
}

bool Poller::addConnection(Connection::Ptr spConnection) const
{
    return m_impl->addConnection(std::move(spConnection));
}

void Poller::removeConnection(uint64_t connectionId) const
{
    return m_impl->removeConnection(connectionId);
}

Poller::Ptr Poller::create(std::string name)
{
    return std::make_shared<Poller>(name);
}
}
