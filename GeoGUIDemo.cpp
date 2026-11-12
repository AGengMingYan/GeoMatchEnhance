//***********************************************************************
//  GeoGUIDemo - 用 Dear ImGui 做的模板匹配快速验证界面
//  (Win32 + DirectX11 后端，不需要 glfw)
//
//  布局:
//    左侧  图像视图: 目标图 / 匹配结果 / 模板源图 / 模板模型
//                   在图像上按住左键拖动可以框选 ROI，ROI 用来创建模板
//    右侧  操作面板: 1. 选择模板图片 + 模板参数 + 创建模板(+保存/加载模型)
//                    2. 选择匹配目标
//                    3. 搜索参数
//                    4. 执行匹配 + 结果列表 + 耗时(含搜索总耗时) + 日志
//          面板宽度随窗口自适应(窗口变窄时收缩)，所有"控件+右侧标签"的行都按标签宽度
//          收窄控件，长提示/日志自动折行，避免右边被裁掉显示不全。
//
//  用法:  GeoGUIDemo [目录]        目录默认为 case1
//         启动时会自动尝试 <目录>/temp.jpg 建模板，并拿目录里第一张
//         其它图片做一次匹配，方便直接看效果。
//***********************************************************************
#define _CRT_SECURE_NO_WARNINGS
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <objbase.h>
#include <shobjidl_core.h>      // IFileDialog / 文件夹选择框

#include <algorithm>
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
#include "GeoMatch/GeoMatch.h"

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

