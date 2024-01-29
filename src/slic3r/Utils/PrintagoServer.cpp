#include "PrintagoServer.hpp"
#include <boost/asio/steady_timer.hpp>
#include <chrono>

void fail(beast::error_code ec, char const* what) { std::cerr << what << ": " << ec.message() << "\n"; }

// PrintagoSession Implementation
PrintagoSession::PrintagoSession(tcp::socket&& socket) : ws_(std::move(socket)) {}

void PrintagoSession::run() { on_run(); }

void PrintagoSession::on_run()
{
    // Set suggested timeout settings for the websocket
    // ... other setup ...
    ws_.async_accept(std::bind(&PrintagoSession::on_accept, shared_from_this(), std::placeholders::_1));
}

void PrintagoSession::on_accept(beast::error_code ec)
{
    if (ec)
        return fail(ec, "accept");

    do_read();
}

void PrintagoSession::do_read()
{
    ws_.async_read(buffer_, std::bind(&PrintagoSession::on_read, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void PrintagoSession::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
    if (ec)
        return fail(ec, "read");

    ws_.async_write(buffer_.data(), std::bind(&PrintagoSession::on_write, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void PrintagoSession::on_write(beast::error_code ec, std::size_t bytes_transferred)
{
    if (ec)
        return fail(ec, "write");

    buffer_.consume(buffer_.size());
    do_read();
}

// PrintagoServer Implementation
PrintagoServer::PrintagoServer(net::io_context& ioc, tcp::endpoint endpoint) : ioc_(ioc), acceptor_(ioc), reconnection_delay_(1)
{
    beast::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
        fail(ec, "open");

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec)
        fail(ec, "set_option");

    acceptor_.bind(endpoint, ec);
    if (ec)
        fail(ec, "bind");

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
        fail(ec, "listen");
}

void PrintagoServer::start() { do_accept(); }

void PrintagoServer::do_accept()
{
    acceptor_.async_accept(net::make_strand(ioc_),
                           std::bind(&PrintagoServer::on_accept, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void PrintagoServer::on_accept(beast::error_code ec, tcp::socket socket)
{
    if (ec) {
        fail(ec, "accept");
        handle_reconnect();
    } else {
        reconnection_delay_ = 1; // Reset delay on successful connection
        std::make_shared<PrintagoSession>(std::move(socket))->run();
        do_accept(); // Continue to accept new connections
    }
}

void PrintagoServer::handle_reconnect()
{
    if (reconnection_delay_ < 120) {
        reconnection_delay_ *= 2; // Exponential back-off
    }
    net::steady_timer timer(ioc_, std::chrono::seconds(reconnection_delay_));
    timer.wait(); // Blocking wait
    do_accept();  // Try to accept again
}
