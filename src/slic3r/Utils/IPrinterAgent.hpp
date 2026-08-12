#ifndef __I_PRINTER_AGENT_HPP__
#define __I_PRINTER_AGENT_HPP__

#include "bambu_networking.hpp"
#include <slic3r/GUI/DeviceManager.hpp>
// why: these extend the BAMBU_NETWORK_* return space rather than opening a new one - the value
// flows through the same int domain callers already compare against BAMBU_NETWORK_SUCCESS.
// They live here and not in bambu_networking.hpp because that file is a vendor header replaced
// wholesale by header-sync commits (see c09252ce11), which would silently clobber them.
// -70xx is free: the vendor occupies -1..-25 and -10xx through -60xx.
#define ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED -7010 // no translation exists for this command
#define ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE -7020 // a translation exists; this printer lacks the capability
#define ORCA_NETWORK_ERR_ACCESS_VERIFICATION_FAILED -7030 // printer access preflight failed before printing
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <cstdint>

#include "NetworkAgent.hpp"

namespace Slic3r {

class ICloudServiceAgent;

/**
 * AgentInfo - Metadata structure for printer agent information.
 *
 * Contains identification and descriptive information about a printer agent
 * implementation, used for discovery and selection purposes.
 */
struct AgentInfo {
    std::string id;         ///< Unique identifier for the agent, e.g. "orca", "bbl"
    std::string name;       ///< Human-readable agent name, e.g. "Orca", "Bambu Lab"
    std::string version;    ///< Agent version string, e.g. "1.0.0"
    std::string description; ///< Brief description of the agent's capabilities, e.g. "Orca printer agent"
};

/**
 * FilamentSyncMode - Modes for filament data synchronization.
 *
 * Defines how filament information is obtained from the printer:
 * - Subscription: Real-time push updates (e.g., MQTT subscriptions)
 * - Pull: On-demand fetch via REST API (blocking call)
 * - None: Filament sync unavailable
 */
enum class FilamentSyncMode {
    none = 0,     ///< Filament synchronization not supported
    subscription, ///< Real-time push updates via subscription (e.g., MQTT)
    pull          ///< On-demand fetch via REST API (blocking call)
};

class IFileTransferTunnel
{
public:
    using ConnectionCb   = std::function<void(bool is_success, int err_code, std::string error_msg)>;
    using TunnelStatusCb = std::function<void(int old_status, int new_status, int err_code, std::string error_msg)>;

    explicit IFileTransferTunnel(const std::string& url) : url_(url) {}
    virtual ~IFileTransferTunnel() = default;

    IFileTransferTunnel(const IFileTransferTunnel&)            = delete;
    IFileTransferTunnel& operator=(const IFileTransferTunnel&) = delete;
    IFileTransferTunnel(IFileTransferTunnel&&)                 = delete;
    IFileTransferTunnel& operator=(IFileTransferTunnel&&)      = delete;

    virtual void start_connect() = 0;
    virtual bool sync_start_connect() = 0;
    virtual void on_connection(ConnectionCb cb) { conn_cb_ = std::move(cb); }
    virtual void on_status(TunnelStatusCb cb) { status_cb_ = std::move(cb); }

    virtual void shutdown() = 0;

    virtual int get_status() const { return status_; }
    virtual bool check_valid() const = 0;

    // why: IFileTransferJob::start_on() only ever sees a tunnel through this interface,
    // but needs the concrete backend handle to hand to its own start-job call - native()
    // is the type-erased escape hatch, same pattern IFileTransferJob::native() already uses.
    virtual void *native() const noexcept { return nullptr; }

protected:
    std::string url_;
    int status_{};
    ConnectionCb conn_cb_{};
    TunnelStatusCb status_cb_{};
};

class IFileTransferJob {
public:
    using ResultCb = std::function<void(int res, int resp_ec, std::string json_res, std::vector<std::byte> bin_res)>;
    using MsgCb = std::function<void(int kind, std::string json)>;

    explicit IFileTransferJob(const std::string &params_json) : params_json_(params_json) {}
    virtual ~IFileTransferJob() = default;

    IFileTransferJob(const IFileTransferJob &)            = delete;
    IFileTransferJob &operator=(const IFileTransferJob &) = delete;
    IFileTransferJob(IFileTransferJob &&)                 = delete;
    IFileTransferJob &operator=(IFileTransferJob &&)      = delete;