static void logLine(const std::string& s)
{
    if (g_log.size() > 400)
        g_log.erase(g_log.begin(), g_log.begin() + 100);
    g_log.push_back(s);
    g_logDirty = true;
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

//=======================================================================
//  纹理: 把 cv::Mat 上传成 D3D11 贴图
//=======================================================================
struct Texture
{
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;

    // 纹理放进 vector 里会搬动，所以只允许移动，不允许拷贝(否则会重复 Release)
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

    // 待销毁队列：帧渲染过程中被 releaseDeferred() 移入这里的纹理，
    // 等本帧 Present 之后再统一 Release，避免 draw list 还引用着就被销毁。
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

    // 延迟释放：把资源转移进待销毁队列，稍后统一 Release
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
        // D3D11 的行距按 16 字节对齐最稳妥
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

// 帧渲染完成后统一释放被延迟销毁的纹理
static void flushPendingTextures()
{
    for (Texture& t : Texture::pending()) t.release();
    Texture::pending().clear();
}

//=======================================================================
//  图片列表里的一项: 只有缩略图纹理，点中时才读原图
//=======================================================================
struct Thumb
{
    std::wstring wpath;         // 读图用
    std::string  path;          // utf8，显示/比较用
    std::string  name;          // 文件名
    Texture      tex;
};

//=======================================================================
//  应用状态
//=======================================================================
struct App
{
    //---- 参数单位说明 ----
    // 界面上的对比度用"灰度差"(0~255)表示，和 Halcon 的 Contrast 可直接对比；
    // 库内部的阈值是 Sobel(3x3) 的梯度幅值，一个理想硬台阶的幅值 = 4 x 灰度差，
    // 所以进出 Config 时乘/除这个系数。真实边缘因模糊会更小(σ=1 时约 2.6x)，
    // 因此界面值和 Halcon 只能"可比"，不能严格相等。
    static constexpr float kSobelPerGray = 4.0f;

    //---- 界面上的参数(度/百分比等单位) ----
    float angleStartDeg  = -180.f;
    float angleExtentDeg = 360.f;
    float angleStepDeg   = 0.f;        // 0 = 自动
    float scaleMin       = 1.f;
    float scaleMax       = 1.f;
    float minContrast    = 3.f;        // 灰度差 (= 库里的 12)
    float contrast       = 5.5f;       // 灰度差 (= 库里的 22)，滞后高阈值
    bool  autoHi         = true;       // 高阈值跟随低阈值(见 hiFor())
    int   maxPoints      = 1200;
    int   numLevels      = 0;          // 0 = 自动
    float minScore       = 0.55f;
    int   maxMatches     = 12;
    float minDistance    = 30.f;
    float greediness     = 0.9f;
    bool  subpixel       = true;
    bool  useOpenCL      = true;
    int   scoreMode      = 1;           // 1 = positive-mean(默认), 0 = mean, 2 = trimmed-mean
                                        // 3 = clipped-mean(只算正向但不缩分母, 部分匹配不上高分)
    float minCoverage    = 0.f;         // 0 = 关闭。 模型点里"梯度方向一致"的最低占比,
                                        // 用来剔除调低最小分数后冒出来的相似位置
    int   boxMode        = 0;           // 结果框样式: 0 = 固定方框(默认), 1 = 旋转框, 2 = 模型凸包
    bool  autoRun        = true;       // 参数改完自动重建模板 + 重匹配
    bool  liveUpdate     = true;       // 拖动滑块的过程中就实时刷新
    float zoom           = 1.0f;       // 1 = 适应窗口
    bool  roiDirty       = false;      // ROI 刚改完，需要重建模板
    double lastOpMs      = 0;          // 上一次"重建 + 匹配"的总耗时，用来自动降频

    //---- 本次搜索结果里的耗时统计(点"重复 20 次计时"时是累计值) ----
    double searchTotalMs = 0;          // 搜索总耗时 = 每次搜索 × 重复次数
    double benchWallMs   = 0;          // 整次 detect 调用的墙钟耗时(含预处理/重复)
    int    searchLoops   = 0;          // 这次搜索重复了几次

    //---- 数据 ----
    geomatch::Matcher matcher;
    bool    modelReady = false;
    double  buildMs    = 0;

    std::string tplPath, tgtPath;
    cv::Mat tplGray, tgtGray;
    cv::Mat modelVis, resultVis;

    //---- 目标图片目录 ----
    std::vector<Thumb> thumbs;
    int    thumbSel = -1;
    std::string dirPath;

    Texture texTarget, texResult, texTplSrc, texModel;

    std::vector<geomatch::Detection> dets;
    geomatch::Timing timing;

    //---- ROI(图像坐标) ----
    cv::Rect roi;
    bool     roiHas   = false;
    int      roiOwner = 0;             // 1 = 目标图, 2 = 模板源图
    bool     dragging = false;
    ImVec2   dragA, dragB;

    //---- 模板掩码(非零 = 参与建模的区域) ----
    std::string maskPath;
    cv::Mat     mask;

    //---- 掩码涂抹 ----
    bool  paintMask    = false;   // 涂抹模式开关
    bool  maskPainting = false;   // 正在按住涂抹
    bool  maskDirty    = false;   // 涂抹结束，需要重建模板
    float brushRadius  = 6.f;     // 画笔半径(图像像素)

    // 高阈值的推荐值(灰度差)。滞后阈值必须有分离度才会"沿着弱边生长"，
    // 分离度太小就退化成纯阈值、线会被切碎；太大又会长出噪声。
    //
    // 分离度必须很小：种子阈值一旦超过模板里最强的那条边，种子数为 0，
    // 模型点会瞬间全灭(本机 case1 模板 Sobel 峰值只有 152 ≈ 灰度差 38，
    // 之前按 2.5x 跟随，最小对比度拖到 20 就已经全没了)。15% + 2 的余量
    // 能让整个 0~38 灰度差区间连续"腐蚀"，到顶才消失。
    static float hiFor(float lo)
    {
        const float hi = lo * 1.15f + 2.f;
        return std::min(hi, 255.f);
    }

    geomatch::Config toConfig() const
    {
        geomatch::Config c;
        c.angleStart   = angleStartDeg  * (float)CV_PI / 180.f;
        c.angleExtent  = angleExtentDeg * (float)CV_PI / 180.f;
        c.angleStep    = angleStepDeg   * (float)CV_PI / 180.f;
        c.scaleMin     = std::min(scaleMin, scaleMax);
        c.scaleMax     = std::max(scaleMin, scaleMax);
        // 灰度差 -> Sobel 幅值(库内部单位)
        c.minContrast  = minContrast * kSobelPerGray;
        c.contrast     = contrast    * kSobelPerGray;
        c.maxPoints    = maxPoints;
        c.numLevels    = numLevels;
        c.mask         = mask;
        c.minScore     = minScore;
        c.maxMatches   = maxMatches;
        c.minDistance  = minDistance;
        c.greediness   = greediness;
        c.subpixel     = subpixel ? 1 : 0;
        c.useOpenCL    = useOpenCL;
        c.debug        = 0;
        c.scoreMode    = scoreMode;
        c.minCoverage  = minCoverage;
        c.boxMode      = boxMode;
        return c;
    }

    void syncFromConfig(const geomatch::Config& c)
    {
        angleStartDeg  = (float)(c.angleStart  * 180.0 / CV_PI);
        angleExtentDeg = (float)(c.angleExtent * 180.0 / CV_PI);
        angleStepDeg   = (float)(c.angleStep   * 180.0 / CV_PI);
        scaleMin    = (float)c.scaleMin;
        scaleMax    = (float)c.scaleMax;
        // Sobel 幅值 -> 灰度差(界面单位)
        minContrast = (float)(c.minContrast / kSobelPerGray);
        contrast    = (float)(c.contrast    / kSobelPerGray);
        maxPoints   = c.maxPoints;
        numLevels   = c.numLevels;
        minScore    = (float)c.minScore;
        maxMatches  = c.maxMatches;
        minDistance = (float)c.minDistance;
        greediness  = (float)c.greediness;
        subpixel    = c.subpixel != 0;
        useOpenCL   = c.useOpenCL;
        scoreMode   = c.scoreMode;
        minCoverage = c.minCoverage;
        // boxMode 只影响画出来的样子，不写进模型文件；加载模型时保留界面上
        // 当前的选择，不要被 Matcher::load() 重置出来的默认值覆盖。
    }
};

static App g_app;

//=======================================================================
//  模板掩码可视化: 把掩码排除区(mask==0)以暗红色叠加到模板源图上
//=======================================================================
static cv::Mat overlayMaskOnTpl(const cv::Mat& gray, const cv::Mat& mask)
{
    cv::Mat vis;
    cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    if (mask.empty())
        return vis;

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
            if (mrow[x] == 0)   // 被掩码排除的区域 -> 压暗并偏紫红
                row[x] = cv::Vec3b(
                    (uchar)(row[x][0] * 0.35f + 45),
                    (uchar)(row[x][1] * 0.35f),
                    (uchar)(row[x][2] * 0.35f + 45));
        }
    }
    return vis;
}

