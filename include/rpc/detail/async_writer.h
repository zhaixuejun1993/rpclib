#pragma once

#ifndef ASYNC_WRITER_H_HQIRH28I
#define ASYNC_WRITER_H_HQIRH28I

#include "rpc/IPC.h"
#include "rpc/detail/log.h"
#include "rpc/msgpack.hpp"

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>

namespace rpc {

class client;

namespace detail {

class ipc_writer : public std::enable_shared_from_this<ipc_writer> {
public:
    ipc_writer() = default;
    virtual ~ipc_writer() = default;

    void write(const RPCLIB_MSGPACK::sbuffer& buffer, rpc::Connection& ipcConnection)
    {
        size_t length = buffer.size();
        if (length == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!ipcConnection.write(&length, sizeof(length))) {
            return;
        }

        if (!ipcConnection.write(buffer.data(), static_cast<int>(length))) {
            return;
        }
        LOG_TRACE("ipc write to connection {}, size {}", ipcConnection.getId(), buffer.size());
    }

private:
    std::mutex m_mutex;
    RPCLIB_CREATE_LOG_CHANNEL(ipc_writer);
};

} /* detail */
} /* rpc  */

#endif /* end of include guard: ASYNC_WRITER_H_HQIRH28I */
