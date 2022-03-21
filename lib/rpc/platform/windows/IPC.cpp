// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <unordered_map>

#include "rpc/IPC.h"
#include "rpc/detail/log.h"

namespace rpc {

constexpr auto NAMEDPIPE_CONNECTION_TRY_TIMES_LIMIT = 300;

//=========================================================================
// callback_queue can be more versatile than a single callable (like TODO list, thread-safe)
//
template <typename>
class CallbackQueue;
template <typename R, typename... Args>
class CallbackQueue<R(Args...)> {
    std::queue<std::function<R(Args...)>> m_queue;
    std::mutex m_mutex;

public:
    // __f the callable to be en-queued.
    template <typename _Callable>
    void push(_Callable&& __f)
    {
        //when you need to store callable or pass it through function call
        //use single template argument like push does, caller of this function is
        // free to choose use lambda or std::bind()

        // orginal version call bind inside push, but since we support arg for callback,
        // the placeholder is more natrual in caller's code when they call bind themselves.
        std::unique_lock<std::mutex> lk(m_mutex);

        //construct the std::function object from of the callable
        m_queue.emplace(std::forward<_Callable>(__f));
    }

    bool empty(void)
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_queue.empty();
    }

    //pop first functor in queue and call it
    R pop(Args... args)
    {
        //we don't use Universal Referencing syntax here because the Args types are already
        //specified manually.
        //Universal Referencing syntax is only necessary for automatic type deduction cases

        //  type     | manually specify T | universal ref T |  return type of forward<T> is always T&&
        // lvalue    |  string            |   string        |    string   &&                     (= string &&)
        // lvalue ref|  string &          |   string &      |    string & &&  /string & &&       (= string &)
        // rvalue ref|  string &&         |   string        |    string && && / string &&        (= string &&)

        std::unique_lock<std::mutex> lk(m_mutex);

        // note that std::forward works fine in both cases.
        auto func = m_queue.front();
        m_queue.pop();
        return func(std::forward<Args>(args)...);
    }

    //drop first functor in queue w/o call it
    void drop(void)
    {
        m_queue.pop();
    }
};

class Connection::Impl : public std::enable_shared_from_this<Connection::Impl> {
public:
    explicit Impl(std::weak_ptr<Poller> poller);
    virtual ~Impl();
    bool wait_for_connection();
    bool listen(const std::string& serverName, int numListen = 5);

    RPCIPCHandle accept();

    bool connect(const std::string& serverName);
    void trigger_async_cache_read(void);
    int read(void* buffer, int bufferSize);
    int write(const void* buffer, int bufferSize);
    bool read(HANDLE& hDstHandle);
    bool write(HANDLE oshd);

    State getState() const;
    uint64_t getId() const;
    RPCIPCHandle getHandle() const;
    std::string getName() const;
    void setFd(HANDLE handle);
    void setState(Connection::State state);

    static void setAccessAttributes(std::tuple<std::string, std::string, int>& accessAttrs);
    std::function<void()> notify(int error_code, int transferred_cnt, uintptr_t hint, Event& p_evt);

private:
    HANDLE m_handle;
    uint64_t m_id;
    std::string m_name;

    Connection::State m_state;

    static std::atomic<uint64_t> connectionCounter;
    static std::tuple<std::string, std::string, int> m_accessAttrs;
    unsigned char m_cache_byte0;
    bool m_cache_empty;
    OVERLAPPED m_waitconn_overlapped;

    OVERLAPPED m_cache_overlapped;
    OVERLAPPED m_error_overlapped;
    OVERLAPPED m_read_overlapped;
    OVERLAPPED m_write_overlapped;

    HANDLE m_hEvent_read;
    HANDLE m_hEvent_write;

    bool m_poller_blocked = false;
    std::weak_ptr<Poller> m_poller;
};

std::atomic<uint64_t> Connection::Impl::connectionCounter(0);
std::tuple<std::string, std::string, int> Connection::Impl::m_accessAttrs = std::make_tuple("", "", 0);

Connection::Impl::Impl(std::weak_ptr<Poller> poller)
    : m_handle(INVALID_HANDLE_VALUE)
    , m_cache_byte0(0)
    , m_cache_empty(true)
    , m_cache_overlapped {}
    , m_error_overlapped {}
    , m_waitconn_overlapped {}
    , m_read_overlapped {}
    , m_write_overlapped {}
    , m_name()
    , m_id(++connectionCounter)
    , m_state(Connection::State::NONE)
    , m_poller(poller)
{
    m_hEvent_read = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_hEvent_write = CreateEvent(NULL, TRUE, FALSE, NULL);
}

