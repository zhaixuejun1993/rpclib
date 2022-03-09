#include "rpc/client.h"
#include "rpc/config.h"
#include "rpc/rpc_error.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

// #include "asio.hpp"
#include "rpc/format.h"

#include "rpc/detail/async_writer.h"
#include "rpc/detail/dev_utils.h"
#include "rpc/detail/response.h"
#include "rpc/ITTProfiler.h"

// using namespace RPCLIB_ASIO;
// using RPCLIB_ASIO::ip::tcp;
using namespace rpc::detail;

namespace rpc {

static constexpr uint32_t default_buffer_size =
    rpc::constants::DEFAULT_BUFFER_SIZE;

struct client::impl {
    impl(client *parent, std::string const &addr, uint16_t port)
        : parent_(parent),
          call_idx_(0),
          addr_(addr),
          port_(port),
          is_connected_(false),
          state_(client::connection_state::initial),
          timeout_(nonstd::nullopt),
          connection_ec_(nonstd::nullopt) {
        m_poller = rpc::Poller::create();
        m_connection = rpc::Connection::create(m_poller);
        ipc_writer_ = std::make_shared<detail::ipc_writer>();
    }
    ~impl() {
        close();
    }

    void close() { m_needStop = true;
        if (m_receiveThread.joinable()) {
            m_receiveThread.join();
        }
    }

    void do_connect() {
        LOG_INFO("Initiating connection.");
        std::string serverPath =
            std::string(IPC_PREFIX) + std::to_string(port_) + ".sock";
        int try_times = 3;
        for(int i = 0; i< try_times; i++){
            if(m_connection->connect(serverPath)){
                m_receiveThread = std::thread(&client::impl::receiveRoutine, this);
                break;
            }
        }

       if(m_connection->getState() == Connection::State::CONNECTED){
           std::unique_lock<std::mutex> lock(mut_connection_finished_);
           LOG_INFO("Client connected to {}:{}", addr_, port_);
           is_connected_ = true;
           state_ = client::connection_state::connected;
           conn_finished_.notify_all();
       } else {
           std::unique_lock<std::mutex> lock(mut_connection_finished_);
           LOG_ERROR("Error during connection: {}", ec);
           state_ = client::connection_state::disconnected;
           conn_finished_.notify_all();
       }
    }

    void do_read_ipc(size_t length, RPCLIB_MSGPACK::unpacker& message) {
        RPCLIB_MSGPACK::unpacked result;
        std::lock_guard<std::mutex> lock(m_mutex);
        while (message.next(result)) {
            ITT_PROFILING_TASK("client.response.deserialize");
            auto r = response(std::move(result));
            auto id = r.get_id();

            auto &current_call = ongoing_calls_[id];
            try {
                if (r.get_error()) {
                    throw rpc_error("rpc::rpc_error during call",
                                    std::get<0>(current_call), r.get_error());
                }
                std::get<1>(current_call).set_value(std::move(*r.get_result()));
            } catch (...) {
                std::get<1>(current_call)
                    .set_exception(std::current_exception());
            }
            ongoing_calls_.erase(id);
        }
    }

    client::connection_state get_connection_state() const { return state_; }

    //! \brief Waits for the write queue and writes any buffers to the network
    //! connection. Should be executed through strand_.
    void write(RPCLIB_MSGPACK::sbuffer item) {
        ipc_writer_->write(std::move(item), *m_connection);
    }

    nonstd::optional<int64_t> get_timeout() { return timeout_; }

    void set_timeout(int64_t value) { timeout_ = value; }

    void clear_timeout() { timeout_ = nonstd::nullopt; }

    using call_t =
        std::pair<std::string, std::promise<RPCLIB_MSGPACK::object_handle>>;

    client *parent_;
    std::atomic<int> call_idx_; /// The index of the last call made
    std::unordered_map<uint32_t, call_t> ongoing_calls_;
    std::string addr_;
    uint16_t port_;
    std::atomic_bool is_connected_;
    std::condition_variable conn_finished_;
    std::mutex mut_connection_finished_;
    std::atomic<client::connection_state> state_;
    std::shared_ptr<detail::ipc_writer> ipc_writer_;
    nonstd::optional<int64_t> timeout_;
    nonstd::optional<std::error_code> connection_ec_;
    RPCLIB_CREATE_LOG_CHANNEL(client)

