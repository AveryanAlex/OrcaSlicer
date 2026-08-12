#include "FileTransferUtils.hpp"

namespace Slic3r {

FileTransferModule::FileTransferModule(ModuleHandle networking_module, int required_abi_version) : networking_(networking_module)
{
    // basic
    ft_abi_version        = sym_lookup<fn_ft_abi_version>(networking_, "ft_abi_version");
    ft_free               = sym_lookup<fn_ft_free>(networking_, "ft_free");
    ft_job_result_destroy = sym_lookup<fn_ft_job_result_destroy>(networking_, "ft_job_result_destroy");
    ft_job_msg_destroy    = sym_lookup<fn_ft_job_msg_destroy>(networking_, "ft_job_msg_destroy");

    // tunnel
    ft_tunnel_create        = sym_lookup<fn_ft_tunnel_create>(networking_, "ft_tunnel_create");
    ft_tunnel_retain        = sym_lookup<fn_ft_tunnel_retain>(networking_, "ft_tunnel_retain");
    ft_tunnel_release       = sym_lookup<fn_ft_tunnel_release>(networking_, "ft_tunnel_release");
    ft_tunnel_start_connect = sym_lookup<fn_ft_tunnel_start_connect>(networking_, "ft_tunnel_start_connect");
    ft_tunnel_sync_connect  = sym_lookup<fn_ft_tunnel_sync_connect>(networking_, "ft_tunnel_sync_connect");
    ft_tunnel_set_status_cb = sym_lookup<fn_ft_tunnel_set_status_cb>(networking_, "ft_tunnel_set_status_cb");
    ft_tunnel_shutdown      = sym_lookup<fn_ft_tunnel_shutdown>(networking_, "ft_tunnel_shutdown");

    // job
    ft_job_create        = sym_lookup<fn_ft_job_create>(networking_, "ft_job_create");
    ft_job_retain        = sym_lookup<fn_ft_job_retain>(networking_, "ft_job_retain");
    ft_job_release       = sym_lookup<fn_ft_job_release>(networking_, "ft_job_release");
    ft_job_set_result_cb = sym_lookup<fn_ft_job_set_result_cb>(networking_, "ft_job_set_result_cb");
    ft_job_get_result    = sym_lookup<fn_ft_job_get_result>(networking_, "ft_job_get_result");
    ft_tunnel_start_job  = sym_lookup<fn_ft_tunnel_start_job>(networking_, "ft_tunnel_start_job");
    ft_job_cancel        = sym_lookup<fn_ft_job_cancel>(networking_, "ft_job_cancel");

    ft_job_set_msg_cb  = sym_lookup<fn_ft_job_set_msg_cb>(networking_, "ft_job_set_msg_cb");
    ft_job_try_get_msg = sym_lookup<fn_ft_job_try_get_msg>(networking_, "ft_job_try_get_msg");
    ft_job_get_msg     = sym_lookup<fn_ft_job_get_msg>(networking_, "ft_job_get_msg");
}

} // namespace Slic3r