Connection::Impl::~Impl()
{
    CloseHandle(m_hEvent_read);
    CloseHandle(m_hEvent_write);

    if (m_handle != INVALID_HANDLE_VALUE) {
        // cancel all pending I/Os

        CancelIoEx(m_handle, &m_cache_overlapped);
        CancelIoEx(m_handle, &m_waitconn_overlapped);
        CancelIoEx(m_handle, &m_error_overlapped);
        CancelIoEx(m_handle, &m_read_overlapped);
        CancelIoEx(m_handle, &m_write_overlapped);

        //close it
        CloseHandle(m_handle);
        // m_poller->remove(this);
        m_handle = INVALID_HANDLE_VALUE;
    }
    m_state = State::NONE;
    LOG_TRACE("[Connection##{}] destruct connection done", m_id);
}

Connection::State Connection::Impl::getState() const
{
    return m_state;
}

uint64_t Connection::Impl::getId() const
{
    return m_id;
}

std::string Connection::Impl::getName() const
{
    return m_name;
}

void Connection::Impl::setFd(HANDLE handle)
{
    m_handle = handle;
}

void Connection::Impl::setState(Connection::State state)
{
    m_state = state;
}

bool Connection::Impl::wait_for_connection()
{
    if (!ConnectNamedPipe(m_handle, &m_waitconn_overlapped)) {
        DWORD dwErr = GetLastError();
        if (dwErr == ERROR_PIPE_CONNECTED) {
            if (!PostQueuedCompletionStatus(m_poller.lock()->getHandle(), 1, (ULONG_PTR)m_handle, &m_waitconn_overlapped)) {
                LOG_ERROR("post queue failed");
                return false;
            }
        } else if (dwErr != ERROR_IO_PENDING && dwErr != ERROR_SUCCESS) {
            LOG_ERROR("wait_for_connection ConnectNamedPipe failed.");
            return false;
        }
    }
    return true;
}
bool Connection::Impl::listen(const std::string& serverName, int numListen)
{
    if (serverName.empty()) {
        LOG_ERROR("Error: serverName should not be empty");
        return false;
    }

    if (numListen <= 0) {
        LOG_ERROR("Error: numListen should be greater than zero");
        return false;
    }

    //server
    constexpr auto PIPE_TIMEOUT = 5000;
    constexpr auto BUFSIZE = 4096;
    LOG_INFO("server name: {}", serverName);
    m_handle = CreateNamedPipe(serverName.c_str(), // pipe name
        PIPE_ACCESS_DUPLEX | // read/write access
            FILE_FLAG_OVERLAPPED, // overlapped mode
        PIPE_TYPE_BYTE | // byte-type pipe
            PIPE_READMODE_BYTE | // message-read mode
            PIPE_WAIT, // blocking mode
        PIPE_UNLIMITED_INSTANCES, // number of instances
        BUFSIZE * sizeof(TCHAR), // output buffer size
        BUFSIZE * sizeof(TCHAR), // input buffer size
        PIPE_TIMEOUT, // client time-out
        NULL); // default security attributes
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateNamedPipe failed.");
        return false;
    }

    m_name = serverName;
    m_state = Connection::State::LISTENING;
    return true;
}

void Connection::Impl::trigger_async_cache_read(void)
{
    if (!m_cache_empty) {
        LOG_ERROR("cache is not empty");
        return;
    }
    m_cache_overlapped = {}; //do it async
    BOOL bRet = ReadFile(m_handle, &m_cache_byte0, 1, NULL, &m_cache_overlapped);
    if (bRet) {
        //for FILE_FLAG_OVERLAPPED, completion IO event will trigger even when the READ opeartion is completed on the spot.
        //PostQueuedCompletionStatus(get_service().nativeHandle(), 1, (ULONG_PTR)nativeHandle(), &m_cache_overlapped);
    } else {
        DWORD dwErr = GetLastError();
        if (dwErr != ERROR_IO_PENDING) {
            //error occurs, make sure on_read() callback running inside io_service() thread, like epoll does
            //std::error_code ec(dwErr, std::system_category());
            //fprintf(stderr, "trigger_async_cache_read() returns %d:%s\n", dwErr, ec.message().c_str());
            m_error_overlapped.Offset = dwErr;
            PostQueuedCompletionStatus(m_poller.lock()->getHandle(), 0, (ULONG_PTR)m_handle, &m_error_overlapped);
        }
    }
}

