//***********************************************************************
//  HalconGUIDemo - Halcon 17 (32 位) 版 ImGui 模板匹配验证界面
//
//  目的：把 GeoGUIDemo 的界面与操作原样搬到 Halcon 后端，用来逐项对比
//        「本项目 GeoMatch/ShapeMatch/NccMatch」和「MVTec Halcon」在
//        分数 / 位置 / 角度 / 尺度 / 耗时上的差距。
//
//  布局与 GeoGUIDemo 完全一致：
//    左侧  图片目录缩略图列表
//    中间  图像视图：匹配结果 / 目标图 / 模板源图 / 模板模型
//          在图上按住左键拖动可框选 ROI；模板源图上可涂抹掩码
//    右侧  ① 匹配  ② 模板参数  ③ 搜索参数  ④ 掩码/模型  ⑤ 界面/日志
//
//  Halcon 后端：
//    形状匹配  create_scaled_shape_model / find_scaled_shape_model
//    NCC 匹配  create_ncc_model / find_ncc_model
//
//  重要限制：仓库内的 Halcon 17 SDK 只有 32 位库
//  （HalconDemo/Halcon17/x86sse2-win32），所以本程序必须用 Win32(x86)
//  工具链构建，与主项目 x64 的 GeoGUIDemo 分别运行、分别看结果。
//  构建：tools\build-halcon-gui.bat
//
//  用法：HalconGUIDemo.exe [目录]     目录默认为 case1
//        启动时自动尝试 <目录>/temp.jpg 建模板，并拿目录里第一张
//        其它图片做一次匹配。
//***********************************************************************
#define _CRT_SECURE_NO_WARNINGS
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <objbase.h>
#include <shobjidl_core.h>      // IFileDialog / 文件夹选择框

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/filesystem.hpp>

#include "halconcpp/HalconCpp.h"

using namespace HalconCpp;

//=======================================================================
//  D3D11 设备 / 交换链
//=======================================================================
static ID3D11Device*           g_dev       = nullptr;
static ID3D11DeviceContext*    g_ctx       = nullptr;
static IDXGISwapChain*         g_swap      = nullptr;
static ID3D11RenderTargetView* g_rtv       = nullptr;
static bool                    g_occluded  = false;
static UINT                    g_resizeW   = 0, g_resizeH = 0;
static float                   g_scale     = 1.0f;
static bool                    g_dirtyFrame = false;
static HWND                    g_hwnd      = nullptr;

static bool    CreateDeviceD3D(HWND hwnd);
static void    CleanupDeviceD3D();
static void    CreateRenderTarget();
static void    CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

//=======================================================================
//  日志
//=======================================================================
static std::vector<std::string> g_log;
static bool                     g_logDirty = false;

// 同时落盘一份，便于崩了之后回看日志（日志里含建模/匹配耗时，正好用来对比算法差异）
static void logToFile(const std::string& s)
{
    static FILE* f = nullptr;
    if (!f)
    {
        f = std::fopen("HalconGUIDemo.log", "w");
        if (!f) return;
    }
    std::fprintf(f, "%s\n", s.c_str());
    std::fflush(f);
}

static void logLine(const std::string& s)
{
    if (g_log.size() > 400)
        g_log.erase(g_log.begin(), g_log.begin() + 100);
    g_log.push_back(s);
    g_logDirty = true;
    logToFile(s);
}

static void logf(const char* fmt, ...);

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
    logf("!! 进程崩溃: code=0x%08X addr=%p",
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0u,
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void logf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logLine(buf);
}

//=======================================================================
//  小工具: 字符串 / 文件对话框 / 读图
//=======================================================================
static std::string ws2s(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring s2ws(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring pickImageFile(const wchar_t* title)
{
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = L"图片文件\0*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff\0所有文件\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 4;
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return ::GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// 用 ifstream + imdecode 读图，避免 OpenCV 在非 ASCII 路径上的问题
static bool loadGrayImage(const std::wstring& path, cv::Mat& gray)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    cv::Mat buf(1, (int)data.size(), CV_8U, data.data());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
    if (img.empty()) return false;
    gray = img;
    return true;
}

// 文件夹选择框
static std::wstring pickFolder()
{
    std::wstring out;
    HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))))
    {
        DWORD flags = 0;
        dlg->GetOptions(&flags);
        dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"选择目标图片目录");
        if (SUCCEEDED(dlg->Show(g_hwnd)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
                {
                    out = psz;
                    ::CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) ::CoUninitialize();
    return out;
}

// 用 Win32 枚举目录里的图片，避免 cv::glob 在中文路径上失败
static std::vector<std::wstring> listDirImages(const std::wstring& dir)
{
    static const wchar_t* exts[] = { L".jpg", L".jpeg", L".png", L".bmp", L".tif", L".tiff" };
    std::vector<std::wstring> out;

    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        const size_t p = n.rfind(L'.');
        if (p == std::wstring::npos) continue;
        std::wstring ext = n.substr(p);
        for (size_t i = 0; i < ext.size(); i++) ext[i] = (wchar_t)::towlower(ext[i]);
        for (const wchar_t* e : exts)
            if (ext == e) { out.push_back(dir + L"\\" + n); break; }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    std::sort(out.begin(), out.end());
    return out;
}

static cv::Mat toBGRA(const cv::Mat& m)
{
    if (m.empty()) return cv::Mat();
    cv::Mat out;
    if      (m.channels() == 1) cv::cvtColor(m, out, cv::COLOR_GRAY2BGRA);
    else if (m.channels() == 3) cv::cvtColor(m, out, cv::COLOR_BGR2BGRA);
    else if (m.channels() == 4) out = m.clone();
    return out;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 高阈值(Contrast)跟随低阈值(MinContrast)：与项目实现完全一致的公式 lo*1.15+2。
// 滞后阈值必须有分离度才会"沿着弱边生长"：分离度太小退化成纯阈值、线被切碎；
// 分离度太大种子数为 0 会导致模型点全灭。取整是为了让 Halcon 收到整数阈值。
static float hiFollow(float lo)
{
    return std::min(std::round(lo * 1.15f + 2.f), 255.f);
}

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double deg2rad(float d) { return (double)d * M_PI / 180.0; }
static float  rad2deg(double r) { return (float)(r * 180.0 / M_PI); }

//=======================================================================
//  纹理: 把 cv::Mat 上传成 D3D11 贴图
//=======================================================================
struct Texture
{
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;

    Texture() {}
    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& o) noexcept
        : tex(o.tex), srv(o.srv), w(o.w), h(o.h)
    { o.tex = nullptr; o.srv = nullptr; o.w = o.h = 0; }
    Texture& operator=(Texture&& o) noexcept
    {
        if (this != &o)
        {
            release();
            tex = o.tex; srv = o.srv; w = o.w; h = o.h;
            o.tex = nullptr; o.srv = nullptr; o.w = o.h = 0;
        }
        return *this;
    }

    bool valid() const { return srv != nullptr; }

    static std::vector<Texture>& pending()
    {
        static std::vector<Texture> v;
        return v;
    }

    void release()
    {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        w = h = 0;
    }

    void releaseDeferred()
    {
        if (!tex && !srv) return;
        pending().push_back(std::move(*this));
    }

    // bgra 必须是 CV_8UC4
    bool set(const cv::Mat& bgra)
    {
        if (bgra.empty() || bgra.type() != CV_8UC4) { releaseDeferred(); return false; }

        const int iw = bgra.cols, ih = bgra.rows;
        const int pitch = ((iw * 4 + 15) / 16) * 16;
        std::vector<unsigned char> buf((size_t)pitch * (size_t)ih, 0);
        for (int y = 0; y < ih; y++)
            memcpy(&buf[(size_t)y * pitch], bgra.ptr(y), (size_t)iw * 4);

        if (tex && (w != iw || h != ih)) releaseDeferred();

        if (!tex)
        {
            D3D11_TEXTURE2D_DESC d;
            ZeroMemory(&d, sizeof(d));
            d.Width     = (UINT)iw;
            d.Height    = (UINT)ih;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count   = 1;
            d.SampleDesc.Quality = 0;
            d.Usage          = D3D11_USAGE_DEFAULT;
            d.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = 0;
            d.MiscFlags      = 0;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) { release(); return false; }

            D3D11_SHADER_RESOURCE_VIEW_DESC v;
            ZeroMemory(&v, sizeof(v));
            v.Format                    = d.Format;
            v.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MostDetailedMip = 0;
            v.Texture2D.MipLevels       = 1;
            if (FAILED(g_dev->CreateShaderResourceView(tex, &v, &srv))) { release(); return false; }

            w = iw;
            h = ih;
        }
        g_ctx->UpdateSubresource(tex, 0, nullptr, buf.data(), (UINT)pitch, 0);
        return true;
    }
};

static void flushPendingTextures()
{
    for (Texture& t : Texture::pending()) t.release();
    Texture::pending().clear();
}

//=======================================================================
//  图片列表里的一项
//=======================================================================
struct Thumb
{
    std::wstring wpath;
    std::string  path;
    std::string  name;
    Texture      tex;
};

//=======================================================================
//  应用状态
//=======================================================================
//  Halcon 后端的一条匹配结果。坐标沿用界面习惯: x = 列, y = 行。
struct Detection
{
    float score    = 0.f;
    float x        = 0.f;     // Halcon Column
    float y        = 0.f;     // Halcon Row
    float angleDeg = 0.f;
    float scale    = 1.f;
    float coverage = 0.f;     // Halcon 没有覆盖率概念，恒为 0(表格显示 "-")
};

struct App
{
    //---- 形状匹配建模参数 ----
    float angleStartDeg  = -180.f;
    float angleExtentDeg = 360.f;
    float angleStepDeg   = 0.f;     // 0 = 自动
    float scaleMin       = 1.f;
    float scaleMax       = 1.f;
    float contrast       = 16.f;    // Halcon Contrast (高阈值, 灰度)；默认 = hiFollow(12)
    float minContrast    = 12.f;    // Halcon MinContrast (低阈值, 灰度)
    bool  autoHi         = true;    // 高阈值跟随低阈值(hiFollow: lo*1.15+2)
    int   numLevels      = 0;       // 0 = 自动

    //---- 形状匹配搜索参数 ----
    float minScore       = 0.70f;
    int   maxMatches     = 12;
    float maxOverlap     = 1.0f;    // 1 = 关闭(对齐 GeoGUIDemo 默认)
    float greediness     = 0.90f;
    int   subpixelMode   = 1;       // 0 = false, 1 = true
    int   metric         = 0;       // 0 use_polarity, 1 ignore_global_polarity, 2 ignore_local_polarity

    //---- 显示 / 自动刷新 ----
    int   boxMode        = 0;       // 0 固定方框, 1 旋转框, 2 模型凸包
    bool  showContour    = true;
    bool  autoRun        = true;
    bool  liveUpdate     = true;
    float zoom           = 1.0f;
    bool  roiDirty       = false;
    double lastOpMs      = 0;

    //---- 本次搜索的耗时统计 ----
    bool   showResultOnce = false;
    double preprocessMs   = 0;
    double searchMs       = 0;
    double searchTotalMs  = 0;
    double benchWallMs    = 0;
    int    searchLoops    = 0;

    int    matchMode      = 0;      // 0 形状匹配, 1 NCC

    //---- NCC 参数 ----
    float nccAngleStartDeg  = -180.f;
    float nccAngleExtentDeg = 360.f;
    float nccAngleStepDeg   = 0.f;
    int   nccModelLevels    = 0;
    int   nccMetric         = 0;
    float nccMinScore       = 0.70f;
    int   nccMaxMatches     = 1;
    float nccMaxOverlap     = 0.5f;
    int   nccSubpixel       = 1;

    //---- Halcon 模型 ----
    HShapeModel shape;
    HNCCModel   ncc;
    bool   shapeReady = false;
    bool   nccReady   = false;
    bool   modelReady = false;
    double buildMs    = 0;

    //---- 数据 ----
    std::string tplPath, tgtPath;
    cv::Mat tplGray, tgtGray;

    cv::Mat     tplSrcGray;
    cv::Rect    tplSrcRoi;
    std::string tplSrcLabel;
    bool        tplSrcValid = false;

    cv::Mat modelVis, resultVis;

