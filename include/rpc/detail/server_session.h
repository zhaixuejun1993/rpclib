#pragma once

#ifndef SESSION_H_5KG6ZMAB
#define SESSION_H_5KG6ZMAB

#include "asio.hpp"
#include <memory>
#include <vector>
#include <queue>

#include "rpc/IPC.h"
#include "rpc/config.h"
#include "rpc/msgpack.hpp"

#include "rpc/detail/async_writer.h"
#include "rpc/detail/log.h"
#include "rpc/dispatcher.h"

namespace rpc {

class server;

namespace detail {

class server_session : public async_writer {
public:
    server_session(server* srv, RPCLIB_ASIO::io_service* io, RPCLIB_ASIO::ip::tcp::socket socket, std::shared_ptr<dispatcher> disp, bool suppress_exceptions);
    void start();

    void close();

private:
    void do_read();

private:
    server* parent_;
    RPCLIB_ASIO::io_service* io_;
    RPCLIB_ASIO::strand read_strand_;
    std::shared_ptr<dispatcher> disp_;
    RPCLIB_MSGPACK::unpacker pac_;
    const bool suppress_exceptions_;
    RPCLIB_CREATE_LOG_CHANNEL(session)
};

class server_ipc_session : public ipc_writer {
public:
    server_ipc_session(std::shared_ptr<dispatcher> disp, int thread_size, bool suppress_exceptions);
    virtual ~server_ipc_session();
    void do_read(const rpc::Connection::Ptr& ipcConnection);

private:
    std::shared_ptr<dispatcher> disp_;
    const bool suppress_exceptions_;
    std::mutex read_mutex_;
    std::mutex task_queue_mutex_;
    std::queue<std::shared_ptr<std::function<void()>>> task_queue_;
    std::condition_variable task_queue_not_empty_;
    std::vector<std::thread> task_threads_;
    std::atomic<bool> stop_task_ {false};
    RPCLIB_CREATE_LOG_CHANNEL(session)
};
} /* detail */
} /* rpc */

#endif /* end of include guard: SESSION_H_5KG6ZMAB */