RPCIPCHandle Connection::Impl::accept()
{
    if (m_state != Connection::State::LISTENING) {
        LOG_ERROR("Error: connection is not in listening state");
        return {};
    }
    auto ret = m_handle;
    m_handle = nullptr;
    m_state = Connection::State::NONE;
    return ret;
}

bool Connection::Impl::connect(const std::string& serverName)
{
    if (serverName.empty()) {
        LOG_ERROR("Error: serverName should not be empty");
        return false;
    }

    HANDLE oshd = INVALID_HANDLE_VALUE;
    int tryTimes = 0;
    while (true) {
        oshd = CreateFile(serverName.c_str(), // pipe name
            GENERIC_READ | // read and write access
                GENERIC_WRITE,
            0, // no sharing
            NULL, // default security attributes
            OPEN_EXISTING, // opens existing pipe
            FILE_FLAG_OVERLAPPED, // default attributes
            NULL); // no template file
        if (oshd == INVALID_HANDLE_VALUE) {
            if (++tryTimes >= NAMEDPIPE_CONNECTION_TRY_TIMES_LIMIT)
                break;
            if (!WaitNamedPipe(serverName.c_str(), 20000)) {
                LOG_ERROR("Error: NamedPipe is busy for a long time.");
                return false;
            }
            continue;
        }

        break;
    }

    m_handle = oshd;
    m_state = State::CONNECTED;
    return true;
}

int Connection::Impl::read(void* buffer, const int bufferSize)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Error: invalid connection");
        return 0;
    }

    if (!buffer || bufferSize < 0) {
        LOG_ERROR("Error: invalid input parameters, buffer=%p bufferSize=%d", buffer, bufferSize);
        return 0;
    }

    uint8_t* pbuff = static_cast<uint8_t*>(buffer);
    int size = bufferSize;
    int cnt = 0;

    //clear the cache if there is data
    if (!m_cache_empty) {
        pbuff[cnt] = m_cache_byte0;
        cnt++;
        m_cache_empty = true;
        m_cache_byte0 = 0;
    }

    m_read_overlapped = { 0 };
    m_read_overlapped.hEvent = m_hEvent_read;
    while (cnt < size) {
        DWORD NumberOfBytesTransferred = 0;
        DWORD io_cnt = size - cnt;
        BOOL bRet = ReadFile(m_handle, pbuff + cnt, io_cnt, &NumberOfBytesTransferred, &m_read_overlapped);
        if (bRet) {
            cnt += NumberOfBytesTransferred;
        } else {
            DWORD dwErr = GetLastError();
            if (dwErr == ERROR_IO_PENDING) {
                NumberOfBytesTransferred = 0;
                if (GetOverlappedResult(m_handle, &m_read_overlapped, &NumberOfBytesTransferred, TRUE)) {
                    cnt += NumberOfBytesTransferred;
                } else {
                    dwErr = GetLastError();
                    if (dwErr != ERROR_SUCCESS)
                        LOG_ERROR("read GetOverlappedResult() failed.");
                    else
                        LOG_ERROR("Unexpected dwErr = {}", dwErr);
                    //cnt += NumberOfBytesTransferred;  //do we need it? little confusing
                    //error_code = dwErr;
                    return 0;
                }
                //ResetEvent(m_sync_overlapped.hEvent);
            } else if (dwErr != ERROR_SUCCESS) {
                LOG_ERROR("read ReadFile() failed.");
                return 0;
            } else {
                LOG_ERROR("Unexpected dwErr= %lu", dwErr);
                return 0;
            }
        }
    };

    return cnt;
}