    std::vector<Thumb> thumbs;
    int         thumbSel = -1;
    std::string dirPath;

    Texture texTarget, texResult, texTplSrc, texModel;

    std::vector<Detection> dets;

    //---- ROI ----
    cv::Rect roi;
    bool     roiHas   = false;
    int      roiOwner = 0;
    bool     dragging = false;
    ImVec2   dragA, dragB;

    //---- 掩码 ----
    std::string maskPath;
    cv::Mat     mask;
    bool  paintMask    = false;
    bool  maskPainting = false;
    bool  maskDirty    = false;
    bool  maskPending  = false;
    float brushRadius  = 6.f;
    int   viewTab      = 0;

    //---- 模型几何(结果绘制用, 都是相对模型原点的坐标) ----
    std::vector<std::vector<cv::Point2f>> modelPolys;
    std::vector<cv::Point2f>              modelPts;
    cv::Rect2f                            modelBox;
    double originRow = 0, originCol = 0;
    int    tplW = 0, tplH = 0;
    bool   modelGeomValid = false;
};

static App g_app;

//=======================================================================
//  Halcon 转换 / 模型几何
//=======================================================================
static HImage toHImage(const cv::Mat& src)
{
    cv::Mat g;
    if (src.channels() == 1) g = src;
    else cv::cvtColor(src, g, cv::COLOR_BGR2GRAY);
    if (!g.isContinuous()) g = g.clone();

    HImage img;
    img.GenImage1("byte", g.cols, g.rows, (void*)g.data);
    return img;
}

// 掩码图(非零=参与建模) -> Halcon 区域。用于 ReduceDomain，
// 让 create_scaled_shape_model / create_ncc_model 只在掩码内提特征。
static bool maskToRegion(const cv::Mat& mask, int w, int h, HRegion& out)
{
    if (mask.empty() || w <= 0 || h <= 0) return false;

    cv::Mat m;
    if (mask.channels() == 1) mask.convertTo(m, CV_8U);
    else if (mask.channels() == 3) cv::cvtColor(mask, m, cv::COLOR_BGR2GRAY);
    else cv::cvtColor(mask, m, cv::COLOR_BGRA2GRAY);

    if (m.size() != cv::Size(w, h))
        cv::resize(m, m, cv::Size(w, h), 0, 0, cv::INTER_NEAREST);
    cv::threshold(m, m, 0, 255, cv::THRESH_BINARY);
    if (!m.isContinuous()) m = m.clone();

    HImage mi;
    mi.GenImage1("byte", m.cols, m.rows, (void*)m.data);

    HObject reg;
    Threshold(mi, &reg, 1, 255);
    out = HRegion(reg);
    return true;
}

// 把掩码作用到模板上(ReduceDomain)，并返回是否真的生效 + 保留面积比例。
// 之前是「尺寸对不上就静默跳过」，涂抹后看不出掩码到底有没有参与建模，
// 所以这里统一打印一行日志，便于对比涂抹前后的模型点数/分数。
static bool applyMaskToImage(const App& a, const cv::Mat& patch, HImage& img,
                             double* keepRatio)
{
    if (keepRatio) *keepRatio = 1.0;
    if (a.mask.empty()) return false;

    if (a.mask.size() != patch.size())
    {
        logf("提示: 掩码 %dx%d 与模板 %dx%d 尺寸不一致，本次建模忽略掩码",
             a.mask.cols, a.mask.rows, patch.cols, patch.rows);
        return false;
    }

    HRegion mreg;
    if (!maskToRegion(a.mask, patch.cols, patch.rows, mreg)) return false;
    img = img.ReduceDomain(mreg);

    const double total = (double)patch.cols * patch.rows;
    if (keepRatio && total > 0)
        *keepRatio = mreg.Area().D() / total;
    return true;
}

static const char* shapeMetricName(int m)
{
    if (m == 1) return "ignore_global_polarity";
    if (m == 2) return "ignore_local_polarity";
    return "use_polarity";
}

static const char* nccMetricName(int m)
{
    if (m == 1) return "ignore_global_polarity";
    if (m == 2) return "ignore_local_polarity";
    return "use_polarity";
}

// get_shape_model_contours 返回的是「相对模型原点」的轮廓，而且可能包含多条子轮廓；
// 直接对整体调用 get_contour_xld 会报 HALCON error #1501（对象参数个数不对），
// 必须先 count_obj/select_obj 逐条取。相邻点跳变(>8px)再切开，纯粹为绘制服务。
static void loadShapeGeometry(App& a)
{
    a.modelPolys.clear();
    a.modelPts.clear();
    a.modelGeomValid = false;

    if (!a.shapeReady) return;
    try
    {
        HXLDCont cont = a.shape.GetShapeModelContours(1);
        const Hlong parts = cont.CountObj();
        for (Hlong k = 1; k <= parts; k++)
        {
            HXLDCont one(cont.SelectObj(k));
            HTuple rows, cols;
            one.GetContourXld(&rows, &cols);
            const Hlong n = rows.Length();

            std::vector<cv::Point2f> cur;
            for (Hlong i = 0; i < n; i++)
            {
                cv::Point2f p((float)cols[i].D(), (float)rows[i].D());
                if (!cur.empty())
                {
                    const cv::Point2f d = p - cur.back();
                    if (std::sqrt(d.x * d.x + d.y * d.y) > 8.f)
                    {
                        if (cur.size() >= 2) a.modelPolys.push_back(cur);
                        cur.clear();
                    }
                }
                cur.push_back(p);
                a.modelPts.push_back(p);
            }
            if (cur.size() >= 2) a.modelPolys.push_back(cur);
        }

        if (!a.modelPts.empty())
        {
            a.modelBox = cv::boundingRect(a.modelPts);
            a.modelGeomValid = true;
        }
    }
    catch (HException& e)
    {
        logf("读取形状模型轮廓失败: %s", e.ErrorMessage().Text());
    }
}

