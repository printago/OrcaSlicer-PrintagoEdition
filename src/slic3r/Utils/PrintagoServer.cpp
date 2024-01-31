#include "PrintagoServer.hpp"
#include <boost/asio/steady_timer.hpp>

#include <chrono>
// #include "slic3r/GUI/SelectMachine.hpp"

namespace beef = boost::beast;

namespace Slic3r {

void printago_ws_error(beef::error_code ec, char const* what) { BOOST_LOG_TRIVIAL(error) << what << ": " << ec.message(); }

//``````````````````````````````````````````````````
//------------------PrintagoSession------------------
//``````````````````````````````````````````````````
PrintagoSession::PrintagoSession(tcp::socket&& socket) : ws_(std::move(socket)) {}

void PrintagoSession::run() { on_run(); }

void PrintagoSession::on_run()
{
    // Set suggested timeout settings for the websocket
    // ... other setup ...
    ws_.async_accept([capture0 = shared_from_this()](auto&& PH1) { capture0->on_accept(std::forward<decltype(PH1)>(PH1)); });
}

void PrintagoSession::on_accept(beef::error_code ec)
{
    if (ec)
        return printago_ws_error(ec, "accept");

    do_read();
}

void PrintagoSession::do_read()
{
    ws_.async_read(buffer_, [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
        capture0->on_read(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void PrintagoSession::on_read(beef::error_code ec, std::size_t bytes_transferred)
{
    if (ec) {
        printago_ws_error(ec, "read");
    } else {
        ws_.text(ws_.got_text());
        const auto msg = beef::buffers_to_string(buffer_.data());
        PrintagoDirector::ParseCommand(msg);
        buffer_.consume(buffer_.size());
        do_read();
    }
}

void PrintagoSession::on_write(beef::error_code ec, std::size_t bytes_transferred)
{
    if (ec)
        return printago_ws_error(ec, "write");

    buffer_.consume(buffer_.size());
    do_read();
}

void PrintagoSession::async_send(const std::string& message)
{
    net::post(ws_.get_executor(), [self = shared_from_this(), message]() { self->do_write(message); });
}

void PrintagoSession::do_write(const std::string& message)
{
    ws_.async_write(net::buffer(message), [self = shared_from_this()](beef::error_code ec, std::size_t length) {
        if (ec) {
            printago_ws_error(ec, "write");
        }
    });
}

//``````````````````````````````````````````````````
//------------------PrintagoServer------------------
//``````````````````````````````````````````````````
PrintagoServer::PrintagoServer(net::io_context& ioc, tcp::endpoint endpoint) : ioc_(ioc), acceptor_(ioc), reconnection_delay_(1)
{
    beef::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
        printago_ws_error(ec, "open");

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec)
        printago_ws_error(ec, "set_option");

    acceptor_.bind(endpoint, ec);
    if (ec)
        printago_ws_error(ec, "bind");

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
        printago_ws_error(ec, "listen");
}

void PrintagoServer::start() { do_accept(); }

void PrintagoServer::do_accept()
{
    acceptor_.async_accept(net::make_strand(ioc_), [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
        capture0->on_accept(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void PrintagoServer::on_accept(beef::error_code ec, tcp::socket socket)
{
    if (ec) {
        printago_ws_error(ec, "accept");
        handle_reconnect();
    } else {
        reconnection_delay_ = 1; // Reset delay on successful connection
        auto session        = std::make_shared<PrintagoSession>(std::move(socket));
        set_session(session);
        session->run();
        do_accept(); // Continue to accept new connections
    }
}

void PrintagoServer::handle_reconnect()
{
    if (reconnection_delay_ < 120) {
        reconnection_delay_ *= 2; // Exponential back-off
    }
    auto timer = std::make_shared<net::steady_timer>(ioc_, std::chrono::seconds(reconnection_delay_));
    timer->async_wait([capture0 = shared_from_this(), timer](const beef::error_code&) { capture0->do_accept(); });
}

//``````````````````````````````````````````````````
//------------------PrintagoGUIJob------------------
//``````````````````````````````````````````````````
bool PrintagoGUIJob::SetCanProcessJob(const bool can_process_job)
{
    if (can_process_job) {
        printerId.Clear();
        command.Clear();
        localFile.Clear();
        serverState = JobServerState::Idle;
        configFiles.clear();
        progress = 0;
        jobId    = "ptgo_default";

        // wxGetApp().mainframe->m_tabpanel->Enable();
        // wxGetApp().mainframe->m_topbar->Enable();
    } else {
        // wxGetApp().mainframe->m_tabpanel->Disable();
        // wxGetApp().mainframe->m_topbar->Disable();
    }
    m_can_process_job = can_process_job;
    return can_process_job;
}

//``````````````````````````````````````````````````
//------------------PrintagoDirector------------------
//``````````````````````````````````````````````````
PrintagoDirector::PrintagoDirector()
{
    // Initialize and start the server
    _io_context   = std::make_shared<net::io_context>();
    auto endpoint = tcp::endpoint(net::ip::make_address("0.0.0.0"), PRINTAGO_PORT);
    server        = std::make_shared<PrintagoServer>(*_io_context, endpoint);
    server->start();

    // Start the server on a separate thread
    server_thread = std::thread([this] { _io_context->run(); });
    server_thread.detach(); // Detach the thread

    // Initialize other members as needed
    m_select_machine_dlg = new Slic3r::GUI::SelectMachineDialog(/* constructor arguments if any */);
}

PrintagoDirector::~PrintagoDirector()
{
    // Ensure proper cleanup
    if (_io_context) {
        _io_context->stop();
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }

    // Clean up other resources
    delete m_select_machine_dlg;
}

void PrintagoDirector::SendMessage(const std::string& message)
{
    auto session = server->get_session();
    if (session) {
        session->async_send(message);
    }
}

bool PrintagoDirector::PrintagoDirector::ParseCommand(const std::string& command) { return true; }

bool PrintagoDirector::ProcessPrintagoCommand(const PrintagoCommand& command)
{
    return true;
}

} // namespace Slic3r