int Connection::Impl::write(const void* buffer, const int bufferSize)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Error: invalid connection");
        return 0;
    }

    if (!buffer || bufferSize < 0) {
        LOG_ERROR("Error: invalid input parameters, buffer={} bufferSize={}", buffer, bufferSize);
        return 0;
    }

    const uint8_t* pbuff = static_cast<const uint8_t*>(buffer);
    int size = bufferSize;
    int cnt = 0;

    m_write_overlapped = { 0 };
    m_write_overlapped.hEvent = m_hEvent_write;

    while (cnt < size) {
        DWORD NumberOfBytesTransferred = 0;
        BOOL bRet = WriteFile(m_handle, pbuff + cnt, size - cnt, &NumberOfBytesTransferred, &m_write_overlapped);
        if (bRet) {
            cnt += NumberOfBytesTransferred;
        } else {
            DWORD dwErr = GetLastError();
            if (dwErr == ERROR_IO_PENDING) {
                NumberOfBytesTransferred = 0;
                if (GetOverlappedResult(m_handle, &m_write_overlapped, &NumberOfBytesTransferred, TRUE)) {
                    cnt += NumberOfBytesTransferred;
                } else {
                    LOG_ERROR("write GetOverlappedResult() failed.");
                    return 0;
                    //cnt += NumberOfBytesTransferred;  //do we need it? little confusing
                }
            } else if (dwErr != ERROR_SUCCESS) {
                LOG_ERROR("Write WriteFile() failed.");
                return 0;
            } else {
                LOG_ERROR("Unexpected error code");
                return 0;
            }
        }
    };
    return cnt;
}

bool Connection::Impl::read(HANDLE& hDstHandle)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Error: invalid connection");
        return false;
    }

    DWORD SrcProcessID;
    HANDLE hSrcHandle;

    read(&SrcProcessID, sizeof(SrcProcessID));
    read(&hSrcHandle, sizeof(hSrcHandle));

    HANDLE hSrcProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, SrcProcessID);
    if (hSrcProcess == NULL) {
        LOG_ERROR("read(OS_HANDLE) OpenProcess failed.");
        return false;
    }

    if (!DuplicateHandle(hSrcProcess, //hSourceProcessHandle
            (HANDLE)hSrcHandle, //hSourceHandle
            GetCurrentProcess(), //hTargetProcessHandle
            &hDstHandle, //lpTargetHandle
            0, //dwDesiredAccess
            FALSE, //bInheritHandle
            DUPLICATE_SAME_ACCESS)) //dwOptions
    {
        LOG_ERROR("read(OS_HANDLE) DuplicateHandle failed.");
        CloseHandle(hSrcProcess);
        return false;
    }
    CloseHandle(hSrcProcess);
    return true;
}

bool Connection::Impl::write(HANDLE oshd)
{
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Error: invalid connection");
        return false;
    }

    DWORD CurProcessID = GetCurrentProcessId();

    //send processID alone with handle
    write(&CurProcessID, sizeof(CurProcessID));
    write(&oshd, sizeof(oshd));

    return true;
}

void Connection::Impl::setAccessAttributes(std::tuple<std::string, std::string, int>& accessAttrs)
{
    m_accessAttrs = accessAttrs;
}

RPCIPCHandle Connection::Impl::getHandle() const
{
    return m_handle;
}

std::function<void()> Connection::Impl::notify(int error_code, int transferred_cnt, uintptr_t hint, Event& evt)
{
    OVERLAPPED* poverlapped = (OVERLAPPED*)hint;

    if (poverlapped == &m_error_overlapped) {
        int ec = m_error_overlapped.Offset;
        if (ec == ERROR_BROKEN_PIPE) {
            evt.type = Event::Type::CONNECTION_OUT;
        } else {
            assert(0);
        }
    } else if (poverlapped == &m_cache_overlapped) {
        if (error_code == 0) {
            assert(transferred_cnt == 1);
            assert(error_code == 0);
            //fprintf(stderr, ">>>>>>>>>> m_cache_byte0=%c from [%s]  \n", m_cache_byte0, pevt);

            //a connection with poller blocked will not trigger poll events
            if (!m_poller_blocked) {
                m_cache_empty = false; //One byte has been received in the m_cache.
                evt.type = Event::Type::MESSAGE_IN;
                return [this]() {
                    // prewait_callback will happen before next poller wait() cycle ,
                    // user of the connection is supposed to have done all reads
                    // and will suspending ifself until next byte arrives.
                    // so that is the point we will trigger async 1byte cache read operation.

                    //a connection with poller blocked will not trigger asyc cache read
                    if (m_poller_blocked)
                        return;

                    //trigger_async_cache_read is designed to not throw or fail, it will post error
                    //to completion IO port instead.
                    this->trigger_async_cache_read();
                };
            }
        } else if (error_code == ERROR_BROKEN_PIPE) {
            evt.type = Event::Type::CONNECTION_OUT;
        } else if (error_code == ERROR_OPERATION_ABORTED) {
            //CancelIoEx() will queue such packet when it successfully canceled an ASYNC IO request.
            assert(transferred_cnt == 0);
        } else {
            assert(0);
        }
    } else if (poverlapped == &m_waitconn_overlapped) {
        //we do not invent another event because our operation mimics linux socket model
        //and the connection is in LISTEN state, so POLLIN can only means new connection
        //is arrived.
        evt.type = Event::Type::CONNECTION_IN;
    }
    return []() { return; };
}