static void buildShapeModelVis(App& a)
{
    if (!a.shapeReady || a.modelPts.empty()) return;

    const float margin = 6.f;
    const bool hasTpl = !a.tplGray.empty();
    cv::Mat canvas;

    if (hasTpl)
    {
        cv::Mat bgr;
        cv::cvtColor(a.tplGray, bgr, cv::COLOR_GRAY2BGR);
        cv::resize(bgr, canvas, cv::Size(a.tplGray.cols * 2, a.tplGray.rows * 2),
                   0, 0, cv::INTER_LINEAR);
    }
    else
    {
        const int w = (int)std::ceil((a.modelBox.width + 2 * margin) * 2);
        const int h = (int)std::ceil((a.modelBox.height + 2 * margin) * 2);
        canvas = cv::Mat(std::max(h, 16), std::max(w, 16), CV_8UC3, cv::Scalar(30, 30, 30));
    }

    auto mapPt = [&](const cv::Point2f& p) -> cv::Point
    {
        float x, y;
        if (hasTpl)
        {
            x = (p.x + (float)a.originCol) * 2.f;
            y = (p.y + (float)a.originRow) * 2.f;
        }
        else
        {
            x = (p.x - a.modelBox.x + margin) * 2.f;
            y = (p.y - a.modelBox.y + margin) * 2.f;
        }
        return cv::Point(cvRound(x), cvRound(y));
    };

    for (const auto& poly : a.modelPolys)
    {
        std::vector<cv::Point> pts;
        pts.reserve(poly.size());
        for (const auto& p : poly) pts.push_back(mapPt(p));
        if (pts.size() >= 2)
            cv::polylines(canvas, pts, false, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    }

    const cv::Point center = hasTpl
        ? cv::Point(cvRound(a.originCol * 2.0), cvRound(a.originRow * 2.0))
        : cv::Point(canvas.cols / 2, canvas.rows / 2);
    cv::drawMarker(canvas, center, cv::Scalar(255, 80, 0), cv::MARKER_CROSS, 14, 1);
    cv::putText(canvas, "shape model", cv::Point(6, 18),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

    a.modelVis = canvas;
}

static void buildNccModelVis(App& a)
{
    if (a.tplGray.empty())
    {
        a.modelVis.release();
        a.texModel.releaseDeferred();
        return;
    }

    cv::Mat bgr;
    cv::cvtColor(a.tplGray, bgr, cv::COLOR_GRAY2BGR);
    cv::Mat canvas;
    cv::resize(bgr, canvas, cv::Size(a.tplGray.cols * 2, a.tplGray.rows * 2),
               0, 0, cv::INTER_LINEAR);

    cv::rectangle(canvas, cv::Point(1, 1), cv::Point(canvas.cols - 2, canvas.rows - 2),
                  cv::Scalar(0, 255, 0), 2);
    cv::drawMarker(canvas,
                   cv::Point(cvRound(a.originCol * 2.0), cvRound(a.originRow * 2.0)),
                   cv::Scalar(255, 80, 0), cv::MARKER_CROSS, 14, 1);
    cv::putText(canvas, "NCC model", cv::Point(6, 18),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

    a.modelVis = canvas;
}

// 结果框 / 轮廓绘制。坐标约定: 模型点相对模型原点, 位姿为 (Row, Column, Angle, Scale)。
static cv::Point2f posePoint(const cv::Point2f& p, double ang, double sc)
{
    const double c = std::cos(ang), s = std::sin(ang);
    const double x = (double)p.x * sc, y = (double)p.y * sc;
    return cv::Point2f((float)(c * x - s * y), (float)(s * x + c * y));
}

static cv::Mat drawResult(const App& a, const cv::Mat& gray,
                          const std::vector<Detection>& dets)
{
    cv::Mat vis;
    if (gray.channels() == 1) cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    else                      vis = gray.clone();

    for (size_t i = 0; i < dets.size(); i++)
    {
        const Detection& d = dets[i];
        const cv::Scalar col = (i == 0) ? cv::Scalar(0, 255, 0)      // 最高分: 亮绿
                                        : cv::Scalar(0, 165, 255);   // 其余: 橙
        const double ang = (double)d.angleDeg * M_PI / 180.0;
        const cv::Point2f org(d.x, d.y);

        if (!a.modelGeomValid)
        {
            cv::drawMarker(vis, cv::Point(cvRound(d.x), cvRound(d.y)), col,
                           cv::MARKER_CROSS, 18, 1);
            continue;
        }

        if (a.boxMode == 0)
        {
            // 固定方框: 尺寸 = 模型包围盒 * scale，不随角度倾斜
            const cv::Point2f local(a.modelBox.x + a.modelBox.width * 0.5f,
                                    a.modelBox.y + a.modelBox.height * 0.5f);
            const cv::Point2f c = org + posePoint(local, ang, d.scale);
            const float w = a.modelBox.width * d.scale;
            const float h = a.modelBox.height * d.scale;
            cv::rectangle(vis,
                          cv::Point(cvRound(c.x - w * 0.5f), cvRound(c.y - h * 0.5f)),
                          cv::Point(cvRound(c.x + w * 0.5f), cvRound(c.y + h * 0.5f)),
                          col, 2);
        }
        else if (a.boxMode == 1 || a.modelPts.empty())
        {
            // 旋转框: 模型包围盒的四角跟随角度
            const cv::Point2f cs[4] = {
                cv::Point2f(a.modelBox.x, a.modelBox.y),
                cv::Point2f(a.modelBox.x + a.modelBox.width, a.modelBox.y),
                cv::Point2f(a.modelBox.x + a.modelBox.width, a.modelBox.y + a.modelBox.height),
                cv::Point2f(a.modelBox.x, a.modelBox.y + a.modelBox.height)
            };
            cv::Point pts[4];
            for (int k = 0; k < 4; k++)
            {
                const cv::Point2f q = org + posePoint(cs[k], ang, d.scale);
                pts[k] = cv::Point(cvRound(q.x), cvRound(q.y));
            }
            const cv::Point* pp[1] = { pts };
            int nn = 4;
            cv::polylines(vis, pp, &nn, 1, true, col, 2, cv::LINE_AA);
        }
        else
        {
            // 模型凸包
            std::vector<cv::Point2f> tp;
            tp.reserve(a.modelPts.size());
            for (const auto& p : a.modelPts) tp.push_back(org + posePoint(p, ang, d.scale));
            std::vector<cv::Point2f> hull;
            cv::convexHull(tp, hull);
            std::vector<cv::Point> hp;
            hp.reserve(hull.size());
            for (const auto& p : hull) hp.push_back(cv::Point(cvRound(p.x), cvRound(p.y)));
            if (hp.size() >= 3) cv::polylines(vis, hp, true, col, 2, cv::LINE_AA);
        }

        if (a.showContour && !a.modelPolys.empty())
        {
            for (const auto& poly : a.modelPolys)
            {
                std::vector<cv::Point> pts;
                pts.reserve(poly.size());
                for (const auto& p : poly)
                {
                    const cv::Point2f q = org + posePoint(p, ang, d.scale);
                    pts.push_back(cv::Point(cvRound(q.x), cvRound(q.y)));
                }
                if (pts.size() >= 2) cv::polylines(vis, pts, false, col, 1, cv::LINE_AA);
            }
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", d.score);
        cv::putText(vis, buf, cv::Point(cvRound(org.x) + 5, cvRound(org.y) - 7),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, col, 1, cv::LINE_AA);
    }
    return vis;
}

//=======================================================================
//  模板掩码可视化
//=======================================================================
static cv::Mat overlayMaskOnTpl(const cv::Mat& gray, const cv::Mat& mask)
{
    cv::Mat vis;
    cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    if (mask.empty()) return vis;

    cv::Mat m1;
    if (mask.channels() == 3) cv::cvtColor(mask, m1, cv::COLOR_BGR2GRAY);
    else mask.convertTo(m1, CV_8U);
    if (m1.size() != gray.size())
        cv::resize(m1, m1, gray.size(), 0, 0, cv::INTER_NEAREST);
    cv::threshold(m1, m1, 0, 255, cv::THRESH_BINARY);

    for (int y = 0; y < vis.rows; y++)
    {
        cv::Vec3b* row = vis.ptr<cv::Vec3b>(y);
        const uchar* mrow = m1.ptr<uchar>(y);
        for (int x = 0; x < vis.cols; x++)
        {
            if (mrow[x] == 0)
                row[x] = cv::Vec3b((uchar)(row[x][0] * 0.35f + 45),
                                   (uchar)(row[x][1] * 0.35f),
                                   (uchar)(row[x][2] * 0.35f + 45));
        }
    }
    return vis;
}

static void refreshTplView(App& a)
{
    if (a.tplGray.empty()) { a.texTplSrc.releaseDeferred(); return; }
    a.texTplSrc.set(toBGRA(overlayMaskOnTpl(a.tplGray, a.mask)));
}

static void ensureMask(App& a, int w, int h)
{
    if (a.mask.empty() || a.mask.cols != w || a.mask.rows != h || a.mask.type() != CV_8UC1)
    {
        a.mask = cv::Mat(h, w, CV_8U, cv::Scalar(255));
        a.maskPath.clear();
    }
}

//=======================================================================
//  模板来源(冻结) —— 与 GeoGUIDemo 完全一致的语义
//=======================================================================
static cv::Mat templatePatch(const App& a)
{
    if (!a.tplGray.empty()) return a.tplGray;

    if (!a.tgtGray.empty())
    {
        if (a.roiHas && a.roiOwner == 1)
        {
            const cv::Rect r = a.roi & cv::Rect(0, 0, a.tgtGray.cols, a.tgtGray.rows);
            if (r.width > 4 && r.height > 4) return a.tgtGray(r);
        }
        return a.tgtGray;
    }
    return cv::Mat();
}

static void freezeTplSource(App& a, const cv::Mat& src, const cv::Rect& roi,
                            const std::string& label)
{
    if (src.empty()) { a.tplSrcValid = false; return; }

    const cv::Mat full = src.clone();
    const cv::Rect  r    = roi & cv::Rect(0, 0, full.cols, full.rows);
    const cv::Mat   patch = (r.width > 4 && r.height > 4) ? full(r).clone() : full;

    a.tplSrcGray  = full;
    a.tplSrcRoi   = r;
    a.tplSrcLabel = label;
    a.tplSrcValid = true;

    a.tplGray = patch;
    a.tplPath = label;

    a.mask.release();
    a.maskPath.clear();
    if (a.paintMask) ensureMask(a, patch.cols, patch.rows);

    refreshTplView(a);
}

//=======================================================================
//  建模
//=======================================================================
// Halcon 17 的 get_*_model_origin 返回的是「相对模板中心的偏移」，默认 (0,0)；
// 而 find_*_model 返回的 Row/Col 是参考点在图像中的实际位置 = 模板中心 + origin。
// 所以内部统一换算成「模板坐标下的绝对参考点」，否则结果框会整体偏半个模板
// （NCC 最明显：方框会跑到匹配点的右下角，看起来像错位）。
static void resolveModelOrigin(double orow, double ocol, int tplW, int tplH,
                               double* refRow, double* refCol)
{
    *refRow = orow + (tplH - 1) * 0.5;
    *refCol = ocol + (tplW - 1) * 0.5;
}

static void createNccTemplate(App& a)
{
    cv::Mat patch = templatePatch(a);
    if (patch.empty())
    {
        logLine("没有可用于创建模板的图像");
        return;
    }

    try
    {
        HImage img = toHImage(patch);
        double maskKeep = 1.0;
        const bool maskOn = applyMaskToImage(a, patch, img, &maskKeep);

        const double t0 = nowMs();
        HNCCModel m(img, a.nccModelLevels,
                    deg2rad(a.nccAngleStartDeg), deg2rad(a.nccAngleExtentDeg),
                    deg2rad(a.nccAngleStepDeg),
                    nccMetricName(a.nccMetric));
        a.buildMs = nowMs() - t0;
        a.ncc = m;

        a.nccReady   = true;
        a.modelReady = true;
        a.tplW = patch.cols;
        a.tplH = patch.rows;

        double orow = 0, ocol = 0;
        a.ncc.GetNccModelOrigin(&orow, &ocol);
        resolveModelOrigin(orow, ocol, patch.cols, patch.rows, &a.originRow, &a.originCol);

        a.modelPolys.clear();
        a.modelPts.clear();
        a.modelBox = cv::Rect2f((float)(-a.originCol), (float)(-a.originRow),
                                (float)patch.cols, (float)patch.rows);
        a.modelGeomValid = true;

        buildNccModelVis(a);
        a.texModel.set(toBGRA(a.modelVis));

        double as = 0, ae = 0, astep = 0;
        HString metric;
        const Hlong levels = a.ncc.GetNccModelParams(&as, &ae, &astep, &metric);
        logf("Halcon NCC 模型已创建: %dx%d  金字塔 %d 层  角度 %.1f..%.1f  步长 %.2f  用时 %.1f ms",
             patch.cols, patch.rows, (int)levels,
             rad2deg(as), rad2deg(as + ae), rad2deg(astep), a.buildMs);
        logf("  参考点(row,col)=(%.2f,%.2f)  Metric=%s  (模板中心+origin偏移；Halcon NCC 无尺度/覆盖率)",
             a.originRow, a.originCol, metric.Text());
        if (maskOn)
            logf("  掩码: 已作用到模板 (保留面积 %.1f%%)", maskKeep * 100.0);
        if (a.nccAngleExtentDeg >= 359.f)
            logf("提示: 360 度 NCC 模型搜索较慢，已知角度时把角度范围收窄");
    }
    catch (HException& e)
    {
        logf("Halcon 创建 NCC 模型失败: %s", e.ErrorMessage().Text());
    }
}

static void createTemplate(App& a)
{
    if (a.matchMode == 1) { createNccTemplate(a); return; }

    cv::Mat patch = templatePatch(a);
    if (patch.empty())
    {
        logLine("没有可用于创建模板的图像");
        return;
    }

    try
    {
        HImage img = toHImage(patch);
        double maskKeep = 1.0;
        const bool maskOn = applyMaskToImage(a, patch, img, &maskKeep);

        // Halcon 要求 Contrast(高阈值) > MinContrast(低阈值)
        Hlong lo = (Hlong)clampf(a.minContrast, 1.f, 255.f);
        Hlong hi = (Hlong)clampf(a.autoHi ? hiFollow(a.minContrast) : a.contrast, 1.f, 255.f);
        if (hi <= lo)
        {
            hi = lo + 1;
            if (hi > 255) { hi = 255; lo = 254; }
        }

        // Halcon 用自动角度步长时会轻微平移起始角(实测 -180 会变成 -180.2)。
        // 这种「网格起点被平移」的模型，在默认贪婪度 0.9 下 FindScaledShapeModel
        // 会偶发 0 结果(实际存在 0.99 分的候选)，表现为「涂抹重建后有时识别失败」。
        // 所以整圈搜索时改用显式步长，保证网格从 -180 开始对齐。
        double aStart  = deg2rad(a.angleStartDeg);
        double aExtent = deg2rad(a.angleExtentDeg);
        double aStep   = deg2rad(a.angleStepDeg);
        const bool stepForced = (aStep <= 0.0 && aExtent >= deg2rad(359.9f));
        if (stepForced)
        {
            aStart  = deg2rad(-180.0f);
            aExtent = deg2rad(360.0f);
            aStep   = deg2rad(0.2f);
        }

        const double t0 = nowMs();
        HShapeModel m(img, a.numLevels, aStart, aExtent, aStep,
                      std::min(a.scaleMin, a.scaleMax), std::max(a.scaleMin, a.scaleMax),
                      0.0,
                      "none", shapeMetricName(a.metric), hi, lo);
        a.buildMs = nowMs() - t0;
        a.shape = m;

        a.shapeReady = true;
        a.modelReady = true;
        a.tplW = patch.cols;
        a.tplH = patch.rows;

        double orow = 0, ocol = 0;
        a.shape.GetShapeModelOrigin(&orow, &ocol);
        resolveModelOrigin(orow, ocol, patch.cols, patch.rows, &a.originRow, &a.originCol);

        loadShapeGeometry(a);
        buildShapeModelVis(a);
        a.texModel.set(toBGRA(a.modelVis));

        double as = 0, ae = 0, astep = 0, smin = 0, smax = 0, sstep = 0;
        HString metric;
        Hlong minc = 0;
        const Hlong levels = a.shape.GetShapeModelParams(&as, &ae, &astep,
                                                         &smin, &smax, &sstep,
                                                         &metric, &minc);
        logf("Halcon 形状模型已创建: %dx%d  金字塔 %d 层  角度 %.1f..%.1f  缩放 %.2f..%.2f  用时 %.1f ms",
             patch.cols, patch.rows, (int)levels,
             rad2deg(as), rad2deg(as + ae), smin, smax, a.buildMs);
        logf("  轮廓点 %d  Contrast=%d  MinContrast=%d  Metric=%s  参考点=(%.2f,%.2f)  (模板中心+origin偏移)",
             (int)a.modelPts.size(), (int)hi, (int)lo, metric.Text(),
             a.originRow, a.originCol);
        if (stepForced)
            logf("  角度步长: 自动步长会让起始角偏移(偶发 0 结果)，整圈改用固定 %.2f° 步长",
                 rad2deg(aStep));
        if (maskOn)
            logf("  掩码: 已作用到模板 (保留面积 %.1f%%)", maskKeep * 100.0);
        logf("  注: Halcon 没有「最大点数 / 最小间距 / 覆盖率 / 评分方式」这些参数");
    }
    catch (HException& e)
    {
        logf("Halcon 创建模板失败: %s", e.ErrorMessage().Text());
    }
}

static void createTemplateSafe(App& a)
{
    try { createTemplate(a); }
    catch (const HException& e) { logf("!! 创建模板异常: %s", e.ErrorMessage().Text()); }
    catch (const std::exception& e) { logf("!! 创建模板异常: %s", e.what()); }
}

//=======================================================================
//  搜索
//=======================================================================
static void runNccMatch(App& a, int loops)
{
    if (!a.nccReady)
    {
        logLine("还没有 NCC 模型，先创建或加载一个");
        return;
    }
    if (a.tgtGray.empty())
    {
        logLine("还没有匹配目标图片");
        return;
    }
    if (a.tplW > a.tgtGray.cols || a.tplH > a.tgtGray.rows)
    {
        logf("NCC 模板 %dx%d 比搜索图 %dx%d 大，无法匹配",
             a.tplW, a.tplH, a.tgtGray.cols, a.tgtGray.rows);
        a.dets.clear();
        a.texResult.releaseDeferred();
        return;
    }

    const int n = std::max(1, loops);
    try
    {
        HImage img = toHImage(a.tgtGray);
        HTuple row, col, angle, score;

        double best = 0;
        const double wall0 = nowMs();
        for (int i = 0; i < n; i++)
        {
            const double t0 = nowMs();
            a.ncc.FindNccModel(img,
                               deg2rad(a.nccAngleStartDeg), deg2rad(a.nccAngleExtentDeg),
                               a.nccMinScore, a.nccMaxMatches, a.nccMaxOverlap,
                               a.nccSubpixel ? "true" : "false", 0,
                               &row, &col, &angle, &score);
            const double dt = nowMs() - t0;
            if (i == 0 || dt < best) best = dt;
        }
        a.benchWallMs = nowMs() - wall0;

        a.preprocessMs  = 0;
        a.searchMs      = best;
        a.searchTotalMs = best * n;
        a.searchLoops   = n;

        a.dets.clear();
        const Hlong cnt = row.Length();
        a.dets.reserve((size_t)cnt);
        for (Hlong i = 0; i < cnt; i++)
        {
            Detection d;
            d.x        = (float)col[i].D();
            d.y        = (float)row[i].D();
            d.angleDeg = rad2deg(angle[i].D());
            d.scale    = 1.f;
            d.score    = (float)score[i].D();
            a.dets.push_back(d);
        }

        a.resultVis = drawResult(a, a.tgtGray, a.dets);
        a.texResult.set(toBGRA(a.resultVis));
        a.showResultOnce = true;

        logf("Halcon NCC 匹配: %d 个结果  预处理 0.00 ms  搜索 %.2f ms  "
             "搜索总耗时 %.2f ms (%d 次)  合计 %.2f ms",
             (int)a.dets.size(), a.searchMs, a.searchTotalMs, n,
             a.preprocessMs + a.searchMs);
        if (n > 1)
            logf("  重复 %d 次: 墙钟总耗时 %.1f ms  平均 %.2f ms/次",
                 n, a.benchWallMs, a.benchWallMs / n);

        // 与形状模型一致：0 命中时补查「最高候选分」，把失败原因落到日志里
        if (a.dets.empty())
        {
            HTuple r2, c2, ang2, sco2;
            a.ncc.FindNccModel(img,
                               deg2rad(a.nccAngleStartDeg), deg2rad(a.nccAngleExtentDeg),
                               0.05, 1, 1.0, a.nccSubpixel ? "true" : "false", 0,
                               &r2, &c2, &ang2, &sco2);
            if (sco2.Length())
                logf("  诊断: 最高候选分 %.4f < 最小分数 %.2f  (row=%.1f col=%.1f 角度 %.2f°)"
                     " → 调低「最小分数」即可命中",
                     sco2[0].D(), a.nccMinScore, r2[0].D(), c2[0].D(), rad2deg(ang2[0].D()));
            else
                logf("  诊断: 最小分数降到 0.05 仍无候选 → 模板/目标差异过大"
                     "（检查角度范围、极性、Contrast；或涂抹后模板只剩无特征区域）");
        }
        else
        {
            const Detection& d0 = a.dets[0];
            logf("  最高分 %.4f  (row=%.1f col=%.1f 角度 %.2f°)", d0.score, d0.y, d0.x, d0.angleDeg);
        }
    }
    catch (HException& e)
    {
        logf("!! Halcon NCC 匹配异常: %s", e.ErrorMessage().Text());
        a.dets.clear();
    }
}

static void runMatch(App& a, int loops)
{
    if (a.matchMode == 1) { runNccMatch(a, loops); return; }

    if (!a.shapeReady)
    {
        logLine("还没有形状模型，先创建或加载一个");
        return;
    }
    if (a.tgtGray.empty())
    {
        logLine("还没有匹配目标图片");
        return;
    }
    if (a.tplW > a.tgtGray.cols || a.tplH > a.tgtGray.rows)
    {
        logf("模板 %dx%d 比搜索图 %dx%d 大，无法匹配（换一张大图，或把模板缩小）",
             a.tplW, a.tplH, a.tgtGray.cols, a.tgtGray.rows);
        a.dets.clear();
        a.texResult.releaseDeferred();
        return;
    }

    const int n = std::max(1, loops);
    try
    {
        HImage img = toHImage(a.tgtGray);
        HTuple row, col, angle, scale, score;

        double best = 0;
        const double wall0 = nowMs();
        for (int i = 0; i < n; i++)
        {
            const double t0 = nowMs();
            a.shape.FindScaledShapeModel(img,
                                         deg2rad(a.angleStartDeg), deg2rad(a.angleExtentDeg),
                                         std::min(a.scaleMin, a.scaleMax),
                                         std::max(a.scaleMin, a.scaleMax),
                                         a.minScore, a.maxMatches, a.maxOverlap,
                                         a.subpixelMode ? "true" : "false",
                                         a.numLevels, a.greediness,
                                         &row, &col, &angle, &scale, &score);
            const double dt = nowMs() - t0;
            if (i == 0 || dt < best) best = dt;
        }
        a.benchWallMs = nowMs() - wall0;

        a.preprocessMs  = 0;
        a.searchMs      = best;
        a.searchTotalMs = best * n;
        a.searchLoops   = n;

        a.dets.clear();
        const Hlong cnt = row.Length();
        a.dets.reserve((size_t)cnt);
        for (Hlong i = 0; i < cnt; i++)
        {
            Detection d;
            d.x        = (float)col[i].D();
            d.y        = (float)row[i].D();
            d.angleDeg = rad2deg(angle[i].D());
            d.scale    = (float)scale[i].D();
            d.score    = (float)score[i].D();
            a.dets.push_back(d);
        }

        a.resultVis = drawResult(a, a.tgtGray, a.dets);
        a.texResult.set(toBGRA(a.resultVis));
        a.showResultOnce = true;

        logf("Halcon 匹配: %d 个结果  预处理 0.00 ms  搜索 %.2f ms  "
             "搜索总耗时 %.2f ms (%d 次)  合计 %.2f ms",
             (int)a.dets.size(), a.searchMs, a.searchTotalMs, n,
             a.preprocessMs + a.searchMs);
        if (n > 1)
            logf("  重复 %d 次: 墙钟总耗时 %.1f ms  平均 %.2f ms/次",
                 n, a.benchWallMs, a.benchWallMs / n);

        // 0 命中时把「最高候选分」查出来：否则涂抹/改参数后只能看到 0 个结果，
        // 分不清是分数压着阈值、还是模板本身已经没有可用特征。
        if (a.dets.empty())
        {
            HTuple r2, c2, ang2, sc2, sco2;
            a.shape.FindScaledShapeModel(img,
                                         deg2rad(a.angleStartDeg), deg2rad(a.angleExtentDeg),
                                         std::min(a.scaleMin, a.scaleMax),
                                         std::max(a.scaleMin, a.scaleMax),
                                         0.05, 1, 1.0, "true", 0, 0.0,
                                         &r2, &c2, &ang2, &sc2, &sco2);
            if (sco2.Length())
                logf("  诊断: 最高候选分 %.4f < 最小分数 %.2f  (row=%.1f col=%.1f 角度 %.2f°)"
                     " → 调低「最小分数」即可命中",
                     sco2[0].D(), a.minScore, r2[0].D(), c2[0].D(), rad2deg(ang2[0].D()));
            else
                logf("  诊断: 最小分数降到 0.05 仍无候选 → 模板/目标差异过大"
                     "（检查角度与缩放范围、极性、Contrast；或涂抹后模板只剩无特征区域）");
        }
        else
        {
            const Detection& d0 = a.dets[0];
            logf("  最高分 %.4f  (row=%.1f col=%.1f 角度 %.2f° 缩放 %.3f)",
                 d0.score, d0.y, d0.x, d0.angleDeg, d0.scale);
        }
    }
    catch (HException& e)
    {
        logf("!! Halcon 匹配异常: %s", e.ErrorMessage().Text());
        a.dets.clear();
    }
}

static void runMatchSafe(App& a, int loops)
{
    try { runMatch(a, loops); }
    catch (const HException& e)
    {
        logf("!! 匹配异常: %s", e.ErrorMessage().Text());
        a.dets.clear();
    }
    catch (const std::exception& e)
    {
        logf("!! 匹配异常: %s", e.what());
        a.dets.clear();
    }
}

//=======================================================================
//  图像 / 目录加载
//=======================================================================
static void loadTemplateImage(App& a, const std::wstring& path)
{
    cv::Mat g;
    if (!loadGrayImage(path, g))
    {
        logf("无法读取模板图片: %s", ws2s(path).c_str());
        return;
    }
    a.tplGray = g;
    a.tplPath = ws2s(path);
    refreshTplView(a);
    logf("模板源图: %s (%dx%d)", a.tplPath.c_str(), g.cols, g.rows);
    freezeTplSource(a, g, cv::Rect(), "模板源图 " + a.tplPath);

    if (a.autoRun)
    {
        createTemplateSafe(a);
        if (!a.tgtGray.empty()) runMatchSafe(a, 1);
    }
}

static void loadTargetImage(App& a, const std::wstring& path)
{
    cv::Mat g;
    if (!loadGrayImage(path, g))
    {
        logf("无法读取目标图片: %s", ws2s(path).c_str());
        return;
    }
    const std::string utf8 = ws2s(path);
    a.tgtGray = g;
    a.tgtPath = utf8;
    a.texTarget.set(toBGRA(g));
    logf("目标图: %s (%dx%d)", utf8.c_str(), g.cols, g.rows);

    a.thumbSel = -1;
    for (size_t i = 0; i < a.thumbs.size(); i++)
        if (a.thumbs[i].path == utf8) { a.thumbSel = (int)i; break; }

    if (a.autoRun && a.modelReady) runMatchSafe(a, 1);
}

static void selectThumb(App& a, int idx)
{
    if (a.thumbs.empty()) return;
    const int n = (int)a.thumbs.size();
    int i = idx % n;
    if (i < 0) i += n;
    a.thumbSel = i;
    loadTargetImage(a, a.thumbs[i].wpath);
}

static void loadTargetDir(App& a, const std::wstring& dir)
{
    for (Thumb& t : a.thumbs) t.tex.releaseDeferred();
    a.thumbs.clear();
    a.thumbSel = -1;
    a.dirPath  = ws2s(dir);

    const double t0 = nowMs();
    const std::vector<std::wstring> files = listDirImages(dir);
    if (files.empty())
    {
        logf("目录里没有图片: %s", a.dirPath.c_str());
        return;
    }

    const int maxThumbs = 200;
    const int thumbH    = (int)(120 * g_scale);
    int bad = 0;

    for (size_t i = 0; i < files.size() && (int)a.thumbs.size() < maxThumbs; i++)
    {
        std::ifstream f(files[i], std::ios::binary);
        if (!f) { bad++; continue; }
        std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) { bad++; continue; }

        cv::Mat buf(1, (int)data.size(), CV_8U, data.data());
        cv::Mat small = cv::imdecode(buf, cv::IMREAD_REDUCED_GRAYSCALE_4);
        if (small.empty()) small = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
        if (small.empty() || small.cols <= 0 || small.rows <= 0) { bad++; continue; }

        int tw = (int)((float)small.cols / (float)small.rows * (float)thumbH);
        tw = std::max(16, std::min(tw, thumbH * 3));
        cv::Mat thumb;
        cv::resize(small, thumb, cv::Size(tw, thumbH), 0, 0, cv::INTER_AREA);
        if (thumb.empty()) { bad++; continue; }

        Thumb t;
        t.wpath = files[i];
        t.path  = ws2s(files[i]);
        const size_t s = t.path.find_last_of("\\/");
        t.name = (s == std::string::npos) ? t.path : t.path.substr(s + 1);
        t.tex.set(toBGRA(thumb));
        a.thumbs.push_back(std::move(t));
    }

    const double ms = nowMs() - t0;
    logf("目录: %s  %d 张图片 (缩略图 %.0f ms%s)",
         a.dirPath.c_str(), (int)a.thumbs.size(), ms,
         (int)files.size() > (int)a.thumbs.size() ? "，已截断到 200 张" : "");
    if (bad) logf("其中 %d 个文件读不出来", bad);

    if (a.autoRun && !a.thumbs.empty()) selectThumb(a, 0);
}

//=======================================================================
//  中间: 图像视图
//=======================================================================
static ImVec2 fitSize(int w, int h, const ImVec2& avail, float zoom)
{
    if (w <= 0 || h <= 0 || avail.x <= 0 || avail.y <= 0) return ImVec2(0, 0);
    float s = std::min(avail.x / (float)w, avail.y / (float)h) * zoom;
    return ImVec2((float)w * s, (float)h * s);
}

// roiOwner: 0 = 不允许框选, 1 = 目标图, 2 = 模板源图
// maskPaint: 为 true 时在模板源图上支持涂抹掩码(左键=排除/右键=恢复)
static void drawImageView(const char* id, const char* info, Texture& tex, int roiOwner, bool maskPaint = false)
{
    App& a = g_app;
    if (!tex.valid())
    {
        ImGui::TextDisabled("(无图像)");
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (avail.y < 40.f) avail.y = 40.f;
    ImVec2 disp = fitSize(tex.w, tex.h, avail, a.zoom);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef((ImTextureID)(uintptr_t)tex.srv), disp);

    // 透明按钮用来接鼠标，实现 ROI 框选
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton(id, disp);

    auto toImg = [&](const ImVec2& p)
    {
        return ImVec2((p.x - origin.x) * (float)tex.w / disp.x,
                      (p.y - origin.y) * (float)tex.h / disp.y);
    };
    auto toScr = [&](const ImVec2& p)
    {
        return ImVec2(origin.x + p.x * disp.x / (float)tex.w,
                      origin.y + p.y * disp.y / (float)tex.h);
    };

    //---- 掩码涂抹模式(模板源图专属，优先于 ROI 框选) ----
    if (maskPaint && roiOwner == 2)
    {
        const bool eraseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);   // 左键 = 排除(置0)
        const bool paintDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);  // 右键 = 恢复(置255)
        const bool anyDown   = paintDown || eraseDown;

        if (ImGui::IsItemHovered() && anyDown)
        {
            ImVec2 p = toImg(ImGui::GetMousePos());
            p.x = clampf(p.x, 0.f, (float)tex.w - 1.f);
            p.y = clampf(p.y, 0.f, (float)tex.h - 1.f);

            ensureMask(a, tex.w, tex.h);
            cv::circle(a.mask, cv::Point((int)p.x, (int)p.y),
                       std::max(1, (int)a.brushRadius),
                       cv::Scalar(eraseDown ? 0 : 255), -1, cv::LINE_AA);
            refreshTplView(a);          // 实时预览覆盖层
            a.maskPainting = true;
        }

        // 松手只记"掩码改过了"，真正重建等切走页签/取消涂抹/点创建或执行
        if (a.maskPainting && !anyDown)
        {
            a.maskPainting = false;
            a.maskPending  = true;
        }

        // 画笔光标圆圈
        if (ImGui::IsItemHovered())
        {
            ImVec2 p = toImg(ImGui::GetMousePos());
            ImVec2 c = toScr(ImVec2(clampf(p.x, 0.f, (float)tex.w - 1.f),
                                    clampf(p.y, 0.f, (float)tex.h - 1.f)));
            float r = std::max(1.0f, a.brushRadius * disp.x / (float)tex.w);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircle(c, r, eraseDown ? IM_COL32(255, 80, 80, 255)
                                          : IM_COL32(0, 255, 0, 255), 0, 2.f);
        }

        ImGui::Text("%s", info);
        return;
    }

    if (roiOwner != 0)
    {
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            a.dragA = toImg(ImGui::GetMousePos());
            a.dragB = a.dragA;
            a.dragging = true;
        }
        if (a.dragging)
        {
            ImVec2 p = toImg(ImGui::GetMousePos());
            p.x = clampf(p.x, 0.f, (float)tex.w - 1.f);
            p.y = clampf(p.y, 0.f, (float)tex.h - 1.f);
            a.dragB = p;

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                a.dragging = false;
                const int x0 = (int)std::min(a.dragA.x, a.dragB.x);
                const int y0 = (int)std::min(a.dragA.y, a.dragB.y);
                const int x1 = (int)std::max(a.dragA.x, a.dragB.x);
                const int y1 = (int)std::max(a.dragA.y, a.dragB.y);
                a.roi = cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                a.roiHas   = (a.roi.width >= 8 && a.roi.height >= 8);
                a.roiOwner = a.roiHas ? roiOwner : 0;
                if (a.roiHas)
                {
                    logf("框选 ROI: (%d,%d) %dx%d", x0, y0, a.roi.width, a.roi.height);
                    if (roiOwner == 1 && !a.tgtGray.empty())
                    {
                        freezeTplSource(a, a.tgtGray, a.roi, "目标图 " + a.tgtPath);
                    }
                    else if (roiOwner == 2 && !a.tplGray.empty())
                    {
                        freezeTplSource(a, a.tplGray, a.roi, "模板源图 " + a.tplPath);
                        a.roiHas   = false;
                        a.roiOwner = 0;
                    }
                    a.roiDirty = true;      // 让右侧面板重建模板
                }
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // ROI 框用青色：绿色是"匹配到的目标"的语义，避免混淆
        const ImU32 roiCol = IM_COL32(0, 200, 255, 255);
        if (a.dragging)
        {
            ImVec2 p0 = toScr(ImVec2(std::min(a.dragA.x, a.dragB.x), std::min(a.dragA.y, a.dragB.y)));
            ImVec2 p1 = toScr(ImVec2(std::max(a.dragA.x, a.dragB.x), std::max(a.dragA.y, a.dragB.y)));
            dl->AddRect(p0, p1, roiCol, 0.f, 0, 2.f);
        }
        else if (a.roiHas && a.roiOwner == roiOwner)
        {
            ImVec2 p0 = toScr(ImVec2((float)a.roi.x, (float)a.roi.y));
            ImVec2 p1 = toScr(ImVec2((float)(a.roi.x + a.roi.width), (float)(a.roi.y + a.roi.height)));
            dl->AddRect(p0, p1, roiCol, 0.f, 0, 2.f);
        }
    }

    ImGui::Text("%s", info);
}

static void drawViewPane()
{
    App& a = g_app;
    char info[256];

    if (ImGui::BeginTabBar("##viewtabs"))
    {
        // 刚跑完一次搜索时自动切到「匹配结果」页
        ImGuiTabItemFlags resFlags = ImGuiTabItemFlags_None;
        if (a.showResultOnce) resFlags |= ImGuiTabItemFlags_SetSelected;

        if (ImGui::BeginTabItem("匹配结果", nullptr, resFlags))
        {
            snprintf(info, sizeof(info), "匹配结果  %dx%d  命中 %d",
                     a.texResult.w, a.texResult.h, (int)a.dets.size());
            drawImageView("##area_result", info, a.texResult, 0);
            a.viewTab = 0;
            ImGui::EndTabItem();
            a.showResultOnce = false;       // 只在这一次请求后清除
        }
        if (ImGui::BeginTabItem("目标图"))
        {
            snprintf(info, sizeof(info), "目标图  %dx%d", a.texTarget.w, a.texTarget.h);
            drawImageView("##area_target", info, a.texTarget, a.texTarget.valid() ? 1 : 0);
            a.viewTab = 1;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模板源图"))
        {
            snprintf(info, sizeof(info), "模板源图  %dx%d", a.texTplSrc.w, a.texTplSrc.h);
            drawImageView("##area_tpl", info, a.texTplSrc, a.texTplSrc.valid() ? 2 : 0, a.paintMask);
            a.viewTab = 2;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模板模型"))
        {
            snprintf(info, sizeof(info), "模板模型(绿=轮廓/模板框 蓝=中心)  %dx%d",
                     a.texModel.w, a.texModel.h);
            drawImageView("##area_model", info, a.texModel, 0);
            a.viewTab = 3;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // 掩码涂抹完不当场重建：等用户切离「模板源图」页签时才应用
    if (a.maskPending && a.viewTab != 2)
    {
        a.maskPending = false;
        a.maskDirty   = true;
        logLine("掩码编辑完成（已切离模板源图页），按新掩码重建模板并识别");
    }
}

//=======================================================================
//  最左: 目标图片目录（纵向列表）
//=======================================================================
static void textDimWrapped(const char* s);   // 定义在下面的面板工具函数区

static void drawThumbList()
{
    App& a = g_app;

    ImGui::TextDisabled("图片目录");
    if (!a.dirPath.empty())
        ImGui::TextWrapped("%s", a.dirPath.c_str());

    if (a.thumbs.empty())
    {
        textDimWrapped("为空。点右侧「匹配目标 → 选择图片目录...」载入。");
        return;
    }

    ImGui::Text("%d 张  当前 %d", (int)a.thumbs.size(), a.thumbSel + 1);

    // 上/下一张、分隔线固定在顶部，不随缩略图列表滚动
    if (ImGui::Button("上一张", ImVec2(-1, 0))) selectThumb(a, a.thumbSel - 1);
    if (ImGui::Button("下一张", ImVec2(-1, 0))) selectThumb(a, a.thumbSel + 1);
    ImGui::Separator();

    ImGui::BeginChild("##thumbs_scroll", ImVec2(0, -1), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoMove);

    // 缩略图按目录宽度缩放，避免出现横向滚动条
    const float availW = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < (int)a.thumbs.size(); i++)
    {
        const Thumb& t = a.thumbs[i];
        if (!t.tex.valid()) continue;

        float w = (float)t.tex.w, h = (float)t.tex.h;
        if (w > availW && w > 0.f)
        {
            const float k = availW / w;
            w *= k;
            h *= k;
        }

        ImGui::PushID(i);
        const bool sel = (i == a.thumbSel);
        const ImVec4 bg = sel ? ImVec4(0.20f, 0.60f, 0.28f, 1.00f)
                              : ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        if (ImGui::ImageButton("##thumb",
                               ImTextureRef((ImTextureID)(uintptr_t)t.tex.srv),
                               ImVec2(w, h), ImVec2(0, 0), ImVec2(1, 1), bg))
        {
            selectThumb(a, i);
        }
        if (sel)
        {
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2),
                IM_COL32(0, 255, 120, 255), 0.f, 0, 2.f);
        }
        ImGui::SetItemTooltip("%s (%d/%d)", t.name.c_str(), i + 1, (int)a.thumbs.size());
        ImGui::PopID();
    }

    ImGui::EndChild();
}

//=======================================================================
//  右侧: 操作面板
//=======================================================================
static void itemWidthForLabel(const char* label)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float labelW = ImGui::CalcTextSize(label, nullptr, true).x;
    float w = ImGui::GetContentRegionAvail().x - labelW - st.ItemInnerSpacing.x;
    const float wMin = 60.f * g_scale;
    if (w < wMin) w = wMin;
    ImGui::SetNextItemWidth(w);
}

static bool fitsOnLine(float needW)
{
    return ImGui::GetContentRegionAvail().x >= needW;
}

static float checkWidth(const char* label)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
           ImGui::CalcTextSize(label, nullptr, true).x;
}