    void receiveRoutine() {
        ITT_PROFILING_TASK("client.ipc.waitEvent");
        while (!m_needStop) {
            auto event = m_poller->waitEvent(100);
            switch (event.type) {
            case rpc::Event::Type::MESSAGE_IN:
                handleMessage();
                break;
            case rpc::Event::Type::CONNECTION_IN: {
                event.connection->accept();
                break;
            }
            case rpc::Event::Type::CONNECTION_OUT:
                LOG_TRACE("ipc connection out");
                break;
            default:
                break;
            }
        }
    }

    void handleMessage() {
        size_t length = 0;
        RPCLIB_MSGPACK::unpacker message;
        {
            ITT_PROFILING_TASK("client.ipc.read");
            if (!m_connection->read(&length, sizeof(length))) {
                return;
            }
            if (length <= 0) {
                return;
            }

            message.reserve_buffer(length);

            if (!m_connection->read(message.buffer(),
                                    static_cast<int>(length))) {
                return;
            }
            message.buffer_consumed(length);
            LOG_TRACE("ipc read from connection {}, size {}",
                      m_connection->getId(), length);
        }
        do_read_ipc(length, message);
    }

    std::mutex m_mutex;
    std::atomic<bool> m_needStop{false};
    rpc::Poller::Ptr m_poller;
    rpc::Connection::Ptr m_connection;
    std::thread m_receiveThread;
};

client::client(std::string const &addr, uint16_t port)
    : pimpl(new client::impl(this, addr, port)) {
    pimpl->do_connect();
}

void client::wait_conn() {
    std::unique_lock<std::mutex> lock(pimpl->mut_connection_finished_);
    while (!pimpl->is_connected_) {
        if (auto ec = pimpl->connection_ec_) {
            throw rpc::system_error(ec.value());
        }

        if (auto timeout = pimpl->timeout_) {
            auto result = pimpl->conn_finished_.wait_for(
                lock, std::chrono::milliseconds(*timeout));
            if (result == std::cv_status::timeout) {
                throw rpc::timeout(RPCLIB_FMT::format(
                    "Timeout of {}ms while connecting to {}:{}", *get_timeout(),
                    pimpl->addr_, pimpl->port_));
            }
        } else {
            pimpl->conn_finished_.wait(lock);
        }
    }
}

int client::get_next_call_idx() { return ++(pimpl->call_idx_); }

void client::post(std::shared_ptr<RPCLIB_MSGPACK::sbuffer> buffer, int idx,
                  std::string const &func_name,
                  std::shared_ptr<rsp_promise> p) {
    {
        ITT_PROFILING_TASK("client.add.onging.calls");
        std::lock_guard<std::mutex> lock(pimpl->m_mutex);
        pimpl->ongoing_calls_.insert(
            std::make_pair(idx, std::make_pair(func_name, std::move(*p))));
    }
    ITT_PROFILING_TASK("client.ipc.write");
    pimpl->write(std::move(*buffer));
}

void client::post(RPCLIB_MSGPACK::sbuffer *buffer) {
    pimpl->write(std::move(*buffer));
    delete buffer;
}

client::connection_state client::get_connection_state() const {
    return pimpl->get_connection_state();
}

nonstd::optional<int64_t> client::get_timeout() const {
    return pimpl->get_timeout();
}

void client::set_timeout(int64_t value) { pimpl->set_timeout(value); }

void client::clear_timeout() { pimpl->clear_timeout(); }

void client::wait_all_responses() {
    std::lock_guard<std::mutex> lock(pimpl->m_mutex);
    for (auto &c : pimpl->ongoing_calls_) {
        c.second.second.get_future().wait();
    }
}

RPCLIB_NORETURN void client::throw_timeout(std::string const &func_name) {
    throw rpc::timeout(
        RPCLIB_FMT::format("Timeout of {}ms while calling RPC function '{}'",
                           *get_timeout(), func_name));
}

client::~client() {
    pimpl->close();
}

} // namespace rpc
