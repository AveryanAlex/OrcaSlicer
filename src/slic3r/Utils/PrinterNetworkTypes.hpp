#pragma once

#include <string>

namespace Slic3r {

enum LiveviewLocal {
    LVL_None,
    LVL_Disable,
    LVL_Local,
    LVL_Rtsps,
    LVL_Rtsp
};

enum LiveviewRemote {
    LVR_None,
    LVR_Tutk,
    LVR_Agora,
    LVR_TutkAgora
};

enum FileLocal {
    FL_None,
    FL_Local
};

enum FileRemote {
    FR_None,
    FR_Tutk,
    FR_Agora,
    FR_TutkAgora
};

enum URL_STATE {
    URL_TCP,
    URL_TUTK,
};

struct CameraURLParams {
    std::string ip_address;
    std::string user;
    std::string password;
    LiveviewLocal protocol;
    std::string device;
    std::string network_version;
    std::string device_version;
    std::string refresh_url;
    std::string client_id;
    std::string client_version;
    bool        apply_meta{false};
};

struct CameraURLResult {
    bool        is_success{false};
    std::string url;
    int         error_code{-1};
};

struct FileTransferURLParams {
    URL_STATE   url_state{URL_TCP};
    std::string ip_address;
    std::string username;
    std::string password;
    std::string device_id;
    std::string network_version;
    std::string device_version;
    std::string refresh_url;
    std::string client_id;
    std::string client_version;
};

struct FileTransferURLResult {
    bool        is_success{false};
    std::string url;
    int         error_code{-1};
};

} // namespace Slic3r