    virtual void on_result(ResultCb cb) { result_cb_ = std::move(cb); }
    virtual bool get_result(int &ec, int &resp_ec, std::string &json, std::vector<std::byte> &bin, uint32_t timeout_ms) = 0;
    virtual void start_on(IFileTransferTunnel &t) = 0;
    virtual void on_msg(MsgCb cb) { msg_cb_ = std::move(cb); }
    virtual bool try_get_msg(int &kind, std::string &json) = 0;
    virtual bool get_msg(uint32_t timeout_ms, int &kind, std::string &json) = 0;
    virtual void *native() const noexcept { return nullptr; }
    virtual bool          check_valid() const = 0;
    virtual bool          finished() const { return finished_; }
    virtual void cancel() = 0;

protected:
    std::string             params_json_;
    ResultCb                result_cb_{};
    MsgCb                   msg_cb_{};
    bool                    finished_ = false;
    int                     res_      = 0;
    int                     resp_ec_  = 0;
    std::string             res_json_;
    std::vector<std::byte>  res_bin_;
};

/**
 * IPrinterAgent - Interface for printer operations.
 *
 * This interface encapsulates all printer-related functionality:
 * - Direct printer communication (LAN and cloud relay)
 * - Certificate management
 * - Device discovery (SSDP)
 * - Printer binding/unbinding
 * - Print job operations
 *
 * Implementations:
 * - OrcaPrinterAgent: Stub implementation (printer ops not yet supported)
 * - PrinterAgentPluginCapability: Python printer-agent plugin capability that
 *   implements IPrinterAgent directly and is handed out as the live agent
 * - BBLPrinterAgent: Wrapper around Bambu Lab's proprietary DLL
 *
 * Token Access:
 * Printer agents receive an ICloudServiceAgent instance via set_cloud_agent() to
 * access tokens for cloud-relay operations.
 */

class IPrinterAgent {
public:
    virtual ~IPrinterAgent() = default;

    // Cloud Agent Dependency
    // ========================================================================
    /**
     * Set the cloud agent used for token access.
     * Must be called before any cloud-relay operations.
     */
    virtual void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) = 0;

    // ========================================================================
    // Communication
    // ========================================================================
    /**
     * Publish a JSON command to a printer through cloud relay.
     */
    virtual int send_message(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    // why: gcode is firmware dialect, not a waist concept - commands whose body is Bambu-dialect
    // gcode live on the agent that speaks it; the default is an honest refusal that MachineObject's
    // publish funnel turns into a dialog.
    virtual int command_ams_refresh_rfid(std::string, std::string, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_calibrate(std::string, int, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_ams_select_tray(std::string, std::string, int, bool)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }
    virtual int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed,
                                      bool is_core_xy, bool supports_mqtt_axis_control, int sequence_id, bool lan_mode)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

    /**
     * Build a ready-to-use local (LAN) camera stream URL for this agent's protocol.
     * Returns an empty string if the agent has no local camera stream support.
     */
    virtual std::string get_local_camera_url(CameraURLParams params) { return ""; }

    /**
     * Build a ready-to-use local (LAN) file transfer URL for this agent's protocol.
     * Returns an empty string if the agent has no local file transfer support.
     */
    virtual std::string get_local_file_transfer_url(const FileTransferURLParams& params) { return ""; }

    virtual int get_file_transfer_url(std::string, std::function<void(FileTransferURLResult)>, FileTransferURLParams)
    { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

    /**
     * Default LAN account username for this agent's protocol, if it has a fixed one.
     * Returns an empty string if the agent has no fixed default (e.g. caller must supply one).
     */
    virtual std::string default_lan_username() const { return {}; }

    /**
     * Establish a direct LAN connection to a printer.
     */
    virtual int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) = 0;

    /**
     * Tear down the active LAN printer connection.
     */
    virtual int disconnect_printer() = 0;

    /**
     * Send a JSON command to a LAN printer (bypassing cloud).
     */
    virtual int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) = 0;

    // ========================================================================
    // Certificates
    // ========================================================================
    /**
     * Validate current user certificates for the printer.
     */
    virtual int check_cert() = 0;

    /**
     * Install or refresh device certificate for LAN TLS.
     */
    virtual void install_device_cert(std::string dev_id, bool lan_only) = 0;

    // ========================================================================
    // Discovery
    // ========================================================================
    /**
     * Start or stop SSDP discovery.
     */
    virtual bool start_discovery(bool start, bool sending) = 0;

    // ========================================================================
    // Binding
    // ========================================================================
    /**
     * Ping the binding endpoint to check printer readiness.
     */
    virtual int ping_bind(std::string ping_code) = 0;