// 刷新模板源图视图(叠加 mask 覆盖层)
static void refreshTplView(App& a)
{
    if (a.tplGray.empty()) { a.texTplSrc.releaseDeferred(); return; }
    a.texTplSrc.set(toBGRA(overlayMaskOnTpl(a.tplGray, a.mask)));
}

// 确保掩码与模板源图同尺寸(默认整幅参与建模)
static void ensureMask(App& a, int w, int h)
{
    if (a.mask.empty() || a.mask.cols != w || a.mask.rows != h ||
        a.mask.type() != CV_8UC1)
    {
        a.mask = cv::Mat(h, w, CV_8U, cv::Scalar(255));
        a.maskPath.clear();   // 手绘掩码没有文件来源
    }
}

//=======================================================================
//  业务动作
//=======================================================================
// 取创建模板要用的那块图: 模板源图优先(ROI owner=2)，没有就用目标图(ROI owner=1)
static cv::Mat templatePatch(const App& a)
{
    const cv::Mat* src = nullptr;

    if (!a.tplGray.empty())      { src = &a.tplGray; }
    else if (!a.tgtGray.empty()) { src = &a.tgtGray; }
    if (!src) return cv::Mat();

    const int want = src == &a.tplGray ? 2 : 1;
    if (a.roiHas && a.roiOwner == want)
    {
        const cv::Rect r = a.roi & cv::Rect(0, 0, src->cols, src->rows);
        if (r.width > 4 && r.height > 4) return (*src)(r);
    }
    return *src;
}

static void createTemplate(App& a)
{
    cv::Mat patch = templatePatch(a);
    if (patch.empty())
    {
        logLine("没有可用于创建模板的图像");
        return;
    }

    const int64 t0 = cv::getTickCount();
    const bool ok = a.matcher.create(patch, a.toConfig());
    const double ms = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();

    if (!ok)
    {
        // 失败时保留上一个模型，免得调参试错时把好模型弄丢
        logf("创建模板失败 (%dx%d)，试试降低最小对比度", patch.cols, patch.rows);
        return;
    }

    a.modelReady = true;
    a.buildMs    = ms;

    a.modelVis = a.matcher.drawModelFeatures(2.0);
    a.texModel.set(toBGRA(a.modelVis));

    logf("模板已创建: %dx%d  点数=%d  层数=%d  用时 %.1f ms",
         patch.cols, patch.rows, a.matcher.modelPointCount(),
         a.matcher.numLevels(), ms);

    // 阈值到底有没有生效：候选像素远多于点数时，实际是"最大点数"在截断，
    // 此时拖动最小对比度不会改变点数(只是换了抽样对象)，很容易让人以为
    // 滑块坏了。候选像素更少时才是阈值在起作用。
    const int cand = a.matcher.candidatePixels();
    const int np   = a.matcher.modelPointCount();
    if (cand > 0)
    {
        if (cand > np * 2)
            logf("提示: 过阈值像素 %d 个 > 点数 %d 的两倍，当前是「最大点数」在截断，"
                 "调最小对比度只会改变取点位置", cand, np);
        else
            logf("过阈值像素 %d 个，点数 %d（阈值在起作用）", cand, np);
    }
    const float sm = a.matcher.strongestMag(), wm = a.matcher.weakestMag();
    if (sm > 1e-6f && wm / sm < 0.5f)
        logf("提示: 最弱点的梯度只有最强点的 %.0f%%，模糊/噪声会先打掉它们，"
             "可适当减小「最大点数」", 100.0f * wm / sm);
}

