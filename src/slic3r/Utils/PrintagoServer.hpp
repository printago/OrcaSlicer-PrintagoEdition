#ifndef PRINTAGOSERVER_HPP
#define PRINTAGOSERVER_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "nlohmann/json.hpp"

#include "slic3r/GUI/SelectMachine.hpp"

using namespace nlohmann;
namespace beef      = boost::beast;
namespace websocket = beef::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

static constexpr short PRINTAGO_PORT = 33647;

void printago_ws_error(beef::error_code ec, char const* what);

//``````````````````````````````````````````````````
//------------------PrintagoSessions------------------
//``````````````````````````````````````````````````
class PrintagoSession : public std::enable_shared_from_this<PrintagoSession>
{
    websocket::stream<tcp::socket> ws_;
    beef::flat_buffer             buffer_;

public:
    explicit PrintagoSession(tcp::socket&& socket);
    void run();

private:
    void on_run();
    void on_accept(beef::error_code ec);
    void do_read();
    void on_read(beef::error_code ec, std::size_t bytes_transferred);
    void on_write(beef::error_code ec, std::size_t bytes_transferred);
    void async_send(const std::string& message);
    void do_write(const std::string& message);
};

//``````````````````````````````````````````````````
//------------------PrintagoServer------------------
//``````````````````````````````````````````````````
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
    void on_accept(beef::error_code ec, tcp::socket socket);
    void handle_reconnect();
};

//``````````````````````````````````````````````````
//------------------PrintagoCommand-----------------
//``````````````````````````````````````````````````
class PrintagoCommand
{
public:
    PrintagoCommand(const wxString&         command_type,
                    const wxString&         action,
                    wxStringToStringHashMap parameters,
                    const wxString&         originalCommandStr)
        : m_command_type(command_type), m_action(action), m_parameters(std::move(parameters)), m_original_command_str(originalCommandStr)
    {}

    PrintagoCommand(const PrintagoCommand& other)
        : m_command_type(other.m_command_type)
        , m_action(other.m_action)
        , m_parameters(other.m_parameters)
        , m_original_command_str(other.m_original_command_str)
    {}

    virtual ~PrintagoCommand() {}

    void SetCommandType(const wxString& command) { m_command_type = command; }
    void SetAction(const wxString& action) { m_action = action; }
    void SetParameters(const wxStringToStringHashMap& parameters) { m_parameters = parameters; }
    void SetOriginalCommandStr(const wxString& originalCommandStr) { m_original_command_str = originalCommandStr; }

    wxString                GetCommandType() const { return m_command_type; }
    wxString                GetAction() const { return m_action; }
    wxStringToStringHashMap GetParameters() const { return m_parameters; }
    wxString                GetOriginalCommandStr() const { return m_original_command_str; }

private:
    wxString                m_command_type;
    wxString                m_action;
    wxStringToStringHashMap m_parameters;
    wxString                m_original_command_str;
};

//``````````````````````````````````````````````````
//------------------PrintagoMessage-----------------
//``````````````````````````````````````````````````
class PrintagoMessageEvent : public wxEvent
{
public:
    PrintagoMessageEvent(wxEventType eventType = wxEVT_NULL) : wxEvent(0, eventType) {}

    PrintagoMessageEvent(const PrintagoMessageEvent& event)
    {
        this->m_message_type = event.m_message_type;
        this->m_printer_id   = event.m_printer_id;
        this->m_command      = event.m_command;
        this->m_data         = event.m_data;
    }

    virtual ~PrintagoMessageEvent() {}

    wxEvent* Clone() const override { return new PrintagoMessageEvent(*this); }

    void SetMessageType(const wxString& message) { this->m_message_type = message; }
    void SetPrinterId(const wxString& printer_id) { this->m_printer_id = printer_id; }
    void SetCommand(const wxString& command) { this->m_command = command; }
    void SetData(const json& data) { this->m_data = data; }

    wxString GetMessageType() const { return this->m_message_type; }
    wxString GetPrinterId() const { return this->m_printer_id; }
    wxString GetCommand() const { return this->m_command; }
    json     GetData() const { return this->m_data; }

private:
    wxString m_message_type;
    wxString m_printer_id;
    wxString m_command;
    json     m_data;
};

//``````````````````````````````````````````````````
//------------------PrintagoGUIJob------------------
//``````````````````````````````````````````````````
class PrintagoGUIJob
{
private:
    static bool SetCanProcessJob(const bool can_process_job);
    inline static bool m_can_process_job;

public:
    enum class JobServerState { Idle, Download, Configure, Slicing, Sending };
    operator std::string() const
    {
        switch (serverState) {
        case JobServerState::Idle: return "idle";
        case JobServerState::Download: return "download";
        case JobServerState::Configure: return "configure";
        case JobServerState::Slicing: return "slicing";
        case JobServerState::Sending: return "sending";
        default: return "unknown";
        }
    }

    inline static JobServerState                              serverState = JobServerState::Idle;
    inline static wxString                                    jobId = "ptgo_default";
    inline static wxString                                    printerId;
    inline static wxString                                    command;
    inline static wxFileName                                  localFile;
    inline static std::unordered_map<std::string, wxFileName> configFiles;
    inline static int                                         progress = 0;

    // Public getter for m_can_process_job
    static bool CanProcessJob() { return m_can_process_job; }
    static bool BlockJobProcessing() { return SetCanProcessJob(false); }
    static bool UnblockJobProcessing() { return SetCanProcessJob(true); }

};

//``````````````````````````````````````````````````
//------------------PrintagoDirector----------------
//``````````````````````````````````````````````````
class PrintagoDirector
{
public:
    PrintagoDirector();
    ~PrintagoDirector();

    void        SendMessage(const std::string& message);
    static bool ParseCommand(const std::string& command);

private:
    std::shared_ptr<net::io_context> _io_context;
    std::shared_ptr<PrintagoServer>  server;
    std::thread                      server_thread;

    Slic3r::GUI::SelectMachineDialog* m_select_machine_dlg = nullptr;

    static bool ProcessPrintagoCommand(const PrintagoCommand& command);

    void SendStatusMessage   (const wxString printer_id, const json     statusData,   const wxString command = "");
    void SendResponseMessage (const wxString printer_id, const json     responseData, const wxString command = "");
    void SendSuccessMessage  (const wxString printer_id, const wxString localCommand, const wxString command = "", const wxString localCommandDetail = "");
    void SendErrorMessage    (const wxString printer_id, const wxString localCommand, const wxString command = "", const wxString errorDetail = "");
    void SendJsonErrorMessage(const wxString printer_id, const wxString localCommand, const wxString command = "", const json     errorDetail = "");
};



#endif // PRINTAGOSERVER_HPP