class Poller::Impl {
public:
    explicit Impl(std::string& name);
    ~Impl();

    bool isOK();
    RPCIPCHandle getHandle() const;
    size_t getTotalConnections();
    Event waitEvent(int milliseconds);
    bool addConnection(Connection::Ptr spConnection);
    void removeConnection(uint64_t connectionId);

private:
    //static void blockPipeSignal();
    void insertConnection(RPCIPCHandle socketFd, Connection::Ptr& spConnection);
    Connection::Ptr getConnectionByHandle(RPCIPCHandle handle);
    Connection::Ptr getConnectionById(uint64_t connectionId);
    void eraseConnectionById(uint64_t connectionId);
    //queue prewait callbacks helper function
    // make sure the callback do not throw exception or return error code
    template <typename _Callable, typename... _Args>
    void queuePrewaitCallback(_Callable&& __f, _Args&&... __args)
    {
        m_prewait_callbacks.push(std::forward<_Callable>(__f), std::forward<_Args>(__args)...);
    }

    Mutex m_mutex;
    HANDLE m_h_io_compl_port;
    std::string m_name;
    std::unordered_map<RPCIPCHandle, Connection::Ptr> m_connections;
    CallbackQueue<void()> m_prewait_callbacks;
};

Poller::Impl::Impl(std::string& name)
    : m_name(std::move(name))
{
    m_h_io_compl_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
}

Poller::Impl::~Impl()
{
    CloseHandle(m_h_io_compl_port);
}

RPCIPCHandle Poller::Impl::getHandle() const
{
    return m_h_io_compl_port;
}

size_t Poller::Impl::getTotalConnections()
{
    AutoMutex autoLock(m_mutex);
    return static_cast<size_t>(m_connections.size());
}

Connection::Ptr Poller::Impl::getConnectionById(uint64_t connectionId)
{
    AutoMutex autoLock(m_mutex);

    for (const auto& entry : m_connections) {
        const auto& connection = entry.second;
        if (connection->getId() == connectionId) {
            return connection;
        }
    }

    return {};
}

Connection::Ptr Poller::Impl::getConnectionByHandle(RPCIPCHandle handle)
{
    AutoMutex autoLock(m_mutex);

    if (m_connections.count(handle)) {
        return m_connections[handle];
    }

    return {};
}

void Poller::Impl::eraseConnectionById(uint64_t connectionId)
{
    AutoMutex autoLock(m_mutex);

    for (const auto& entry : m_connections) {
        const auto& connection = entry.second;
        if (connection->getId() == connectionId) {
            m_connections.erase(entry.first);
            break;
        }
    }
}

bool Poller::Impl::addConnection(Connection::Ptr spConnection)
{
    if (!spConnection) {
        LOG_ERROR("Error: invalid connection");
        return false;
    }

    auto oshd = spConnection->getHandle();
    if (oshd == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Error: invalid connection");
        return false;
    }

    if (!getConnectionByHandle(oshd)) {
        //first time association: register into IO completion port system.
        //we use the handle as completion key directly to be more compatible with linux epoll
        //though this is not the best solution
        auto temp_handle = m_h_io_compl_port;
        m_h_io_compl_port = CreateIoCompletionPort(oshd, m_h_io_compl_port, (ULONG_PTR)oshd, 0);
        if (m_h_io_compl_port == NULL || m_h_io_compl_port != temp_handle) {
            m_h_io_compl_port = temp_handle;
            LOG_ERROR("add() CreateIoCompletionPort failed.");
            return false;
        }
    }

    insertConnection(oshd, spConnection);
    LOG_DEBUG("[Poller##{}] add connection done, numElem={}", m_name, m_connections.size());
    return true;
}

