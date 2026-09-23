#include "astra/status_server.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <utility>
#include <chrono>
#include <thread>

namespace astra {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

}  // namespace

struct StatusServer::Implementation {
    asio::io_context io;
    tcp::acceptor acceptor{io};
};

void StatusStore::publish(std::uint64_t sequence, std::string json) {
    const std::lock_guard lock(mutex_);
    if (sequence < sequence_) return;
    sequence_ = sequence;
    json_ = std::move(json);
}

std::string StatusStore::snapshot() const {
    const std::lock_guard lock(mutex_);
    return json_;
}

std::uint64_t StatusStore::sequence() const {
    const std::lock_guard lock(mutex_);
    return sequence_;
}

StatusServer::StatusServer(std::shared_ptr<StatusStore> store, std::uint16_t port)
    : store_(std::move(store)), port_(port), implementation_(std::make_unique<Implementation>()),
      thread_([this] { run(); }) {}

StatusServer::~StatusServer() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void StatusServer::stop() noexcept {
    if (stopping_.exchange(true)) return;
    boost::system::error_code error;
    implementation_->acceptor.close(error);
    implementation_->io.stop();
}

void StatusServer::run() {
    try {
        tcp::endpoint endpoint(tcp::v4(), port_);
        boost::system::error_code error;
        implementation_->acceptor.open(endpoint.protocol(), error);
        if (error) return;
        implementation_->acceptor.set_option(asio::socket_base::reuse_address(true), error);
        implementation_->acceptor.bind(endpoint, error);
        if (error) return;
        implementation_->acceptor.listen(asio::socket_base::max_listen_connections, error);
        if (error) return;
        implementation_->acceptor.non_blocking(true, error);
        if (error) return;

        while (!stopping_.load()) {
            tcp::socket socket(implementation_->io);
            implementation_->acceptor.accept(socket, error);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (!stopping_.load()) continue;
                return;
            }

            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) continue;

            http::response<http::string_body> response{http::status::ok, request.version()};
            response.set(http::field::server, "astra-trader/0.1");
            response.set(http::field::access_control_allow_origin, "*");
            response.keep_alive(false);
            if (request.method() != http::verb::get ||
                (request.target() != "/api/status" && request.target() != "/api/events")) {
                response.result(http::status::not_found);
                response.body() = R"({"error":"not_found"})";
                response.set(http::field::content_type, "application/json");
            } else if (request.target() == "/api/events") {
                response.set(http::field::content_type, "text/event-stream");
                response.set(http::field::cache_control, "no-cache");
                response.body() = "event: snapshot\ndata: " + store_->snapshot() + "\n\n";
            } else {
                response.set(http::field::content_type, "application/json");
                response.body() = store_->snapshot();
            }
            response.prepare_payload();
            http::write(socket, response, error);
            boost::system::error_code ignored;
            socket.shutdown(tcp::socket::shutdown_both, ignored);
        }
    } catch (...) {
        // The status endpoint is observational. A listener failure must not affect the paper core.
    }
}

}  // namespace astra