static float buttonWidth(const char* label)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.f;
}

static void textDimWrapped(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

static void textWarnWrapped(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

static bool sliderF(const char* label, float* v, float lo, float hi, const char* fmt, bool& commit)
{
    itemWidthForLabel(label);
    const bool c = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
    return c;
}

static bool sliderI(const char* label, int* v, int lo, int hi, bool& commit)
{
    itemWidthForLabel(label);
    const bool c = ImGui::SliderInt(label, v, lo, hi);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
    return c;
}

static void drawPanel()
{
    App& a = g_app;
    bool needCreate = false, needMatch = false;

    bool changedModel = false, commitModel = false;
    bool changedSearch = false, commitSearch = false;

    auto group = [](const char* id, bool defaultOpen) -> bool
    {
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (defaultOpen) fl |= ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::CollapsingHeader(id, fl);
        if (open) ImGui::Indent(10.f * g_scale);
        return open;
    };
    auto endGroup = []() { ImGui::Unindent(10.f * g_scale); };

    //---- Tab ① 匹配: 匹配方式（形状 / NCC） ----
    auto panelMatchMode = [&]()
    {
    ImGui::SeparatorText("0. 匹配方式");

    int mode = a.matchMode;
    ImGui::RadioButton("形状匹配 (Halcon)", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("NCC (Halcon)", &mode, 1);

    if (mode != a.matchMode)
    {
        a.matchMode  = mode;
        a.modelReady = false;      // 形状模型和 NCC 模型不通用，必须重建
        needCreate   = true;
        logf("匹配方式切换为: %s", mode == 1 ? "NCC (Halcon)" : "形状匹配 (Halcon)");
        logLine(mode == 1 ? "NCC: 请在「② 模板参数」确认角度范围，然后建模板"
                          : "形状匹配: 请在「② 模板参数」调 Contrast/MinContrast，然后建模板");
    }

    if (a.matchMode == 1)
        textDimWrapped("Halcon NCC（归一化灰度相关）：对亮度/对比度变化不敏感，"
                       "只搜索旋转、没有尺度。");
    else
        textDimWrapped("Halcon 形状匹配（梯度方向）：对遮挡、光照变化、噪声更鲁棒，"
                       "支持旋转 + 尺度。");
    };

    //---- Tab ① 匹配: 模板源 ----
    auto panelTemplateSource = [&]()
    {
    ImGui::SeparatorText("1. 模板图像");

    if (ImGui::Button("选择模板图片...", ImVec2(-1, 0)))
    {
        std::wstring p = pickImageFile(L"选择模板图片");
        if (!p.empty()) loadTemplateImage(a, p);
    }
    ImGui::TextWrapped("模板源: %s", a.tplPath.empty() ? "(未选择，默认用整幅目标图)" : a.tplPath.c_str());
    if (a.tplSrcValid)
    {
        const cv::Rect& r = a.tplSrcRoi;
        if (r.width > 4 && r.height > 4)
            textDimWrapped(("模板来源已锁定: " + a.tplSrcLabel + "  ROI(" +
                            std::to_string(r.x) + "," + std::to_string(r.y) + ") " +
                            std::to_string(r.width) + "x" + std::to_string(r.height) +
                            "  —— 切换目标图 / 调参数都从这份来源重建").c_str());
        else
            textDimWrapped(("模板来源已锁定: " + a.tplSrcLabel +
                            "（整幅）—— 切换目标图 / 调参数都从这份来源重建").c_str());
    }

    if (ImGui::Button("用目标图的 ROI 当模板", ImVec2(-1, 0)))
    {
        if (a.tgtGray.empty())
        {
            logLine("还没有目标图");
        }
        else
        {
            cv::Rect r = (a.roiHas && a.roiOwner == 1)
                             ? (a.roi & cv::Rect(0, 0, a.tgtGray.cols, a.tgtGray.rows))
                             : cv::Rect(0, 0, a.tgtGray.cols, a.tgtGray.rows);
            if (r.width < 8 || r.height < 8) logLine("ROI 太小，先在目标图上拖一个框");
            else
            {
                logf("已截取模板: (%d,%d) %dx%d", r.x, r.y, r.width, r.height);
                freezeTplSource(a, a.tgtGray, r, a.tgtPath + "  [ROI]");
                logf("模板来源已锁定: %s  ROI(%d,%d) %dx%d（切换目标图或调参数都从这里重建）",
                     a.tplSrcLabel.c_str(), r.x, r.y, r.width, r.height);
                a.roiOwner = 0;
                a.roiHas   = false;
                a.roiDirty = true;
            }
        }
    }

    if (a.roiHas)
    {
        ImGui::Text("ROI: (%d,%d) %dx%d", a.roi.x, a.roi.y, a.roi.width, a.roi.height);
        if (fitsOnLine(buttonWidth("清除"))) ImGui::SameLine();
        if (ImGui::SmallButton("清除"))
        {
            a.roiHas   = false;
            a.roiOwner = 0;
            if (a.tplSrcValid && !a.tplSrcGray.empty())
                freezeTplSource(a, a.tplSrcGray, cv::Rect(), a.tplSrcLabel);
            else if (!a.tgtGray.empty())
                freezeTplSource(a, a.tgtGray, cv::Rect(), "目标图 " + a.tgtPath);
            a.roiDirty = true;
        }
    }
    else
    {
        textDimWrapped("ROI: 整幅图 (在左侧图上拖动可框选)");
    }
    };

    //---- Tab ② 模板参数（建模用） ----
    auto panelTplParams = [&]()
    {
    //---- NCC 模式: 只有旋转/极性 ----
    if (a.matchMode == 1)
    {
        if (group("NCC 模型参数: 旋转与极性", true))
        {
            changedModel |= sliderF("起始角(度)",  &a.nccAngleStartDeg,  -180.f, 180.f, "%.0f", commitModel);
            changedModel |= sliderF("角度范围(度)", &a.nccAngleExtentDeg,   0.f, 360.f, "%.0f", commitModel);
            changedModel |= sliderF("角度步长(度, 0=自动)", &a.nccAngleStepDeg, 0.f, 5.f, "%.2f", commitModel);
            changedModel |= sliderI("金字塔层数(0=自动)", &a.nccModelLevels, 0, 8, commitModel);
            {
                const char* items[] = { "use_polarity (要求明暗一致)",
                                        "ignore_global_polarity (允许整体反相)",
                                        "ignore_local_polarity (允许局部反相)" };
                itemWidthForLabel("极性(Metric)");
                if (ImGui::Combo("极性(Metric)", &a.nccMetric, items, 3))
                {
                    changedModel = true;
                    commitModel  = true;
                }
            }
            textDimWrapped("Halcon NCC 只搜索旋转、不搜索尺度，所以这里没有缩放参数。"
                           "角度范围 360 度时画布要放大到对角线(搜索变慢)，已知角度就收窄。");
            endGroup();
        }

        ImGui::Separator();
        if (ImGui::Button("创建 NCC 模型 / 应用参数", ImVec2(-1, 0))) needCreate = true;
        textDimWrapped("改完参数也可以切回「① 匹配」再点创建；开了「参数改动后自动刷新」时会立即重建。");
        return;
    }

    //---- 模板参数 ----
    if (group("模板参数: 旋转与缩放", true))
    {
        changedModel |= sliderF("起始角(度)",  &a.angleStartDeg,  -180.f, 180.f, "%.0f", commitModel);
        changedModel |= sliderF("角度范围(度)", &a.angleExtentDeg,   0.f, 360.f, "%.0f", commitModel);
        changedModel |= sliderF("角度步长(度, 0=自动)", &a.angleStepDeg, 0.f, 5.f, "%.2f", commitModel);
        changedModel |= sliderF("最小缩放", &a.scaleMin, 0.2f, 2.f, "%.2f", commitModel);
        changedModel |= sliderF("最大缩放", &a.scaleMax, 0.2f, 2.f, "%.2f", commitModel);
        if (a.scaleMax < a.scaleMin) a.scaleMax = a.scaleMin;
        textDimWrapped("无旋转/缩放需求时把范围收到 1.0 和 0 度，搜索最快、假峰最少。");
        endGroup();
    }

    if (group("模板参数: 特征提取 (Contrast)", false))
    {
        // Halcon 的 Contrast/MinContrast 是灰度阈值：Contrast 必须大于 MinContrast
        changedModel |= sliderF("MinContrast(低阈值)", &a.minContrast, 1.f, 254.f, "%.0f", commitModel);

        // 高阈值跟随低阈值：保证两者始终有分离度，拖低阈值就是连续的"沿弱边腐蚀"
        if (a.autoHi)
        {
            const float want = hiFollow(a.minContrast);
            if (std::fabs(want - a.contrast) > 1e-4f) { a.contrast = want; changedModel = true; }
        }
        if (a.autoHi) ImGui::BeginDisabled();
        changedModel |= sliderF("Contrast(高阈值)", &a.contrast, 2.f, 255.f, "%.0f", commitModel);
        if (a.autoHi) ImGui::EndDisabled();
        if (ImGui::Checkbox("高阈值自动跟随 (推荐)", &a.autoHi))
        {
            changedModel = true;
            commitModel  = true;
        }

        if (a.contrast <= a.minContrast)
        {
            a.contrast = std::min(255.f, a.minContrast + 1.f);
            if (a.contrast <= a.minContrast) a.minContrast = a.contrast - 1.f;
        }
        changedModel |= sliderI("金字塔层数(0=自动)", &a.numLevels, 0, 8, commitModel);
        textDimWrapped("Halcon 用 Contrast(高阈值) / MinContrast(低阈值) 做滞后阈值提边；"
                       "拉大 = 只保留强边，特征点变少。");
        textWarnWrapped("Halcon 没有「最大点数 / 最小间距 / 覆盖率 / 评分方式」这些参数，"
                        "所以这几项在本 Demo 中无法与项目实现一一对应。");
        endGroup();
    }

    ImGui::Separator();
    if (ImGui::Button("创建模板 / 应用参数", ImVec2(-1, 0))) needCreate = true;
    textDimWrapped("改完参数也可以切回「① 匹配」再点创建；开了「参数改动后自动刷新」时会立即重建。");
    };

    //---- Tab ① 匹配: 创建模板 + 模型信息 ----
    auto panelCreate = [&]()
    {
    if (ImGui::Button("创建模板 / 应用参数", ImVec2(-1, 0))) needCreate = true;

    if (a.modelReady)
    {
        if (a.matchMode == 1)
            ImGui::Text("NCC 模型: %dx%d 模板 / 建模板 %.1f ms",
                        a.tplW, a.tplH, a.buildMs);
        else
            ImGui::Text("形状模型: %d 个轮廓点 / 建模板 %.1f ms",
                        (int)a.modelPts.size(), a.buildMs);
    }
    else
    {
        ImGui::TextDisabled("模型: 未创建");
    }
    };

    //---- Tab ④ 掩码 / 模型文件 ----
    auto panelMaskModel = [&]()
    {
    //---- 模板掩码(非零区域参与建模) ----
    if (group("模板掩码 (排除重复/不稳定区域)", false))
    {
        if (a.mask.empty())
            textDimWrapped("未加载 - 默认整幅模板参与建模。"
                           "若模板里有全图重复的圆角/直线/阵列，建议涂掉，只留唯一结构。");
        else
        {
            const double total = (double)a.mask.cols * a.mask.rows;
            const double keep = 100.0 * (total > 0 ? cv::countNonZero(a.mask) / total : 0.0);
            ImGui::Text("已加载: %.1f%% 区域参与建模", keep);
            ImGui::TextWrapped("%s", a.maskPath.c_str());
        }
        if (ImGui::Button("加载掩码...", ImVec2(-140, 0)))
        {
            std::wstring p = pickImageFile(L"选择掩码图(非零=参与建模)");
            if (!p.empty())
            {
                cv::Mat m;
                if (!loadGrayImage(p, m)) logf("无法读取掩码图: %s", ws2s(p).c_str());
                else
                {
                    a.maskPath = ws2s(p);
                    a.mask     = m;
                    refreshTplView(a);
                    const double total = (double)m.cols * m.rows;
                    logf("掩码已加载: %s (%dx%d) 非零占比 %.1f%%",
                         a.maskPath.c_str(), m.cols, m.rows,
                         100.0 * (total > 0 ? cv::countNonZero(m) / total : 0.0));
                    needCreate = true;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("清除掩码", ImVec2(-1, 0)))
        {
            a.maskPath.clear();
            a.mask.release();
            refreshTplView(a);
            logLine("掩码已清除");
            needCreate = true;
        }

        //---- 掩码涂抹 ----
        if (ImGui::Checkbox("涂抹掩码模式", &a.paintMask))
        {
            if (a.paintMask && !a.tplGray.empty())
            {
                ensureMask(a, a.tplGray.cols, a.tplGray.rows);
                refreshTplView(a);
            }
            else if (!a.paintMask && a.maskPending)
            {
                a.maskPending = false;
                a.maskDirty   = true;
                logLine("已退出涂抹模式，按新掩码重建模板并识别");
            }
        }
        if (a.paintMask)
        {
            itemWidthForLabel("画笔半径(px)");
            ImGui::SliderFloat("画笔半径(px)", &a.brushRadius, 1.f, 30.f, "%.0f");
            ImGui::TextWrapped("在左侧\"模板源图\"标签页: 左键涂抹=排除, 右键涂抹=恢复参与");
            ImGui::TextWrapped("涂的过程中不会重新识别；涂完切到其他页签（如\"匹配结果\"）"
                               "或取消勾选本项后才应用。");
        }
        if (a.maskPending)
        {
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f),
                "掩码已修改，尚未生效 —— 切到「匹配结果」等页签，或取消勾选涂抹模式即可应用。");
        }
        endGroup();
    }

    //---- 模型文件 ----
    if (group("模型文件 (保存 / 加载)", false))
    {
        // Halcon 原生格式：形状模型 .shm，NCC 模型 .ncm
        static char modelPath[260] = "case1/model.shm";
        static int  lastMode = -1;
        if (lastMode != a.matchMode)
        {
            lastMode = a.matchMode;
            strncpy(modelPath, a.matchMode == 1 ? "case1/model.ncm" : "case1/model.shm",
                    sizeof(modelPath) - 1);
            modelPath[sizeof(modelPath) - 1] = '\0';
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##modelpath", modelPath, sizeof(modelPath));

        if (ImGui::Button("保存模型", ImVec2(-120, 0)))
        {
            try
            {
                if (a.matchMode == 1)
                {
                    if (!a.nccReady) logLine("没有 NCC 模型可保存");
                    else { a.ncc.WriteNccModel(modelPath); logf("NCC 模型已保存: %s", modelPath); }
                }
                else if (!a.shapeReady) logLine("没有形状模型可保存");
                else { a.shape.WriteShapeModel(modelPath); logf("形状模型已保存: %s", modelPath); }
            }
            catch (HException& e) { logf("保存失败: %s", e.ErrorMessage().Text()); }
        }
        ImGui::SameLine();
        if (ImGui::Button("加载模型", ImVec2(-1, 0)))
        {
            try
            {
                if (a.matchMode == 1)
                {
                    a.ncc.ReadNccModel(modelPath);
                    a.nccReady   = true;
                    a.modelReady = true;
                    a.buildMs    = 0;

                    double orow = 0, ocol = 0;
                    a.ncc.GetNccModelOrigin(&orow, &ocol);

                    double as = 0, ae = 0, astep = 0;
                    HString metric;
                    a.ncc.GetNccModelParams(&as, &ae, &astep, &metric);
                    a.nccAngleStartDeg  = rad2deg(as);
                    a.nccAngleExtentDeg = rad2deg(ae);
                    a.nccAngleStepDeg   = rad2deg(astep);
                    const std::string ms = metric.Text();
                    a.nccMetric = (ms == "ignore_global_polarity") ? 1
                                : (ms == "ignore_local_polarity"  ? 2 : 0);

                    // NCC 模型文件里不含模板尺寸，尺寸只能从当前模板源图取
                    if (!a.tplGray.empty()) { a.tplW = a.tplGray.cols; a.tplH = a.tplGray.rows; }
                    else
                    {
                        a.tplW = std::max(1, a.tplW);
                        a.tplH = std::max(1, a.tplH);
                        logLine("提示: NCC 模型不含模板尺寸，结果框按当前模板尺寸绘制；"
                                "建议先选一张模板源图");
                    }
                    a.modelPolys.clear();
                    a.modelPts.clear();
                    resolveModelOrigin(orow, ocol, a.tplW, a.tplH, &a.originRow, &a.originCol);
                    a.modelBox = cv::Rect2f((float)(-a.originCol), (float)(-a.originRow),
                                            (float)a.tplW, (float)a.tplH);
                    a.modelGeomValid = true;
                    buildNccModelVis(a);
                    a.texModel.set(toBGRA(a.modelVis));
                    logf("NCC 模型已加载: %s  (%dx%d)  角度 %.1f..%.1f",
                         modelPath, a.tplW, a.tplH,
                         a.nccAngleStartDeg, a.nccAngleStartDeg + a.nccAngleExtentDeg);
                }
                else
                {
                    a.shape.ReadShapeModel(modelPath);
                    a.shapeReady = true;
                    a.modelReady = true;
                    a.buildMs    = 0;

                    double orow = 0, ocol = 0;
                    a.shape.GetShapeModelOrigin(&orow, &ocol);
                    if (!a.tplGray.empty())
                        resolveModelOrigin(orow, ocol, a.tplGray.cols, a.tplGray.rows,
                                           &a.originRow, &a.originCol);
                    else
                    {
                        // 没有模板源图时无法换算中心偏移；模板模型视图会退回用轮廓包围盒定位
                        a.originRow = orow;
                        a.originCol = ocol;
                    }

                    double as = 0, ae = 0, astep = 0, smin = 0, smax = 0, sstep = 0;
                    HString metric;
                    Hlong minc = 0;
                    a.shape.GetShapeModelParams(&as, &ae, &astep, &smin, &smax, &sstep,
                                                &metric, &minc);
                    a.angleStartDeg  = rad2deg(as);
                    a.angleExtentDeg = rad2deg(ae);
                    a.angleStepDeg   = rad2deg(astep);
                    a.scaleMin       = (float)smin;
                    a.scaleMax       = (float)smax;
                    a.minContrast    = (float)minc;
                    const std::string ms = metric.Text();
                    a.metric = (ms == "ignore_global_polarity") ? 1
                             : (ms == "ignore_local_polarity"  ? 2 : 0);

                    loadShapeGeometry(a);
                    buildShapeModelVis(a);
                    a.texModel.set(toBGRA(a.modelVis));

                    a.tplW = !a.tplGray.empty() ? a.tplGray.cols
                                                : std::max(1, (int)std::lround(a.modelBox.width));
                    a.tplH = !a.tplGray.empty() ? a.tplGray.rows
                                                : std::max(1, (int)std::lround(a.modelBox.height));
                    logf("形状模型已加载: %s  (%d 轮廓点)  角度 %.1f..%.1f  缩放 %.2f..%.2f",
                         modelPath, (int)a.modelPts.size(),
                         a.angleStartDeg, a.angleStartDeg + a.angleExtentDeg,
                         a.scaleMin, a.scaleMax);
                }
            }
            catch (HException& e) { logf("加载失败: %s (形状用 .shm / NCC 用 .ncm)", e.ErrorMessage().Text()); }
        }
        endGroup();
    }
    };

    //---- Tab ① 匹配: 选择目标 ----
    auto panelTarget = [&]()
    {
    ImGui::SeparatorText("2. 匹配目标");

    if (ImGui::Button("选择图片目录...", ImVec2(-1, 0)))
    {
        std::wstring d = pickFolder();
        if (!d.empty()) loadTargetDir(a, d);
    }
    if (ImGui::Button("选择单张图片...", ImVec2(-1, 0)))
    {
        std::wstring p = pickImageFile(L"选择匹配目标图片");
        if (!p.empty()) loadTargetImage(a, p);
    }

    if (!a.thumbs.empty())
    {
        ImGui::Text("目录: %s", a.dirPath.c_str());
        ImGui::Text("%d 张  当前 %d/%d", (int)a.thumbs.size(),
                    a.thumbSel + 1, (int)a.thumbs.size());
        if (ImGui::Button("<< 上一张", ImVec2(-110, 0))) selectThumb(a, a.thumbSel - 1);
        ImGui::SameLine();
        if (ImGui::Button("下一张 >>", ImVec2(-1, 0)))  selectThumb(a, a.thumbSel + 1);
    }

    ImGui::TextWrapped("目标: %s", a.tgtPath.empty() ? "(未选择)" : a.tgtPath.c_str());
    if (!a.tgtGray.empty())
        ImGui::Text("尺寸: %dx%d", a.tgtGray.cols, a.tgtGray.rows);
    };

    //---- Tab ① 匹配: 匹配判据 ----
    auto panelMatchCriteria = [&]()
    {
    ImGui::SeparatorText("3. 搜索参数");

    if (a.matchMode == 1)
    {
        if (group("NCC 判据 (分数)", true))
        {
            changedSearch |= sliderF("最小分数", &a.nccMinScore, 0.05f, 1.00f, "%.2f", commitSearch);
            textDimWrapped("Halcon NCC 分数范围 [-1, 1]：0.5 左右算「较像」，0.8 以上基本确定。"
                           "实例数 / 最大重叠 / 亚像素 在「③ 搜索参数」里。");
            endGroup();
        }
        return;
    }

    //---- 匹配判据 ----
    if (group("匹配判据 (分数)", true))
    {
        changedSearch |= sliderF("最小分数", &a.minScore, 0.05f, 1.f, "%.2f", commitSearch);
        {
            // 极性(Halcon 的 Metric)
            const char* items[] = { "要求极性一致(use_polarity)",
                                    "忽略整体反转(ignore_global_polarity)",
                                    "忽略逐点极性(ignore_local_polarity)" };
            itemWidthForLabel("极性(Metric)");
            if (ImGui::Combo("极性(Metric)", &a.metric, items, 3))
            {
                changedSearch = true;
                commitSearch  = true;
            }
        }
        if (a.metric == 2)
            textWarnWrapped("「忽略逐点极性」比默认慢；只在确实有局部反光时用。");
        else if (a.metric == 1)
            textDimWrapped("「忽略整体反转」把两遍扫描取较大值，耗时约为默认的 2 倍。");
        textDimWrapped("Halcon 没有「最小覆盖率 / 评分方式」参数，结果表的\"覆盖\"列恒为 \"-\"。");
        endGroup();
    }
    };

    //---- Tab ③ 搜索参数（高级） ----
    auto panelSearchParams = [&]()
    {
    if (a.matchMode == 1)
    {
        if (group("NCC 搜索参数", true))
        {
            changedSearch |= sliderI("最大匹配数", &a.nccMaxMatches, 1, 64, commitSearch);
            changedSearch |= sliderF("最大重叠", &a.nccMaxOverlap, 0.f, 1.f, "%.2f", commitSearch);
            {
                // Halcon 的 find_ncc_model 只接受 "true"/"false"
                const char* items[] = { "false (关闭)", "true (插值)" };
                itemWidthForLabel("亚像素");
                if (ImGui::Combo("亚像素", &a.nccSubpixel, items, 2))
                {
                    changedSearch = true;
                    commitSearch  = true;
                }
            }
            textDimWrapped("最大重叠 = 两个实例最多重叠多少比例仍算两个 (Halcon 的 MaxOverlap)。"
                           "Halcon NCC 亚像素只有开/关两种。");
            endGroup();
        }
        return;
    }

    //---- 结果筛选 ----
    if (group("结果筛选 (实例数 / 去重 / 框样式)", true))
    {
        changedSearch |= sliderI("最大匹配数", &a.maxMatches, 1, 64, commitSearch);
        changedSearch |= sliderF("最大重叠(1=关闭)", &a.maxOverlap, 0.f, 1.f, "%.2f", commitSearch);
        if (a.maxOverlap < 0.999f)
            textDimWrapped("两个实例的模型框重叠超过该比例时只保留高分那个。"
                           "Halcon 没有「最小间距」参数。");
        {
            const char* items[] = { "固定方框(水平，尺寸=模板x缩放)",
                                    "旋转框(跟角度倾斜)",
                                    "模型凸包(不规则多边形)" };
            itemWidthForLabel("结果框样式");
            if (ImGui::Combo("结果框样式", &a.boxMode, items, 3))
            {
                changedSearch = true;
                commitSearch  = true;
            }
        }
        textDimWrapped("多实例场景把最大匹配数调大。"
                       "固定方框尺寸恒定、不随角度摇摆，多帧之间位置更好对比。");
        endGroup();
    }

    //---- 性能 ----
    if (group("性能与加速", false))
    {
        changedSearch |= sliderF("贪婪度", &a.greediness, 0.3f, 1.f, "%.2f", commitSearch);
        {
            // Halcon 的 SubPixel 只接受 "true"/"false"
            const char* items[] = { "false (关闭)", "true (插值)" };
            itemWidthForLabel("亚像素");
            if (ImGui::Combo("亚像素", &a.subpixelMode, items, 2))
            {
                changedSearch = true;
                commitSearch  = true;
            }
        }
        textDimWrapped("贪婪度越大越快，但可能漏检。Halcon 亚像素只有开/关两种。");
        textDimWrapped("Halcon 版没有 OpenCL 开关（由 HALCONARCH / 许可决定）。");
        endGroup();
    }
    };

    //---- Tab ⑤ 界面选项 ----
    auto panelUi = [&]()
    {
    if (group("界面选项", true))
    {
        if (ImGui::Checkbox("显示模型轮廓(折线)", &a.showContour))
        {
            changedSearch = true;
            commitSearch  = true;
        }
        textDimWrapped("勾选后除了结果框，还会把模型轮廓的每一条折线按位姿画出来。");
        ImGui::Checkbox("参数改动后自动刷新", &a.autoRun);
        ImGui::Checkbox("拖动时实时刷新", &a.liveUpdate);
        itemWidthForLabel("显示缩放");
        ImGui::SliderFloat("显示缩放", &a.zoom, 0.2f, 4.f, "%.2f");
        endGroup();
    }
    };

    //---- Tab ① 匹配: 执行按钮 ----
    auto panelRunButtons = [&]()
    {
    ImGui::SeparatorText("4. 执行匹配");

    if (ImGui::Button("执行匹配", ImVec2(-1, 0)))
    {
        if (a.maskPending) { a.maskPending = false; needCreate = true; }
        else               needMatch = true;
    }
    if (ImGui::Button("重复 20 次计时", ImVec2(-1, 0)))
    {
        if (a.maskPending) { a.maskPending = false; createTemplateSafe(a); }
        runMatchSafe(a, 20);
    }
    };

    //---- Tab ① 匹配: 结果 ----
    auto panelRunResult = [&]()
    {
    const int nLoops = a.searchLoops > 0 ? a.searchLoops : 1;
    ImGui::TextWrapped("耗时: 预处理 %.2f ms | 搜索 %.2f ms | 合计 %.2f ms | 刷新 %.1f ms",
                       a.preprocessMs, a.searchMs,
                       a.preprocessMs + a.searchMs, a.lastOpMs);
    if (nLoops > 1)
        ImGui::TextWrapped("搜索总耗时 %.2f ms (%d 次, 平均 %.2f ms/次) | 墙钟总耗时 %.1f ms",
                           a.searchTotalMs, nLoops, a.searchTotalMs / nLoops, a.benchWallMs);
    else
        ImGui::TextWrapped("搜索总耗时 %.2f ms (%d 次)", a.searchTotalMs, nLoops);

    if (ImGui::BeginTable("##dets", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(-1, 160)))
    {
        ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("分数", ImGuiTableColumnFlags_WidthFixed, 52);
        ImGui::TableSetupColumn("覆盖", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("位置", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("角度", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("缩放", ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < a.dets.size(); i++)
        {
            const Detection& d = a.dets[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", (int)i);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", d.score);
            ImGui::TableSetColumnIndex(2);
            if (d.coverage > 0.f) ImGui::Text("%.2f", d.coverage);
            else                  ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f, %.2f", d.x, d.y);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", d.angleDeg);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f", d.scale);
        }
        ImGui::EndTable();
    }
    if (a.dets.empty()) ImGui::TextDisabled("没有匹配结果");

    if (ImGui::Button("保存结果图", ImVec2(-1, 0)))
    {
        if (a.resultVis.empty()) logLine("没有结果图可保存");
        else
        {
            const std::string dir = "case1/out";
            cv::utils::fs::createDirectory(dir);
            const std::string out = dir + "/halcon_gui_result.png";
            if (cv::imwrite(out, a.resultVis)) logf("结果图已保存: %s", out.c_str());
            else logf("保存失败: %s", out.c_str());
        }
    }
    };

    //---- Tab ⑤ 日志 ----
    auto panelLog = [&]()
    {
    ImGui::SeparatorText("日志");
    const float logH = std::max(100.f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##log", ImVec2(-1, logH), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.0f);
    for (size_t i = 0; i < g_log.size(); i++)
        ImGui::TextUnformatted(g_log[i].c_str());
    ImGui::PopTextWrapPos();
    if (g_logDirty)
    {
        ImGui::SetScrollHereY(1.0f);
        g_logDirty = false;
    }
    ImGui::EndChild();
    };

    //================= 参数分页 =================
    if (ImGui::BeginTabBar("##paneltabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem("① 匹配"))
        {
            panelMatchMode();
            panelTemplateSource();
            panelCreate();
            panelTarget();
            panelMatchCriteria();
            panelRunButtons();
            panelRunResult();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("② 模板参数"))
        {
            panelTplParams();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("③ 搜索参数"))
        {
            panelSearchParams();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("④ 掩码/模型"))
        {
            panelMaskModel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("⑤ 界面/日志"))
        {
            panelUi();
            panelLog();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    //===== 统一执行：任何 tab 里改的参数都在这里生效 =====
    if (a.autoRun)
    {
        const bool live = a.liveUpdate && a.lastOpMs < 150.0;
        if (commitModel  || (changedModel  && live)) needCreate = true;
        if (commitSearch || (changedSearch && live)) needMatch  = true;
        if (a.roiDirty)  { a.roiDirty  = false; needCreate = true; }
        if (a.maskDirty) { a.maskDirty = false; needCreate = true; }
    }
    else
    {
        a.roiDirty  = false;
        a.maskDirty = false;
    }

    if (needCreate || needMatch)
    {
        const double t0 = nowMs();
        if (needCreate) a.maskPending = false;
        if (needCreate)
        {
            createTemplateSafe(a);
            if (a.autoRun && !a.tgtGray.empty()) runMatchSafe(a, 1);
        }
        else
        {
            runMatchSafe(a, 1);
        }
        a.lastOpMs = nowMs() - t0;
    }
}

//=======================================================================
//  主循环里的整屏窗口
//=======================================================================
static void drawUI()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("HalconMatch", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // 右栏参数面板宽度（与 GeoGUIDemo 相同的自适应策略）
    const float panelW0   = 470.f * g_scale;
    const float panelMinW = 360.f * g_scale;
    const float thumbW0   = 200.f * g_scale;
    const float thumbMinW = 130.f * g_scale;
    const float minViewW  = 260.f * g_scale;
    const float gap       = ImGui::GetStyle().ItemSpacing.x;

    const float availW = ImGui::GetContentRegionAvail().x;
    float panelW = std::min(panelW0, std::max(panelMinW, availW - thumbW0 - minViewW - gap * 2));
    float thumbW = std::min(thumbW0,
                            std::max(thumbMinW, availW - panelW - minViewW - gap * 2));

    //---------------- 最左: 图片目录 ----------------
    ImGui::BeginChild("##thumblist", ImVec2(thumbW, -1), ImGuiChildFlags_Borders);
    drawThumbList();
    ImGui::EndChild();

    //---------------- 中间: 图像视图 ----------------
    ImGui::SameLine();
    ImGui::BeginChild("##view", ImVec2(-panelW - gap, -1), ImGuiChildFlags_Borders);
    drawViewPane();
    ImGui::EndChild();

    //---------------- 右列: 参数面板 ----------------
    ImGui::SameLine();
    ImGui::BeginChild("##panel", ImVec2(panelW, -1), ImGuiChildFlags_Borders);
    drawPanel();
    ImGui::EndChild();

    ImGui::End();
}

//=======================================================================
//  默认目录: 自动建模板 + 自动匹配一次
//=======================================================================
static void autoInit(App& a, const std::string& dir)
{
    // 目录可能带反斜杠，拼 "/temp.jpg" 时统一成正斜杠
    std::string d = dir;
    for (size_t i = 0; i < d.size(); i++) if (d[i] == '\\') d[i] = '/';
    const std::string tpl = d + "/temp.jpg";

    // 先把目录读进来：即使没有默认模板，用户也应该看到图片列表
    loadTargetDir(a, s2ws(dir));

    if (!loadGrayImage(s2ws(tpl), a.tplGray))
    {
        logf("没有默认模板 %s，请在右侧手动选择模板图片（图片列表已载入）", tpl.c_str());
    }
    else
    {
        a.tplPath = tpl;
        refreshTplView(a);
        logf("模板源图: %s (%dx%d)", tpl.c_str(), a.tplGray.cols, a.tplGray.rows);
        freezeTplSource(a, a.tplGray, cv::Rect(), "模板源图 " + a.tplPath);
        createTemplateSafe(a);
    }

    if (a.tgtGray.empty())
    {
        logf("%s 里没有可用于匹配的图片", dir.c_str());
        return;
    }
    // 第一张如果正好是模板本身，直接跳到下一张
    const std::string tplName = tpl.substr(tpl.find_last_of("\\/") + 1);
    if (a.thumbSel == 0 && a.thumbs.size() > 1 && a.thumbs[0].name == tplName)
        selectThumb(a, 1);

    // 启动后先跑一次匹配，否则「匹配结果」页是空的
    if (a.modelReady && !a.tgtGray.empty())
    {
        runMatchSafe(a, 1);
    }
}

//=======================================================================
//  main
//=======================================================================
int main(int argc, char** argv)
{
    // 参数：--ncc 切换默认匹配方式；第一个非选项参数当目录（默认 case1）
    std::string dir = "case1";
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--ncc") == 0) { g_app.matchMode = 1; continue; }
        if (argv[i][0] != '-') { dir = argv[i]; }
    }

    ::SetUnhandledExceptionFilter(crashFilter);
    logf("启动: cwd=%s  目录=%s", cv::utils::fs::getcwd().c_str(), dir.c_str());

    //---------------- 窗口 + D3D11 ----------------
    ImGui_ImplWin32_EnableDpiAwareness();
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"HalconGUIDemo", nullptr };
    ::RegisterClassExW(&wc);
    g_hwnd = ::CreateWindowW(wc.lpszClassName, L"Halcon 模板匹配验证工具 (32 位)",
                             WS_OVERLAPPEDWINDOW, 100, 100,
                             (int)(1500 * g_scale), (int)(920 * g_scale),
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hwnd);

    //---------------- ImGui ----------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_scale);
    style.FontScaleDpi = g_scale;

    // 默认字体不含中文，这里换成系统里的黑体
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", 16.0f * g_scale, nullptr,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttf", 16.0f * g_scale, nullptr,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\simhei.ttf", 16.0f * g_scale, nullptr,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) font = io.Fonts->AddFontDefault();
    io.FontDefault = font;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    //---------------- Halcon 环境 ----------------
    try
    {
        HTuple version;
        GetSystem("version", &version);
        logf("Halcon 后端就绪: version=%s  arch=%s", version.S().Text(),
             getenv("HALCONARCH") ? getenv("HALCONARCH") : "(未设置)");
    }
    catch (HException& e)
    {
        logf("Halcon 初始化失败: %s", e.ErrorMessage().Text());
    }
    autoInit(g_app, dir);

    //---------------- 主循环 ----------------
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resizeW != 0 && g_resizeH != 0)
        {
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 任何异常都只写日志，不能让进程直接 terminate
        try
        {
            drawUI();
        }
        catch (const HException& e)
        {
            logf("!! 界面绘制异常，本帧已跳过: %s", e.ErrorMessage().Text());
            ImGui::EndFrame();
            g_dirtyFrame = true;
        }
        catch (const cv::Exception& e)
        {
            logf("!! 界面绘制异常，本帧已跳过: %s", e.what());
            ImGui::EndFrame();
            g_dirtyFrame = true;
        }
        catch (const std::exception& e)
        {
            logf("!! 界面绘制异常，本帧已跳过: %s", e.what());
            ImGui::EndFrame();
            g_dirtyFrame = true;
        }

        if (!g_dirtyFrame)
            ImGui::Render();
        else
            g_dirtyFrame = false;
        const float clear[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);

        // 上一帧 draw list 已经提交，现在可以安全释放延迟销毁的纹理
        flushPendingTextures();
    }

    //---------------- 收尾 ----------------
    flushPendingTextures();
    g_app.texTarget.release();
    g_app.texResult.release();
    g_app.texTplSrc.release();
    g_app.texModel.release();
    for (Thumb& t : g_app.thumbs) t.tex.release();
    g_app.thumbs.clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

//=======================================================================
//  D3D11 / Win32 样板代码
//=======================================================================
static bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (res == DXGI_ERROR_UNSUPPORTED)   // 没有硬件设备就用 WARP 软渲染
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx  = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev  = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back)
    {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeW = (UINT)LOWORD(lParam);
        g_resizeH = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // 屏蔽 ALT 菜单
        break;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT r = { 0, 0, (LONG)(800 * g_scale), (LONG)(600 * g_scale) };
        ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