static void runMatch(App& a, int loops)
{
    if (!a.modelReady)
    {
        logLine("还没有模板模型，先创建或加载一个");
        return;
    }
    if (a.tgtGray.empty())
    {
        logLine("还没有匹配目标图片");
        return;
    }

    a.matcher.setConfig(a.toConfig());

    const int loopsUsed = std::max(1, loops);

    geomatch::Timing tm;
    const int64 t0 = cv::getTickCount();
    std::vector<geomatch::Detection> dets =
        loopsUsed > 1 ? a.matcher.detectTimed(a.tgtGray, loopsUsed, &tm)
                      : a.matcher.detect(a.tgtGray, &tm);
    const double wallMs = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();

    a.dets   = dets;
    a.timing = tm;

    // 搜索结果区要显示的耗时: detectTimed 只保留最后一轮，累计值按 每轮 × 轮数 折算，
    // 另外记一份真实墙钟耗时(含预处理/循环开销)，重复计时时用来看总账。
    a.searchLoops   = loopsUsed;
    a.searchTotalMs = tm.searchMs * loopsUsed;
    a.benchWallMs   = wallMs;

    const int64 tDraw = cv::getTickCount();
    a.resultVis = a.matcher.drawResult(a.tgtGray, dets);
    const double drawMs = (cv::getTickCount() - tDraw) * 1000.0 / cv::getTickFrequency();
    a.texResult.set(toBGRA(a.resultVis));

    logf("匹配: %d 个结果  预处理 %.2f ms  搜索 %.2f ms  搜索总耗时 %.2f ms (%d 次)  合计 %.2f ms  绘图 %.1f ms",
         (int)dets.size(), tm.preprocessMs, tm.searchMs, a.searchTotalMs, loopsUsed,
         tm.recognitionMs(), drawMs);
    if (loopsUsed > 1)
        logf("  重复 %d 次: 墙钟总耗时 %.1f ms  平均 %.2f ms/次",
             loopsUsed, wallMs, wallMs / loopsUsed);
}

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

    if (a.autoRun)          // 换图后直接把模型和结果刷出来
    {
        createTemplate(a);
        if (!a.tgtGray.empty()) runMatch(a, 1);
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

    // 如果这张图在列表里，把选中框跟上
    a.thumbSel = -1;
    for (size_t i = 0; i < a.thumbs.size(); i++)
        if (a.thumbs[i].path == utf8) { a.thumbSel = (int)i; break; }

    if (a.autoRun && a.modelReady) runMatch(a, 1);
}

// 切换到列表里的第 idx 张(支持负数/越界回绕)
static void selectThumb(App& a, int idx)
{
    if (a.thumbs.empty()) return;
    const int n = (int)a.thumbs.size();
    int i = idx % n;
    if (i < 0) i += n;
    a.thumbSel = i;
    loadTargetImage(a, a.thumbs[i].wpath);
}

// 读一个目录: 列出全部图片 + 生成缩略图，然后自动选中第一张
static void loadTargetDir(App& a, const std::wstring& dir)
{
    for (Thumb& t : a.thumbs) t.tex.releaseDeferred();
    a.thumbs.clear();
    a.thumbSel = -1;
    a.dirPath  = ws2s(dir);

    const int64 t0 = cv::getTickCount();
    const std::vector<std::wstring> files = listDirImages(dir);
    if (files.empty())
    {
        logf("目录里没有图片: %s", a.dirPath.c_str());
        return;
    }

    const int maxThumbs = 200;                       // 再多就没必要了
    const int thumbH    = (int)(96 * g_scale);
    int bad = 0;

    for (size_t i = 0; i < files.size() && (int)a.thumbs.size() < maxThumbs; i++)
    {
        std::ifstream f(files[i], std::ios::binary);
        if (!f) { bad++; continue; }
        std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) { bad++; continue; }

        cv::Mat buf(1, (int)data.size(), CV_8U, data.data());
        // REDUCED_GRAYSCALE_4: 只解 1/4 尺寸，列几十张也不会卡
        cv::Mat small = cv::imdecode(buf, cv::IMREAD_REDUCED_GRAYSCALE_4);
        if (small.empty()) small = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
        if (small.empty()) { bad++; continue; }

        int tw = (int)((float)small.cols / (float)small.rows * (float)thumbH);
        tw = std::max(16, std::min(tw, thumbH * 3));
        cv::Mat thumb;
        cv::resize(small, thumb, cv::Size(tw, thumbH), 0, 0, cv::INTER_AREA);

        Thumb t;
        t.wpath = files[i];
        t.path  = ws2s(files[i]);
        const size_t s = t.path.find_last_of("\\/");
        t.name = (s == std::string::npos) ? t.path : t.path.substr(s + 1);
        t.tex.set(toBGRA(thumb));
        a.thumbs.push_back(std::move(t));
    }

    const double ms = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    logf("目录: %s  %d 张图片 (缩略图 %.0f ms%s)",
         a.dirPath.c_str(), (int)a.thumbs.size(), ms,
         (int)files.size() > (int)a.thumbs.size() ? "，已截断到 200 张" : "");
    if (bad) logf("其中 %d 个文件读不出来", bad);

    if (a.autoRun && !a.thumbs.empty()) selectThumb(a, 0);   // 选中第一张(有模型就顺带匹配)
}

//=======================================================================
//  左侧: 图像视图
//=======================================================================
static ImVec2 fitSize(int w, int h, const ImVec2& avail, float zoom)
{
    if (w <= 0 || h <= 0 || avail.x <= 0 || avail.y <= 0) return ImVec2(0, 0);
    float s = std::min(avail.x / (float)w, avail.y / (float)h) * zoom;
    return ImVec2((float)w * s, (float)h * s);
}

