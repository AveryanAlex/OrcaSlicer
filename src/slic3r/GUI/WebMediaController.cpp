#include "WebMediaController.hpp"

#include <wx/webview.h>

namespace Slic3r { namespace GUI {

WebMediaController::WebMediaController(wxWebView *webview) : m_webview(webview) {}

void WebMediaController::Load(wxURI url)
{
    m_url = url.BuildURI().ToStdString();
}

void WebMediaController::Play()
{
    if (!m_webview)
        return;
    // why: navigating straight to the MJPEG stream URL makes the browser render its
    // default bare-image document (native resolution, centered, white page background) -
    // wrap it in a page that stretches the image to fill the view instead.
    wxString url = wxString::FromUTF8(m_url);
    wxString html = "<html><head><style>"
                    "html,body{margin:0;height:100%;background:#000;overflow:hidden;}"
                    "img{width:100%;height:100%;object-fit:contain;display:block;}"
                    "</style></head><body><img src=\"" + url + "\"></body></html>";
    m_webview->SetPage(html, url);
}

void WebMediaController::Stop()
{
    m_url.clear();
    if (m_webview)
        m_webview->Stop();
}

// wxMediaState WebMediaController::GetState()
// {
//     return wxMediaState{};
// }

// int WebMediaController::GetLastError() const
// {
//     return 0;
// }

// wxSize WebMediaController::GetVideoSize() const
// {
//     return wxSize{};
// }

}} // namespace Slic3r::GUI
