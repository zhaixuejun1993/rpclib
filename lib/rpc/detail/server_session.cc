#include "rpc/detail/server_session.h"

#include "rpc/config.h"
#include "rpc/server.h"
#include "rpc/this_handler.h"
#include "rpc/this_server.h"
#include "rpc/this_session.h"
#include "rpc/ITTProfiler.h"

#include "rpc/detail/log.h"
#include <iostream>

namespace rpc {
namespace detail {

static constexpr std::size_t default_buffer_size =
    rpc::constants::DEFAULT_BUFFER_SIZE;

server_session::server_session(server *srv, RPCLIB_ASIO::io_service *io,
                               RPCLIB_ASIO::ip::tcp::socket socket,
                               std::shared_ptr<dispatcher> disp,
                               bool suppress_exceptions)
    : async_writer(io, std::move(socket)),
      parent_(srv),
      io_(io),
      read_strand_(*io),
      disp_(disp),
      pac_(),
      suppress_exceptions_(suppress_exceptions) {
    pac_.reserve_buffer(default_buffer_size); // TODO: make this configurable
                                              // [sztomi 2016-01-13]
}

void server_session::start() { do_read(); }

void server_session::close() {
    LOG_INFO("Closing session.");
    async_writer::close();
}

void server_session::do_read() {
    auto self(shared_from_base<server_session>());
    constexpr std::size_t max_read_bytes = default_buffer_size;
    socket().async_read_some(
        RPCLIB_ASIO::buffer(pac_.buffer(), default_buffer_size),
        // I don't think max_read_bytes needs to be captured explicitly
        // (since it's constexpr), but MSVC insists.
        read_strand_.wrap([this, self, max_read_bytes](std::error_code ec,
                                                       std::size_t length) {
            if (is_closed()) {
                return;
            }
            if (ec == RPCLIB_ASIO::error::eof ||
                ec == RPCLIB_ASIO::error::connection_reset) {
                LOG_INFO("Client disconnected");
                self->close();
            } else {
                LOG_ERROR("Unhandled error code: {} | '{}'", ec, ec.message());
            }
        }));
}

server_ipc_session::server_ipc_session(std::shared_ptr<dispatcher> disp,
                                       int thread_size,
                                       bool suppress_exceptions)
    : disp_(std::move(disp)), suppress_exceptions_(suppress_exceptions) {
    for (int i = 0; i < thread_size; i++) {
        task_threads_.emplace_back([this]() {
            while (!stop_task_) {
                std::shared_ptr<std::function<void()>> task;
                {
                    ITT_PROFILING_TASK("server.task.dequeue");
                    std::unique_lock<std::mutex> lock(task_queue_mutex_);
                    task_queue_not_empty_.wait(lock, [this]() {
                        return !task_queue_.empty() || stop_task_;
                    });
                    if (stop_task_) {
                        break;
                    }
                    task = task_queue_.front();
                    task_queue_.pop();
                }
                (*task)();
            }
        });
    };
}

server_ipc_session::~server_ipc_session() {
    stop_task_ = true;
    task_queue_not_empty_.notify_all();
    for (auto &task : task_threads_) {
        if (task.joinable()) {
            task.join();
        }
    }
};

void server_ipc_session::do_read(const rpc::Connection::Ptr &ipcConnection) {
    std::shared_ptr<RPCLIB_MSGPACK::unpacker> message =
        std::make_shared<RPCLIB_MSGPACK::unpacker>();
    {
        ITT_PROFILING_TASK("server.ipc.read");
        size_t length = 0;
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (!ipcConnection->read(&length, sizeof(length))) {
            LOG_ERROR("read ipc length error");
            return;
        }

        if (length <= 0) {
            LOG_ERROR("invalid ipc length error");
            return;
        }

        message->reserve_buffer(length);

        if (!ipcConnection->read(message->buffer(), static_cast<int>(length))) {
            LOG_ERROR("read ipc error");
            return;
        }
        message->buffer_consumed(length);
        LOG_TRACE("ipc read from connection {}, size {}",
                  ipcConnection->getId(), length);
    }

    {
        ITT_PROFILING_TASK("server.task.enqueue");
        std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
        task_queue_.push(std::make_shared<std::function<void()>>(
            [this, message, ipcConnection]() {
                RPCLIB_MSGPACK::unpacked result;
                while (message->next(result)) {
                    auto msg = result.get();
                    ITT_PROFILING_TASK("server.dispatch");
                    auto resp = disp_->dispatch(msg, suppress_exceptions_);
                    if (!resp.is_empty()) {
                        ITT_PROFILING_TASK("server.ipc.write");
                        write(resp.get_data(), *ipcConnection);
                    }
                }
            }));
        task_queue_not_empty_.notify_one();
    }
}
} // namespace detail
} // namespace rpc
