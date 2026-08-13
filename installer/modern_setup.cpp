#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <setupapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "resource.h"

namespace {
constexpr wchar_t kAppName[] = L"\u8f7b\u9605PDF";
constexpr wchar_t kExeName[] = L"\u8f7b\u9605PDF.exe";
constexpr wchar_t kPayloadExe[] = L"QuietPDF.Win32.exe";
constexpr wchar_t kPayloadUninstaller[] = L"QingYuePDF.Uninstaller.exe";
constexpr wchar_t kDllName[] = L"quietmupdf.dll";
constexpr wchar_t kRuntimeName[] = L"vcruntime140.dll";
constexpr wchar_t kUninstallName[] = L"\u5378\u8f7d\u8f7b\u9605PDF.exe";
constexpr wchar_t kWindowClass[] = L"QingYuePDFSetupWindow";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QingYuePDF";
constexpr wchar_t kInstallKey[] = L"Software\\QingYuePDF";
constexpr wchar_t kProgId[] = L"QingYuePDF.Document";
constexpr wchar_t kCapabilitiesKey[] = L"Software\\QingYuePDF\\Capabilities";
constexpr UINT kMsgProgress = WM_APP + 31;
constexpr UINT kMsgComplete = WM_APP + 32;
constexpr UINT_PTR kTimer = 1;
constexpr int kPathEdit = 1001;

enum class Page { Welcome, Options, Installing, Complete, Failed, UninstallConfirm, Uninstalling, UninstallComplete, UninstallFailed };
enum class Hot { None, Minimize, Close, Back, Primary, WelcomeCancel, OptionsCancel, Browse, Desktop, DeleteData, Launch };
struct InstallResult { bool success; std::wstring error; };
struct ProgressMessage { int percent; std::wstring text; };
struct CabinetContext { std::filesystem::path target; };
struct DirectoryCleanup {
    std::filesystem::path path;
    ~DirectoryCleanup() { std::error_code ec; if(!path.empty())std::filesystem::remove_all(path,ec); }
};

HINSTANCE g_instance{};
HWND g_window{}, g_pathEdit{};
HFONT g_uiFont{}, g_smallFont{}, g_titleFont{}, g_displayFont{};
ULONG_PTR g_gdiplusToken{};
UINT g_dpi = 96;
Page g_page = Page::Welcome;
Hot g_hover = Hot::None, g_pressed = Hot::None;
RECT g_minimize{}, g_close{}, g_back{}, g_primary{}, g_welcomePrimary{}, g_welcomeCancel{}, g_optionsCancel{}, g_browse{}, g_desktop{}, g_deleteData{}, g_launch{}, g_editFrame{};
bool g_desktopShortcut = true, g_launchAfterInstall = true, g_installing = false;
bool g_uninstallMode = false, g_uninstallWorker = false, g_deleteAppData = true;
bool g_pathFocused = false;
float g_progress = 0.0f, g_targetProgress = 0.0f;
int g_shine = 0;
std::wstring g_stage, g_error;
std::filesystem::path g_installedDir;
std::filesystem::path g_uninstallDir;

int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }
float Sf(float v) { return v * static_cast<float>(g_dpi) / 96.0f; }
bool Hit(RECT r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto f = [t](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + .5f); };
    return RGB(f(GetRValue(a), GetRValue(b)), f(GetGValue(a), GetGValue(b)), f(GetBValue(a), GetBValue(b)));
}
Gdiplus::Color Gc(COLORREF c, BYTE a = 255) { return {a, GetRValue(c), GetGValue(c), GetBValue(c)}; }
Gdiplus::RectF Rf(RECT r) { return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right-r.left), static_cast<float>(r.bottom-r.top)}; }
void AddRoundPath(Gdiplus::GraphicsPath& path, RECT r, float radius) {
    auto b = Rf(r); float d = std::min(radius * 2.0f, std::min(b.Width, b.Height));
    path.AddArc(b.X,b.Y,d,d,180,90); path.AddArc(b.GetRight()-d,b.Y,d,d,270,90);
    path.AddArc(b.GetRight()-d,b.GetBottom()-d,d,d,0,90); path.AddArc(b.X,b.GetBottom()-d,d,d,90,90); path.CloseFigure();
}
void FillRound(Gdiplus::Graphics& g, RECT r, float radius, COLORREF c, BYTE a = 255) {
    Gdiplus::GraphicsPath p(Gdiplus::FillModeAlternate); AddRoundPath(p,r,radius); Gdiplus::SolidBrush b(Gc(c,a)); g.FillPath(&b,&p);
}
void StrokeRound(Gdiplus::Graphics& g, RECT r, float radius, COLORREF c, BYTE a = 255) {
    Gdiplus::GraphicsPath p(Gdiplus::FillModeAlternate); AddRoundPath(p,r,radius); Gdiplus::Pen pen(Gc(c,a),std::max(1.0f,Sf(1.0f))); g.DrawPath(&pen,&p);
}
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value=nullptr; if (FAILED(SHGetKnownFolderPath(id,0,nullptr,&value))) return {};
    std::filesystem::path result(value); CoTaskMemFree(value); return result;
}
std::filesystem::path DefaultInstallDir() { return KnownFolder(FOLDERID_LocalAppData)/L"Programs"/kAppName; }
std::filesystem::path DesktopLink() { return KnownFolder(FOLDERID_Desktop)/(std::wstring(kAppName)+L".lnk"); }
std::filesystem::path StartFolder() { return KnownFolder(FOLDERID_StartMenu)/L"Programs"/kAppName; }
std::filesystem::path StartLink() { return StartFolder()/(std::wstring(kAppName)+L".lnk"); }
std::wstring WindowText(HWND h) {
    int n=GetWindowTextLengthW(h); std::wstring s(static_cast<size_t>(n+1),L'\0'); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s;
}
void MakeFonts() {
    for (HFONT* f:{&g_uiFont,&g_smallFont,&g_titleFont,&g_displayFont}) { if(*f) DeleteObject(*f); *f=nullptr; }
    auto make=[](int pt,int weight){ return CreateFontW(-MulDiv(pt,static_cast<int>(g_dpi),72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_NATURAL_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI"); };
    g_uiFont=make(10,FW_NORMAL); g_smallFont=make(8,FW_NORMAL); g_titleFont=make(15,FW_SEMIBOLD); g_displayFont=make(20,FW_SEMIBOLD);
    if(g_pathEdit) SendMessageW(g_pathEdit,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE);
}
void ApplyWindowChrome(HWND h) {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(h, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const BOOL darkMode = FALSE;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
}
void Layout(HWND h) {
    RECT c{}; GetClientRect(h,&c);
    constexpr int kWindowButtonSize = 30;
    constexpr int kWindowButtonGap = 6;
    g_close={c.right-S(14)-S(kWindowButtonSize),S(18),c.right-S(14),S(18)+S(kWindowButtonSize)};
    g_minimize={g_close.left-S(kWindowButtonGap)-S(kWindowButtonSize),g_close.top,g_close.left-S(kWindowButtonGap),g_close.bottom}; g_back={S(38),c.bottom-S(74),S(116),c.bottom-S(36)};
    g_primary={c.right-S(176),c.bottom-S(74),c.right-S(38),c.bottom-S(36)};
    g_welcomeCancel=g_primary; g_welcomePrimary={c.right-S(324),c.bottom-S(74),c.right-S(186),c.bottom-S(36)}; g_optionsCancel=g_primary;
    g_editFrame={S(40),S(225),c.right-S(136),S(265)}; g_browse={c.right-S(122),S(225),c.right-S(40),S(265)};
    g_desktop={S(40),S(286),S(350),S(320)}; g_deleteData={S(40),S(250),S(520),S(286)}; g_launch=g_desktop;
    if(g_pathEdit) {
        HDC dc=GetDC(g_pathEdit);
        HGDIOBJ oldFont=SelectObject(dc,g_uiFont);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc,&metrics);
        SelectObject(dc,oldFont);
        ReleaseDC(g_pathEdit,dc);

        const int width=g_editFrame.right-g_editFrame.left-S(24);
        const int frameHeight=g_editFrame.bottom-g_editFrame.top;
        const int textHeight=static_cast<int>(metrics.tmHeight);
        const int height=std::min(frameHeight-S(6),textHeight+S(6));
        const int top=g_editFrame.top+(frameHeight-height)/2+S(2);
        MoveWindow(g_pathEdit,g_editFrame.left+S(12),top,width,height,TRUE);
        SendMessageW(g_pathEdit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELONG(S(2),S(2)));
        ShowWindow(g_pathEdit,g_page==Page::Options?SW_SHOW:SW_HIDE);
    }
}
void DrawText(HDC dc,const std::wstring& s,RECT r,HFONT f,COLORREF c,UINT flags) {
    HGDIOBJ old=SelectObject(dc,f); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,c); DrawTextW(dc,s.c_str(),static_cast<int>(s.size()),&r,flags); SelectObject(dc,old);
}
void DrawText(Gdiplus::Graphics& g,const std::wstring& s,RECT r,HFONT f,COLORREF c,UINT flags) { HDC dc=g.GetHDC(); DrawText(dc,s,r,f,c,flags); g.ReleaseHDC(dc); }
void DrawLogo(Gdiplus::Graphics& g) {
    // The installer uses a wordmark; a checkmark is reserved for completion state.
}
void DrawClose(Gdiplus::Graphics& g) {
    const bool hover = g_hover==Hot::Close;
    const bool pressed = g_pressed==Hot::Close;
    if(hover||pressed) FillRound(g,g_close,Sf(6),pressed?RGB(226,239,232):RGB(239,247,242));

    const float cx = static_cast<float>(g_close.left+g_close.right)/2.0f;
    const float cy = static_cast<float>(g_close.top+g_close.bottom)/2.0f;
    Gdiplus::Pen p(Gc(pressed?RGB(29,99,68):(hover?RGB(36,119,82):RGB(59,73,65)),255),Sf(1.8f));
    p.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
    g.DrawLine(&p,cx-Sf(4.5f),cy-Sf(4.5f),cx+Sf(4.5f),cy+Sf(4.5f));
    g.DrawLine(&p,cx+Sf(4.5f),cy-Sf(4.5f),cx-Sf(4.5f),cy+Sf(4.5f));
}
void DrawMinimize(Gdiplus::Graphics& g) {
    const bool hover = g_hover==Hot::Minimize;
    const bool pressed = g_pressed==Hot::Minimize;
    if(hover||pressed) FillRound(g,g_minimize,Sf(6),pressed?RGB(226,239,232):RGB(239,247,242));
    const COLORREF ink = pressed?RGB(24,92,63):(hover?RGB(35,116,78):RGB(40,58,48));
    Gdiplus::Pen p(Gc(ink),Sf(1.8f));
    p.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
    const float cx=static_cast<float>(g_minimize.left+g_minimize.right)/2.0f;
    const float cy=static_cast<float>(g_minimize.top+g_minimize.bottom)/2.0f;
    g.DrawLine(&p,cx-S(5),cy,cx+S(5),cy);
}
void DrawButton(Gdiplus::Graphics& g,RECT r,const std::wstring& label,Hot hot,bool primary) {
    bool a=g_hover==hot||g_pressed==hot; COLORREF fill=primary?RGB(48,164,118):RGB(255,255,255);
    if(a) fill=primary?Mix(fill,RGB(21,111,74),g_pressed==hot?.20f:.09f):RGB(238,244,240);
    FillRound(g,r,Sf(8),fill); StrokeRound(g,r,Sf(8),primary?RGB(48,164,118):RGB(211,222,216),primary?255:220);
    DrawText(g,label,r,g_uiFont,primary?RGB(255,255,255):RGB(53,65,58),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
void DrawCheckbox(Gdiplus::Graphics& g,RECT r,bool checked,bool active) {
    const int side=S(18);
    RECT b{r.left,r.top+(r.bottom-r.top-side)/2,r.left+side,r.top+(r.bottom-r.top-side)/2+side};
    const COLORREF fill=checked?RGB(48,164,118):(active?RGB(244,249,246):RGB(255,255,255));
    FillRound(g,b,Sf(4),fill);
    StrokeRound(g,b,Sf(4),checked?RGB(48,164,118):(active?RGB(48,164,118):RGB(181,195,186)),255);
    if(!checked) return;
    Gdiplus::Pen p(Gc(RGB(255,255,255),255),Sf(2.0f));
    p.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
    g.DrawLine(&p,static_cast<float>(b.left)+Sf(4),static_cast<float>(b.top)+Sf(9),static_cast<float>(b.left)+Sf(8),static_cast<float>(b.top)+Sf(13));
    g.DrawLine(&p,static_cast<float>(b.left)+Sf(8),static_cast<float>(b.top)+Sf(13),static_cast<float>(b.left)+Sf(15),static_cast<float>(b.top)+Sf(5));
}
void DrawProgress(Gdiplus::Graphics& g,RECT r) {
    FillRound(g,r,Sf(5),RGB(225,234,229)); RECT f=r; f.right=f.left+static_cast<int>((r.right-r.left)*std::clamp(g_progress,0.0f,1.0f)); if(f.right<=f.left)return;
    FillRound(g,f,Sf(5),RGB(48,164,118)); RECT sh{std::max(f.left,f.right-S(84)+g_shine),f.top,std::min(f.right,f.right+S(18)),f.bottom}; if(sh.right>sh.left) FillRound(g,sh,Sf(5),RGB(190,241,220),70);
}
void DrawPage(Gdiplus::Graphics& g,RECT c) {
    COLORREF text=RGB(39,53,46), muted=RGB(105,122,112); g.Clear(Gc(RGB(245,248,246))); Gdiplus::SolidBrush header(Gc(RGB(235,244,239))); g.FillRectangle(&header,0,0,c.right,S(88)); Gdiplus::SolidBrush footer(Gc(RGB(248,250,249))); g.FillRectangle(&footer,0,c.bottom-S(92),c.right,S(92)); Gdiplus::Pen divider(Gc(RGB(220,231,224)),Sf(1.0f)); g.DrawLine(&divider,0.0f,Sf(88),static_cast<float>(c.right),Sf(88));
    DrawText(g,L"\u8f7b\u9605PDF",{S(52),S(28),S(260),S(58)},g_titleFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE); DrawText(g,L"PDF \u9605\u8bfb\u5668",{S(52),S(55),S(190),S(76)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE); DrawMinimize(g); DrawClose(g);
    if(g_uninstallMode) {
        if(g_page==Page::UninstallConfirm) {
            DrawText(g,L"\u5378\u8f7d\u8f7b\u9605PDF",{S(40),S(128),c.right-S(40),S(168)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawText(g,L"\u8f7b\u9605PDF \u5c06\u4ece\u5f53\u524d\u7535\u8111\u4e2d\u79fb\u9664\u3002",{S(42),S(184),c.right-S(70),S(218)},g_uiFont,muted,DT_LEFT|DT_WORDBREAK);
            DrawCheckbox(g,g_deleteData,g_deleteAppData,g_hover==Hot::DeleteData||g_pressed==Hot::DeleteData);
            DrawText(g,L"\u5220\u9664\u5e94\u7528\u914d\u7f6e\u6570\u636e\uff08\u9605\u8bfb\u8fdb\u5ea6\u3001\u6700\u8fd1\u6587\u6863\u3001\u4e3b\u9898\u8bbe\u7f6e\uff09",{S(70),S(250),c.right-S(40),S(286)},g_uiFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawButton(g,g_back,L"\u53d6\u6d88",Hot::Back,false); DrawButton(g,g_primary,L"\u5378\u8f7d",Hot::Primary,true); return;
        }
        if(g_page==Page::Uninstalling) {
            DrawText(g,L"\u6b63\u5728\u5378\u8f7d",{S(40),S(126),c.right-S(40),S(176)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawText(g,L"\u8bf7\u7a0d\u5019\uff0c\u6b63\u5728\u79fb\u9664\u8f7b\u9605PDF\u3002",{S(42),S(188),c.right-S(40),S(216)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawProgress(g,{S(42),S(258),c.right-S(42),S(270)}); std::wstring pct=std::to_wstring(static_cast<int>(g_progress*100+.5f))+L"%";
            DrawText(g,pct,{S(42),S(286),c.right-S(42),S(316)},g_titleFont,RGB(48,133,99),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawText(g,g_stage.empty()?L"\u6b63\u5728\u51c6\u5907":g_stage,{S(42),S(326),c.right-S(42),S(352)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE); return;
        }
        if(g_page==Page::UninstallComplete) {
            FillRound(g,{S(40),S(124),S(92),S(176)},Sf(22),RGB(225,244,235)); Gdiplus::Pen check(Gc(RGB(48,164,118)),Sf(3)); check.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
            g.DrawLine(&check,Sf(57),Sf(149),Sf(64),Sf(157)); g.DrawLine(&check,Sf(64),Sf(157),Sf(78),Sf(141));
            DrawText(g,L"\u5378\u8f7d\u5b8c\u6210",{S(126),S(124),c.right-S(40),S(168)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawText(g,L"\u8f7b\u9605PDF \u5df2\u4ece\u7535\u8111\u4e2d\u79fb\u9664\u3002",{S(128),S(176),c.right-S(40),S(208)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawButton(g,g_primary,L"\u5b8c\u6210",Hot::Primary,true); return;
        }
        if(g_page==Page::UninstallFailed) {
            DrawText(g,L"\u5378\u8f7d\u672a\u5b8c\u6210",{S(40),S(126),c.right-S(40),S(176)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            DrawText(g,g_error.empty()?L"\u5378\u8f7d\u8fc7\u7a0b\u4e2d\u51fa\u73b0\u4e86\u95ee\u9898\u3002":g_error,{S(42),S(188),c.right-S(42),S(248)},g_uiFont,muted,DT_LEFT|DT_WORDBREAK);
            DrawButton(g,g_primary,L"\u5173\u95ed",Hot::Primary,true); return;
        }
    }
    if(g_page==Page::Welcome) {
        DrawText(g,L"\u8ba9\u9605\u8bfb\u56de\u5230\u5185\u5bb9\u672c\u8eab",{S(42),S(128),c.right-S(42),S(168)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u8f7b\u9605PDF \u662f\u4e00\u6b3e\u8f7b\u91cf\u3001\u5b89\u9759\u7684 PDF \u9605\u8bfb\u5668\u3002\u5b89\u88c5\u53ea\u9700\u51e0\u79d2\uff0c\u4e0d\u4f1a\u6346\u7ed1\u5176\u4ed6\u7ec4\u4ef6\u3002",{S(44),S(184),c.right-S(70),S(232)},g_uiFont,muted,DT_LEFT|DT_WORDBREAK);
        DrawText(g,L"\u7248\u672c 0.3.1    Windows 10 / 11    x64",{S(44),S(265),c.right-S(40),S(293)},g_smallFont,RGB(139,151,144),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawButton(g,g_welcomePrimary,L"\u7ee7\u7eed",Hot::Primary,true); DrawButton(g,g_welcomeCancel,L"\u53d6\u6d88",Hot::WelcomeCancel,false); return;
    }
    if(g_page==Page::Options) {
        DrawText(g,L"\u5b89\u88c5\u9009\u9879",{S(40),S(108),c.right-S(40),S(148)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e\u548c\u5feb\u6377\u65b9\u5f0f\u8bbe\u7f6e",{S(42),S(162),c.right-S(40),S(190)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u5b89\u88c5\u4f4d\u7f6e",{S(40),S(202),S(180),S(222)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        FillRound(g,g_editFrame,Sf(7),RGB(255,255,255)); StrokeRound(g,g_editFrame,Sf(7),g_pathFocused?RGB(48,164,118):RGB(210,221,215),g_pathFocused?235:220); DrawButton(g,g_browse,L"\u6d4f\u89c8",Hot::Browse,false);
        DrawCheckbox(g,g_desktop,g_desktopShortcut,g_hover==Hot::Desktop||g_pressed==Hot::Desktop); DrawText(g,L"\u521b\u5efa\u684c\u9762\u5feb\u6377\u65b9\u5f0f",{S(70),S(286),S(360),S(322)},g_uiFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawButton(g,g_back,L"\u8fd4\u56de",Hot::Back,false); DrawButton(g,g_welcomePrimary,L"\u5f00\u59cb\u5b89\u88c5",Hot::Primary,true); DrawButton(g,g_optionsCancel,L"\u53d6\u6d88",Hot::OptionsCancel,false); return;
    }
    if(g_page==Page::Installing) {
        DrawText(g,L"\u6b63\u5728\u5b89\u88c5",{S(40),S(126),c.right-S(40),S(176)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u8bf7\u7a0d\u5019\uff0c\u8f7b\u9605PDF \u6b63\u5728\u51c6\u5907\u6587\u4ef6\u3002",{S(42),S(188),c.right-S(40),S(216)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawProgress(g,{S(42),S(258),c.right-S(42),S(270)}); std::wstring pct=std::to_wstring(static_cast<int>(g_progress*100+.5f))+L"%";
        DrawText(g,pct,{S(42),S(286),c.right-S(42),S(316)},g_titleFont,RGB(48,133,99),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,g_stage.empty()?L"\u6b63\u5728\u51c6\u5907":g_stage,{S(42),S(326),c.right-S(42),S(352)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE); return;
    }
    if(g_page==Page::Complete) {
        FillRound(g,{S(40),S(124),S(92),S(176)},Sf(22),RGB(225,244,235)); Gdiplus::Pen p(Gc(RGB(48,164,118)),Sf(3)); p.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
        g.DrawLine(&p,Sf(57),Sf(149),Sf(64),Sf(157)); g.DrawLine(&p,Sf(64),Sf(157),Sf(78),Sf(141));
        DrawText(g,L"\u5b89\u88c5\u5b8c\u6210",{S(126),S(124),c.right-S(40),S(168)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u8f7b\u9605PDF \u5df2\u51c6\u5907\u5c31\u7eea\u3002",{S(128),S(176),c.right-S(40),S(208)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawCheckbox(g,g_launch,g_launchAfterInstall,g_hover==Hot::Launch||g_pressed==Hot::Launch); DrawText(g,L"\u5b8c\u6210\u540e\u6253\u5f00\u8f7b\u9605PDF",{S(70),S(286),S(360),S(322)},g_uiFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawButton(g,g_primary,L"\u5b8c\u6210",Hot::Primary,true); return;
    }
    DrawText(g,L"\u672a\u80fd\u5b8c\u6210\u5b89\u88c5",{S(40),S(126),c.right-S(40),S(176)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawText(g,g_error.empty()?L"\u5b89\u88c5\u8fc7\u7a0b\u4e2d\u51fa\u73b0\u4e86\u95ee\u9898\uff0c\u8bf7\u91cd\u8bd5\u3002":g_error,{S(42),S(188),c.right-S(42),S(248)},g_uiFont,muted,DT_LEFT|DT_WORDBREAK);
    DrawButton(g,g_back,L"\u8fd4\u56de",Hot::Back,false); DrawButton(g,g_primary,L"\u91cd\u8bd5",Hot::Primary,true);
}
void Paint(HWND h) {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT c{}; GetClientRect(h,&c); HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,c.right,c.bottom); HGDIOBJ old=SelectObject(mem,bmp);
    Gdiplus::Graphics g(mem); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit); DrawPage(g,c);
    BitBlt(dc,0,0,c.right,c.bottom,mem,0,0,SRCCOPY); SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); EndPaint(h,&ps);
}
Hot HotAt(POINT p) {
    if(Hit(g_minimize,p))return Hot::Minimize; if(Hit(g_close,p))return Hot::Close;
    if(g_page==Page::UninstallConfirm){if(Hit(g_back,p))return Hot::Back;if(Hit(g_primary,p))return Hot::Primary;if(Hit(g_deleteData,p))return Hot::DeleteData;return Hot::None;}
    if(g_page==Page::UninstallComplete||g_page==Page::UninstallFailed)return Hit(g_primary,p)?Hot::Primary:Hot::None;
    if(g_page==Page::Welcome){if(Hit(g_welcomePrimary,p))return Hot::Primary;if(Hit(g_welcomeCancel,p))return Hot::WelcomeCancel;return Hot::None;}
    if(g_page==Page::Options){if(Hit(g_back,p))return Hot::Back;if(Hit(g_welcomePrimary,p))return Hot::Primary;if(Hit(g_optionsCancel,p))return Hot::OptionsCancel;if(Hit(g_browse,p))return Hot::Browse;if(Hit(g_desktop,p))return Hot::Desktop;}
    if(g_page==Page::Complete){if(Hit(g_primary,p))return Hot::Primary;if(Hit(g_launch,p))return Hot::Launch;}
    if(g_page==Page::Failed){if(Hit(g_back,p))return Hot::Back;if(Hit(g_primary,p))return Hot::Primary;} return Hot::None;
}
void UpdateHover(HWND h,POINT p){Hot n=HotAt(p);if(n!=g_hover){g_hover=n;InvalidateRect(h,nullptr,FALSE);}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}
bool WriteResource(int id,const wchar_t* type,const std::filesystem::path& target) {
    HMODULE m=GetModuleHandleW(nullptr);HRSRC r=FindResourceW(m,MAKEINTRESOURCEW(id),type);if(!r)return false;HGLOBAL loaded=LoadResource(m,r);DWORD size=SizeofResource(m,r);void* data=LockResource(loaded);
    HANDLE f=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(f==INVALID_HANDLE_VALUE)return false;DWORD written=0;BOOL ok=WriteFile(f,data,size,&written,nullptr);CloseHandle(f);return ok&&written==size;
}

std::filesystem::path ModulePath() {
    std::wstring value(32768,static_cast<wchar_t>(0));
    DWORD length=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));
    if(length==0||length>=value.size())return {};
    value.resize(length);
    return value;
}
bool CreateShortcut(const std::filesystem::path& linkPath,const std::filesystem::path& target,const std::filesystem::path& workDir) {
    IShellLinkW* link=nullptr;if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))))return false;
    link->SetPath(target.c_str());link->SetWorkingDirectory(workDir.c_str());link->SetDescription(L"\u8f7b\u91cf PDF \u9605\u8bfb\u5668");link->SetIconLocation(target.c_str(),0);
    IPersistFile* file=nullptr;HRESULT hr=link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file));bool ok=false;if(SUCCEEDED(hr)){std::filesystem::create_directories(linkPath.parent_path());ok=SUCCEEDED(file->Save(linkPath.c_str(),TRUE));file->Release();}link->Release();return ok;
}
void DeleteRegValue(HKEY root,const std::wstring& path,const wchar_t* name) { HKEY key{}; if(RegOpenKeyExW(root,path.c_str(),0,KEY_SET_VALUE,&key)==ERROR_SUCCESS){ RegDeleteValueW(key,name); RegCloseKey(key); } }
bool RegStringEquals(HKEY root,const std::wstring& path,const wchar_t* name,const wchar_t* expected) {
    HKEY key{};if(RegOpenKeyExW(root,path.c_str(),0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;wchar_t value[256]{};DWORD bytes=sizeof(value),type=0;LSTATUS status=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes);RegCloseKey(key);return status==ERROR_SUCCESS&&type==REG_SZ&&_wcsicmp(value,expected)==0;
}
void SetRegString(HKEY root,const std::wstring& path,const wchar_t* name,const std::wstring& value) {
    HKEY key{};if(RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS){RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));RegCloseKey(key);}
}
void RegisterInstallation(const std::filesystem::path& dir) {
    auto exe=dir/kExeName,un=dir/kUninstallName;std::wstring app=std::wstring(L"Software\\Classes\\Applications\\")+kExeName, prog=std::wstring(L"Software\\Classes\\")+kProgId;
    SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"DisplayName",kAppName);SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"DisplayVersion",L"0.3.1");SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"Publisher",L"\u8f7b\u9605");
    SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"DisplayIcon",exe.wstring());SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"InstallLocation",dir.wstring());SetRegString(HKEY_CURRENT_USER,kUninstallKey,L"UninstallString",L"\""+un.wstring()+L"\" /uninstall");
    SetRegString(HKEY_CURRENT_USER,kInstallKey,L"InstallLocation",dir.wstring());SetRegString(HKEY_CURRENT_USER,app+L"\\DefaultIcon",nullptr,exe.wstring()+L",0");SetRegString(HKEY_CURRENT_USER,app+L"\\shell\\open\\command",nullptr,L"\""+exe.wstring()+L"\" \"%1\"");
    SetRegString(HKEY_CURRENT_USER,app+L"\\SupportedTypes",L".pdf",L"");SetRegString(HKEY_CURRENT_USER,L"Software\\Classes\\.pdf\\OpenWithProgids",kProgId,L"");SetRegString(HKEY_CURRENT_USER,prog+L"\\DefaultIcon",nullptr,exe.wstring()+L",0");
    SetRegString(HKEY_CURRENT_USER,prog+L"\\shell\\open\\command",nullptr,L"\""+exe.wstring()+L"\" \"%1\"");SetRegString(HKEY_CURRENT_USER,kCapabilitiesKey,L"ApplicationName",kAppName);SetRegString(HKEY_CURRENT_USER,std::wstring(kCapabilitiesKey)+L"\\FileAssociations",L".pdf",kProgId);
    SetRegString(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",L"QingYuePDF",kCapabilitiesKey);SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
}
std::filesystem::path RegisteredInstallDir(){HKEY k{};if(RegOpenKeyExW(HKEY_CURRENT_USER,kUninstallKey,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)return DefaultInstallDir();wchar_t b[MAX_PATH*4]{};DWORD n=sizeof(b),t=0;std::filesystem::path r=DefaultInstallDir();if(RegQueryValueExW(k,L"InstallLocation",nullptr,&t,reinterpret_cast<BYTE*>(b),&n)==ERROR_SUCCESS&&t==REG_SZ)r=b;RegCloseKey(k);return r;}
bool BrowseForFolder(HWND owner,std::filesystem::path& selected) {
    IFileDialog* d=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_IFileDialog,reinterpret_cast<void**>(&d))))return false;DWORD o=0;d->GetOptions(&o);d->SetOptions(o|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);d->SetTitle(L"\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e");bool ok=false;
    if(SUCCEEDED(d->Show(owner))){IShellItem* i=nullptr;if(SUCCEEDED(d->GetResult(&i))){PWSTR p=nullptr;if(SUCCEEDED(i->GetDisplayName(SIGDN_FILESYSPATH,&p))){selected=p;CoTaskMemFree(p);if(_wcsicmp(selected.filename().c_str(),kAppName)!=0)selected/=kAppName;ok=true;}i->Release();}}d->Release();return ok;
}
UINT CALLBACK CabinetCallback(PVOID context, UINT note, UINT_PTR p1, UINT_PTR p2) {
    if(note==SPFILENOTIFY_FILEINCABINET){auto* f=reinterpret_cast<FILE_IN_CABINET_INFO_W*>(p1);auto* c=reinterpret_cast<CabinetContext*>(context);auto target=c->target/f->NameInCabinet;std::filesystem::create_directories(target.parent_path());wcsncpy_s(f->FullTargetName,ARRAYSIZE(f->FullTargetName),target.c_str(),_TRUNCATE);return FILEOP_DOIT;}
    return note==SPFILENOTIFY_NEEDNEWCABINET?FILEOP_ABORT:NO_ERROR;
}
std::filesystem::path AppDataDir() { return KnownFolder(FOLDERID_RoamingAppData)/L"QingYuePDF"; }
std::filesystem::path InstallDataDir(const std::filesystem::path& installDir) { return installDir/L"Data"; }
bool PreserveInstalledData(const std::filesystem::path& installDir) {
    auto source=InstallDataDir(installDir);
    std::error_code ec;
    if(!std::filesystem::is_directory(source,ec))return true;
    auto target=AppDataDir();
    std::filesystem::create_directories(target,ec);
    if(ec)return false;
    for(std::filesystem::recursive_directory_iterator it(source,ec),end;!ec&&it!=end;it.increment(ec)) {
        auto relative=std::filesystem::relative(it->path(),source,ec);
        if(ec)return false;
        auto destination=target/relative;
        if(it->is_directory(ec))std::filesystem::create_directories(destination,ec);
        else if(it->is_regular_file(ec)) {
            std::filesystem::create_directories(destination.parent_path(),ec);
            if(!ec)std::filesystem::copy_file(it->path(),destination,std::filesystem::copy_options::overwrite_existing,ec);
        }
        if(ec)return false;
    }
    return !ec;
}
bool SamePath(const std::filesystem::path& a,const std::filesystem::path& b) {
    std::error_code ecA,ecB;
    auto left=std::filesystem::weakly_canonical(a,ecA).wstring();
    auto right=std::filesystem::weakly_canonical(b,ecB).wstring();
    if(ecA)left=std::filesystem::absolute(a,ecA).wstring();
    if(ecB)right=std::filesystem::absolute(b,ecB).wstring();
    return !left.empty()&&!right.empty()&&_wcsicmp(left.c_str(),right.c_str())==0;
}
bool ProcessPath(DWORD pid,std::filesystem::path& path,DWORD access=PROCESS_QUERY_LIMITED_INFORMATION) {
    HANDLE process=OpenProcess(access,FALSE,pid);
    if(!process)return false;
    wchar_t value[32768]{};DWORD length=ARRAYSIZE(value);
    bool ok=QueryFullProcessImageNameW(process,0,value,&length)!=FALSE;
    CloseHandle(process);
    if(ok)path=std::wstring(value,length);
    return ok;
}
std::vector<DWORD> RunningAppProcesses(const std::filesystem::path& installDir) {
    std::vector<DWORD> result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return result;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry))do {
        if(_wcsicmp(entry.szExeFile,kExeName)!=0)continue;
        std::filesystem::path processPath;
        if(ProcessPath(entry.th32ProcessID,processPath)&&SamePath(processPath.parent_path(),installDir))result.push_back(entry.th32ProcessID);
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    return result;
}
bool AppIsRunning(const std::filesystem::path& installDir) {
    return !installDir.empty()&&!RunningAppProcesses(installDir).empty();
}
bool WaitForProcessesToExit(const std::vector<DWORD>& processIds,DWORD timeoutMs) {
    const ULONGLONG deadline=GetTickCount64()+timeoutMs;
    for(;;) {
        bool anyRunning=false;
        for(DWORD pid:processIds) {
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
            if(!process)continue;
            if(WaitForSingleObject(process,0)==WAIT_TIMEOUT)anyRunning=true;
            CloseHandle(process);
        }
        if(!anyRunning)return true;
        if(GetTickCount64()>=deadline)return false;
        Sleep(50);
    }
}
bool CloseAppProcesses(const std::vector<std::filesystem::path>& installDirs) {
    std::vector<DWORD> processIds;
    for(size_t i=0;i<installDirs.size();++i) {
        if(installDirs[i].empty())continue;
        bool duplicate=false;
        for(size_t j=0;j<i;++j)if(SamePath(installDirs[i],installDirs[j])){duplicate=true;break;}
        if(duplicate)continue;
        for(DWORD pid:RunningAppProcesses(installDirs[i]))
            if(std::find(processIds.begin(),processIds.end(),pid)==processIds.end())processIds.push_back(pid);
    }
    if(processIds.empty())return true;
    EnumWindows([](HWND window,LPARAM parameter)->BOOL {
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        auto* ids=reinterpret_cast<std::vector<DWORD>*>(parameter);
        if(std::find(ids->begin(),ids->end(),pid)!=ids->end())PostMessageW(window,WM_CLOSE,0,0);
        return TRUE;
    },reinterpret_cast<LPARAM>(&processIds));
    if(WaitForProcessesToExit(processIds,5000))return true;
    for(DWORD pid:processIds) {
        HANDLE process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,pid);
        if(process){TerminateProcess(process,0);CloseHandle(process);}
    }
    return WaitForProcessesToExit(processIds,2000);
}
bool PromptForAppClose(HWND owner,const std::vector<std::filesystem::path>& installDirs,bool uninstalling) {
    bool running=false;
    for(const auto& dir:installDirs)if(AppIsRunning(dir)){running=true;break;}
    if(!running)return true;
    const wchar_t* message=uninstalling
        ? L"\u68c0\u6d4b\u5230 \u8f7b\u9605PDF \u6b63\u5728\u8fd0\u884c\u3002\n\n\u5378\u8f7d\u524d\u9700\u8981\u5173\u95ed\u7a0b\u5e8f\u3002\u70b9\u51fb\u201c\u786e\u5b9a\u201d\u81ea\u52a8\u5173\u95ed\u540e\u7ee7\u7eed\u5378\u8f7d\uff0c\u6216\u70b9\u51fb\u201c\u53d6\u6d88\u201d\u9000\u51fa\u5378\u8f7d\u3002"
        : L"\u68c0\u6d4b\u5230 \u8f7b\u9605PDF \u6b63\u5728\u8fd0\u884c\u3002\n\n\u5b89\u88c5\u524d\u9700\u8981\u5173\u95ed\u7a0b\u5e8f\u3002\u70b9\u51fb\u201c\u786e\u5b9a\u201d\u81ea\u52a8\u5173\u95ed\u540e\u7ee7\u7eed\u5b89\u88c5\uff0c\u6216\u70b9\u51fb\u201c\u53d6\u6d88\u201d\u9000\u51fa\u5b89\u88c5\u3002";
    const wchar_t* title=uninstalling?L"\u8f7b\u9605PDF \u5378\u8f7d":L"\u8f7b\u9605PDF \u5b89\u88c5";
    if(MessageBoxW(owner,message,title,MB_OKCANCEL|MB_ICONINFORMATION|MB_DEFBUTTON1)!=IDOK){PostMessageW(owner,WM_CLOSE,0,0);return false;}
    if(CloseAppProcesses(installDirs))return true;
    MessageBoxW(owner,L"\u672a\u80fd\u5173\u95ed\u8f7b\u9605PDF\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5\u3002",title,MB_OK|MB_ICONWARNING);
    return false;
}bool ReplaceFileFrom(const std::filesystem::path& source,const std::filesystem::path& target) {
    std::filesystem::path pending=target;pending+=L".new";
    std::error_code ec;std::filesystem::copy_file(source,pending,std::filesystem::copy_options::overwrite_existing,ec);
    if(ec)return false;
    if(!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { DeleteFileW(pending.c_str()); return false; }
    return true;
}
bool LooksLikeInstallDir(const std::filesystem::path& dir) {
    if(dir.empty())return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(dir/kExeName,ec)
        ||std::filesystem::is_regular_file(dir/kDllName,ec)
        ||std::filesystem::is_regular_file(dir/kUninstallName,ec);
}bool StartUninstallWorker(const std::filesystem::path& installDir) {
    auto source=ModulePath();
    if(source.empty())return false;
    auto tempDir=std::filesystem::temp_directory_path();
    auto worker=tempDir/(L"QingYuePDF-Uninstall-"+std::to_wstring(GetCurrentProcessId())+L".exe");
    std::error_code ec;
    std::filesystem::copy_file(source,worker,std::filesystem::copy_options::overwrite_existing,ec);
    if(ec)return false;
    std::wstring parameters=L"/uninstall-worker /wait-pid "+std::to_wstring(GetCurrentProcessId())+L" /install-dir \""+installDir.wstring()+L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask=SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb=L"open";
    info.lpFile=worker.c_str();
    info.lpParameters=parameters.c_str();
    info.lpDirectory=tempDir.c_str();
    info.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info)) {
        DeleteFileW(worker.c_str());
        return false;
    }
    if(info.hProcess)CloseHandle(info.hProcess);
    return true;
}
void DeleteUninstallWorkerAfterExit() {
    if(!g_uninstallWorker)return;
    auto self=ModulePath();
    if(self.empty())return;
    wchar_t commandProcessor[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"ComSpec",commandProcessor,ARRAYSIZE(commandProcessor)))return;
    std::wstring command=L"\""+std::wstring(commandProcessor)+L"\" /D /Q /C ping 127.0.0.1 -n 2 >nul & del /F /Q \""+self.wstring()+L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(CreateProcessW(commandProcessor,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
void PostProgress(int p,const std::wstring& s){PostMessageW(g_window,kMsgProgress,reinterpret_cast<WPARAM>(new ProgressMessage{p,s}),0);}
std::wstring InstallPackage(const std::filesystem::path& request,bool desktop) {
    std::filesystem::path staging;
    HRESULT comResult=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const bool uninitializeCom=SUCCEEDED(comResult);
    struct ComCleanup { bool active; ~ComCleanup(){if(active)CoUninitialize();} } comCleanup{uninitializeCom};
    try {
        if(request.empty())return L"\u8bf7\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e\u3002";
        auto dir=std::filesystem::absolute(request);
        staging=std::filesystem::temp_directory_path()/(L"QingYuePDF-Install-"+std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;std::filesystem::remove_all(staging,ec);std::filesystem::create_directories(staging);
        DirectoryCleanup cleanup{staging};
        PostProgress(5,L"\u6b63\u5728\u51c6\u5907\u5b89\u88c5\u6587\u4ef6");
        auto cab=staging/L"payload.cab";
        if(!WriteResource(IDR_PAYLOAD_CAB,RT_RCDATA,cab))return L"\u65e0\u6cd5\u8bfb\u53d6\u5b89\u88c5\u6587\u4ef6\u3002";
        PostProgress(18,L"\u6b63\u5728\u89e3\u538b\u5b89\u88c5\u6587\u4ef6");
        CabinetContext context{staging};
        if(!SetupIterateCabinetW(cab.c_str(),0,CabinetCallback,reinterpret_cast<LPVOID>(&context)))return L"\u5b89\u88c5\u6587\u4ef6\u89e3\u538b\u5931\u8d25\u3002";
        DeleteFileW(cab.c_str());
        PostProgress(48,L"\u6b63\u5728\u68c0\u67e5\u8fd0\u884c\u72b6\u6001");
        auto previousDir=RegisteredInstallDir();
        if((!SamePath(previousDir,dir)&&AppIsRunning(previousDir))||AppIsRunning(dir))return L"\u8f7b\u9605PDF \u4ecd\u5728\u8fd0\u884c\uff0c\u8bf7\u5173\u95ed\u540e\u91cd\u8bd5\u3002";
        std::filesystem::create_directories(dir);
        std::filesystem::create_directories(dir/L"Data");
        std::filesystem::create_directories(dir/L"Cache"/L"HomeThumbs");
        PostProgress(62,L"\u6b63\u5728\u66f4\u65b0\u7a0b\u5e8f\u6587\u4ef6");
        auto exe=dir/kExeName;
        if(!ReplaceFileFrom(staging/kPayloadExe,exe)||!ReplaceFileFrom(staging/kDllName,dir/kDllName)||!ReplaceFileFrom(staging/kRuntimeName,dir/kRuntimeName))return L"\u65e0\u6cd5\u66f4\u65b0\u7a0b\u5e8f\u6587\u4ef6\uff0c\u8bf7\u68c0\u67e5\u5b89\u88c5\u76ee\u5f55\u6743\u9650\u540e\u91cd\u8bd5\u3002";
        if(!ReplaceFileFrom(staging/kPayloadUninstaller,dir/kUninstallName))return L"\u65e0\u6cd5\u521b\u5efa\u5378\u8f7d\u7a0b\u5e8f\u3002";
        PostProgress(82,L"\u6b63\u5728\u521b\u5efa\u5feb\u6377\u65b9\u5f0f");
        if(desktop&&!CreateShortcut(DesktopLink(),exe,dir))return L"\u65e0\u6cd5\u521b\u5efa\u684c\u9762\u5feb\u6377\u65b9\u5f0f\u3002";
        if(!CreateShortcut(StartLink(),exe,dir))return L"\u65e0\u6cd5\u521b\u5efa\u5f00\u59cb\u83dc\u5355\u5feb\u6377\u65b9\u5f0f\u3002";
        PostProgress(92,L"\u6b63\u5728\u6ce8\u518c PDF \u6587\u4ef6\u5173\u8054");
        RegisterInstallation(dir);g_installedDir=dir;PostProgress(100,L"\u5b89\u88c5\u5b8c\u6210");return {};
    } catch (...) {
        return L"\u5b89\u88c5\u5931\u8d25\uff0c\u8bf7\u9009\u62e9\u6709\u5199\u5165\u6743\u9650\u7684\u76ee\u5f55\u3002";
    }
}
void LaunchInstalled(){if(!g_installedDir.empty()){auto exe=g_installedDir/kExeName;ShellExecuteW(g_window,L"open",exe.c_str(),nullptr,g_installedDir.c_str(),SW_SHOWNORMAL);}}
std::wstring UninstallPackage(bool deleteAppData) {
    try {
        auto dir=g_uninstallDir.empty()?RegisteredInstallDir():g_uninstallDir;
        if(dir.empty())return L"\u65e0\u6cd5\u786e\u5b9a\u8f7b\u9605PDF \u7684\u5b89\u88c5\u4f4d\u7f6e\u3002";
        PostProgress(8,L"\u6b63\u5728\u68c0\u67e5\u8fd0\u884c\u72b6\u6001");
        if(AppIsRunning(dir))return L"\u8f7b\u9605PDF \u4ecd\u5728\u8fd0\u884c\uff0c\u8bf7\u5173\u95ed\u540e\u91cd\u8bd5\u3002";
        PostProgress(22,L"\u6b63\u5728\u79fb\u9664\u5feb\u6377\u65b9\u5f0f");
        DeleteFileW(DesktopLink().c_str());
        DeleteFileW(StartLink().c_str());
        std::error_code ec;
        std::filesystem::remove(StartFolder(),ec);
        if(deleteAppData) {
            PostProgress(42,L"\u6b63\u5728\u5220\u9664\u5e94\u7528\u914d\u7f6e\u6570\u636e");
            ec.clear();
            std::filesystem::remove_all(AppDataDir(),ec);
            if(ec||std::filesystem::exists(AppDataDir()))return L"\u5e94\u7528\u914d\u7f6e\u6570\u636e\u672a\u80fd\u5b8c\u5168\u5220\u9664\uff0c\u8bf7\u68c0\u67e5\u76ee\u5f55\uff1a\n"+AppDataDir().wstring();
        } else if(!PreserveInstalledData(dir)) {
            return L"\u65e0\u6cd5\u4fdd\u7559\u5e94\u7528\u914d\u7f6e\u6570\u636e\uff0c\u8bf7\u68c0\u67e5\u76ee\u5f55\uff1a\n"+AppDataDir().wstring();
        }
        PostProgress(60,L"\u6b63\u5728\u79fb\u9664\u7a0b\u5e8f\u6587\u4ef6");
        for(int attempt=0;attempt<20&&std::filesystem::exists(dir);++attempt) {
            ec.clear();
            std::filesystem::remove_all(dir,ec);
            if(!std::filesystem::exists(dir))break;
            Sleep(100);
        }
        if(ec||std::filesystem::exists(dir))return L"\u7a0b\u5e8f\u6587\u4ef6\u672a\u80fd\u5b8c\u5168\u5220\u9664\uff0c\u8bf7\u68c0\u67e5\u76ee\u5f55\uff1a\n"+dir.wstring();
        PostProgress(78,L"\u6b63\u5728\u6e05\u7406\u8f7b\u9605\u6ce8\u518c\u4fe1\u606f");
        RegDeleteTreeW(HKEY_CURRENT_USER,kUninstallKey);
        RegDeleteTreeW(HKEY_CURRENT_USER,kInstallKey);
        RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\Applications\\")+kExeName).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+kProgId).c_str());
        DeleteRegValue(HKEY_CURRENT_USER,L"Software\\Classes\\.pdf\\OpenWithProgids",kProgId);
        DeleteRegValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",L"QingYuePDF");
        const std::wstring userChoice=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice";
        if(RegStringEquals(HKEY_CURRENT_USER,userChoice,L"ProgId",kProgId))RegDeleteTreeW(HKEY_CURRENT_USER,userChoice.c_str());
        DeleteRegValue(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",L"QingYuePDF.Document_.pdf");
        DeleteRegValue(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",(std::wstring(L"Applications\\")+kExeName+L"_.pdf").c_str());
        SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
        PostProgress(100,L"\u5378\u8f7d\u5b8c\u6210");
        return {};
    } catch (...) {
        return L"\u5378\u8f7d\u8fc7\u7a0b\u4e2d\u51fa\u73b0\u4e86\u95ee\u9898\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5\u3002";
    }
}
LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE:
        g_window=h; g_dpi=GetDpiForWindow(h); MakeFonts();
        g_pathEdit=CreateWindowExW(0,L"EDIT",RegisteredInstallDir().c_str(),WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEdit)),g_instance,nullptr);
        SendMessageW(g_pathEdit,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE); Layout(h); return 0;
    case WM_PAINT: Paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: Layout(h); InvalidateRect(h,nullptr,FALSE); return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc=reinterpret_cast<HDC>(w); SetTextColor(dc,RGB(46,60,52)); SetBkColor(dc,RGB(255,255,255)); SetBkMode(dc,OPAQUE);
        static HBRUSH brush=CreateSolidBrush(RGB(255,255,255)); return reinterpret_cast<LRESULT>(brush);
    }
    case WM_COMMAND:
        if(LOWORD(w)==kPathEdit&&(HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS)) { g_pathFocused=HIWORD(w)==EN_SETFOCUS; InvalidateRect(h,nullptr,FALSE); return 0; }
        return 0;
    case WM_MOUSEMOVE: UpdateHover(h,{GET_X_LPARAM(l),GET_Y_LPARAM(l)}); return 0;
    case WM_MOUSELEAVE: g_hover=Hot::None; InvalidateRect(h,nullptr,FALSE); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr,IDC_ARROW)); return TRUE;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; Hot hot=HotAt(p);
        if(hot!=Hot::None) { g_pressed=hot; SetCapture(h); InvalidateRect(h,nullptr,FALSE); return 0; }
        if(!g_installing&&p.y<S(78)) { ReleaseCapture(); SendMessageW(h,WM_NCLBUTTONDOWN,HTCAPTION,0); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; Hot pressed=g_pressed; g_pressed=Hot::None; ReleaseCapture();
        if(pressed==Hot::Close&&Hit(g_close,p)) { PostMessageW(h,WM_CLOSE,0,0); return 0; }
        if(pressed==Hot::None||pressed!=HotAt(p)||g_installing) return 0;
        if(pressed==Hot::Minimize) { ShowWindow(h,SW_MINIMIZE); return 0; }
        if(pressed==Hot::WelcomeCancel&&g_page==Page::Welcome) { PostMessageW(h,WM_CLOSE,0,0); return 0; }
        if(pressed==Hot::OptionsCancel&&g_page==Page::Options) { PostMessageW(h,WM_CLOSE,0,0); return 0; }
        if(g_uninstallMode) {
            if(pressed==Hot::Back&&g_page==Page::UninstallConfirm) { PostMessageW(h,WM_CLOSE,0,0); return 0; }
            if(pressed==Hot::DeleteData&&g_page==Page::UninstallConfirm) { g_deleteAppData=!g_deleteAppData; InvalidateRect(h,nullptr,FALSE); return 0; }
            if(pressed==Hot::Primary&&g_page==Page::UninstallConfirm) {
                if(!PromptForAppClose(h,{g_uninstallDir},true))return 0;
                g_page=Page::Uninstalling; g_installing=true; g_progress=g_targetProgress=0; g_stage=L"\u6b63\u5728\u51c6\u5907"; SetTimer(h,kTimer,16,nullptr);
                std::thread([] { bool deleteData=g_deleteAppData; auto error=UninstallPackage(deleteData); PostMessageW(g_window,kMsgComplete,reinterpret_cast<WPARAM>(new InstallResult{error.empty(),error}),0); }).detach();
                Layout(h); InvalidateRect(h,nullptr,FALSE); return 0;
            }
            if(pressed==Hot::Primary&&(g_page==Page::UninstallComplete||g_page==Page::UninstallFailed)) { PostMessageW(h,WM_CLOSE,0,0); return 0; }
            return 0;
        }
        if(pressed==Hot::Primary) {
            if(g_page==Page::Welcome) { g_page=Page::Options; Layout(h); InvalidateRect(h,nullptr,FALSE); }
            else if(g_page==Page::Options) {
                auto requestedPath=std::filesystem::path(WindowText(g_pathEdit));
                auto absolutePath=requestedPath.empty()?requestedPath:std::filesystem::absolute(requestedPath);
                if(!PromptForAppClose(h,{RegisteredInstallDir(),absolutePath},false))return 0;
                g_page=Page::Installing; g_installing=true; g_progress=g_targetProgress=0; g_stage=L"\u6b63\u5728\u51c6\u5907"; Layout(h); SetTimer(h,kTimer,16,nullptr);
                auto path=std::move(requestedPath); bool desktop=g_desktopShortcut;
                std::thread([path,desktop] { auto error=InstallPackage(path,desktop); PostMessageW(g_window,kMsgComplete,reinterpret_cast<WPARAM>(new InstallResult{error.empty(),error}),0); }).detach(); InvalidateRect(h,nullptr,FALSE);
            } else if(g_page==Page::Complete) { if(g_launchAfterInstall) LaunchInstalled(); PostMessageW(h,WM_CLOSE,0,0); }
            else if(g_page==Page::Failed) { g_page=Page::Options; g_error.clear(); Layout(h); InvalidateRect(h,nullptr,FALSE); }
        } else if(pressed==Hot::Back) {
            g_page=g_page==Page::Options?Page::Welcome:Page::Options; Layout(h); InvalidateRect(h,nullptr,FALSE);
        } else if(pressed==Hot::Browse) {
            std::filesystem::path selected; if(BrowseForFolder(h,selected)) SetWindowTextW(g_pathEdit,selected.c_str());
        } else if(pressed==Hot::Desktop) { g_desktopShortcut=!g_desktopShortcut; InvalidateRect(h,nullptr,FALSE); }
        else if(pressed==Hot::Launch) { g_launchAfterInstall=!g_launchAfterInstall; InvalidateRect(h,nullptr,FALSE); }
        return 0;
    }
    case WM_TIMER:
        if(w==kTimer&&(g_page==Page::Installing||g_page==Page::Uninstalling)) { g_progress+=(g_targetProgress-g_progress)*.18f; if(g_targetProgress-g_progress<.003f)g_progress=g_targetProgress; g_shine=(g_shine+S(4))%std::max(1,S(100)); InvalidateRect(h,nullptr,FALSE); }
        return 0;
    case kMsgProgress: {
        auto* progress=reinterpret_cast<ProgressMessage*>(w); if(progress){g_targetProgress=std::clamp(progress->percent/100.0f,0.0f,1.0f);g_stage=std::move(progress->text);delete progress;} return 0;
    }
    case kMsgComplete: {
        std::unique_ptr<InstallResult> result(reinterpret_cast<InstallResult*>(w)); KillTimer(h,kTimer); g_installing=false;
        if(g_uninstallMode) { if(result&&result->success){g_page=Page::UninstallComplete;g_progress=g_targetProgress=1;}else{g_page=Page::UninstallFailed;g_error=result?result->error:L"\u5378\u8f7d\u5931\u8d25\u3002";} }
        else { if(result&&result->success){g_page=Page::Complete;g_progress=g_targetProgress=1;}else{g_page=Page::Failed;g_error=result?result->error:L"\u5b89\u88c5\u5931\u8d25\u3002";} }
        Layout(h); InvalidateRect(h,nullptr,FALSE); return 0;
    }
    case WM_CLOSE: if(g_installing)return 0; DestroyWindow(h); return 0;
    case WM_DESTROY: for(HFONT font:{g_uiFont,g_smallFont,g_titleFont,g_displayFont})if(font)DeleteObject(font); if(g_uninstallWorker&&g_page==Page::UninstallComplete)DeleteUninstallWorkerAfterExit(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(h,m,w,l);
    }
}
int ShowWizard(HINSTANCE instance,int show) {
    g_instance=instance;g_page=g_uninstallMode?Page::UninstallConfirm:Page::Welcome;g_dpi=GetDpiForSystem();WNDCLASSW wc{};wc.style=CS_DBLCLKS|CS_DROPSHADOW;wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=kWindowClass;if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 1;
    HWND h=CreateWindowExW(WS_EX_APPWINDOW,kWindowClass,kAppName,WS_POPUP|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,S(680),S(470),nullptr,nullptr,instance,nullptr);if(!h)return 1;ApplyWindowChrome(h);RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);SetWindowPos(h,nullptr,work.left+(work.right-work.left-S(680))/2,work.top+(work.bottom-work.top-S(470))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);ShowWindow(h,show);UpdateWindow(h);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken,&input,nullptr);
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr,modulePath,MAX_PATH);
    std::filesystem::path moduleName(modulePath);
    const bool launchedFromInstalledUninstaller=_wcsicmp(moduleName.filename().c_str(),kUninstallName)==0;
#ifdef QINGYUE_UNINSTALLER
    g_uninstallMode=true;
#else
    g_uninstallMode=launchedFromInstalledUninstaller;
#endif
    DWORD waitPid=0;
    int argc=0;
    PWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;argv&&i<argc;++i) {
        if(_wcsicmp(argv[i],L"/uninstall")==0||_wcsicmp(argv[i],L"/uninstall-worker")==0)g_uninstallMode=true;
        if(_wcsicmp(argv[i],L"/uninstall-worker")==0)g_uninstallWorker=true;
        if(_wcsicmp(argv[i],L"/wait-pid")==0&&i+1<argc)waitPid=wcstoul(argv[++i],nullptr,10);
        if(_wcsicmp(argv[i],L"/install-dir")==0&&i+1<argc)g_uninstallDir=argv[++i];
    }
    if(argv) LocalFree(argv);
    if(g_uninstallWorker) {
        auto tempDir=std::filesystem::temp_directory_path();
        SetCurrentDirectoryW(tempDir.c_str());
    }
    if(waitPid) {
        HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,waitPid);
        if(parent){WaitForSingleObject(parent,10000);CloseHandle(parent);}
    }
    if(g_uninstallMode&&!g_uninstallWorker) {
        auto registeredDir=RegisteredInstallDir();
        auto sourceDir=moduleName.parent_path();
        g_uninstallDir=launchedFromInstalledUninstaller&&LooksLikeInstallDir(sourceDir)?sourceDir:registeredDir;
        const bool started=StartUninstallWorker(g_uninstallDir);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        CoUninitialize();
        return started?0:1;
    }
    if(g_uninstallMode&&g_uninstallDir.empty())g_uninstallDir=RegisteredInstallDir();
    int result=ShowWizard(instance,show);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return result;
}
