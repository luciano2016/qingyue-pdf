#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include "resource.h"

namespace {
constexpr wchar_t kAppName[] = L"\u8f7b\u9605PDF";
constexpr wchar_t kExeName[] = L"\u8f7b\u9605PDF.exe";
constexpr wchar_t kPayloadExe[] = L"QuietPDF.Win32.exe";
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

enum class Page { Welcome, Options, Installing, Complete, Failed };
enum class Hot { None, Minimize, Close, Back, Primary, Browse, Desktop, Launch };
struct InstallResult { bool success; std::wstring error; };
struct ProgressMessage { int percent; std::wstring text; };
struct CabinetContext { std::filesystem::path target; };

HINSTANCE g_instance{};
HWND g_window{}, g_pathEdit{};
HFONT g_uiFont{}, g_smallFont{}, g_titleFont{}, g_displayFont{};
ULONG_PTR g_gdiplusToken{};
UINT g_dpi = 96;
Page g_page = Page::Welcome;
Hot g_hover = Hot::None, g_pressed = Hot::None;
RECT g_minimize{}, g_close{}, g_back{}, g_primary{}, g_browse{}, g_desktop{}, g_launch{}, g_editFrame{};
bool g_desktopShortcut = true, g_launchAfterInstall = true, g_installing = false;
bool g_pathFocused = false;
float g_progress = 0.0f, g_targetProgress = 0.0f;
int g_shine = 0;
std::wstring g_stage, g_error;
std::filesystem::path g_installedDir;

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
    g_editFrame={S(40),S(225),c.right-S(136),S(265)}; g_browse={c.right-S(122),S(225),c.right-S(40),S(265)};
    g_desktop={S(40),S(286),S(350),S(320)}; g_launch=g_desktop;
    if(g_pathEdit) {
        const int inset = S(4);
        const int width = g_editFrame.right-g_editFrame.left-S(24);
        const int height = g_editFrame.bottom-g_editFrame.top-S(2*inset);
        MoveWindow(g_pathEdit,g_editFrame.left+S(12),g_editFrame.top+inset,width,height,TRUE);
        SendMessageW(g_pathEdit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELONG(S(2),S(2)));
        RECT format{S(2),S(3),width-S(2),height-S(3)};
        SendMessageW(g_pathEdit,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&format));
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
    COLORREF text=RGB(39,53,46), muted=RGB(105,122,112); g.Clear(Gc(RGB(245,248,246))); Gdiplus::SolidBrush header(Gc(RGB(235,244,239))); g.FillRectangle(&header,0,0,c.right,S(88)); Gdiplus::SolidBrush footer(Gc(RGB(248,250,249))); g.FillRectangle(&footer,0,c.bottom-S(92),c.right,S(92)); Gdiplus::Pen divider(Gc(RGB(220,231,224)),Sf(1.0f)); g.DrawLine(&divider,Sf(32),Sf(88),Sf(c.right-32),Sf(88));
    DrawText(g,L"\u8f7b\u9605PDF",{S(52),S(28),S(260),S(58)},g_titleFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE); DrawText(g,L"PDF \u9605\u8bfb\u5668",{S(52),S(55),S(190),S(76)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE); DrawMinimize(g); DrawClose(g);
    if(g_page==Page::Welcome) {
        DrawText(g,L"\u8ba9\u9605\u8bfb\u56de\u5230\u5185\u5bb9\u672c\u8eab",{S(42),S(128),c.right-S(42),S(168)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u8f7b\u9605PDF \u662f\u4e00\u6b3e\u8f7b\u91cf\u3001\u5b89\u9759\u7684 PDF \u9605\u8bfb\u5668\u3002\u5b89\u88c5\u53ea\u9700\u51e0\u79d2\uff0c\u4e0d\u4f1a\u6346\u7ed1\u5176\u4ed6\u7ec4\u4ef6\u3002",{S(44),S(184),c.right-S(70),S(232)},g_uiFont,muted,DT_LEFT|DT_WORDBREAK);
        DrawText(g,L"\u7248\u672c 0.3.1    Windows 10 / 11    x64",{S(44),S(265),c.right-S(40),S(293)},g_smallFont,RGB(139,151,144),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawButton(g,g_primary,L"\u7ee7\u7eed",Hot::Primary,true); return;
    }
    if(g_page==Page::Options) {
        DrawText(g,L"\u5b89\u88c5\u9009\u9879",{S(40),S(108),c.right-S(40),S(148)},g_displayFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e\u548c\u5feb\u6377\u65b9\u5f0f\u8bbe\u7f6e",{S(42),S(162),c.right-S(40),S(190)},g_uiFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawText(g,L"\u5b89\u88c5\u4f4d\u7f6e",{S(40),S(202),S(180),S(222)},g_smallFont,muted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        FillRound(g,g_editFrame,Sf(7),RGB(255,255,255)); StrokeRound(g,g_editFrame,Sf(7),g_pathFocused?RGB(48,164,118):RGB(210,221,215),g_pathFocused?235:220); DrawButton(g,g_browse,L"\u6d4f\u89c8",Hot::Browse,false);
        DrawCheckbox(g,g_desktop,g_desktopShortcut,g_hover==Hot::Desktop||g_pressed==Hot::Desktop); DrawText(g,L"\u521b\u5efa\u684c\u9762\u5feb\u6377\u65b9\u5f0f",{S(70),S(286),S(360),S(322)},g_uiFont,text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawButton(g,g_back,L"\u8fd4\u56de",Hot::Back,false); DrawButton(g,g_primary,L"\u5f00\u59cb\u5b89\u88c5",Hot::Primary,true); return;
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
    if(Hit(g_minimize,p))return Hot::Minimize; if(Hit(g_close,p))return Hot::Close; if(g_page==Page::Welcome)return Hit(g_primary,p)?Hot::Primary:Hot::None;
    if(g_page==Page::Options){if(Hit(g_back,p))return Hot::Back;if(Hit(g_primary,p))return Hot::Primary;if(Hit(g_browse,p))return Hot::Browse;if(Hit(g_desktop,p))return Hot::Desktop;}
    if(g_page==Page::Complete){if(Hit(g_primary,p))return Hot::Primary;if(Hit(g_launch,p))return Hot::Launch;}
    if(g_page==Page::Failed){if(Hit(g_back,p))return Hot::Back;if(Hit(g_primary,p))return Hot::Primary;} return Hot::None;
}
void UpdateHover(HWND h,POINT p){Hot n=HotAt(p);if(n!=g_hover){g_hover=n;InvalidateRect(h,nullptr,FALSE);}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}
bool WriteResource(int id,const wchar_t* type,const std::filesystem::path& target) {
    HMODULE m=GetModuleHandleW(nullptr);HRSRC r=FindResourceW(m,MAKEINTRESOURCEW(id),type);if(!r)return false;HGLOBAL loaded=LoadResource(m,r);DWORD size=SizeofResource(m,r);void* data=LockResource(loaded);
    HANDLE f=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(f==INVALID_HANDLE_VALUE)return false;DWORD written=0;BOOL ok=WriteFile(f,data,size,&written,nullptr);CloseHandle(f);return ok&&written==size;
}
bool CopySelf(const std::filesystem::path& target){wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);return CopyFileW(self,target.c_str(),FALSE)!=0;}
bool CreateShortcut(const std::filesystem::path& linkPath,const std::filesystem::path& target,const std::filesystem::path& workDir) {
    IShellLinkW* link=nullptr;if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,reinterpret_cast<void**>(&link))))return false;
    link->SetPath(target.c_str());link->SetWorkingDirectory(workDir.c_str());link->SetDescription(L"\u8f7b\u91cf PDF \u9605\u8bfb\u5668");link->SetIconLocation(target.c_str(),0);
    IPersistFile* file=nullptr;HRESULT hr=link->QueryInterface(IID_IPersistFile,reinterpret_cast<void**>(&file));bool ok=false;if(SUCCEEDED(hr)){std::filesystem::create_directories(linkPath.parent_path());ok=SUCCEEDED(file->Save(linkPath.c_str(),TRUE));file->Release();}link->Release();return ok;
}
void DeleteRegValue(HKEY root,const std::wstring& path,const wchar_t* name) { HKEY key{}; if(RegOpenKeyExW(root,path.c_str(),0,KEY_SET_VALUE,&key)==ERROR_SUCCESS){ RegDeleteValueW(key,name); RegCloseKey(key); } }
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
    if(SUCCEEDED(d->Show(owner))){IShellItem* i=nullptr;if(SUCCEEDED(d->GetResult(&i))){PWSTR p=nullptr;if(SUCCEEDED(i->GetDisplayName(SIGDN_FILESYSPATH,&p))){selected=p;CoTaskMemFree(p);ok=true;}i->Release();}}d->Release();return ok;
}
UINT CALLBACK CabinetCallback(PVOID context, UINT note, UINT_PTR p1, UINT_PTR p2) {
    if(note==SPFILENOTIFY_FILEINCABINET){auto* f=reinterpret_cast<FILE_IN_CABINET_INFO_W*>(p1);auto* c=reinterpret_cast<CabinetContext*>(context);auto target=c->target/f->NameInCabinet;std::filesystem::create_directories(target.parent_path());wcsncpy_s(f->FullTargetName,ARRAYSIZE(f->FullTargetName),target.c_str(),_TRUNCATE);return FILEOP_DOIT;}
    return note==SPFILENOTIFY_NEEDNEWCABINET?FILEOP_ABORT:NO_ERROR;
}
void PostProgress(int p,const std::wstring& s){PostMessageW(g_window,kMsgProgress,reinterpret_cast<WPARAM>(new ProgressMessage{p,s}),0);}
std::wstring InstallPackage(const std::filesystem::path& request,bool desktop) {
    try{if(request.empty())return L"\u8bf7\u9009\u62e9\u5b89\u88c5\u4f4d\u7f6e\u3002";auto dir=std::filesystem::absolute(request);PostProgress(5,L"\u6b63\u5728\u51c6\u5907\u5b89\u88c5\u76ee\u5f55");std::filesystem::create_directories(dir);
    auto cab=std::filesystem::temp_directory_path()/(L"QingYuePDF-"+std::to_wstring(GetCurrentProcessId())+L".cab");if(!WriteResource(IDR_PAYLOAD_CAB,RT_RCDATA,cab))return L"\u65e0\u6cd5\u8bfb\u53d6\u5b89\u88c5\u6587\u4ef6\u3002";
    PostProgress(18,L"\u6b63\u5728\u89e3\u538b\u5b89\u88c5\u6587\u4ef6");CabinetContext ctx{dir};if(!SetupIterateCabinetW(cab.c_str(),0,CabinetCallback,reinterpret_cast<LPVOID>(&ctx))){DeleteFileW(cab.c_str());return L"\u5b89\u88c5\u6587\u4ef6\u89e3\u538b\u5931\u8d25\u3002";}DeleteFileW(cab.c_str());
    PostProgress(70,L"\u6b63\u5728\u6574\u7406\u7a0b\u5e8f\u6587\u4ef6");auto payload=dir/kPayloadExe,exe=dir/kExeName;if(!MoveFileExW(payload.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return L"\u7a0b\u5e8f\u6587\u4ef6\u6b63\u5728\u88ab\u5360\u7528\u3002";
    if(!CopySelf(dir/kUninstallName))return L"\u65e0\u6cd5\u521b\u5efa\u5378\u8f7d\u7a0b\u5e8f\u3002";PostProgress(82,L"\u6b63\u5728\u521b\u5efa\u5feb\u6377\u65b9\u5f0f");if(desktop)CreateShortcut(DesktopLink(),exe,dir);CreateShortcut(StartLink(),exe,dir);
    PostProgress(92,L"\u6b63\u5728\u6ce8\u518c PDF \u6587\u4ef6\u5173\u8054");RegisterInstallation(dir);g_installedDir=dir;PostProgress(100,L"\u5b89\u88c5\u5b8c\u6210");return {};}catch(...){return L"\u5b89\u88c5\u5931\u8d25\uff0c\u8bf7\u9009\u62e9\u6709\u5199\u5165\u6743\u9650\u7684\u76ee\u5f55\u3002";}
}
void LaunchInstalled(){if(!g_installedDir.empty()){auto exe=g_installedDir/kExeName;ShellExecuteW(g_window,L"open",exe.c_str(),nullptr,g_installedDir.c_str(),SW_SHOWNORMAL);}}
int Uninstall() {
    if(MessageBoxW(nullptr,L"\u786e\u5b9a\u8981\u5378\u8f7d\u8f7b\u9605PDF\u5417\uff1f",kAppName,MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;auto dir=RegisteredInstallDir();DeleteFileW(DesktopLink().c_str());DeleteFileW(StartLink().c_str());std::error_code ec;std::filesystem::remove(StartFolder(),ec);
    DeleteFileW((dir/kExeName).c_str());DeleteFileW((dir/kDllName).c_str());DeleteFileW((dir/kRuntimeName).c_str());RegDeleteTreeW(HKEY_CURRENT_USER,kUninstallKey);RegDeleteTreeW(HKEY_CURRENT_USER,kInstallKey);RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\Applications\\")+kExeName).c_str());RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+kProgId).c_str());
    DeleteRegValue(HKEY_CURRENT_USER,L"Software\\Classes\\.pdf\\OpenWithProgids",kProgId);DeleteRegValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",L"QingYuePDF");SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);MoveFileExW(self,nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);MessageBoxW(nullptr,L"\u8f7b\u9605PDF \u5df2\u5378\u8f7d\u3002",kAppName,MB_OK|MB_ICONINFORMATION);return 0;
}
LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m){
    case WM_CREATE:g_window=h;g_dpi=GetDpiForWindow(h);MakeFonts();g_pathEdit=CreateWindowExW(0,L"EDIT",RegisteredInstallDir().c_str(),WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(kPathEdit),g_instance,nullptr);SendMessageW(g_pathEdit,WM_SETFONT,reinterpret_cast<WPARAM>(g_uiFont),TRUE);Layout(h);return 0;
    case WM_PAINT:Paint(h);return 0;case WM_ERASEBKGND:return 1;case WM_SIZE:Layout(h);InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_CTLCOLOREDIT:{HDC dc=reinterpret_cast<HDC>(w);SetTextColor(dc,RGB(46,60,52));SetBkColor(dc,RGB(255,255,255));SetBkMode(dc,OPAQUE);static HBRUSH b=CreateSolidBrush(RGB(255,255,255));return reinterpret_cast<LRESULT>(b);}
    case WM_COMMAND:if(LOWORD(w)==kPathEdit&&(HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS)){g_pathFocused=HIWORD(w)==EN_SETFOCUS;InvalidateRect(h,nullptr,FALSE);return 0;}return 0;
    case WM_MOUSEMOVE:UpdateHover(h,{GET_X_LPARAM(l),GET_Y_LPARAM(l)});return 0;case WM_MOUSELEAVE:g_hover=Hot::None;InvalidateRect(h,nullptr,FALSE);return 0;case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;
    case WM_LBUTTONDOWN:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};Hot hot=HotAt(p);if(hot!=Hot::None){g_pressed=hot;SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}if(!g_installing&&p.y<S(78)){ReleaseCapture();SendMessageW(h,WM_NCLBUTTONDOWN,HTCAPTION,0);}return 0;}
    case WM_LBUTTONUP:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};Hot pressed=g_pressed;g_pressed=Hot::None;ReleaseCapture();if(pressed==Hot::Close&&Hit(g_close,p)){PostMessageW(h,WM_CLOSE,0,0);return 0;}if(pressed==Hot::None||pressed!=HotAt(p)||g_installing)return 0;
        if(pressed==Hot::Minimize){ShowWindow(h,SW_MINIMIZE);return 0;} if(pressed==Hot::Primary){if(g_page==Page::Welcome){g_page=Page::Options;Layout(h);InvalidateRect(h,nullptr,FALSE);}else if(g_page==Page::Options){g_page=Page::Installing;g_installing=true;g_progress=g_targetProgress=0;g_stage=L"\u6b63\u5728\u51c6\u5907";Layout(h);SetTimer(h,kTimer,16,nullptr);auto path=std::filesystem::path(WindowText(g_pathEdit));bool desktop=g_desktopShortcut;std::thread([path,desktop]{auto e=InstallPackage(path,desktop);PostMessageW(g_window,kMsgComplete,reinterpret_cast<WPARAM>(new InstallResult{e.empty(),e}),0);}).detach();InvalidateRect(h,nullptr,FALSE);}else if(g_page==Page::Complete){if(g_launchAfterInstall)LaunchInstalled();PostMessageW(h,WM_CLOSE,0,0);}else if(g_page==Page::Failed){g_page=Page::Options;g_error.clear();Layout(h);InvalidateRect(h,nullptr,FALSE);}}
        else if(pressed==Hot::Back){g_page=g_page==Page::Options?Page::Welcome:Page::Options;Layout(h);InvalidateRect(h,nullptr,FALSE);}else if(pressed==Hot::Browse){std::filesystem::path pth;if(BrowseForFolder(h,pth))SetWindowTextW(g_pathEdit,pth.c_str());}else if(pressed==Hot::Desktop){g_desktopShortcut=!g_desktopShortcut;InvalidateRect(h,nullptr,FALSE);}else if(pressed==Hot::Launch){g_launchAfterInstall=!g_launchAfterInstall;InvalidateRect(h,nullptr,FALSE);}return 0;}
    case WM_TIMER:if(w==kTimer&&g_page==Page::Installing){g_progress+=(g_targetProgress-g_progress)*.18f;if(g_targetProgress-g_progress<.003f)g_progress=g_targetProgress;g_shine=(g_shine+S(4))%std::max(1,S(100));InvalidateRect(h,nullptr,FALSE);}return 0;
    case kMsgProgress:{auto* p=reinterpret_cast<ProgressMessage*>(w);if(p){g_targetProgress=std::clamp(p->percent/100.0f,0.0f,1.0f);g_stage=std::move(p->text);delete p;}return 0;}
    case kMsgComplete:{std::unique_ptr<InstallResult> r(reinterpret_cast<InstallResult*>(w));KillTimer(h,kTimer);g_installing=false;if(r&&r->success){g_page=Page::Complete;g_progress=g_targetProgress=1;}else{g_page=Page::Failed;g_error=r?r->error:L"\u5b89\u88c5\u5931\u8d25\u3002";}Layout(h);InvalidateRect(h,nullptr,FALSE);return 0;}
    case WM_CLOSE:if(g_installing)return 0;DestroyWindow(h);return 0;case WM_DESTROY:for(HFONT f:{g_uiFont,g_smallFont,g_titleFont,g_displayFont})if(f)DeleteObject(f);PostQuitMessage(0);return 0;default:return DefWindowProcW(h,m,w,l);}
}
int ShowWizard(HINSTANCE instance,int show) {
    g_instance=instance;g_dpi=GetDpiForSystem();WNDCLASSW wc{};wc.style=CS_DBLCLKS|CS_DROPSHADOW;wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=kWindowClass;if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 1;
    HWND h=CreateWindowExW(WS_EX_APPWINDOW,kWindowClass,kAppName,WS_POPUP|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,S(680),S(470),nullptr,nullptr,instance,nullptr);if(!h)return 1;ApplyWindowChrome(h);RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);SetWindowPos(h,nullptr,work.left+(work.right-work.left-S(680))/2,work.top+(work.bottom-work.top-S(470))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);ShowWindow(h,show);UpdateWindow(h);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Gdiplus::GdiplusStartupInput in;Gdiplus::GdiplusStartup(&g_gdiplusToken,&in,nullptr);int argc=0;PWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool uninstall=false;for(int i=1;argv&&i<argc;++i)if(_wcsicmp(argv[i],L"/uninstall")==0)uninstall=true;if(argv)LocalFree(argv);int result=uninstall?Uninstall():ShowWizard(instance,show);Gdiplus::GdiplusShutdown(g_gdiplusToken);CoUninitialize();return result;
}