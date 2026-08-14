#ifndef __BBL_PRINTER_AGENT_HPP__
#define __BBL_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include "FileTransferUtils.hpp"
#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace Slic3r {

/**
 * BBLFileTransferTunnel - Bambu eMMC tunnel, backed by the Bambu network
 * plugin's ft_tunnel_* ABI (see FileTransferUtils.hpp). Only BBLPrinterAgent
 * constructs these; callers only ever see them through IFileTransferTunnel.
 */
class BBLFileTransferTunnel : public IFileTransferTunnel
{
public:
    BBLFileTransferTunnel(const std::string &url);
    ~BBLFileTransferTunnel() override { reset(); }

    void start_connect() override;
    bool sync_start_connect() override;
    void shutdown() override;
    bool check_valid() const override { return h_ != nullptr; }
    void *native() const noexcept override { return h_; }

private:
    void reset() noexcept
    {
        if (h_) {
            m_->ft_tunnel_release(h_);
            h_ = nullptr;
        }
    }

    FileTransferModule *m_{};
    FT_TunnelHandle    *h_{};
};

/**
 * BBLFileTransferJob - a single ft_job_* operation (media-ability query,
 * file upload, ...) run on a BBLFileTransferTunnel. Same ABI-wrapper role
 * as BBLFileTransferTunnel; only BBLPrinterAgent constructs these.
 */
class BBLFileTransferJob : public IFileTransferJob
{
public:
    explicit BBLFileTransferJob(const std::string &params_json);
    ~BBLFileTransferJob() override { reset(); }

    bool get_result(int &ec, int &resp_ec, std::string &json, std::vector<std::byte> &bin, uint32_t timeout_ms) override;
    void start_on(IFileTransferTunnel &t) override;
    // why: unlike on_result() (fires from a trampoline registered once in the ctor),
    // ft_job_set_msg_cb is only wired up here, lazily, on first real subscriber -
    // that ABI call has to happen in the concrete class, not the vendor-neutral base.
    void on_msg(MsgCb cb) override;
    bool try_get_msg(int &kind, std::string &json) override;
    bool get_msg(uint32_t timeout_ms, int &kind, std::string &json) override;
    void *native() const noexcept override { return h_; }
    bool check_valid() const override { return h_ != nullptr; }
    void cancel() override
    {
        if (m_->ft_job_cancel && h_) m_->ft_job_cancel(h_);
    }

private:
    void reset() noexcept
    {
        if (h_) {
            m_->ft_job_release(h_);
            h_ = nullptr;
        }
    }
    void solve_result(ft_job_result result);

    FileTransferModule *m_{};
    FT_JobHandle        *h_{};
};

/**
 * BBLPrinterAgent - BBL DLL wrapper implementation of IPrinterAgent.
 *
 * Delegates all printer operations to the proprietary BBL network DLL
 * through function pointers obtained from BBLNetworkPlugin singleton.
 */
class BBLPrinterAgent : public IPrinterAgent {
public:
    BBLPrinterAgent();
    ~BBLPrinterAgent() override;

    // Cloud Agent Dependency (not used by BBL - tokens managed internally)
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // ========================================================================
    // IPrinterAgent Interface Implementation
    // ========================================================================

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    static std::string ams_refresh_rfid_gcode(const std::string& tray_id);
    static std::string ams_calibrate_gcode(int ams_id);
    static std::string ams_select_tray_gcode(const std::string& tray_id);
    int command_ams_refresh_rfid(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override;
    int command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode) override;
    int command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override;
    int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode) override;
    int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode) override;
    int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode) override;
    int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode) override;
    int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode) override;
    int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed,
                              bool is_core_xy, bool supports_mqtt_axis_control, int sequence_id, bool lan_mode) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    std::string get_local_camera_url(CameraURLParams params) override;
    std::string get_local_file_transfer_url(const FileTransferURLParams& params) override;
    bool supports_remote_liveview(const std::string& printer_type) const override;
    int get_file_transfer_url(std::string dev_id, std::function<void(FileTransferURLResult)> callback,
                              FileTransferURLParams params) override;
    std::string default_lan_username() const override { return "bblp"; }

    // Certificates
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Subscriptions
    int start_subscribe(std::string module) override;
    int stop_subscribe(std::string module) override;
    int add_subscribe(std::vector<std::string> dev_list) override;
    int del_subscribe(std::vector<std::string> dev_list) override;

    /**
     * Get agent information.
     *
     * @return AgentInfo struct containing agent identification and descriptive information
     */
     static AgentInfo get_agent_info_static();
     AgentInfo get_agent_info() override { return get_agent_info_static(); }

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;
    FilamentSyncMode get_filament_sync_mode() const override;

    void prepare_file_transfer(const FileTransferRequest& request, FileTransferCallbacks cb) override;
    void get_file_destinations(FileTransferCallbacks cb) override;
    void upload_file(const std::string& path, const std::string& name, const std::string& destination,
                     FileTransferCallbacks cb) override;
    void cancel_file_transfer() override;


private:
    int verify_local_print_access(PrintParams params);

    // why: the lan/cloud DECISION stays machine-side; keep this mechanical branch in sync with publish_json.
    int publish(const std::string& dev_id, const nlohmann::json& j, bool lan_mode);

    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::unique_ptr<IFileTransferTunnel> m_file_transfer_tunnel;
    std::unique_ptr<IFileTransferJob>    m_file_transfer_job;
    FileTransferCallbacks               m_file_transfer_callbacks;
    FileTransferRequest                 m_file_transfer_request;
    bool                                m_file_transfer_tcp{true};
    int                                 m_file_transfer_try_count{0};
    uint64_t                            m_file_transfer_generation{0};

    void start_file_transfer_attempt(uint64_t generation);
    void handle_file_transfer_connection(uint64_t generation, bool is_success, int error_code, std::string error_msg);
};

} // namespace Slic3r

#endif // __BBL_PRINTER_AGENT_HPP__
