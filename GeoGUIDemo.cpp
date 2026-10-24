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
//                    4. 执行匹配 + 结果列表 + 耗时 + 日志
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

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

    bool valid() const { return srv != nullptr; }

    void release()
    {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        w = h = 0;
    }

    // bgra 必须是 CV_8UC4
    bool set(const cv::Mat& bgra)
    {
        if (bgra.empty() || bgra.type() != CV_8UC4) { release(); return false; }

        const int iw = bgra.cols, ih = bgra.rows;
        // D3D11 的行距按 16 字节对齐最稳妥
        const int pitch = ((iw * 4 + 15) / 16) * 16;
        std::vector<unsigned char> buf((size_t)pitch * (size_t)ih, 0);
        for (int y = 0; y < ih; y++)
            memcpy(&buf[(size_t)y * pitch], bgra.ptr(y), (size_t)iw * 4);

        if (tex && (w != iw || h != ih)) release();

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

//=======================================================================
//  应用状态
//=======================================================================
struct App
{
    //---- 界面上的参数(度/百分比等单位) ----
    float angleStartDeg  = -180.f;
    float angleExtentDeg = 360.f;
    float scaleMin       = 1.f;
    float scaleMax       = 1.f;
    float minContrast    = 12.f;
    int   maxPoints      = 1200;
    int   numLevels      = 0;          // 0 = 自动
    float minScore       = 0.55f;
    int   maxMatches     = 12;
    float minDistance    = 30.f;
    float greediness     = 0.9f;
    bool  subpixel       = true;
    bool  useOpenCL      = true;
    bool  autoRun        = true;       // 参数改完自动重建模板 + 重匹配
    bool  liveUpdate     = true;       // 拖动滑块的过程中就实时刷新
    float zoom           = 1.0f;       // 1 = 适应窗口
    bool  roiDirty       = false;      // ROI 刚改完，需要重建模板
    double lastOpMs      = 0;          // 上一次"重建 + 匹配"的总耗时，用来自动降频

    //---- 数据 ----
    geomatch::Matcher matcher;
    bool    modelReady = false;
    double  buildMs    = 0;

    std::string tplPath, tgtPath;
    cv::Mat tplGray, tgtGray;
    cv::Mat modelVis, resultVis;

    Texture texTarget, texResult, texTplSrc, texModel;

    std::vector<geomatch::Detection> dets;
    geomatch::Timing timing;

    //---- ROI(图像坐标) ----
    cv::Rect roi;
    bool     roiHas   = false;
    int      roiOwner = 0;             // 1 = 目标图, 2 = 模板源图
    bool     dragging = false;
    ImVec2   dragA, dragB;

    geomatch::Config toConfig() const
    {
        geomatch::Config c;
        c.angleStart   = angleStartDeg  * (float)CV_PI / 180.f;
        c.angleExtent  = angleExtentDeg * (float)CV_PI / 180.f;
        c.scaleMin     = std::min(scaleMin, scaleMax);
        c.scaleMax     = std::max(scaleMin, scaleMax);
        c.minContrast  = minContrast;
        c.maxPoints    = maxPoints;
        c.numLevels    = numLevels;
        c.minScore     = minScore;
        c.maxMatches   = maxMatches;
        c.minDistance  = minDistance;
        c.greediness   = greediness;
        c.subpixel     = subpixel ? 1 : 0;
        c.useOpenCL    = useOpenCL;
        c.debug        = 0;
        return c;
    }

    void syncFromConfig(const geomatch::Config& c)
    {
        angleStartDeg  = (float)(c.angleStart  * 180.0 / CV_PI);
        angleExtentDeg = (float)(c.angleExtent * 180.0 / CV_PI);
        scaleMin    = (float)c.scaleMin;
        scaleMax    = (float)c.scaleMax;
        minContrast = (float)c.minContrast;
        maxPoints   = c.maxPoints;
        numLevels   = c.numLevels;
        minScore    = (float)c.minScore;
        maxMatches  = c.maxMatches;
        minDistance = (float)c.minDistance;
        greediness  = (float)c.greediness;
        subpixel    = c.subpixel != 0;
        useOpenCL   = c.useOpenCL;
    }
};

static App g_app;

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

    geomatch::Timing tm;
    std::vector<geomatch::Detection> dets =
        loops > 1 ? a.matcher.detectTimed(a.tgtGray, loops, &tm)
                  : a.matcher.detect(a.tgtGray, &tm);

    a.dets   = dets;
    a.timing = tm;

    const int64 t0 = cv::getTickCount();
    a.resultVis = a.matcher.drawResult(a.tgtGray, dets);
    const double drawMs = (cv::getTickCount() - t0) * 1000.0 / cv::getTickFrequency();
    a.texResult.set(toBGRA(a.resultVis));

    logf("匹配: %d 个结果  预处理 %.2f ms  搜索 %.2f ms  合计 %.2f ms  绘图 %.1f ms",
         (int)dets.size(), tm.preprocessMs, tm.searchMs, tm.recognitionMs(), drawMs);
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
    a.texTplSrc.set(toBGRA(g));
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
    a.tgtGray = g;
    a.tgtPath = ws2s(path);
    a.texTarget.set(toBGRA(g));
    logf("目标图: %s (%dx%d)", a.tgtPath.c_str(), g.cols, g.rows);

    if (a.autoRun && a.modelReady) runMatch(a, 1);
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
static void drawImageView(const char* id, const char* info, Texture& tex, int roiOwner)
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
            drawImageView("##area_tpl", info, a.texTplSrc, a.texTplSrc.valid() ? 2 : 0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("模板模型"))
        {
            snprintf(info, sizeof(info), "模板模型(特征点/梯度)  %dx%d",
                     a.texModel.w, a.texModel.h);
            drawImageView("##area_model", info, a.texModel, 0);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

//=======================================================================
//  右侧: 操作面板
//=======================================================================
static bool sliderF(const char* label, float* v, float lo, float hi, const char* fmt, bool& commit)
{
    const bool c = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;   // 拖动结束后才触发重跑
    return c;
}

static bool sliderI(const char* label, int* v, int lo, int hi, bool& commit)
{
    const bool c = ImGui::SliderInt(label, v, lo, hi);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
    return c;
}

static void drawPanel()
{
    App& a = g_app;
    bool needCreate = false, needMatch = false;

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
                a.texTplSrc.set(toBGRA(a.tplGray));
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
        ImGui::SameLine();
        if (ImGui::SmallButton("清除"))
        {
            a.roiHas   = false;
            a.roiOwner = 0;
            a.roiDirty = true;      // 回到整幅图，重建一次
        }
    }
    else
    {
        ImGui::TextDisabled("ROI: 整幅图 (在左侧图上拖动可框选)");
    }

    ImGui::SeparatorText("模板参数");
    bool changedModel = false, commitModel = false;
    changedModel |= sliderF("起始角(度)",  &a.angleStartDeg,  -180.f, 180.f, "%.0f", commitModel);
    changedModel |= sliderF("角度范围(度)", &a.angleExtentDeg,   0.f, 360.f, "%.0f", commitModel);
    changedModel |= sliderF("最小缩放", &a.scaleMin, 0.2f, 2.f, "%.2f", commitModel);
    changedModel |= sliderF("最大缩放", &a.scaleMax, 0.2f, 2.f, "%.2f", commitModel);
    if (a.scaleMax < a.scaleMin) a.scaleMax = a.scaleMin;
    changedModel |= sliderF("最小对比度", &a.minContrast, 0.f, 80.f, "%.0f", commitModel);
    changedModel |= sliderI("最大点数", &a.maxPoints, 100, 4000, commitModel);
    changedModel |= sliderI("金字塔层数(0=自动)", &a.numLevels, 0, 8, commitModel);

    if (ImGui::Button("创建模板", ImVec2(-1, 0))) needCreate = true;

    if (a.modelReady)
    {
        ImGui::Text("模型: %d 个点 / %d 层 / 建模板 %.1f ms",
                    a.matcher.modelPointCount(), a.matcher.numLevels(), a.buildMs);
    }
    else
    {
        ImGui::TextDisabled("模型: 未创建");
    }

    static char modelPath[260] = "case1/model.shm";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##modelpath", modelPath, sizeof(modelPath));
    if (ImGui::Button("保存模型", ImVec2(-1, 0)))
    {
        if (!a.modelReady) logLine("没有模型可保存");
        else if (a.matcher.save(modelPath)) logf("模型已保存: %s", modelPath);
        else logf("保存失败: %s", modelPath);
    }
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

    //---------------- 2. 匹配目标 ----------------
    ImGui::SeparatorText("2. 匹配目标");

    if (ImGui::Button("选择匹配目标...", ImVec2(-1, 0)))
    {
        std::wstring p = pickImageFile(L"选择匹配目标图片");
        if (!p.empty()) loadTargetImage(a, p);
    }
    ImGui::TextWrapped("目标: %s", a.tgtPath.empty() ? "(未选择)" : a.tgtPath.c_str());
    if (!a.tgtGray.empty())
        ImGui::Text("尺寸: %dx%d", a.tgtGray.cols, a.tgtGray.rows);

    //---------------- 3. 搜索参数 ----------------
    ImGui::SeparatorText("3. 搜索参数");
    bool changedSearch = false, commitSearch = false;
    changedSearch |= sliderF("最小分数", &a.minScore, 0.05f, 1.f, "%.2f", commitSearch);
    changedSearch |= sliderI("最大匹配数", &a.maxMatches, 1, 64, commitSearch);
    changedSearch |= sliderF("最小间距(px)", &a.minDistance, 0.f, 300.f, "%.0f", commitSearch);
    changedSearch |= sliderF("贪婪度", &a.greediness, 0.3f, 1.f, "%.2f", commitSearch);
    if (ImGui::Checkbox("亚像素", &a.subpixel)) { changedSearch = true; commitSearch = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("OpenCL", &a.useOpenCL))
    {
        changedSearch = true;
        commitSearch  = true;
        if (!geomatch::enableOpenCL(a.useOpenCL)) logLine("OpenCL 不可用，已回退到 CPU");
    }

    ImGui::Checkbox("参数改动后自动刷新", &a.autoRun);
    ImGui::SameLine();
    ImGui::Checkbox("拖动时实时刷新", &a.liveUpdate);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("显示缩放", &a.zoom, 0.2f, 4.f, "%.2f");

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
        if (a.roiDirty) { a.roiDirty = false; needCreate = true; }
    }
    else
    {
        a.roiDirty = false;
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

    ImGui::Text("耗时: 预处理 %.2f ms | 搜索 %.2f ms | 合计 %.2f ms | 刷新 %.1f ms",
                a.timing.preprocessMs, a.timing.searchMs, a.timing.recognitionMs(),
                a.lastOpMs);

    if (ImGui::BeginTable("##dets", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(-1, 160)))
    {
        ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 26);
        ImGui::TableSetupColumn("分数", ImGuiTableColumnFlags_WidthFixed, 52);
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
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f, %.0f", d.pos.x, d.pos.y);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f", d.angle * 180.0f / (float)CV_PI);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f", d.scale);
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
    ImGui::BeginChild("##log", ImVec2(-1, -1), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < g_log.size(); i++)
        ImGui::TextUnformatted(g_log[i].c_str());
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

    const float panelW = 390.f * g_scale;
    const float gap    = ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("##view", ImVec2(-panelW - gap, -1), ImGuiChildFlags_Borders);
    drawViewPane();
    ImGui::EndChild();

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
    a.texTplSrc.set(toBGRA(a.tplGray));
    logf("模板源图: %s (%dx%d)", tpl.c_str(), a.tplGray.cols, a.tplGray.rows);

    createTemplate(a);

    std::vector<std::string> fs = geomatch::listImages(dir);
    fs = geomatch::excludeFile(fs, tpl);
    for (size_t i = 0; i < fs.size(); i++)
    {
        if (!loadGrayImage(s2ws(fs[i]), a.tgtGray)) continue;
        a.tgtPath = fs[i];
        a.texTarget.set(toBGRA(a.tgtGray));
        logf("目标图: %s (%dx%d)", fs[i].c_str(), a.tgtGray.cols, a.tgtGray.rows);
        runMatch(a, 1);
        break;
    }
    if (a.tgtGray.empty()) logf("%s 里没有可用于匹配的图片", dir.c_str());
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
    }

    //---------------- 收尾 ----------------
    g_app.texTarget.release();
    g_app.texResult.release();
    g_app.texTplSrc.release();
    g_app.texModel.release();

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
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
