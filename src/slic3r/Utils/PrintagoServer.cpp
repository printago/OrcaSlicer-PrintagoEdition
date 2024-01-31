#include "PrintagoServer.hpp"
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <wx/tokenzr.h>
// #include "slic3r/GUI/SelectMachine.hpp"

namespace beef = boost::beast;
using namespace Slic3r::GUI;

namespace Slic3r {

#define PRINTAGO_TEMP_THRESHOLD_ALLOW_E_CTRL 170.0f // Minimum temperature to allow extrusion control (per StatusPanel.cpp)

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
bool PBJob::SetCanProcessJob(const bool can_process_job)
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

void PrintagoDirector::PostErrorMessage(const wxString printer_id,
                                        const wxString localCommand,
                                        const wxString command,
                                        const wxString errorDetail)
{
    if (PBJob::CanProcessJob()) {
        PBJob::UnblockJobProcessing();
    }

    json errorResponse;
    errorResponse["local_command"] = localCommand.ToStdString();
    errorResponse["error_detail"]  = errorDetail.ToStdString();
    errorResponse["success"]       = false;

    auto* resp = new PrintagoResponse();
    resp->SetMessageType("error");
    resp->SetPrinterId(printer_id);
    const wxURL url(command);
    wxString    path = url.GetPath();
    resp->SetCommand((!path.empty() && path[0] == '/') ? path.Remove(0, 1).ToStdString() : path.ToStdString());
    resp->SetData(errorResponse);

    _PostResponse(*resp);
    
}

void PrintagoDirector::_PostResponse(const PrintagoResponse response)
{
    wxDateTime now = wxDateTime::Now();
    now.MakeUTC();
    const wxString timestamp = now.FormatISOCombined() + "Z";

    json message;
    message["type"]        = response.GetMessageType().ToStdString();
    message["timestamp"]   = timestamp.ToStdString();
    message["printer_id"]  = response.GetPrinterId().ToStdString();
    message["client_type"] = "bambu";
    message["command"]     = response.GetCommand().ToStdString();
    message["data"]        = response.GetData();

    const std::string messageStr = message.dump();

    auto session = server->get_session();
    if (session) {
        session->async_send(messageStr);
    }
}

bool PrintagoDirector::ParseCommand(const std::string& command)
{
    const wxString command_str(command);
    if (!command_str.StartsWith("printago://")) {
        return false;
    }

    wxURI         uri(command_str);
    wxString      path           = uri.GetPath();
    wxArrayString pathComponents = wxStringTokenize(path, "/");
    wxString      commandType, action;

    if (pathComponents.GetCount() != 3) {
        PostErrorMessage("", "", "", "invalid printago command");
        return false;
    }

    commandType = pathComponents.Item(1); 
    action      = pathComponents.Item(2); 

    wxString                query      = uri.GetQuery();          
    wxStringToStringHashMap parameters = _ParseQueryString(query); 

    PrintagoCommand printagoCommand;
    printagoCommand.SetCommandType(commandType);
    printagoCommand.SetAction(action);
    printagoCommand.SetParameters(parameters);
    printagoCommand.SetOriginalCommandStr(command_str.ToStdString());

    if (!ValidatePrintagoCommand(printagoCommand)) {
        PostErrorMessage("", "", command_str, "invalid printago command");
        return false;
    } else {
        ProcessPrintagoCommand(printagoCommand);
        // CallAfter([=]() { HandlePrintagoCommand(printagoCommand); });
    }

    return true;
}

wxStringToStringHashMap PrintagoDirector::_ParseQueryString(const wxString& queryString)
{
    wxStringToStringHashMap params;

    // Split the query string on '&' to get key-value pairs
    wxStringTokenizer tokenizer(queryString, "&");
    while (tokenizer.HasMoreTokens()) {
        wxString token = tokenizer.GetNextToken();

        // Split each key-value pair on '='
        wxString key   = token.BeforeFirst('=');
        wxString value = token.AfterFirst('=');

        // URL-decode the key and value
        wxString decodedKey   = wxURI::Unescape(key);
        wxString decodedValue = wxURI::Unescape(value);

        params[decodedKey] = decodedValue;
    }
    return params;
}

bool PrintagoDirector::ValidatePrintagoCommand(const PrintagoCommand& cmd)
{
    wxString commandType = cmd.GetCommandType();
    wxString action      = cmd.GetAction();

    // Map of valid command types to their corresponding valid actions
    std::map<std::string, std::set<std::string>> validCommands = {{"status", {"get_machine_list", "get_config", "switch_active"}},
                                                                  {"printer_control",
                                                                   {"pause_print", "resume_print", "stop_print", "get_status",
                                                                    "start_print_bbl"}},
                                                                  {"temperature_control", {"set_hotend", "set_bed"}},
                                                                  {"movement_control", {"jog", "home", "extrude"}}};

    auto commandIter = validCommands.find(commandType.ToStdString());
    if (commandIter != validCommands.end() && commandIter->second.find(action.ToStdString()) != commandIter->second.end()) {
        return true;
    }
    return false;
}

bool PrintagoDirector::ProcessPrintagoCommand(const PrintagoCommand& cmd)
{
    wxString                commandType        = cmd.GetCommandType();
    wxString                action             = cmd.GetAction();
    wxStringToStringHashMap parameters         = cmd.GetParameters();
    wxString                originalCommandStr = cmd.GetOriginalCommandStr();
    wxString                actionDetail;

    MachineObject* printer = {nullptr};

    wxString printerId    = parameters.count("printer_id") ? parameters["printer_id"] : "Unspecified";
    bool     hasPrinterId = printerId.compare("Unspecified");

    if (!commandType.compare("status")) {
        std::string username = wxGetApp().getAgent()->is_user_login() ? wxGetApp().getAgent()->get_user_name() : "nouser@bab";
        if (!action.compare("get_machine_list")) {
            SendResponseMessage(username, GetAllStatus(), originalCommandStr);
        } else if (!action.compare("get_config")) {
            wxString config_type = parameters["config_type"];                                 // printer, filament, print
            wxString config_name = Http::url_decode(parameters["config_name"].ToStdString()); // name of the config
            json     configJson  = GetConfigByName(config_type, config_name);
            if (!configJson.empty()) {
                SendResponseMessage(username, configJson, originalCommandStr);
            } else {
                PostErrorMessage(username, action, originalCommandStr, "config not found; valid types are: print, printer, or filament");
                return false;
            }
        } else if (!action.compare("switch_active")) {
            if (!PBJob::CanProcessJob()) {
                PostErrorMessage("", action, originalCommandStr, "unable, UI blocked");
            }
            if (hasPrinterId) {
                if (!SwitchSelectedPrinter(printerId)) {
                    PostErrorMessage("", action, originalCommandStr, "unable, unknown");
                } else {
                    actionDetail = wxString::Format("connecting to %s", printerId);
                    SendSuccessMessage(printerId, action, originalCommandStr, actionDetail);
                }
            } else {
                PostErrorMessage("", action, originalCommandStr, "no printer_id specified");
            }
        }
        return true;
    }

    if (!hasPrinterId) {
        PostErrorMessage("", action, originalCommandStr, "no printer_id specified");
        return false;
    }
    // Find the printer in the machine list
    auto machineList = GUI_App().getDeviceManager()->get_my_machine_list();
    auto it          = std::find_if(machineList.begin(), machineList.end(),
                                    [&printerId](const std::pair<std::string, MachineObject*>& pair) { return pair.second->dev_id == printerId; });

    if (it != machineList.end()) {
        printer = it->second;
    } else {
        PostErrorMessage(printerId, action, originalCommandStr, "no printer not found with ID: " + printerId);
        return false;
    }

    // select the printer for updates in the monitor for updates.
    SwitchSelectedPrinter(printerId);

    if (!commandType.compare("printer_control")) {
        if (!action.compare("pause_print")) {
            try {
                if (!printer->can_pause()) {
                    PostErrorMessage(printerId, action, originalCommandStr, "cannot pause printer");
                    return false;
                }
                printer->command_task_pause();
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred issuing pause_print");
                return false;
            }
        } else if (!action.compare("resume_print")) {
            try {
                if (!printer->can_resume()) {
                    PostErrorMessage(printerId, action, originalCommandStr, "cannot resume printer");
                    return false;
                }
                printer->command_task_resume();
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred issuing resume_print");
                return false;
            }
        } else if (!action.compare("stop_print")) {
            try {
                if (!printer->can_abort()) {
                    PostErrorMessage(printerId, action, originalCommandStr, "cannot abort printer");
                    return false;
                }
                printer->command_task_abort();
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred issuing stop_print");
                return false;
            }
        } else if (!action.compare("get_status")) {
            SendStatusMessage(printerId, GetMachineStatus(printer), originalCommandStr);
            return true;
        } else if (!action.compare("start_print_bbl")) {
            if (!printer->can_print() && PBJob::printerId.compare(printerId)) { // printer can print, and we're not already prepping for it.
                PostErrorMessage(printerId, action, originalCommandStr, "cannot start print");
                return false;
            }

            wxString printagoModelUrl = parameters["model"];
            wxString printerConfUrl   = parameters["printer_conf"];
            wxString printConfUrl     = parameters["print_conf"];
            wxString filamentConfUrl  = parameters["filament_conf"];

            wxString printagoId = parameters["printago_job"];
            if (!printagoId.empty()) {
                PBJob::jobId = printagoId;
            }

            PBJob::printerId = printerId;
            PBJob::command   = originalCommandStr;
            PBJob::localFile = "";

            if (!m_select_machine_dlg)
                m_select_machine_dlg = new SelectMachineDialog(wxGetApp().plater());

            if (!PBJob::CanProcessJob()) {
                PostErrorMessage(printerId, action, originalCommandStr, "busy with current job - check status");
                return false;
            }

            PBJob::BlockJobProcessing();

            if (printagoModelUrl.empty()) {
                PostErrorMessage(printerId, action, originalCommandStr, "no url specified");
                return false;
            } else if (printConfUrl.empty() || printerConfUrl.empty() || filamentConfUrl.empty()) {
                PostErrorMessage(printerId, action, originalCommandStr, "must specify printer, filament, and print configurations");
                return false;
            } else {
                printagoModelUrl = Http::url_decode(printagoModelUrl.ToStdString());
                printerConfUrl   = Http::url_decode(printerConfUrl.ToStdString());
                printConfUrl     = Http::url_decode(printConfUrl.ToStdString());
                filamentConfUrl  = Http::url_decode(filamentConfUrl.ToStdString());
            }

            PBJob::serverState = PBJob::JobServerState::Download;
            PBJob::progress = 10;

            // Second param is reference and modified inside SavePrintagoFile.
            if (SavePrintagoFile(printagoModelUrl, PBJob::localFile)) {
            } else {
                PostErrorMessage(printerId, wxString::Format("%s:%s", action, PBJob::serverState), originalCommandStr,
                                    "model download failed");
                return false;
            }

            // Do the configuring here: this allows 3MF files to load, then we can configure the slicer and override the 3MF conf settings
            // from what Printago sent.
            wxFileName localPrinterConf, localFilamentConf, localPrintConf;
            if (SavePrintagoFile(printerConfUrl, localPrinterConf) && SavePrintagoFile(filamentConfUrl, localFilamentConf) &&
                SavePrintagoFile(printConfUrl, localPrintConf)) {
            } else {
                PostErrorMessage(printerId, wxString::Format("%s:%s", action, PBJob::serverState), originalCommandStr,
                                    "config download failed");
                return false;
            }

            PBJob::configFiles["printer"] = localPrinterConf;
            PBJob::configFiles["filament"] = localFilamentConf;
            PBJob::configFiles["print"]    = localPrintConf;

            PBJob::serverState = PBJob::JobServerState::Configure;
            PBJob::progress = 30;

            wxGetApp().mainframe->select_tab(1);
            wxGetApp().plater()->reset();

            actionDetail = wxString::Format("slice_config: %s", PBJob::localFile.GetFullPath());

            // Loads the configs into the UI, if able; selects them in the dropdowns.
            ImportPrintagoConfigs();
            SetPrintagoConfigs();

            try {
                if (!PBJob::localFile.GetExt().MakeUpper().compare("3MF")) {
                    // The last 'true' tells the function to not ask the user to confirm the load; save any existing work.
                    wxGetApp().plater()->load_project(PBJob::localFile.GetFullPath(), "-", true);
                    SetPrintagoConfigs(); // since the 3MF may have it's own configs that get set on load.
                } else {
                    std::vector<std::string> filePathArray;
                    filePathArray.push_back(PBJob::localFile.GetFullPath().ToStdString());
                    LoadStrategy strategy = LoadStrategy::LoadModel |
                                            LoadStrategy::Silence; // LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary
                    wxGetApp().plater()->load_files(filePathArray, strategy, false);
                }
            } catch (...) {
                PostErrorMessage(PBJob::printerId, wxString::Format("%s:%s", "start_print_bbl", PBJob::serverState), PBJob::command,
                                    "and error occurred loading the model and config");
                return false;
            }

            PBJob::serverState = PBJob::JobServerState::Slicing;
            PBJob::progress    = 45;

            wxGetApp().plater()->select_plate(0, true);
            wxGetApp().plater()->reslice();
            actionDetail = wxString::Format("slice_start: %s", PBJob::localFile.GetFullPath());
        }
    } else if (!commandType.compare("temperature_control")) {
        if (!printer->can_print() && PBJob::printerId.compare(printerId)) {
            PostErrorMessage(printerId, action, originalCommandStr, "cannot control temperature; printer busy");
            return false;
        }
        wxString tempStr = parameters["temperature"];
        long     targetTemp;
        if (!tempStr.ToLong(&targetTemp)) {
            PostErrorMessage(printerId, action, originalCommandStr, "invalid temperature value");
            return false;
        }

        if (!action.compare("set_hotend")) {
            try {
                printer->command_set_nozzle(targetTemp);
                actionDetail = wxString::Format("%d", targetTemp);
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred setting nozzle temperature");
                return false;
            }
        } else if (!action.compare("set_bed")) {
            try {
                int limit = printer->get_bed_temperature_limit();
                if (targetTemp >= limit) {
                    targetTemp = limit;
                }
                printer->command_set_bed(targetTemp);
                actionDetail = wxString::Format("%d", targetTemp);
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred setting bed temperature");
                return false;
            }
        }
    } else if (!commandType.compare("movement_control")) {
        if (!printer->can_print() && PBJob::printerId.compare(printerId)) {
            PostErrorMessage(printerId, action, originalCommandStr, "cannot control movement; printer busy");
            return false;
        }
        if (!action.compare("jog")) {
            auto axes = ExtractPrefixedParams(parameters, "axes");
            if (axes.empty()) {
                PostErrorMessage(printerId, action, originalCommandStr, "no axes specified");
                return false;
            }

            if (!printer->is_axis_at_home("X") || !printer->is_axis_at_home("Y") || !printer->is_axis_at_home("Z")) {
                PostErrorMessage(printerId, action, originalCommandStr, "must home axes before moving");
                return false;
            }
            // Iterate through each axis and its value; we do this loop twice to ensure the input in clean.
            // this ensures we do not move the head unless all input moves are valid.
            for (const auto& axis : axes) {
                wxString axisName = axis.first;
                axisName.MakeUpper();
                if (axisName != "X" && axisName != "Y" && axisName != "Z") {
                    PostErrorMessage(printerId, action, originalCommandStr, "invalid axis name: " + axisName);
                    return false;
                }
                wxString axisValueStr = axis.second;
                double   axisValue;
                if (!axisValueStr.ToDouble(&axisValue)) {
                    PostErrorMessage(printerId, action, originalCommandStr, "invalid value for axis " + axisName);
                    return false;
                }
            }

            for (const auto& axis : axes) {
                wxString axisName = axis.first;
                axisName.MakeUpper();
                wxString axisValueStr = axis.second;
                double   axisValue;
                axisValueStr.ToDouble(&axisValue);
                try {
                    printer->command_axis_control(axisName.ToStdString(), 1.0, axisValue, 3000);
                } catch (...) {
                    PostErrorMessage(printerId, action, originalCommandStr, "an error occurred moving axis " + axisName);
                    return false;
                }
            }

        } else if (!action.compare("home")) {
            try {
                printer->command_go_home();
            } catch (...) {
                PostErrorMessage(printerId, action, originalCommandStr, "an error occurred homing axes");
                return false;
            }

        } else if (!action.compare("extrude")) {
            wxString amtStr = parameters["amount"];
            long     extrudeAmt;
            if (!amtStr.ToLong(&extrudeAmt)) {
                PostErrorMessage(printerId, action, originalCommandStr, "invalid extrude amount value");
                return false;
            }

            if (printer->nozzle_temp >= PRINTAGO_TEMP_THRESHOLD_ALLOW_E_CTRL) {
                try {
                    printer->command_axis_control("E", 1.0, extrudeAmt, 900);
                    actionDetail = wxString::Format("%d", extrudeAmt);
                } catch (...) {
                    PostErrorMessage(printerId, action, originalCommandStr, "an error occurred extruding filament");
                    return false;
                }
            } else {
                PostErrorMessage(printerId, action, originalCommandStr,
                                 wxString::Format("nozzle temperature too low to extrude (min: %.1f)",
                                                  PRINTAGO_TEMP_THRESHOLD_ALLOW_E_CTRL));
                return false;
            }
        }
    }

    // only send this response if it's *not* a start_print_bbl command.
    if (action.compare("start_print_bbl")) {
        SendSuccessMessage(printerId, action, originalCommandStr, actionDetail);
    }
    return true;
}

} // namespace Slic3r