void Poller::Impl::removeConnection(uint64_t connectionId)
{
    auto spConnection = getConnectionById(connectionId);
    if (!spConnection) {
        LOG_ERROR("Error: cannot find connection with id=%lu", connectionId);
        return;
    }

    eraseConnectionById(connectionId);

    LOG_DEBUG("[Poller##{}] remove connection(id:{}) done, numElem={}", m_name, spConnection->getId(), m_connections.size());
}

void Poller::Impl::insertConnection(RPCIPCHandle handle, Connection::Ptr& spConnection)
{
    AutoMutex autoLock(m_mutex);
    m_connections[handle] = spConnection;
}

Event Poller::Impl::waitEvent(int milliseconds)
{
    while (!m_prewait_callbacks.empty()) {
        m_prewait_callbacks.pop();
    }

    DWORD NumberOfBytes;
    ULONG_PTR CompletionKey;
    LPOVERLAPPED lpOverlapped;

    BOOL bSuccess = GetQueuedCompletionStatus(m_h_io_compl_port,
        &NumberOfBytes,
        &CompletionKey,
        &lpOverlapped,
        milliseconds);

    DWORD dwErr = bSuccess ? 0 : (::GetLastError());

    if (WAIT_TIMEOUT == dwErr)
        return { nullptr, Event::Type::NONE }; //no result on timeout

    if (ERROR_ABANDONED_WAIT_0 == dwErr) {
        LOG_ERROR("Completion port handle closed.");
        return { nullptr, Event::Type::NONE };
    }

    if (lpOverlapped == NULL) {
        LOG_ERROR("GetQueuedCompletionStatus() failed.");
        return { nullptr, Event::Type::NONE };
    }

    //SYNC OP failure also cause IO port return, ignore that
    if (lpOverlapped && lpOverlapped->hEvent)
        return { nullptr, Event::Type::NONE };

    //lpOverlapped  is not null
    // CompletionKey is the file handle
    // we don't need a map because this Key is the Callback
    RPCIPCHandle oshd = static_cast<RPCIPCHandle>((void*)CompletionKey);

    // Connection is erased from poller on sender side
    // At the same time, the trigger_async_cache_read has post notification on the INVALID_OS_HANDLE
    // Check it here to avoid throwing an exception below.
    if (oshd == INVALID_HANDLE_VALUE) {
        return { nullptr, Event::Type::NONE };
    }
    auto pconn = getConnectionByHandle(oshd);
    if (!pconn) {
        LOG_ERROR("no connection object for handle ");
        return { nullptr, Event::Type::NONE };
    }
    //assert_line(pconn);
    //do not let exception from one connection terminate whole service thread!
    Event evt;
    evt.connection = pconn;
    auto callBack = pconn->m_impl->notify(dwErr, NumberOfBytes, (uintptr_t)lpOverlapped, evt);
    queuePrewaitCallback(callBack);
    return evt;
}

bool Poller::Impl::isOK()
{
    return m_h_io_compl_port != INVALID_HANDLE_VALUE;
}

Connection::Connection(std::weak_ptr<Poller> poller)
    : m_isInPoller(false)
    , m_poller(std::move(poller))
    , m_impl(new Connection::Impl(m_poller))
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
            LOG_ERROR("Error: add connection to poller failed");
            return false;
        }
    }

    return m_impl->wait_for_connection();
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
            LOG_ERROR("Error: add connection to poller failed");
            return false;
        }
    }

    m_impl->trigger_async_cache_read();
    return true;
}

Connection::Ptr Connection::accept()
{
    auto connectFd = m_impl->accept();
    if (connectFd == INVALID_HANDLE_VALUE) {
        return {};
    }

    Connection::Ptr spConnection = Connection::create(m_poller);
    if (!spConnection) {
        LOG_ERROR("Error: create Connection::Impl instance failed");
        return {};
    }

    spConnection->m_impl->setFd(connectFd);
    spConnection->m_impl->setState(Connection::State::CONNECTED);

    auto spPoller = m_poller.lock();
    if (spPoller) {
        if (!spPoller->addConnection(spConnection)) {
            LOG_ERROR("Error: add connection(%lu) to poller failed", spConnection->getId());
            return {};
        }
    }

    spConnection->m_impl->trigger_async_cache_read(); //trigger ASYNC 1byte cache read for connected

    listen(m_impl->getName());
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

RPCIPCHandle Poller::getHandle() const
{
    return m_impl->getHandle();
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
