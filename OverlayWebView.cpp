#include "OverlayWebView.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>

using Microsoft::WRL::ComPtr;

static HWND g_ovWnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_ctrl;
static ComPtr<ICoreWebView2> g_web;
static bool g_visible = false;
static std::wstring g_pendingUrl;  // set before controller available

// ---- small helpers ----
static std::wstring ToW(const std::string& s){
    if(s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
    std::wstring w(n,0);
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n);
    return w;
}
static std::wstring ExeDir(){
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr,buf,MAX_PATH);
    std::wstring p(buf);
    size_t k=p.find_last_of(L"\\/");
    return k==std::wstring::npos?L".":p.substr(0,k);
}
static std::wstring FileUrl(const std::wstring& path){
    std::wstring u=L"file:///"; u.reserve(path.size()+8);
    for(wchar_t c: path) u += (c==L'\\'?L'/':c);
    return u;
}
static LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM w, LPARAM l){
    if(m==WM_SIZE && g_ctrl){
        RECT rc; GetClientRect(h,&rc);
        g_ctrl->put_Bounds(rc);
    }
    return DefWindowProc(h,m,w,l);
}

bool OV_Show(const std::string& pid, int pollMs){
    // Create window if needed
    if(!g_ovWnd){
        WNDCLASSW wc{}; wc.lpfnWndProc=OvProc; wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"AOE4_OV_WEBVIEW";
        RegisterClassW(&wc);

        // Topmost, borderless, layered, click-through (remove WS_EX_TRANSPARENT if you want mouse input)
        DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
        g_ovWnd = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP,
                                  100,100, 1100, 280, nullptr, nullptr, wc.hInstance, nullptr);
        if(!g_ovWnd) return false;
        SetLayeredWindowAttributes(g_ovWnd, RGB(0,0,0), 255, LWA_ALPHA);
        ShowWindow(g_ovWnd, SW_SHOW);

        // Create WebView2 environment & controller asynchronously
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, nullptr, nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT envHr, ICoreWebView2Environment* env)->HRESULT{
                    if (FAILED(envHr) || !env) return envHr ? envHr : E_FAIL;
                    return env->CreateCoreWebView2Controller(
                        g_ovWnd,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT ctrlHr, ICoreWebView2Controller* ctrl)->HRESULT{
                                if (FAILED(ctrlHr) || !ctrl) return ctrlHr ? ctrlHr : E_FAIL;
                                g_ctrl = ctrl;
                                g_ctrl->get_CoreWebView2(&g_web);

                                // Transparent background for overlay
                                ComPtr<ICoreWebView2Controller2> c2;
                                if (SUCCEEDED(g_ctrl.As(&c2))) {
                                    COREWEBVIEW2_COLOR col{0,0,0,0}; // RGBA, 0 alpha = transparent
                                    c2->put_DefaultBackgroundColor(col);
                                }

                                RECT rc; GetClientRect(g_ovWnd,&rc);
                                g_ctrl->put_Bounds(rc);

                                // Navigate if we already have a pending URL
                                if(!g_pendingUrl.empty() && g_web){
                                    g_web->Navigate(g_pendingUrl.c_str());
                                }
                                return S_OK;
                            }
                        ).Get()
                    );
                }
            ).Get()
        );
        if (FAILED(hr)) {
            // Could not even start WebView2 creation (runtime missing?)
            return false;
        }
    }

    // Build URL for our overlay.html (next to the EXE)
    std::wstring url = FileUrl(ExeDir()+L"\\overlay.html") +
                       L"?pid=" + ToW(pid) + L"&poll=" + ToW(std::to_string(pollMs)) +
                       L"#pid=" + ToW(pid);
    g_pendingUrl = url;

    if (g_web) {
        g_web->Navigate(url.c_str());
    }

    ShowWindow(g_ovWnd, SW_SHOW);
    SetWindowPos(g_ovWnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    g_visible = true;
    return true;
}

void OV_Hide(){
    if(g_ovWnd) ShowWindow(g_ovWnd, SW_HIDE);
    g_visible = false;
}

void OV_SetBounds(int x,int y,int w,int h){
    if(!g_ovWnd) return;
    SetWindowPos(g_ovWnd, HWND_TOPMOST, x,y,w,h, 0);
    if(g_ctrl){ RECT rc{0,0,w,h}; g_ctrl->put_Bounds(rc); }
}

bool OV_IsVisible(){ return g_visible && g_ovWnd && IsWindowVisible(g_ovWnd); }

bool OV_IsAvailable(){ return g_web != nullptr; }