// roiOwner: 0 = 不允许框选，1 = 目标图，2 = 模板源图
// maskPaint: 为 true 时在模板源图上支持涂抹掩码(左键=参与/右键=排除)
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

        // 松手 -> 触发一次模板重建
        if (a.maskPainting && !anyDown)
        {
            a.maskPainting = false;
            a.maskDirty = true;
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
                    a.roiDirty = true;      // 让右侧面板重建模板
                }
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (a.dragging)
        {
            ImVec2 p0 = toScr(ImVec2(std::min(a.dragA.x, a.dragB.x), std::min(a.dragA.y, a.dragB.y)));
            ImVec2 p1 = toScr(ImVec2(std::max(a.dragA.x, a.dragB.x), std::max(a.dragA.y, a.dragB.y)));
            dl->AddRect(p0, p1, IM_COL32(0, 255, 0, 255), 0.f, 0, 2.f);
        }
        else if (a.roiHas && a.roiOwner == roiOwner)
        {
            ImVec2 p0 = toScr(ImVec2((float)a.roi.x, (float)a.roi.y));
            ImVec2 p1 = toScr(ImVec2((float)(a.roi.x + a.roi.width), (float)(a.roi.y + a.roi.height)));
            dl->AddRect(p0, p1, IM_COL32(0, 255, 0, 255), 0.f, 0, 2.f);
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
        if (ImGui::BeginTabItem("匹配结果"))
        {
            snprintf(info, sizeof(info), "匹配结果  %dx%d  命中 %d",
                     a.texResult.w, a.texResult.h, (int)a.dets.size());
            drawImageView("##area_result", info, a.texResult, a.texResult.valid() ? 1 : 0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("目标图"))
        {
            snprintf(info, sizeof(info), "目标图  %dx%d", a.texTarget.w, a.texTarget.h);
            drawImageView("##area_target", info, a.texTarget, a.texTarget.valid() ? 1 : 0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模板源图"))
        {
            snprintf(info, sizeof(info), "模板源图  %dx%d", a.texTplSrc.w, a.texTplSrc.h);
            drawImageView("##area_tpl", info, a.texTplSrc, a.texTplSrc.valid() ? 2 : 0, a.paintMask);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模板模型"))
        {
            snprintf(info, sizeof(info), "模板模型(绿=轮廓 红=采样点 蓝=中心)  %dx%d",
                     a.texModel.w, a.texModel.h);
            drawImageView("##area_model", info, a.texModel, 0);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

//=======================================================================
//  左侧下方: 目标图片预览列表(点一张就匹配一张)
//=======================================================================
static void drawThumbBar()
{
    App& a = g_app;

    if (a.thumbs.empty())
    {
        ImGui::TextDisabled("图片列表为空 - 点右侧 \"选择图片目录...\" 载入一个目录");
        return;
    }

    ImGui::Text("%d 张，点缩略图切换 (%d/%d)",
                (int)a.thumbs.size(), a.thumbSel + 1, (int)a.thumbs.size());

    for (int i = 0; i < (int)a.thumbs.size(); i++)
    {
        const Thumb& t = a.thumbs[i];
        if (!t.tex.valid()) continue;

        if (i != 0) ImGui::SameLine();

        ImGui::PushID(i);
        const bool sel = (i == a.thumbSel);
        const ImVec4 bg = sel ? ImVec4(0.20f, 0.60f, 0.28f, 1.00f) : ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        if (ImGui::ImageButton("##thumb",
                               ImTextureRef((ImTextureID)(uintptr_t)t.tex.srv),
                               ImVec2((float)t.tex.w, (float)t.tex.h),
                               ImVec2(0, 0), ImVec2(1, 1), bg))
        {
            selectThumb(a, i);
        }
        ImGui::SetItemTooltip("%s", t.name.c_str());
        ImGui::PopID();
    }
}

//=======================================================================
//  右侧: 操作面板
//=======================================================================
// 控件后面还跟着标签时，把控件宽度收窄到"可用宽度 - 标签宽度"，
// 这样窗口/面板再窄，右侧标签也不会被右边界裁掉。
static void itemWidthForLabel(const char* label)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float labelW = ImGui::CalcTextSize(label, nullptr, true).x;
    float w = ImGui::GetContentRegionAvail().x - labelW - st.ItemInnerSpacing.x;
    const float wMin = 60.f * g_scale;
    if (w < wMin) w = wMin;                 // 再窄也留一条能拖动的最小宽度
    ImGui::SetNextItemWidth(w);
}

// 同一行剩余宽度够不够放下下一个控件(不够就不 SameLine，直接换行，免得被右边裁掉)
static bool fitsOnLine(float needW)
{
    return ImGui::GetContentRegionAvail().x >= needW;
}

static float checkWidth(const char* label)      // Checkbox 占宽 = 方框 + 间距 + 文字
{
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
           ImGui::CalcTextSize(label, nullptr, true).x;
}

static float buttonWidth(const char* label)     // Button 占宽 = 文字 + 两侧内边距
{
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.f;
}

// 暗色 + 自动换行，长提示不会被右边界裁掉
static void textDimWrapped(const char* s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

static bool sliderF(const char* label, float* v, float lo, float hi, const char* fmt, bool& commit)
{
    itemWidthForLabel(label);
    const bool c = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;   // 拖动结束后才触发重跑
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

    // 这两组标志要跨"折叠分组"使用，所以提到函数顶部声明
    bool changedModel = false, commitModel = false;
    bool changedSearch = false, commitSearch = false;

    // 分组折叠头: 展开时缩进内容。endGroup 只在 if 成立(已展开)的分支里调用，
    // 所以它无条件取消缩进是对的。
    auto group = [](const char* id, bool defaultOpen) -> bool
    {
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (defaultOpen) fl |= ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::CollapsingHeader(id, fl);
        if (open) ImGui::Indent(10.f * g_scale);
        return open;
    };
    auto endGroup = []() { ImGui::Unindent(10.f * g_scale); };

    //---------------- 1. 模板 ----------------
    ImGui::SeparatorText("1. 模板图像");

    if (ImGui::Button("选择模板图片...", ImVec2(-1, 0)))
    {
        std::wstring p = pickImageFile(L"选择模板图片");
        if (!p.empty()) loadTemplateImage(a, p);
    }
    ImGui::TextWrapped("模板源: %s", a.tplPath.empty() ? "(未选择，默认用整幅目标图)" : a.tplPath.c_str());

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
                a.tplGray = a.tgtGray(r).clone();
                a.tplPath = a.tgtPath + "  [ROI]";
                refreshTplView(a);
                logf("已截取模板: (%d,%d) %dx%d", r.x, r.y, r.width, r.height);
                a.roiOwner = 0;                 // 截出来的是新图，ROI 不再是目标图上的框
                a.roiHas   = false;
                a.roiDirty = true;              // 立刻用它建模板
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
            a.roiDirty = true;      // 回到整幅图，重建一次
        }
    }
    else
    {
        textDimWrapped("ROI: 整幅图 (在左侧图上拖动可框选)");
    }

    //---- 模板参数(分组折叠，避免一屏塞满) ----
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

    if (group("模板参数: 特征提取", false))
    {
        // 对比度用"灰度差"显示(和 Halcon 的 Contrast 可比)，上限到 255 = 满量程，
        // 拉到底时模型点会全部消失，等价于 Halcon 把 Contrast 拉到 255 的效果。
        changedModel |= sliderF("最小对比度(灰度差)", &a.minContrast, 0.f, 255.f, "%.1f", commitModel);

        // 高阈值(滞后种子)跟随低阈值: 保证两者始终有分离度，这样拖低阈值就是
        // 连续的"沿弱边腐蚀"，而不是忽有忽无。
        if (a.autoHi)
        {
            const float want = App::hiFor(a.minContrast);
            if (std::fabs(want - a.contrast) > 1e-4f) { a.contrast = want; changedModel = true; }
        }
        if (a.autoHi) ImGui::BeginDisabled();
        changedModel |= sliderF("高阈值(灰度差)", &a.contrast, 0.f, 255.f, "%.1f", commitModel);
        if (a.autoHi) ImGui::EndDisabled();
        if (ImGui::Checkbox("高阈值自动跟随 (推荐)", &a.autoHi))
        {
            changedModel = true;
            commitModel  = true;
        }

        changedModel |= sliderI("最大点数", &a.maxPoints, 100, 4000, commitModel);
        changedModel |= sliderI("金字塔层数(0=自动)", &a.numLevels, 0, 8, commitModel);
        textDimWrapped("低阈值决定\"哪些边弱到可以放弃\"，高阈值是滞后起点，两者之间相连的弱边会被保住。"
                       "低阈值拉大 = 从弱边开始逐圈啃掉特征(类似腐蚀)；拉到 255 特征全消失。");
        endGroup();
    }

    // 参数调完再建模板(点击即重建，并按需自动重匹配)
    if (ImGui::Button("创建模板 / 应用参数", ImVec2(-1, 0))) needCreate = true;

    if (a.modelReady)
    {
        ImGui::Text("模型: %d 个点 / %d 层 / 建模板 %.1f ms",
                    a.matcher.modelPointCount(), a.matcher.numLevels(), a.buildMs);
    }
    else
    {
        ImGui::TextDisabled("模型: 未创建");
    }

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
            // 开启时立即初始化掩码(默认全参与)并刷新视图
            if (a.paintMask && !a.tplGray.empty())
            {
                ensureMask(a, a.tplGray.cols, a.tplGray.rows);
                refreshTplView(a);
            }
        }
        if (a.paintMask)
        {
            itemWidthForLabel("画笔半径(px)");
            ImGui::SliderFloat("画笔半径(px)", &a.brushRadius, 1.f, 30.f, "%.0f");
            ImGui::TextWrapped("在左侧\"模板源图\"标签页: 左键涂抹=排除(红), 右键涂抹=恢复参与(绿)");
        }
        endGroup();
    }

    //---- 模型文件 ----
    if (group("模型文件 (保存 / 加载)", false))
    {
        static char modelPath[260] = "case1/model.shm";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##modelpath", modelPath, sizeof(modelPath));
        if (ImGui::Button("保存模型", ImVec2(-120, 0)))
        {
            if (!a.modelReady) logLine("没有模型可保存");
            else if (a.matcher.save(modelPath)) logf("模型已保存: %s", modelPath);
            else logf("保存失败: %s", modelPath);
        }
        ImGui::SameLine();
        if (ImGui::Button("加载模型", ImVec2(-1, 0)))
        {
            if (a.matcher.load(modelPath))
            {
                a.modelReady = true;
                a.buildMs    = 0;
                a.syncFromConfig(a.matcher.config());
                a.modelVis = a.matcher.drawModelFeatures(2.0);
                a.texModel.set(toBGRA(a.modelVis));
                logf("模型已加载: %s  (%d 点 / %d 层)", modelPath,
                     a.matcher.modelPointCount(), a.matcher.numLevels());
            }
            else logf("加载失败: %s", modelPath);
        }
        endGroup();
    }

    //---------------- 2. 匹配目标 ----------------
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

    //---------------- 3. 搜索参数 ----------------
    ImGui::SeparatorText("3. 搜索参数");

    //---- 匹配判据: 最常调，默认展开 ----
    if (group("匹配判据 (分数 / 覆盖率)", true))
    {
        changedSearch |= sliderF("最小分数", &a.minScore, 0.05f, 1.f, "%.2f", commitSearch);
        {
            const char* items[] = { "mean(原始)",
                                    "positive-mean(方向一致点比例)",
                                    "trimmed-mean(去尾10%)",
                                    "clipped-mean(正向但不缩分母)" };
            itemWidthForLabel("评分方式");
            if (ImGui::Combo("评分方式", &a.scoreMode, items, 4))
            {
                changedSearch = true;
                commitSearch  = true;
            }
        }
        changedSearch |= sliderF("最小覆盖率", &a.minCoverage, 0.f, 1.f, "%.2f", commitSearch);
        if (a.minCoverage > 0.f)
            textDimWrapped("0 = 关闭。覆盖率 = 模型点里梯度方向一致的占比，结果表\"覆盖\"列就是它。"
                           "调低最小分数后冒出来的相似结构通常能拿高分，但只对上模型的一部分，覆盖率明显偏低。"
                           "实测参考: case1 真值 0.99 / 误检 0.62(取 0.80)；case3 真值 0.81 / 误检 0.75(取 0.78)。");
        endGroup();
    }

    //---- 结果筛选 ----
    if (group("结果筛选 (实例数 / 去重 / 框样式)", true))
    {
        changedSearch |= sliderI("最大匹配数", &a.maxMatches, 1, 64, commitSearch);
        changedSearch |= sliderF("最小间距(px)", &a.minDistance, 0.f, 300.f, "%.0f", commitSearch);
        {
            // 结果框样式只影响画出来的样子，不影响匹配结果
            const char* items[] = { "固定方框(水平，尺寸=模板x缩放)",
                                    "旋转框(跟角度倾斜)",
                                    "模型凸包(旧版不规则多边形)" };
            itemWidthForLabel("结果框样式");
            if (ImGui::Combo("结果框样式", &a.boxMode, items, 3))
            {
                changedSearch = true;
                commitSearch  = true;
            }
        }
        textDimWrapped("多实例场景把最大匹配数调大；最小间距用来抑制相邻周期的重复检测。"
                       "固定方框尺寸恒定、不随角度摇摆，多帧之间位置更好对比。");
        endGroup();
    }

    //---- 性能与加速 ----
    if (group("性能与加速", false))
    {
        changedSearch |= sliderF("贪婪度", &a.greediness, 0.3f, 1.f, "%.2f", commitSearch);
        textDimWrapped("越大越快，但可能漏检。");
        if (ImGui::Checkbox("亚像素", &a.subpixel)) { changedSearch = true; commitSearch = true; }
        if (fitsOnLine(checkWidth("OpenCL"))) ImGui::SameLine();
        if (ImGui::Checkbox("OpenCL", &a.useOpenCL))
        {
            changedSearch = true;
            commitSearch  = true;
            if (!geomatch::enableOpenCL(a.useOpenCL)) logLine("OpenCL 不可用，已回退到 CPU");
        }
        endGroup();
    }

    //---- 界面选项 ----
    if (group("界面选项", false))
    {
        ImGui::Checkbox("参数改动后自动刷新", &a.autoRun);
        ImGui::Checkbox("拖动时实时刷新", &a.liveUpdate);
        itemWidthForLabel("显示缩放");
        ImGui::SliderFloat("显示缩放", &a.zoom, 0.2f, 4.f, "%.2f");
        endGroup();
    }

    //---------------- 4. 执行 ----------------
    ImGui::SeparatorText("4. 执行匹配");

    if (ImGui::Button("执行匹配", ImVec2(-1, 0))) needMatch = true;
    if (ImGui::Button("重复 20 次计时", ImVec2(-1, 0))) runMatch(a, 20);

    // 参数一动就把模型和结果刷出来:
    //   松手/键盘提交 -> 一定刷新
    //   拖动过程中   -> 开了实时刷新就每帧刷; 上一轮太慢(>150ms)就等松手再刷, 免得卡住
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
        const int64 t0 = cv::getTickCount();
        if (needCreate)
        {
            createTemplate(a);
            if (a.autoRun && !a.tgtGray.empty()) runMatch(a, 1);
        }
        else
        {
            runMatch(a, 1);
        }
        a.lastOpMs = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    }

    //---- 结果区耗时: 每轮明细 + 搜索总耗时(重复计时时是 N 次的累计) ----
    const int nLoops = a.searchLoops > 0 ? a.searchLoops : 1;
    ImGui::TextWrapped("耗时: 预处理 %.2f ms | 搜索 %.2f ms | 合计 %.2f ms | 刷新 %.1f ms",
                       a.timing.preprocessMs, a.timing.searchMs,
                       a.timing.recognitionMs(), a.lastOpMs);
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
            const geomatch::Detection& d = a.dets[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", (int)i);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", d.score);
            ImGui::TableSetColumnIndex(2);
            if (d.coverage > 0.f) ImGui::Text("%.2f", d.coverage);
            else                  ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f, %.0f", d.pos.x, d.pos.y);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", d.angle * 180.0f / (float)CV_PI);
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
            const std::string out = dir + "/gui_result.png";
            if (cv::imwrite(out, a.resultVis)) logf("结果图已保存: %s", out.c_str());
            else logf("保存失败: %s", out.c_str());
        }
    }

    //---------------- 日志 ----------------
    ImGui::SeparatorText("日志");
    // 填满面板剩下的高度；面板内容超高时可滚动，日志最少保留 100px
    const float logH = std::max(100.f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##log", ImVec2(-1, logH), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.0f);          // 长行自动折行，避免被面板右边界裁掉
    for (size_t i = 0; i < g_log.size(); i++)
        ImGui::TextUnformatted(g_log[i].c_str());
    ImGui::PopTextWrapPos();
    if (g_logDirty)
    {
        ImGui::SetScrollHereY(1.0f);
        g_logDirty = false;
    }
    ImGui::EndChild();
}