    /**
     * Perform binding detection/handshake on a LAN printer.
     */
    virtual int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) = 0;

    /**
     * Execute the multi-stage printer binding workflow.
     */
    virtual int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) = 0;

    /**
     * Remove the association between account and printer.
     */
    virtual int unbind(std::string dev_id) = 0;

    /**
     * Request a one-time bind ticket from the server.
     */
    virtual int request_bind_ticket(std::string* ticket) = 0;

    /**
     * Fetch the cloud snapshot image captured at a print failure.
     * Returns 0 if the request was dispatched; the image body arrives via callback(body, http_status).
     */
    virtual int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) = 0;

    /**
     * Register callback for fatal HTTP errors.
     */
    virtual int set_server_callback(OnServerErrFn fn) = 0;

    // ========================================================================
    // Machine Selection
    // ========================================================================
    /**
     * Return the currently selected printer ID.
     */
    virtual std::string get_user_selected_machine() = 0;

    /**
     * Update the selected machine preference.
     */
    virtual int set_user_selected_machine(std::string dev_id) = 0;

    // ========================================================================
    // Subscriptions
    // ========================================================================
    /**
     * Subscribe to a logical module (for example app- or tunnel-scoped streams).
     */
    virtual int start_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Stop listening to a formerly subscribed module.
     */
    virtual int stop_subscribe(std::string module) { (void) module; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Subscribe to push streams for specific device identifiers.
     */
    virtual int add_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

    /**
     * Remove device-level subscriptions.
     */
    virtual int del_subscribe(std::vector<std::string> dev_list) { (void) dev_list; return BAMBU_NETWORK_SUCCESS; }

    // ========================================================================
    // Print Job Operations
    // ========================================================================
    /**
     * Start a fully managed cloud print.
     */
    virtual int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a local print with cloud record.
     */
    virtual int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Upload gcode to printer's SD card without starting.
     */
    virtual int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = 0;

    /**
     * Start a LAN-only print.
     */
    virtual int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    /**
     * Start a print from printer's SD card.
     */
    virtual int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = 0;

    // ========================================================================
    // Callback Registration
    // ========================================================================
    /**
     * Register SSDP discovery callback.
     */
    virtual int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) = 0;

    /**
     * Register printer MQTT connection callback.
     */
    virtual int set_on_printer_connected_fn(OnPrinterConnectedFn fn) = 0;

    /**
     * Register subscription failure callback.
     */
    virtual int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) = 0;

    /**
     * Register cloud device message callback.
     */
    virtual int set_on_message_fn(OnMessageFn fn) = 0;

    /**
     * Register user-scoped message callback.
     */
    virtual int set_on_user_message_fn(OnMessageFn fn) = 0;

    /**
     * Register LAN connection status callback.
     */
    virtual int set_on_local_connect_fn(OnLocalConnectedFn fn) = 0;

    /**
     * Register LAN message callback.
     */
    virtual int set_on_local_message_fn(OnMessageFn fn) = 0;

    /**
     * Provide main thread queue callback.
     */
    virtual int set_queue_on_main_fn(QueueOnMainFn fn) = 0;

    /**
     * Get agent information.
     */
    virtual AgentInfo get_agent_info() = 0;

    // ========================================================================
    // Filament Operations
    // ========================================================================
    /**
     * Get the filament synchronization mode for this agent.
     * 
     * @return FilamentSyncMode indicating how filament data is obtained:
     *         - subscription: Real-time push updates via MQTT (no fetch needed)
     *         - pull: On-demand fetch via REST API (call fetch_filament_info())
     *         - none: Filament synchronization not supported
     */
    virtual FilamentSyncMode get_filament_sync_mode() const { return FilamentSyncMode::none; }

    /**
     * Refresh filament info from the printer synchronously.
     * Should only be called when get_filament_sync_mode() returns FilamentSyncMode::pull.
     * Populates the MachineObject's DevFilaSystem with fetched filament data.
     */
    virtual bool fetch_filament_info(std::string dev_id) { return false; }

    /**
     * Build a local eMMC transfer tunnel for this agent's protocol, if it has one.
     * Returns nullptr if the agent has no local file-transfer tunnel concept
     * (matches the inert-default pattern used by get_local_camera_url() etc. above -
     * this stays non-pure so adding it doesn't force every IPrinterAgent implementation,
     * including the Python plugin capability bridge, to override a Bambu-only concept).
     */
    virtual std::unique_ptr<IFileTransferTunnel> create_file_transfer_tunnel(std::string& dev_ip, std::string& access_code)
    { return nullptr; }

    /**
     * Wrap an already-resolved transfer URL (e.g. a cloud-relay/TUTK URL obtained via
     * a camera-url lookup) in a local transfer tunnel. Same inert-default reasoning as
     * create_file_transfer_tunnel() above; use that one instead when building a tunnel
     * straight from dev_ip/access_code.
     */
    virtual std::unique_ptr<IFileTransferTunnel> create_file_transfer_tunnel_from_url(std::string url)
    { return nullptr; }

    /**
     * Build a file-transfer job (media-ability query, upload, ...) to run on a tunnel
     * from create_file_transfer_tunnel()/create_file_transfer_tunnel_from_url(). Same
     * inert-default reasoning as the tunnel factories above.
     */
    virtual std::unique_ptr<IFileTransferJob> create_file_transfer_job(std::string params_json)
    { return nullptr; }
};

} // namespace Slic3r

#endif // __I_PRINTER_AGENT_HPP__
