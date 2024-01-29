#ifndef PRINTAGOSERVER_HPP
#define PRINTAGOSERVER_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

void fail(beast::error_code ec, char const* what);

class PrintagoSession : public std::enable_shared_from_this<PrintagoSession>
{
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer             buffer_;

public:
    explicit PrintagoSession(tcp::socket&& socket);
    void run();

private:
    void on_run();
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
};

class PrintagoServer : public std::enable_shared_from_this<PrintagoServer>
{
    net::io_context& ioc_;
    tcp::acceptor    acceptor_;
    std::size_t      reconnection_delay_;

public:
    PrintagoServer(net::io_context& ioc, tcp::endpoint endpoint);
    void start();

private:
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);
    void handle_reconnect();
};

#endif // PRINTAGOSERVER_HPP