//=======================================================================
//  主循环里的整屏窗口
//=======================================================================
static void drawUI()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("GeoMatch", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // 右侧参数面板宽度。参数较多，面板给宽一点，滑块才能拉得动；
    // 窗口变窄时按 panelMinW 收缩，保证整个面板仍在窗口内。
    const float panelW0   = 560.f * g_scale;    // 面板理想宽度
    const float panelMinW = 420.f * g_scale;    // 面板允许的最窄宽度
    const float minLeftW  = 260.f * g_scale;    // 左侧视图至少留这么宽
    const float gap       = ImGui::GetStyle().ItemSpacing.x;
    const float thumbH    = 150.f * g_scale;    // 底部图片预览列表的高度

    // 窗口变窄时收缩右侧面板，保证面板整个落在窗口内(否则标签/面板右边框会被窗口右边界裁掉)
    const float availW = ImGui::GetContentRegionAvail().x;
    float panelW = std::min(panelW0, std::max(panelMinW, availW - minLeftW - gap));

    //---------------- 左列: 上=图像视图, 下=图片列表 ----------------
    ImGui::BeginChild("##left", ImVec2(-panelW - gap, -1), ImGuiChildFlags_Borders);
    {
        ImGui::BeginChild("##view", ImVec2(-1, -thumbH - gap), ImGuiChildFlags_Borders);
        drawViewPane();
        ImGui::EndChild();

        // 图片预览列表: 横向排开，可左右滚动
        ImGui::BeginChild("##thumbs", ImVec2(-1, thumbH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        drawThumbBar();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    //---------------- 右列: 操作面板 ----------------
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
    const std::string tpl = dir + "/temp.jpg";
    if (!loadGrayImage(s2ws(tpl), a.tplGray))
    {
        logf("没有默认模板 %s，请在右侧手动选择模板图片", tpl.c_str());
        return;
    }

    a.tplPath = tpl;
    refreshTplView(a);
    logf("模板源图: %s (%dx%d)", tpl.c_str(), a.tplGray.cols, a.tplGray.rows);

    createTemplate(a);

    // 把整个目录读进来做成预览列表，并自动选中第一张跑一次
    loadTargetDir(a, s2ws(dir));
    if (a.tgtGray.empty())
    {
        logf("%s 里没有可用于匹配的图片", dir.c_str());
        return;
    }
    // 第一张如果正好是模板本身，直接跳到下一张
    const std::string tplName = tpl.substr(tpl.find_last_of("\\/") + 1);
    if (a.thumbSel == 0 && a.thumbs.size() > 1 && a.thumbs[0].name == tplName)
        selectThumb(a, 1);
}

//=======================================================================
//  main
//=======================================================================
int main(int argc, char** argv)
{
    const std::string dir = (argc > 1) ? argv[1] : "case1";

    //---------------- 窗口 + D3D11 ----------------
    ImGui_ImplWin32_EnableDpiAwareness();
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"GeoGUIDemo", nullptr };
    ::RegisterClassExW(&wc);
    g_hwnd = ::CreateWindowW(wc.lpszClassName, L"GeoMatch 模板匹配验证工具",
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
    if (!font) font = io.Fonts->AddFontDefault();   // 实在没有就退回默认(中文会缺字)
    io.FontDefault = font;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    //---------------- 匹配环境 ----------------
    if (!geomatch::enableOpenCL(g_app.useOpenCL))
        logLine("OpenCL 不可用，使用 CPU");
    logf("%s", geomatch::environmentReport().c_str());
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

        drawUI();

        ImGui::Render();
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
        // 窗口再小右侧面板就会被右边界裁掉(标签显示不全)，这里限制一个最小尺寸。
        // 最小宽度跟着面板走: panelMinW(420) + minLeftW(260) + 间距和边框余量。
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
