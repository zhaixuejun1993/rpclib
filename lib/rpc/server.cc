#include "rpc/server.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <thread>

#include "rpc/format.h"
#include "rpc/IPC.h"
#include "rpc/detail/dev_utils.h"
#include "rpc/detail/log.h"
#include "rpc/detail/server_session.h"
#include "rpc/this_server.h"
#include "rpc/ITTProfiler.h"

using namespace rpc::detail;
// using RPCLIB_ASIO::ip::tcp;
// using namespace RPCLIB_ASIO;

static constexpr std::size_t default_buffer_size =
    rpc::constants::DEFAULT_BUFFER_SIZE;
namespace rpc {

struct server::impl {
    impl(server *parent, std::string const &address, uint16_t port)
        : parent_(parent), suppress_exceptions_(false), ipc_index_(port) {}

    impl(server *parent, uint16_t port)
        : parent_(parent), suppress_exceptions_(false), ipc_index_(port) {}

    void start_accept(size_t threads_num = 1) {
        ipc_session_ = std::make_shared<server_ipc_session>(
            parent_->disp_, threads_num, suppress_exceptions_);

        m_poller = rpc::Poller::create();
        m_connection = rpc::Connection::create(m_poller);
        std::string serverPath =
            std::string(IPC_PREFIX) + std::to_string(ipc_index_) + ".sock";
        m_connection->listen(serverPath);
    }

    void run() { receiveRoutine(); }

    void async_run() {
        m_receiveThread = std::thread(&server::impl::receiveRoutine, this);
    }

    ~impl() { stop(); }

    void stop() {
        m_needStop = true;
        if (m_receiveThread.joinable()) {
            m_receiveThread.join();
        }
    }

    unsigned short port() const { return ipc_index_; }

    server *parent_;
    std::atomic_bool suppress_exceptions_;
    RPCLIB_CREATE_LOG_CHANNEL(server)

    std::atomic<bool> m_needStop{false};
    rpc::Poller::Ptr m_poller;
    rpc::Connection::Ptr m_connection;
    std::thread m_receiveThread;
    std::shared_ptr<server_ipc_session> ipc_session_;
    int ipc_index_;

    void receiveRoutine() {
        ITT_PROFILING_TASK("server.ipc.waitEvent");
        while (!m_needStop) {
            auto event = m_poller->waitEvent(100);
            switch (event.type) {
            case rpc::Event::Type::CONNECTION_IN:
                event.connection->accept();
                break;
            case rpc::Event::Type::CONNECTION_OUT:
                break;
            case rpc::Event::Type::MESSAGE_IN: {
                handleMessage(event.connection);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleMessage(rpc::Connection::Ptr &connection) {
        ipc_session_->do_read(connection);
    }
};

RPCLIB_CREATE_LOG_CHANNEL(server)

server::server(uint16_t port)
    : pimpl(new server::impl(this, port)),
      disp_(std::make_shared<dispatcher>()) {
    LOG_INFO("Created server on localhost:{}", port);
}

server::server(server &&other) noexcept { *this = std::move(other); }

server::server(std::string const &address, uint16_t port)
    : pimpl(new server::impl(this, address, port)),
      disp_(std::make_shared<dispatcher>()) {
    LOG_INFO("Created server on address {}:{}", address, port);
}

server::~server() {
    if (pimpl) {
        pimpl->stop();
    }
}

server &server::operator=(server &&other) {
    if (this != &other) {
        pimpl = std::move(other.pimpl);
        other.pimpl = nullptr;
        disp_ = std::move(other.disp_);
        other.disp_ = nullptr;
    }
    return *this;
}

void server::suppress_exceptions(bool suppress) {
    pimpl->suppress_exceptions_ = suppress;
}

void server::run() {
    pimpl->start_accept();
    pimpl->run();
}

void server::async_run(std::size_t worker_threads) {
    pimpl->start_accept(worker_threads);
    pimpl->async_run();
}

void server::stop() { pimpl->stop(); }

unsigned short server::port() const { return pimpl->port(); }

} // namespace rpc
