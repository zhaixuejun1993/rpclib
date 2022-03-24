#include "rpc/detail/server_session.h"

#include "rpc/config.h"
#include "rpc/server.h"

#include "rpc/detail/log.h"
#include <iostream>

namespace rpc {
namespace detail {

static constexpr std::size_t default_buffer_size =
    rpc::constants::DEFAULT_BUFFER_SIZE;

server_ipc_session::server_ipc_session(std::shared_ptr<dispatcher> disp,
                                       int thread_size,
                                       bool suppress_exceptions)
    : disp_(std::move(disp)), suppress_exceptions_(suppress_exceptions) {
    for (int i = 0; i < thread_size; i++) {
        task_threads_.emplace_back([this]() {
            while (!stop_task_) {
                std::shared_ptr<std::function<void()>> task;
                {
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
        std::lock_guard<std::mutex> task_lock(task_queue_mutex_);
        task_queue_.push(std::make_shared<std::function<void()>>(
            [this, message, ipcConnection]() {
                RPCLIB_MSGPACK::unpacked result;
                while (message->next(result)) {
                    auto msg = result.get();
                    auto resp = disp_->dispatch(msg, suppress_exceptions_);
                    if (!resp.is_empty()) {
                        write(resp.get_data(), *ipcConnection);
                    }
                }
            }));
        task_queue_not_empty_.notify_one();
    }
}
} // namespace detail
} // namespace rpc
