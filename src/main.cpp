#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "resource.h"

namespace
{
    struct fz_alloc_context;
    struct fz_context;
    struct fz_colorspace;
    struct fz_document;
    struct fz_locks_context;
    struct fz_page;
    struct fz_pixmap;

    struct fz_matrix { float a, b, c, d, e, f; };
    struct fz_rect { float x0, y0, x1, y1; };
    struct fz_point { float x, y; };
    struct fz_quad { fz_point ul, ur, ll, lr; };
    struct fz_location { int chapter; int page; };
    struct fz_outline
    {
        int refs;
        char* title;
        char* uri;
        fz_location page;
        float x, y;
        fz_outline* next;
        fz_outline* down;
        unsigned int is_open : 1;
        unsigned int flags : 7;
        unsigned int r : 8;
        unsigned int g : 8;
        unsigned int b : 8;
    };

    constexpr wchar_t const* AppName = L"轻阅PDF";

    struct Theme
    {
        const wchar_t* name;
        COLORREF bg, panel, panel2, stage, paper, text, muted, line, accent;
    };

    struct Button
    {
        int id{};
        RECT rect{};
        std::wstring text;
        std::wstring tip;
    };

    struct PageImage
    {
        int width{};
        int height{};
        std::vector<std::uint8_t> bgra;
    };

    struct TileKey
    {
        int page{};
        int zoom{};
        int theme{};
        int x{};
        int y{};
        int w{};
        int h{};

        bool operator<(TileKey const& other) const
        {
            return std::tie(page, zoom, theme, x, y, w, h) <
                std::tie(other.page, other.zoom, other.theme, other.x, other.y, other.w, other.h);
        }
    };

    struct PageInfo
    {
        double widthPt{ 612.0 };
        double heightPt{ 792.0 };
    };

    struct OutlineItem
    {
        std::wstring title;
        int page{};
        double yPt{};
        int depth{};
    };

    struct SearchResult
    {
        std::wstring title;
        std::wstring snippet;
        int page{};
        int count{};
        int occurrence{};
        fz_quad quad{};
        bool hasQuad{};
        int highlightStart{};
        int highlightLength{};
    };

    enum class AnnotationKind
    {
        Highlight = 0,
        Underline = 1,
        Strikeout = 2
    };

    struct PdfAnnotation
    {
        int page{};
        double x0{};
        double y0{};
        double x1{};
        double y1{};
        AnnotationKind kind{ AnnotationKind::Highlight };
        int colorIndex{};
        long long createdAt{};
        std::wstring text;
        std::wstring note;
    };

    struct QuietPdfAnnotation
    {
        int page{};
        int kind{};
        float x0{};
        float y0{};
        float x1{};
        float y1{};
        float colorR{ 1.0f };
        float colorG{ 0.82f };
        float colorB{ 0.24f };
        float opacity{ 0.45f };
        const char* contents{};
    };

    struct OutlineLayoutRow
    {
        int top{};
        int height{};
        int page{};
        int id{};
        int depth{};
        int textHeight{};
        bool hasChildren{};
    };

    struct PdfTab
    {
        fz_context* ctx{};
        fz_document* doc{};
        int pageCount{};
        int currentPage{};
        double zoom{ 1.0 };
        double cachedZoom{};
        unsigned cachedDpi{};
        std::wstring path;
        std::wstring name;
        std::vector<PageInfo> pages;
        std::vector<OutlineItem> outline;
        std::map<int, PageImage> cache;
        std::map<TileKey, PageImage> tileCache;
        std::map<int, PageImage> thumbCache;
        std::vector<PdfAnnotation> annotations;
        std::vector<OutlineLayoutRow> outlineLayout;
        int outlineLayoutWidth{};
        int outlineLayoutFont{};
        unsigned outlineLayoutDpi{};
        int outlineLayoutRows{};
        int outlineLayoutContentHeight{};
        int outlineLayoutCollapseVersion{};
        int cachedTheme{ -1 };
        int cachedThumbSize{};
        int cachedThumbTheme{ -1 };
        std::filesystem::file_time_type fileStamp{};
        bool hasFileStamp{};
        bool dirty{};
    };

    struct RecentItem
    {
        std::wstring path;
        std::wstring name;
        int page{ 1 };
        long long openedAt{};
    };

    enum CommandId
    {
        CmdHome = 10,
        CmdOpen,
        CmdSidebar,
        CmdSearch,
        CmdBookmark,
        CmdSettings,
        CmdTheme,
        CmdView,
        CmdZoomOut,
        CmdZoomIn,
        CmdFirst,
        CmdPrev,
        CmdNext,
        CmdLast,
        CmdImmersive,
        CmdOutlineSmall,
        CmdOutlineLarge,
        CmdOutlineToggleAll,
        CmdSearchRun,
        CmdSearchPrev,
        CmdSearchNext,
        CmdSearchClear,
        CmdZoomMenu,
        CmdStatusPin,
        CmdPageInput,
        CmdNoteExpandAll,
        CmdNoteCollapseAll,
        CmdNotePrev,
        CmdNoteNext,
        CmdNoteDelete,
        CmdNoteDeleteAll
    };

    std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), len, nullptr, nullptr);
        result.pop_back();
        return result;
    }

    std::wstring FromUtf8(const std::string& text)
    {
        if (text.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring result(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), len);
        result.pop_back();
        return result;
    }

    std::filesystem::path ExeDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    long long NowMs()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER value{};
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return static_cast<long long>((value.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    class MuPdf
    {
    public:
        using NewContextFn = fz_context*(__cdecl*)(const fz_alloc_context*, const fz_locks_context*, size_t, const char*);
        using DropContextFn = void(__cdecl*)(fz_context*);
        using RegisterHandlersFn = void(__cdecl*)(fz_context*);
        using OpenDocumentFn = fz_document*(__cdecl*)(fz_context*, const char*);
        using CountPagesFn = int(__cdecl*)(fz_context*, fz_document*);
        using DropDocumentFn = void(__cdecl*)(fz_context*, fz_document*);
        using LoadOutlineFn = fz_outline*(__cdecl*)(fz_context*, fz_document*);
        using DropOutlineFn = void(__cdecl*)(fz_context*, fz_outline*);
        using PageNumberFromLocationFn = int(__cdecl*)(fz_context*, fz_document*, fz_location);
        using SetAaLevelFn = void(__cdecl*)(fz_context*, int);
        using ScaleFn = fz_matrix(__cdecl*)(float, float);
        using LoadPageFn = fz_page*(__cdecl*)(fz_context*, fz_document*, int);
        using BoundPageFn = fz_rect(__cdecl*)(fz_context*, fz_page*);
        using DropPageFn = void(__cdecl*)(fz_context*, fz_page*);
        using DeviceRgbFn = fz_colorspace*(__cdecl*)(fz_context*);
        using NewPixmapFromPageNumberFn = fz_pixmap*(__cdecl*)(fz_context*, fz_document*, int, fz_matrix, fz_colorspace*, int);
        using NewPixmapFromPageNumberClipFn = fz_pixmap*(__cdecl*)(fz_context*, fz_document*, int, fz_matrix, fz_colorspace*, int, int, int, int, int);
        using SearchPageNumberFn = int(__cdecl*)(fz_context*, fz_document*, int, const char*, int*, fz_quad*, int);
        using SaveAnnotationsFn = int(__cdecl*)(fz_context*, fz_document*, const char*, const QuietPdfAnnotation*, int);
        using SelectTextFn = int(__cdecl*)(fz_context*, fz_document*, int, float, float, float, float, fz_quad*, int, char*, int);
        using PixmapIntFn = int(__cdecl*)(fz_context*, fz_pixmap*);
        using PixmapSamplesFn = unsigned char*(__cdecl*)(fz_context*, fz_pixmap*);
        using DropPixmapFn = void(__cdecl*)(fz_context*, fz_pixmap*);

        void Load()
        {
            if (module) return;
            module = LoadLibraryW(L"quietmupdf.dll");
            if (!module) throw std::runtime_error("quietmupdf.dll not found beside the exe.");
            Get(newContext, "fz_new_context_imp");
            Get(dropContext, "fz_drop_context");
            Get(registerHandlers, "fz_register_document_handlers");
            Get(openDocument, "fz_open_document");
            Get(countPages, "fz_count_pages");
            Get(dropDocument, "fz_drop_document");
            Get(loadOutline, "fz_load_outline");
            Get(dropOutline, "fz_drop_outline");
            Get(pageNumberFromLocation, "fz_page_number_from_location");
            Get(setAaLevel, "fz_set_aa_level");
            Get(scale, "fz_scale");
            Get(loadPage, "fz_load_page");
            Get(boundPage, "fz_bound_page");
            Get(dropPage, "fz_drop_page");
            Get(deviceRgb, "fz_device_rgb");
            Get(newPixmapFromPageNumber, "fz_new_pixmap_from_page_number");
            Get(newPixmapFromPageNumberClip, "quiet_new_pixmap_from_page_number_clip");
            Get(searchPageNumber, "fz_search_page_number");
            Get(saveAnnotations, "quiet_pdf_save_annotations");
            Get(selectText, "quiet_pdf_select_text");
            Get(pixmapWidth, "fz_pixmap_width");
            Get(pixmapHeight, "fz_pixmap_height");
            Get(pixmapComponents, "fz_pixmap_components");
            Get(pixmapStride, "fz_pixmap_stride");
            Get(pixmapSamples, "fz_pixmap_samples");
            Get(dropPixmap, "fz_drop_pixmap");
        }

        HMODULE module{};
        NewContextFn newContext{};
        DropContextFn dropContext{};
        RegisterHandlersFn registerHandlers{};
        OpenDocumentFn openDocument{};
        CountPagesFn countPages{};
        DropDocumentFn dropDocument{};
        LoadOutlineFn loadOutline{};
        DropOutlineFn dropOutline{};
        PageNumberFromLocationFn pageNumberFromLocation{};
        SetAaLevelFn setAaLevel{};
        ScaleFn scale{};
        LoadPageFn loadPage{};
        BoundPageFn boundPage{};
        DropPageFn dropPage{};
        DeviceRgbFn deviceRgb{};
        NewPixmapFromPageNumberFn newPixmapFromPageNumber{};
        NewPixmapFromPageNumberClipFn newPixmapFromPageNumberClip{};
        SearchPageNumberFn searchPageNumber{};
        SaveAnnotationsFn saveAnnotations{};
        SelectTextFn selectText{};
        PixmapIntFn pixmapWidth{};
        PixmapIntFn pixmapHeight{};
        PixmapIntFn pixmapComponents{};
        PixmapIntFn pixmapStride{};
        PixmapSamplesFn pixmapSamples{};
        DropPixmapFn dropPixmap{};

    private:
        template <typename T>
        void Get(T& target, const char* name)
        {
            target = reinterpret_cast<T>(GetProcAddress(module, name));
            if (!target) throw std::runtime_error(std::string("Missing MuPDF export: ") + name);
        }
    };

    MuPdf g_pdf;
    HWND g_hwnd{};
    unsigned g_dpi{ 96 };
    std::vector<std::unique_ptr<PdfTab>> g_tabs;
    std::vector<RecentItem> g_recent;
    std::map<std::wstring, PageImage> g_homeThumbCache;
    std::vector<Button> g_buttons;
    std::vector<RECT> g_tabRects;
    std::vector<RECT> g_recentRects;
    std::vector<int> g_sidebarRowPages;
    std::vector<RECT> g_sidebarRowRects;
    std::vector<int> g_sidebarRowIds;
    std::vector<RECT> g_outlineToggleRects;
    std::vector<int> g_outlineToggleRowIds;
    std::vector<RECT> g_sideTabRects;
    std::vector<RECT> g_searchResultRects;
    std::vector<int> g_searchResultIds;
    constexpr UINT WM_QUIETPDF_FILE_CHANGED = WM_APP + 41;
    std::mutex g_fileWatchMutex;
    std::thread g_fileWatchThread;
    HANDLE g_fileWatchDirHandle{};
    HANDLE g_fileWatchStopEvent{};
    std::atomic<unsigned long> g_fileWatchGeneration{ 0 };
    RECT g_mainScrollTrackRect{};
    RECT g_mainScrollThumbRect{};
    RECT g_stage{};
    RECT g_sidebar{};
    RECT g_searchInputRect{};
    RECT g_thumbSliderRect{};
    WINDOWPLACEMENT g_preImmersivePlacement{ sizeof(WINDOWPLACEMENT) };
    LONG_PTR g_preImmersiveStyle{};
    LONG_PTR g_preImmersiveExStyle{};
    int g_active{ -1 };
    int g_scrollY{};
    int g_stageScrollTarget{};
    int g_contentHeight{};
    int g_sidebarScrollY{};
    int g_sidebarScrollTargetY{};
    int g_sidebarContentHeight{};
    int g_sidebarWidth{ 520 };
    int g_sidebarTargetWidth{ 520 };
    int g_sidebarExpandedWidth{ 520 };
    int g_sidebarAnimationStartWidth{};
    int g_outlineFont{ 10 };
    int g_sideMode{}; // 0 outline, 1 thumbnails, 2 search
    int g_thumbSize{ 116 };
    constexpr int ThumbSizeMin = 82;
    constexpr int ThumbSizeMax = 252;
    int g_homeThumbWidth{};
    int g_theme{};
    int g_viewMode{};
    int g_contextTab{ -1 };
    int g_searchCurrent{};
    int g_selectedOutlineRow{ -1 };
    int g_hoverTab{ -1 };
    int g_hoverTabClose{ -1 };
    int g_hoverOutlineRow{ -1 };
    int g_hoverOutlineToggle{ -1 };
    int g_hoverSearchResult{ -1 };
    int g_outlineCollapseVersion{};
    RECT g_pressedButtonRect{};
    RECT g_hoverButtonRect{};
    RECT g_hoverTipRect{};
    RECT g_pageInputRect{};
    POINT g_mousePos{};
    bool g_sidebarOpen{ true };
    bool g_immersive{};
    bool g_hasPreImmersivePlacement{};
    bool g_resizingSidebar{};
    bool g_draggingThumbSlider{};
    bool g_searchInputFocused{};
    bool g_searchHasRun{};
    bool g_searchCaretVisible{ true };
    bool g_hasPressedButton{};
    bool g_hasHoverButton{};
    bool g_hasHoverTip{};
    bool g_pageInputFocused{};
    bool g_pageCaretVisible{ true };
    bool g_smoothStageScroll{};
    bool g_smoothSidebarScroll{};
    bool g_statusPinned{ true };
    bool g_statusReveal{};
    bool g_draggingMainScroll{};
    bool g_mainScrollHover{};
    bool g_stageRepaintPending{};
    bool g_sidebarRepaintPending{};
    bool g_sidebarResizePending{};
    bool g_sidebarAnimating{};
    bool g_gesturePanning{};
    bool g_gestureSidebar{};
    ULONGLONG g_lastStageRepaintMs{};
    ULONGLONG g_lastSidebarRepaintMs{};
    ULONGLONG g_lastSidebarResizeMs{};
    ULONGLONG g_sidebarAnimationStartMs{};
    ULONGLONG g_deferPageRenderUntil{};
    HBITMAP g_glassBackdrop{};
    int g_glassBackdropW{};
    int g_glassBackdropH{};
    int g_glassBackdropTheme{ -1 };
    int g_statusOffset{};
    int g_statusTargetOffset{};
    int g_pageWheelDeltaAccum{};
    int g_gestureLastY{};
    double g_stageWheelRemainder{};
    double g_sidebarWheelRemainder{};
    ULONG_PTR g_gdiplusToken{};
    std::wstring g_hoverTip;
    std::wstring g_searchQuery;
    std::wstring g_searchLastQuery;
    std::wstring g_pageInputText;
    std::vector<SearchResult> g_searchResults;
    std::set<int> g_collapsedOutlineRows;
    int g_mainScrollDragOffset{};
    bool g_selectingContent{};
    bool g_hasContentSelection{};
    int g_selectionPage{ -1 };
    POINT g_selectionStart{};
    POINT g_selectionEnd{};
    RECT g_selectionPageRect{};
    fz_rect g_selectionNorm{};
    std::vector<fz_quad> g_selectionQuads;
    std::wstring g_selectionText;
    int g_activeAnnotation{ -1 };
    bool g_annotationToolbarOpen{};
    bool g_annotationNoteOpen{};
    bool g_annotationNoteFocused{};
    bool g_annotationNoteCaretVisible{ true };
    RECT g_annotationHitRect{};
    RECT g_annotationToolbarRect{};
    RECT g_annotationCopyRect{};
    RECT g_annotationNoteRect{};
    RECT g_annotationDeleteRect{};
    RECT g_annotationFormatRect{};
    RECT g_annotationNoteCloseRect{};
    RECT g_annotationNotePanelRect{};
    RECT g_annotationNoteInputRect{};
    std::vector<RECT> g_annotationColorRects;
    std::set<int> g_collapsedAnnotationPages;
    bool g_continuousRead{ true };
    constexpr int TileSize = 384;

    Theme g_themes[] = {
        { L"云母白", RGB(244,247,250), RGB(255,255,255), RGB(250,252,254), RGB(238,243,247), RGB(255,255,255), RGB(31,35,40), RGB(105,113,125), RGB(210,217,226), RGB(59,170,124) },
        { L"护眼米白", RGB(236,231,216), RGB(245,240,221), RGB(255,252,240), RGB(225,217,196), RGB(255,249,233), RGB(45,39,29), RGB(124,113,95), RGB(202,191,166), RGB(124,143,66) },
        { L"极夜黑", RGB(12,14,20), RGB(25,29,38), RGB(38,44,56), RGB(8,10,15), RGB(30,34,43), RGB(243,246,251), RGB(169,178,191), RGB(58,65,78), RGB(126,162,255) },
        { L"石墨黑", RGB(29,30,32), RGB(48,49,52), RGB(59,60,64), RGB(20,20,21), RGB(43,42,39), RGB(244,240,232), RGB(184,177,168), RGB(78,79,84), RGB(240,189,103) },
        { L"极光绿", RGB(4,19,24), RGB(10,37,43), RGB(18,58,62), RGB(5,23,27), RGB(7,31,36), RGB(231,255,244), RGB(142,194,184), RGB(31,75,79), RGB(45,245,143) }
    };

    Theme const& T()
    {
        return g_themes[std::clamp(g_theme, 0, static_cast<int>(std::size(g_themes)) - 1)];
    }

    bool IsGlassTheme()
    {
        return g_theme == 0;
    }

    bool IsAuroraTheme()
    {
        return g_theme == 4;
    }

    bool UsesBackdropTheme()
    {
        return IsGlassTheme() || IsAuroraTheme();
    }

    int Luma(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        return (static_cast<int>(r) * 299 + static_cast<int>(g) * 587 + static_cast<int>(b) * 114) / 1000;
    }

    std::uint8_t MixByte(std::uint8_t a, std::uint8_t b, double t)
    {
        t = std::clamp(t, 0.0, 1.0);
        return static_cast<std::uint8_t>(static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * t + 0.5);
    }

    COLORREF MixColor(COLORREF a, COLORREF b, double t)
    {
        return RGB(
            MixByte(static_cast<std::uint8_t>(GetRValue(a)), static_cast<std::uint8_t>(GetRValue(b)), t),
            MixByte(static_cast<std::uint8_t>(GetGValue(a)), static_cast<std::uint8_t>(GetGValue(b)), t),
            MixByte(static_cast<std::uint8_t>(GetBValue(a)), static_cast<std::uint8_t>(GetBValue(b)), t));
    }

    bool IsDarkTheme()
    {
        return g_theme >= 2;
    }

    COLORREF ForegroundOn(COLORREF bg)
    {
        return Luma(static_cast<std::uint8_t>(GetRValue(bg)), static_cast<std::uint8_t>(GetGValue(bg)), static_cast<std::uint8_t>(GetBValue(bg))) < 148
            ? RGB(255, 255, 255)
            : T().text;
    }

    COLORREF SidebarHoverFill()
    {
        if (IsAuroraTheme()) return MixColor(T().panel2, T().accent, 0.12);
        if (IsGlassTheme()) return MixColor(T().panel2, T().accent, 0.08);
        if (IsDarkTheme()) return MixColor(T().panel2, T().accent, 0.16);
        return MixColor(T().panel2, T().accent, 0.10);
    }

    COLORREF SidebarSelectedFill()
    {
        if (IsAuroraTheme()) return MixColor(T().accent, T().panel2, 0.34);
        if (IsGlassTheme()) return MixColor(T().accent, RGB(255, 255, 255), 0.24);
        if (IsDarkTheme()) return MixColor(T().accent, T().panel2, 0.42);
        return MixColor(T().accent, T().paper, 0.30);
    }

    COLORREF SidebarTreeColor()
    {
        if (IsAuroraTheme()) return MixColor(T().line, T().accent, 0.30);
        if (IsDarkTheme()) return MixColor(T().line, T().muted, 0.28);
        return MixColor(T().line, T().muted, 0.44);
    }

    COLORREF SidebarToggleFill(bool hovered)
    {
        if (IsAuroraTheme()) return hovered ? MixColor(T().panel2, T().accent, 0.30) : MixColor(T().panel2, T().accent, 0.16);
        if (IsDarkTheme()) return hovered ? MixColor(T().panel2, T().text, 0.18) : MixColor(T().panel2, T().line, 0.12);
        return hovered ? MixColor(T().panel2, T().text, 0.12) : MixColor(T().panel2, T().line, 0.08);
    }

    COLORREF SidebarToggleLine(bool hovered)
    {
        if (IsAuroraTheme()) return hovered ? MixColor(T().accent, RGB(255, 255, 255), 0.18) : MixColor(T().accent, T().text, 0.20);
        return hovered ? MixColor(T().text, T().muted, 0.22) : MixColor(T().muted, T().text, 0.16);
    }

    COLORREF SidebarIconColor()
    {
        if (IsAuroraTheme()) return MixColor(T().muted, T().accent, 0.34);
        return MixColor(T().muted, T().text, IsDarkTheme() ? 0.18 : 0.10);
    }

    constexpr COLORREF AnnotationPalette[] = {
        RGB(255, 236, 0), RGB(49, 211, 68), RGB(82, 174, 232), RGB(244, 97, 142),
        RGB(156, 120, 229), RGB(255, 157, 50), RGB(20, 211, 214), RGB(235, 82, 207)
    };

    COLORREF AnnotationColor(PdfAnnotation const& annotation)
    {
        int index = std::clamp(annotation.colorIndex, 0, static_cast<int>(std::size(AnnotationPalette)) - 1);
        return AnnotationPalette[index];
    }

    std::wstring AnnotationKindName(AnnotationKind kind)
    {
        if (kind == AnnotationKind::Underline) return L"\u4e0b\u5212\u7ebf";
        if (kind == AnnotationKind::Strikeout) return L"\u5220\u9664\u7ebf";
        return L"\u9ad8\u4eae";
    }

    std::wstring FormatTimeMs(long long ms)
    {
        std::time_t seconds = static_cast<std::time_t>(ms / 1000);
        std::tm local{};
        localtime_s(&local, &seconds);
        wchar_t buffer[32]{};
        wcsftime(buffer, std::size(buffer), L"%Y/%m/%d %H:%M:%S", &local);
        return buffer;
    }

    COLORREF ScrollTrackColor()
    {
        if (IsAuroraTheme()) return MixColor(T().bg, T().panel, 0.58);
        if (IsGlassTheme()) return MixColor(T().panel, RGB(255, 255, 255), 0.34);
        if (IsDarkTheme()) return MixColor(T().bg, T().panel, 0.64);
        return MixColor(T().bg, T().panel, 0.42);
    }

    COLORREF ScrollThumbColor(bool active)
    {
        if (active) return T().accent;
        if (IsAuroraTheme()) return MixColor(T().accent, T().paper, 0.26);
        if (IsDarkTheme()) return MixColor(T().muted, T().text, 0.22);
        return MixColor(T().muted, T().text, 0.14);
    }

    bool Hit(RECT r, int x, int y);
    bool Intersects(RECT a, RECT b);
    void RoundFill(HDC dc, RECT r, int radius, COLORREF fill, COLORREF line);

    void ApplyPdfTheme(std::uint8_t& b, std::uint8_t& g, std::uint8_t& r)
    {
        if (g_theme == 0) return;

        int lum = Luma(r, g, b);
        COLORREF paper = T().paper;
        COLORREF text = T().text;
        int maxc = std::max<int>(r, std::max<int>(g, b));
        int minc = std::min<int>(r, std::min<int>(g, b));
        int sat = maxc - minc;

        if ((g_theme == 2 || g_theme == 3 || g_theme == 4) && sat < 28)
        {
            double t = static_cast<double>(lum) / 255.0;
            r = MixByte(static_cast<std::uint8_t>(GetRValue(text)), static_cast<std::uint8_t>(GetRValue(paper)), t);
            g = MixByte(static_cast<std::uint8_t>(GetGValue(text)), static_cast<std::uint8_t>(GetGValue(paper)), t);
            b = MixByte(static_cast<std::uint8_t>(GetBValue(text)), static_cast<std::uint8_t>(GetBValue(paper)), t);
            return;
        }

        if (lum > 185)
        {
            double t = static_cast<double>(lum - 185) / 70.0;
            r = MixByte(r, static_cast<std::uint8_t>(GetRValue(paper)), t);
            g = MixByte(g, static_cast<std::uint8_t>(GetGValue(paper)), t);
            b = MixByte(b, static_cast<std::uint8_t>(GetBValue(paper)), t);
        }

        if (Luma(static_cast<std::uint8_t>(GetRValue(paper)), static_cast<std::uint8_t>(GetGValue(paper)), static_cast<std::uint8_t>(GetBValue(paper))) < 90 &&
            Luma(static_cast<std::uint8_t>(GetRValue(text)), static_cast<std::uint8_t>(GetGValue(text)), static_cast<std::uint8_t>(GetBValue(text))) > 150 &&
            lum < 95)
        {
            double t = static_cast<double>(95 - lum) / 95.0 * 0.88;
            r = MixByte(r, static_cast<std::uint8_t>(GetRValue(text)), t);
            g = MixByte(g, static_cast<std::uint8_t>(GetGValue(text)), t);
            b = MixByte(b, static_cast<std::uint8_t>(GetBValue(text)), t);
        }
    }

    int S(int value)
    {
        return MulDiv(value, static_cast<int>(g_dpi), 96);
    }

    float S(float value)
    {
        return value * static_cast<float>(g_dpi) / 96.0f;
    }

    int EffectiveOutlineFont()
    {
        return std::max(8, g_outlineFont - 1);
    }

    HFONT Font(int pt, int weight = FW_NORMAL)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(g_dpi), 72);
        lf.lfWeight = weight;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        return CreateFontIndirectW(&lf);
    }

    void Fill(HDC dc, RECT r, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }

    void LinearGradient(HDC dc, RECT r, COLORREF from, COLORREF to, bool vertical)
    {
        if (r.right <= r.left || r.bottom <= r.top) return;
        TRIVERTEX vertex[2]{};
        vertex[0].x = r.left;
        vertex[0].y = r.top;
        vertex[0].Red = GetRValue(from) << 8;
        vertex[0].Green = GetGValue(from) << 8;
        vertex[0].Blue = GetBValue(from) << 8;
        vertex[0].Alpha = 0xffff;
        vertex[1].x = r.right;
        vertex[1].y = r.bottom;
        vertex[1].Red = GetRValue(to) << 8;
        vertex[1].Green = GetGValue(to) << 8;
        vertex[1].Blue = GetBValue(to) << 8;
        vertex[1].Alpha = 0xffff;
        GRADIENT_RECT mesh{ 0, 1 };
        GradientFill(dc, vertex, 2, &mesh, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
    }

    void AlphaFill(HDC dc, RECT r, COLORREF color, BYTE alpha)
    {
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;

        HDC mem = CreateCompatibleDC(dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
        {
            DeleteDC(mem);
            return;
        }
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH brush = CreateSolidBrush(color);
        RECT local{ 0, 0, w, h };
        FillRect(mem, &local, brush);
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, 0 };
        AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, w, h, blend);
        DeleteObject(brush);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void PaintGlassBackdrop(HDC dc, RECT r)
    {
        RECT client{};
        if (g_hwnd) GetClientRect(g_hwnd, &client);
        int fullH = std::max<int>(1, client.bottom - client.top);
        int fullW = std::max<int>(1, client.right - client.left);
        if (!g_glassBackdrop || g_glassBackdropW != fullW || g_glassBackdropH != fullH || g_glassBackdropTheme != g_theme)
        {
            if (g_glassBackdrop)
            {
                DeleteObject(g_glassBackdrop);
                g_glassBackdrop = nullptr;
            }
            HDC mem = CreateCompatibleDC(dc);
            g_glassBackdrop = CreateCompatibleBitmap(dc, fullW, fullH);
            if (!g_glassBackdrop)
            {
                DeleteDC(mem);
                Fill(dc, r, T().bg);
                return;
            }
            HGDIOBJ oldBmp = SelectObject(mem, g_glassBackdrop);
            RECT full{ 0, 0, fullW, fullH };
            if (IsAuroraTheme())
            {
                Gdiplus::Graphics g(mem);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                std::filesystem::path auroraPath = ExeDir() / L"aurora-theme.jpg";
                Gdiplus::Image auroraImage(auroraPath.c_str());
                if (auroraImage.GetLastStatus() == Gdiplus::Ok)
                {
                    double srcW = std::max<UINT>(1, auroraImage.GetWidth());
                    double srcH = std::max<UINT>(1, auroraImage.GetHeight());
                    double scale = std::max(static_cast<double>(fullW) / srcW, static_cast<double>(fullH) / srcH);
                    double cropW = static_cast<double>(fullW) / scale;
                    double cropH = static_cast<double>(fullH) / scale;
                    double srcX = std::max(0.0, (srcW - cropW) * 0.54);
                    double srcY = std::max(0.0, (srcH - cropH) * 0.36);
                    if (srcX + cropW > srcW) srcX = srcW - cropW;
                    if (srcY + cropH > srcH) srcY = srcH - cropH;
                    g.DrawImage(
                        &auroraImage,
                        Gdiplus::Rect(0, 0, fullW, fullH),
                        static_cast<Gdiplus::REAL>(srcX),
                        static_cast<Gdiplus::REAL>(srcY),
                        static_cast<Gdiplus::REAL>(cropW),
                        static_cast<Gdiplus::REAL>(cropH),
                        Gdiplus::UnitPixel);
                }
                else
                {
                    for (int y = 0; y < fullH; ++y)
                    {
                        double ty = static_cast<double>(y) / static_cast<double>(fullH);
                        COLORREF color = MixColor(RGB(2, 16, 21), RGB(7, 34, 39), ty);
                        if (ty > 0.60) color = MixColor(color, RGB(4, 12, 17), 0.48);
                        RECT line{ 0, y, fullW, y + 1 };
                        Fill(mem, line, color);
                    }
                }
                AlphaFill(mem, full, RGB(0, 8, 12), 14);
            }
            else for (int y = 0; y < fullH; ++y)
            {
                double ty = static_cast<double>(y) / static_cast<double>(fullH);
                COLORREF color = MixColor(RGB(247, 250, 252), RGB(232, 239, 246), ty);
                if (ty < 0.44) color = MixColor(color, RGB(255, 248, 241), 0.20);
                else if (ty > 0.58) color = MixColor(color, RGB(229, 242, 253), 0.24);
                RECT line{ 0, y, fullW, y + 1 };
                Fill(mem, line, color);
            }
            if (!IsAuroraTheme())
            {
                RECT leftWash{ 0, 0, std::max(1, fullW / 3), fullH };
                RECT topWash{ 0, 0, fullW, std::max(1, fullH / 3) };
                AlphaFill(mem, leftWash, RGB(226, 244, 239), 30);
                AlphaFill(mem, topWash, RGB(255, 255, 255), 42);
            }
            SelectObject(mem, oldBmp);
            DeleteDC(mem);
            g_glassBackdropW = fullW;
            g_glassBackdropH = fullH;
            g_glassBackdropTheme = g_theme;
        }

        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, g_glassBackdrop);
        BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, r.left - client.left, r.top - client.top, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    }

    void GlassFill(HDC dc, RECT r, COLORREF fallback, BYTE alpha = 205)
    {
        if (IsGlassTheme())
        {
            AlphaFill(dc, r, RGB(255, 255, 255), std::min<BYTE>(alpha, 166));
            int halfH = std::max<int>(1, static_cast<int>(r.bottom - r.top) / 2);
            RECT topWash{ r.left, r.top, r.right, r.top + halfH };
            AlphaFill(dc, topWash, RGB(255, 255, 255), 56);
            RECT coolWash{ r.left, r.top + halfH, r.right, r.bottom };
            AlphaFill(dc, coolWash, RGB(229, 242, 253), 18);
            HPEN highlight = CreatePen(PS_SOLID, std::max(1, S(1)), RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(dc, highlight);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(highlight);
        }
        else if (IsAuroraTheme())
        {
            AlphaFill(dc, r, RGB(6, 32, 37), std::min<BYTE>(alpha, 184));
            int halfH = std::max<int>(1, static_cast<int>(r.bottom - r.top) / 2);
            RECT topWash{ r.left, r.top, r.right, r.top + halfH };
            AlphaFill(dc, topWash, RGB(42, 245, 143), 22);
            HPEN highlight = CreatePen(PS_SOLID, std::max(1, S(1)), RGB(37, 112, 105));
            HGDIOBJ oldPen = SelectObject(dc, highlight);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(highlight);
        }
        else Fill(dc, r, fallback);
    }

    void GlassSurface(HDC dc, RECT r, BYTE alpha = 150)
    {
        if (IsAuroraTheme())
        {
            AlphaFill(dc, r, RGB(5, 31, 36), std::max<BYTE>(alpha, 150));
            RECT top{ r.left, r.top, r.right, r.top + std::max<int>(1, (r.bottom - r.top) / 2) };
            AlphaFill(dc, top, RGB(36, 240, 141), 18);
            return;
        }
        if (!IsGlassTheme())
        {
            Fill(dc, r, T().panel);
            return;
        }
        AlphaFill(dc, r, RGB(255, 255, 255), alpha);
        RECT top{ r.left, r.top, r.right, r.top + std::max<int>(1, (r.bottom - r.top) / 2) };
        AlphaFill(dc, top, RGB(255, 255, 255), 34);
    }

    void GlassDivider(HDC dc, RECT r)
    {
        Fill(dc, r, IsAuroraTheme() ? RGB(29, 93, 88) : (IsGlassTheme() ? RGB(211, 218, 226) : T().line));
    }

    void UpdateMainScrollRects(RECT client)
    {
        g_mainScrollTrackRect = {};
        g_mainScrollThumbRect = {};
        if (g_immersive || g_active < 0) return;
        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        if (g_contentHeight <= stageH + 1) return;
        int trackW = S(12);
        int gutter = S(3);
        g_mainScrollTrackRect = RECT{ client.right - gutter - trackW, g_stage.top + S(8), client.right - gutter, g_stage.bottom - S(8) };
        int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
        int thumbH = std::clamp(MulDiv(stageH, trackH, std::max(1, g_contentHeight)), S(28), trackH);
        int maxScroll = std::max<int>(1, g_contentHeight - stageH);
        int thumbTravel = std::max<int>(0, trackH - thumbH);
        int thumbTop = g_mainScrollTrackRect.top + MulDiv(g_scrollY, thumbTravel, maxScroll);
        if (g_scrollY >= maxScroll) thumbTop = g_mainScrollTrackRect.bottom - thumbH;
        g_mainScrollThumbRect = RECT{ g_mainScrollTrackRect.left, thumbTop, g_mainScrollTrackRect.right, thumbTop + thumbH };
    }

    void PaintMainScrollBar(HDC dc, RECT client, RECT paintClip)
    {
        UpdateMainScrollRects(client);
        if (IsRectEmpty(&g_mainScrollTrackRect)) return;
        if (!Intersects(paintClip, g_mainScrollTrackRect) && !Intersects(paintClip, g_mainScrollThumbRect)) return;
        bool active = g_draggingMainScroll || Hit(g_mainScrollTrackRect, g_mousePos.x, g_mousePos.y);
        int centerX = (g_mainScrollTrackRect.left + g_mainScrollTrackRect.right) / 2;
        int trackW = active ? S(7) : std::max(2, S(3));
        RECT visualThumb{ centerX - trackW / 2, g_mainScrollThumbRect.top, centerX + (trackW + 1) / 2, g_mainScrollThumbRect.bottom };
        RoundFill(dc, visualThumb, S(5), ScrollThumbColor(active), ScrollThumbColor(active));
    }

    void RoundFill(HDC dc, RECT r, int radius, COLORREF fill, COLORREF line)
    {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, line);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    Gdiplus::Color GdiColor(COLORREF c, BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    Gdiplus::RectF Rf(RECT r)
    {
        return Gdiplus::RectF(
            static_cast<Gdiplus::REAL>(r.left),
            static_cast<Gdiplus::REAL>(r.top),
            static_cast<Gdiplus::REAL>(r.right - r.left),
            static_cast<Gdiplus::REAL>(r.bottom - r.top));
    }

    void DrawGdiRoundRect(Gdiplus::Graphics& g, Gdiplus::Pen& pen, Gdiplus::RectF r, float radius)
    {
        float d = radius * 2.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
        g.DrawPath(&pen, &path);
    }

    void FillGdiRoundRect(Gdiplus::Graphics& g, Gdiplus::Brush& brush, Gdiplus::RectF r, float radius)
    {
        float d = radius * 2.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
        g.FillPath(&brush, &path);
    }

    void DrawRoundedShadow(HDC dc, RECT r, float radius, int offsetX, int offsetY, int spread, BYTE alpha)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF shadow = RGB(142, 154, 168);
        for (int i = spread; i >= 1; --i)
        {
            double t = static_cast<double>(i) / static_cast<double>(std::max(1, spread));
            BYTE layerAlpha = static_cast<BYTE>(static_cast<double>(alpha) * (1.0 - t) * 0.42 + 0.5);
            if (layerAlpha == 0) continue;
            RECT layer{
                r.left + offsetX - i,
                r.top + offsetY - i,
                r.right + offsetX + i,
                r.bottom + offsetY + i
            };
            Gdiplus::SolidBrush brush(GdiColor(shadow, layerAlpha));
            FillGdiRoundRect(g, brush, Rf(layer), radius + S(static_cast<float>(i)));
        }
    }

    void DrawContactShadow(HDC dc, RECT r, BYTE alpha)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF shadow = RGB(134, 146, 160);
        for (int i = 3; i >= 1; --i)
        {
            RECT layer{
                r.left + S(10) - S(i),
                r.bottom + S(1),
                r.right + S(8) + S(i),
                r.bottom + S(3) + S(i)
            };
            BYTE layerAlpha = static_cast<BYTE>(alpha / (i + 4));
            Gdiplus::SolidBrush brush(GdiColor(shadow, layerAlpha));
            FillGdiRoundRect(g, brush, Rf(layer), S(4.0f + i));
        }
    }

    void PaintLiquidGlassPill(HDC dc, RECT r)
    {
        float radius = static_cast<float>(r.bottom - r.top) * 0.48f;
        DrawRoundedShadow(dc, r, radius, S(1), S(2), S(3), 26);

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF pill = Rf(r);
        COLORREF base = IsDarkTheme() ? RGB(25, 32, 42) : RGB(255, 255, 255);
        COLORREF edge = IsDarkTheme() ? RGB(92, 112, 126) : RGB(205, 216, 228);
        Gdiplus::SolidBrush fill(GdiColor(base, IsDarkTheme() ? 138 : 126));
        FillGdiRoundRect(g, fill, pill, radius);

        RECT top{ r.left + S(2), r.top + S(1), r.right - S(2), r.top + std::max<int>(S(8), (r.bottom - r.top) / 2) };
        Gdiplus::SolidBrush topWash(GdiColor(RGB(255, 255, 255), IsDarkTheme() ? 26 : 78));
        FillGdiRoundRect(g, topWash, Rf(top), std::max(1.0f, radius - S(2.0f)));

        RECT lower{ r.left + S(4), r.top + (r.bottom - r.top) / 2, r.right - S(4), r.bottom - S(2) };
        Gdiplus::SolidBrush lowerTint(GdiColor(IsAuroraTheme() ? RGB(80, 255, 176) : RGB(178, 212, 246), IsDarkTheme() ? 14 : 22));
        FillGdiRoundRect(g, lowerTint, Rf(lower), std::max(1.0f, radius - S(3.0f)));

        RECT shine{ r.left + S(12), r.top + S(4), r.left + S(72), r.top + S(9) };
        Gdiplus::SolidBrush shineBrush(GdiColor(RGB(255, 255, 255), IsDarkTheme() ? 30 : 88));
        FillGdiRoundRect(g, shineBrush, Rf(shine), S(4.0f));

        Gdiplus::Pen border(GdiColor(edge, IsDarkTheme() ? 120 : 150), std::max<Gdiplus::REAL>(1.0f, S(1.0f)));
        DrawGdiRoundRect(g, border, pill, radius);
    }

    void GlassRoundedSurface(HDC dc, RECT r, BYTE alpha, float radius)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF base = IsAuroraTheme() ? RGB(7, 34, 39) : (IsGlassTheme() ? RGB(255, 255, 255) : T().panel);
        COLORREF line = IsAuroraTheme() ? RGB(43, 112, 105) : (IsGlassTheme() ? RGB(207, 216, 226) : T().line);
        Gdiplus::SolidBrush fill(GdiColor(base, alpha));
        Gdiplus::Pen border(GdiColor(line, IsGlassTheme() ? 170 : 140), std::max<Gdiplus::REAL>(1.0f, S(1.0f)));
        FillGdiRoundRect(g, fill, Rf(r), radius);

        RECT top{ r.left + S(1), r.top + S(1), r.right - S(1), r.top + std::max<int>(S(2), (r.bottom - r.top) / 2) };
        Gdiplus::SolidBrush wash(GdiColor(IsAuroraTheme() ? RGB(66, 255, 163) : RGB(255, 255, 255), IsAuroraTheme() ? 20 : 42));
        FillGdiRoundRect(g, wash, Rf(top), std::max(1.0f, radius - S(1.0f)));
        DrawGdiRoundRect(g, border, Rf(r), radius);
    }

    void DrawSoftCircle(HDC dc, int cx, int cy, int radius, COLORREF color)
    {
        int scale = 4;
        int pad = 2;
        int size = (radius * 2 + pad * 2) * scale;
        if (size <= 0) return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return;

        auto* px = static_cast<std::uint32_t*>(bits);
        double center = size / 2.0 - 0.5;
        double rr = radius * scale;
        BYTE r = GetRValue(color);
        BYTE g = GetGValue(color);
        BYTE b = GetBValue(color);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                double dx = x - center;
                double dy = y - center;
                double dist = std::sqrt(dx * dx + dy * dy);
                double alpha = std::clamp(rr + 1.2 - dist, 0.0, 1.0);
                BYTE a = static_cast<BYTE>(alpha * 255.0 + 0.5);
                px[y * size + x] =
                    (static_cast<std::uint32_t>(a) << 24) |
                    (static_cast<std::uint32_t>(r * a / 255) << 16) |
                    (static_cast<std::uint32_t>(g * a / 255) << 8) |
                    static_cast<std::uint32_t>(b * a / 255);
            }
        }

        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, bmp);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        int destSize = size / scale;
        AlphaBlend(dc, cx - destSize / 2, cy - destSize / 2, destSize, destSize, mem, 0, 0, size, size, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }

    void Text(HDC dc, const std::wstring& s, RECT r, UINT flags, COLORREF color)
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, s.c_str(), -1, &r, flags);
    }

    void DrawImageFit(HDC dc, const PageImage& image, RECT box)
    {
        if (image.bgra.empty() || image.width <= 0 || image.height <= 0) return;

        int boxW = std::max<int>(1, box.right - box.left);
        int boxH = std::max<int>(1, box.bottom - box.top);
        double scale = std::min(
            static_cast<double>(boxW) / static_cast<double>(image.width),
            static_cast<double>(boxH) / static_cast<double>(image.height));
        int w = std::max(1, static_cast<int>(image.width * scale + 0.5));
        int h = std::max(1, static_cast<int>(image.height * scale + 0.5));
        RECT dest{
            box.left + (boxW - w) / 2,
            box.top + (boxH - h) / 2,
            box.left + (boxW - w) / 2 + w,
            box.top + (boxH - h) / 2 + h
        };

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        int oldMode = SetStretchBltMode(dc, HALFTONE);
        POINT oldBrush{};
        SetBrushOrgEx(dc, 0, 0, &oldBrush);
        StretchDIBits(dc, dest.left, dest.top, w, h, 0, 0, image.width, image.height, image.bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        SetBrushOrgEx(dc, oldBrush.x, oldBrush.y, nullptr);
        SetStretchBltMode(dc, oldMode);
    }

    int MeasureTextHeight(HDC dc, const std::wstring& s, int width, UINT flags)
    {
        RECT r{ 0, 0, std::max<int>(1, width), 0 };
        DrawTextW(dc, s.c_str(), -1, &r, flags | DT_CALCRECT);
        return std::max<int>(1, r.bottom - r.top);
    }

    void EnsureOutlineLayout(HDC dc, PdfTab& tab, int layoutWidth)
    {
        int rows = tab.outline.empty() ? std::min(tab.pageCount, 300) : static_cast<int>(tab.outline.size());
        if ((g_resizingSidebar || g_sidebarAnimating) && !tab.outlineLayout.empty())
        {
            g_sidebarContentHeight = tab.outlineLayoutContentHeight;
            return;
        }
        if (tab.outlineLayoutWidth == layoutWidth &&
            tab.outlineLayoutFont == EffectiveOutlineFont() &&
            tab.outlineLayoutDpi == g_dpi &&
            tab.outlineLayoutRows == rows &&
            tab.outlineLayoutCollapseVersion == g_outlineCollapseVersion)
        {
            g_sidebarContentHeight = tab.outlineLayoutContentHeight;
            return;
        }

        tab.outlineLayout.clear();
        tab.outlineLayout.reserve(static_cast<size_t>(rows));
        int y = S(6);
        int collapsedDepth = -1;
        for (int i = 0; i < rows; ++i)
        {
            int depth = tab.outline.empty() ? 0 : tab.outline[static_cast<size_t>(i)].depth;
            if (collapsedDepth >= 0)
            {
                if (depth > collapsedDepth) continue;
                collapsedDepth = -1;
            }
            int baseRowH = S(depth == 0 ? 36 : 32);
            int lineX = g_sidebar.left + S(10 + depth * 18);
            int iconLeft = lineX + S(12);
            int pageLeft = g_sidebar.right - S(30);
            int textWidth = std::max(24, pageLeft - (iconLeft + S(10) + S(5)) - S(4));
            std::wstring label = tab.outline.empty()
                ? (L"第 " + std::to_wstring(i + 1) + L" 页")
                : tab.outline[static_cast<size_t>(i)].title;
            int textH = MeasureTextHeight(dc, label, textWidth, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            int rowH = std::max(baseRowH, textH + S(8));
            int page = tab.outline.empty() ? i : tab.outline[static_cast<size_t>(i)].page;
            bool hasChildren = !tab.outline.empty() && i + 1 < rows && tab.outline[static_cast<size_t>(i + 1)].depth > depth;
            tab.outlineLayout.push_back(OutlineLayoutRow{ y, rowH, page, i, depth, textH, hasChildren });
            y += rowH + S(4);
            if (hasChildren && g_collapsedOutlineRows.find(i) != g_collapsedOutlineRows.end())
            {
                collapsedDepth = depth;
            }
        }

        tab.outlineLayoutWidth = layoutWidth;
        tab.outlineLayoutFont = EffectiveOutlineFont();
        tab.outlineLayoutDpi = g_dpi;
        tab.outlineLayoutRows = rows;
        tab.outlineLayoutCollapseVersion = g_outlineCollapseVersion;
        tab.outlineLayoutContentHeight = std::max(0, y);
        g_sidebarContentHeight = tab.outlineLayoutContentHeight;
    }

    void DrawBookmarkIcon(HDC dc, RECT r, COLORREF color, bool filled)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Color c = GdiColor(color);
        Gdiplus::Pen pen(c, std::max<Gdiplus::REAL>(1.05f, S(1.0f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(c);
        Gdiplus::GraphicsPath path;
        auto rr = Rf(r);
        float x = rr.X + 0.8f;
        float y = rr.Y + 0.8f;
        float w = std::max(2.0f, rr.Width - 1.6f);
        float h = std::max(2.0f, rr.Height - 1.6f);
        float radius = std::min(std::max(2.0f, w * 0.24f), h * 0.26f);
        float bottomY = y + h - 0.4f;
        float notchY = y + h * 0.70f;
        float centerX = x + w * 0.5f;
        path.StartFigure();
        path.AddLine(x + radius, y, x + w - radius, y);
        path.AddBezier(x + w - radius, y, x + w - radius * 0.36f, y, x + w, y + radius * 0.36f, x + w, y + radius);
        path.AddLine(x + w, y + radius, x + w, bottomY);
        path.AddBezier(x + w, bottomY, x + w * 0.86f, bottomY - h * 0.04f, centerX + w * 0.16f, notchY, centerX, notchY);
        path.AddBezier(centerX, notchY, centerX - w * 0.16f, notchY, x + w * 0.14f, bottomY - h * 0.04f, x, bottomY);
        path.AddLine(x, bottomY, x, y + radius);
        path.AddBezier(x, y + radius, x, y + radius * 0.36f, x + radius * 0.36f, y, x + radius, y);
        path.CloseFigure();
        if (filled) g.FillPath(&brush, &path);
        g.DrawPath(&pen, &path);
    }

    void AddButton(int id, RECT r, std::wstring text, std::wstring tip = L"")
    {
        g_buttons.push_back(Button{ id, r, std::move(text), std::move(tip) });
    }

    void DrawButton(HDC dc, const Button& b)
    {
        bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &b.rect);
        COLORREF fill = pressed ? T().line : T().panel2;
        COLORREF line = pressed ? T().accent : T().line;
        RoundFill(dc, b.rect, S(8), fill, line);
        Text(dc, b.text, b.rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
    }

    void DrawFlatMenuItem(HDC dc, const Button& b)
    {
        Text(dc, b.text, b.rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
    }

    void DrawStatusHotArea(HDC dc, RECT r, int radius = 6)
    {
        if (g_mousePos.x >= r.left && g_mousePos.x <= r.right && g_mousePos.y >= r.top && g_mousePos.y <= r.bottom)
        {
            RoundFill(dc, r, S(radius), T().panel2, T().line);
        }
    }

    enum class StatusIcon
    {
        First,
        Prev,
        Next,
        Last,
        RotateLeft,
        RotateRight,
        Theme,
        View,
        Actual,
        ZoomOut,
        ZoomIn,
        Fullscreen,
        Pin
    };

    enum class SideTabIcon
    {
        Outline,
        Thumbnails,
        Search,
        Notes
    };

    void DrawDownCaret(HDC dc, int x, int y, COLORREF color)
    {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, x - S(3), y - S(1), nullptr);
        LineTo(dc, x, y + S(2));
        LineTo(dc, x + S(3), y - S(1));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void DrawStatusIcon(HDC dc, RECT r, StatusIcon icon, COLORREF color)
    {
        int cx = (r.left + r.right) / 2;
        int cy = (r.top + r.bottom) / 2;
        if (icon == StatusIcon::First || icon == StatusIcon::Prev || icon == StatusIcon::Next || icon == StatusIcon::Last)
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen aaPen(GdiColor(color), std::max<Gdiplus::REAL>(1.35f, S(1.25f)));
            aaPen.SetStartCap(Gdiplus::LineCapRound);
            aaPen.SetEndCap(Gdiplus::LineCapRound);
            aaPen.SetLineJoin(Gdiplus::LineJoinRound);
            float x = static_cast<float>(cx);
            float y = static_cast<float>(cy);
            float w = S(4.6f);
            float h = S(5.5f);
            auto drawChevron = [&](bool left)
            {
                if (left)
                {
                    g.DrawLine(&aaPen, x + w * 0.5f, y - h, x - w * 0.5f, y);
                    g.DrawLine(&aaPen, x - w * 0.5f, y, x + w * 0.5f, y + h);
                }
                else
                {
                    g.DrawLine(&aaPen, x - w * 0.5f, y - h, x + w * 0.5f, y);
                    g.DrawLine(&aaPen, x + w * 0.5f, y, x - w * 0.5f, y + h);
                }
            };
            if (icon == StatusIcon::First || icon == StatusIcon::Prev)
            {
                if (icon == StatusIcon::First) g.DrawLine(&aaPen, x - S(6.0f), y - S(6.0f), x - S(6.0f), y + S(6.0f));
                x += icon == StatusIcon::First ? S(1.0f) : 0.0f;
                drawChevron(true);
            }
            else
            {
                x -= icon == StatusIcon::Last ? S(1.0f) : 0.0f;
                drawChevron(false);
                if (icon == StatusIcon::Last) g.DrawLine(&aaPen, x + S(6.0f), y - S(6.0f), x + S(6.0f), y + S(6.0f));
            }
            return;
        }
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

        auto chevron = [&](int dir)
        {
            int w = S(4);
            int h = S(5);
            MoveToEx(dc, cx + dir * w / 2, cy - h, nullptr);
            LineTo(dc, cx - dir * w / 2, cy);
            LineTo(dc, cx + dir * w / 2, cy + h);
        };
        auto square = [&](int left, int top, int size)
        {
            Rectangle(dc, left, top, left + size, top + size);
        };

        switch (icon)
        {
        case StatusIcon::First:
            MoveToEx(dc, cx - S(6), cy - S(6), nullptr);
            LineTo(dc, cx - S(6), cy + S(6));
            cx += S(1);
            chevron(1);
            break;
        case StatusIcon::Prev:
            chevron(1);
            break;
        case StatusIcon::Next:
            chevron(-1);
            break;
        case StatusIcon::Last:
            cx -= S(1);
            chevron(-1);
            MoveToEx(dc, cx + S(6), cy - S(6), nullptr);
            LineTo(dc, cx + S(6), cy + S(6));
            break;
        case StatusIcon::RotateLeft:
            Arc(dc, cx - S(7), cy - S(7), cx + S(7), cy + S(7), cx - S(7), cy, cx, cy - S(7));
            MoveToEx(dc, cx - S(8), cy, nullptr);
            LineTo(dc, cx - S(10), cy - S(4));
            MoveToEx(dc, cx - S(8), cy, nullptr);
            LineTo(dc, cx - S(4), cy - S(3));
            break;
        case StatusIcon::RotateRight:
            Arc(dc, cx - S(7), cy - S(7), cx + S(7), cy + S(7), cx, cy - S(7), cx + S(7), cy);
            MoveToEx(dc, cx + S(8), cy, nullptr);
            LineTo(dc, cx + S(10), cy - S(4));
            MoveToEx(dc, cx + S(8), cy, nullptr);
            LineTo(dc, cx + S(4), cy - S(3));
            break;
        case StatusIcon::Theme:
        {
            int left = r.left + S(4);
            int top = cy - S(6);
            square(left, top, S(12));
            MoveToEx(dc, left + S(3), top + S(12), nullptr);
            LineTo(dc, left + S(12), top + S(3));
            MoveToEx(dc, left + S(3), top + S(9), nullptr);
            LineTo(dc, left + S(9), top + S(3));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            DrawDownCaret(dc, left + S(20), cy, color);
            return;
        }
        case StatusIcon::View:
        {
            int left = r.left + S(5);
            int top = cy - S(5);
            square(left, top, S(10));
            MoveToEx(dc, left + S(3), cy, nullptr);
            LineTo(dc, left + S(7), cy);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            DrawDownCaret(dc, left + S(19), cy, color);
            return;
        }
        case StatusIcon::Actual:
        {
            int left = r.left + S(4);
            int top = cy - S(6);
            square(left, top, S(12));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            HFONT f = Font(5);
            HGDIOBJ oldFont = SelectObject(dc, f);
            RECT label{ left, top, left + S(12), top + S(12) };
            Text(dc, L"1:1", label, DT_CENTER | DT_VCENTER | DT_SINGLELINE, color);
            SelectObject(dc, oldFont);
            DeleteObject(f);
            DrawDownCaret(dc, left + S(20), cy, color);
            return;
        }
        case StatusIcon::ZoomOut:
            MoveToEx(dc, cx - S(6), cy, nullptr);
            LineTo(dc, cx + S(6), cy);
            break;
        case StatusIcon::ZoomIn:
            MoveToEx(dc, cx - S(6), cy, nullptr);
            LineTo(dc, cx + S(6), cy);
            MoveToEx(dc, cx, cy - S(6), nullptr);
            LineTo(dc, cx, cy + S(6));
            break;
        case StatusIcon::Fullscreen:
        {
            int d = S(6);
            int l = cx - d, t = cy - d, rr = cx + d, b = cy + d;
            MoveToEx(dc, l, t + S(4), nullptr); LineTo(dc, l, t); LineTo(dc, l + S(4), t);
            MoveToEx(dc, rr - S(4), t, nullptr); LineTo(dc, rr, t); LineTo(dc, rr, t + S(4));
            MoveToEx(dc, rr, b - S(4), nullptr); LineTo(dc, rr, b); LineTo(dc, rr - S(4), b);
            MoveToEx(dc, l + S(4), b, nullptr); LineTo(dc, l, b); LineTo(dc, l, b - S(4));
            break;
        }
        case StatusIcon::Pin:
        {
            HPEN greenPen = CreatePen(PS_SOLID, std::max(1, S(1)), color);
            SelectObject(dc, greenPen);
            MoveToEx(dc, cx - S(5), cy - S(6), nullptr);
            LineTo(dc, cx + S(5), cy - S(6));
            MoveToEx(dc, cx - S(3), cy - S(6), nullptr);
            LineTo(dc, cx - S(3), cy + S(1));
            LineTo(dc, cx + S(3), cy + S(1));
            LineTo(dc, cx + S(3), cy - S(6));
            MoveToEx(dc, cx, cy + S(1), nullptr);
            LineTo(dc, cx, cy + S(8));
            SelectObject(dc, pen);
            DeleteObject(greenPen);
            break;
        }
        }

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void DrawSideTabIcon(HDC dc, RECT r, SideTabIcon icon, COLORREF color)
    {
        UNREFERENCED_PARAMETER(color);
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF iconColor = RGB(142, 142, 142);
        Gdiplus::Pen pen(GdiColor(iconColor), std::max<Gdiplus::REAL>(1.55f, S(1)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(GdiColor(iconColor));
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;

        switch (icon)
        {
        case SideTabIcon::Outline:
        {
            Gdiplus::RectF outer{ cx - S(9.5f), cy - S(9.5f), S(19.0f), S(19.0f) };
            DrawGdiRoundRect(g, pen, outer, S(3.2f));
            float ys[] = { cy - S(5.6f), cy, cy + S(5.6f) };
            for (float y : ys)
            {
                Gdiplus::RectF dot{ cx - S(5.8f), y - S(1.1f), S(2.2f), S(2.2f) };
                FillGdiRoundRect(g, brush, dot, S(0.7f));
                Gdiplus::RectF bar{ cx - S(0.6f), y - S(0.9f), S(7.8f), S(1.8f) };
                FillGdiRoundRect(g, brush, bar, S(1.0f));
            }
            break;
        }
        case SideTabIcon::Thumbnails:
        {
            float s = S(6.8f);
            float gap = S(2.0f);
            DrawGdiRoundRect(g, pen, { cx - gap - s, cy - gap - s, s, s }, S(2.2f));
            DrawGdiRoundRect(g, pen, { cx + gap, cy - gap - s, s, s }, S(2.2f));
            DrawGdiRoundRect(g, pen, { cx - gap - s, cy + gap, s, s }, S(2.2f));
            DrawGdiRoundRect(g, pen, { cx + gap, cy + gap, s, s }, S(2.2f));
            break;
        }
        case SideTabIcon::Search:
        {
            g.DrawEllipse(&pen, cx - S(7.4f), cy - S(7.4f), S(13.0f), S(13.0f));
            Gdiplus::Pen handle(GdiColor(iconColor), std::max<Gdiplus::REAL>(1.7f, S(1)));
            handle.SetStartCap(Gdiplus::LineCapRound);
            handle.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&handle, cx + S(4.8f), cy + S(4.8f), cx + S(10.0f), cy + S(10.0f));
            break;
        }
        case SideTabIcon::Notes:
        {
            Gdiplus::RectF note{ cx - S(7.0f), cy - S(8.2f), S(14.0f), S(16.4f) };
            DrawGdiRoundRect(g, pen, note, S(2.4f));
            g.DrawLine(&pen, cx - S(3.8f), cy - S(3.2f), cx + S(3.8f), cy - S(3.2f));
            g.DrawLine(&pen, cx - S(3.8f), cy + S(1.5f), cx + S(2.0f), cy + S(1.5f));
            Gdiplus::Pen markPen(GdiColor(iconColor), std::max<Gdiplus::REAL>(1.25f, S(1.0f)));
            markPen.SetStartCap(Gdiplus::LineCapRound);
            markPen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&markPen, cx + S(3.0f), cy + S(5.4f), cx + S(6.0f), cy + S(8.2f));
            break;
        }
        }
    }

    void DrawSearchGlyph(HDC dc, RECT r, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.25f, S(1.0f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float cx = (r.left + r.right) * 0.5f - S(1.3f);
        float cy = (r.top + r.bottom) * 0.5f - S(1.0f);
        float rad = S(6.2f);
        g.DrawEllipse(&pen, cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
        g.DrawLine(&pen, cx + S(4.5f), cy + S(4.5f), cx + S(9.4f), cy + S(9.4f));
    }

    void DrawGearGlyph(HDC dc, RECT r, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.05f, S(0.9f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float outer = S(6.4f);
        float inner = S(2.55f);
        for (int i = 0; i < 8; ++i)
        {
            double a = i * 3.14159265358979323846 / 4.0;
            float x1 = cx + std::cos(a) * S(8.3f);
            float y1 = cy + std::sin(a) * S(8.3f);
            float x2 = cx + std::cos(a) * S(6.9f);
            float y2 = cy + std::sin(a) * S(6.9f);
            g.DrawLine(&pen, x1, y1, x2, y2);
        }
        g.DrawEllipse(&pen, cx - outer, cy - outer, outer * 2.0f, outer * 2.0f);
        g.DrawEllipse(&pen, cx - inner, cy - inner, inner * 2.0f, inner * 2.0f);
    }

    void DrawCloseGlyph(HDC dc, RECT r, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.35f, S(1.05f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float d = S(4.5f);
        g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
        g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d);
    }

    void DrawSearchActionButton(HDC dc, int id, RECT r, const std::wstring& label)
    {
        bool hovered = Hit(r, g_mousePos.x, g_mousePos.y);
        bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &r);
        COLORREF fill = pressed ? MixColor(T().panel2, T().line, 0.45) : (hovered ? T().panel2 : T().paper);
        COLORREF line = pressed ? T().accent : T().line;
        RoundFill(dc, r, S(7), fill, line);
        HFONT f = Font(10);
        HGDIOBJ oldFont = SelectObject(dc, f);
        Text(dc, label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(f);
        (void)id;
    }

    void DrawHighlightedSnippet(HDC dc, SearchResult const& result, RECT textRect, COLORREF textColor)
    {
        std::wstring const& snippet = result.snippet.empty() ? result.title : result.snippet;
        std::wstring before = snippet.substr(0, std::clamp(result.highlightStart, 0, static_cast<int>(snippet.size())));
        std::wstring hit = g_searchLastQuery.empty() ? g_searchQuery : g_searchLastQuery;
        if (hit.empty() || result.highlightLength <= 0)
        {
            Text(dc, snippet, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, textColor);
            return;
        }
        SIZE beforeSize{};
        GetTextExtentPoint32W(dc, before.c_str(), static_cast<int>(before.size()), &beforeSize);
        SIZE hitSize{};
        GetTextExtentPoint32W(dc, hit.c_str(), static_cast<int>(hit.size()), &hitSize);

        RECT prefix{ textRect.left, textRect.top, textRect.right, textRect.bottom };
        Text(dc, before, prefix, DT_LEFT | DT_VCENTER | DT_SINGLELINE, textColor);

        int hx = textRect.left + beforeSize.cx;
        RECT mark{ hx - S(2), textRect.top + S(6), hx + hitSize.cx + S(4), textRect.bottom - S(6) };
        COLORREF markFill = IsDarkTheme() || IsAuroraTheme() ? MixColor(T().panel2, RGB(245, 164, 72), 0.24) : RGB(255, 250, 235);
        COLORREF markLine = IsDarkTheme() || IsAuroraTheme() ? MixColor(T().accent, RGB(245, 164, 72), 0.55) : RGB(239, 174, 70);
        RoundFill(dc, mark, S(4), markFill, markLine);
        RECT hitText{ hx, textRect.top, hx + hitSize.cx + S(2), textRect.bottom };
        Text(dc, hit, hitText, DT_LEFT | DT_VCENTER | DT_SINGLELINE, textColor);

        int suffixStart = std::min<int>(static_cast<int>(snippet.size()), result.highlightStart + result.highlightLength);
        std::wstring suffix = snippet.substr(static_cast<size_t>(suffixStart));
        RECT suffixRect{ hitText.right + S(2), textRect.top, textRect.right, textRect.bottom };
        Text(dc, suffix, suffixRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, textColor);
    }

    void PaintHoverTip(HDC dc, RECT client)
    {
        if (!g_hasHoverTip || g_hoverTip.empty()) return;
        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, g_hoverTip.c_str(), static_cast<int>(g_hoverTip.size()), &sz);
        int w = sz.cx + S(20);
        int h = S(28);
        int x = (g_hoverTipRect.left + g_hoverTipRect.right - w) / 2;
        x = std::clamp<int>(x, S(8), client.right - w - S(8));
        int y = g_hoverTipRect.top - h - S(8);
        if (y < S(8)) y = g_hoverTipRect.bottom + S(8);
        y = std::clamp<int>(y, S(8), client.bottom - h - S(8));
        RECT box{ x, y, x + w, y + h };
        RoundFill(dc, box, S(6), T().panel2, T().line);
        Text(dc, g_hoverTip, box, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    PdfTab* Active()
    {
        if (g_active < 0 || g_active >= static_cast<int>(g_tabs.size())) return nullptr;
        return g_tabs[static_cast<size_t>(g_active)].get();
    }

    std::filesystem::path StatePath()
    {
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) return L"quietpdf-state.txt";
        std::filesystem::path p = appData;
        CoTaskMemFree(appData);
        p /= L"QingYuePDF";
        std::filesystem::create_directories(p);
        p /= L"state.txt";
        return p;
    }

    void LoadState()
    {
        std::ifstream in(StatePath(), std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        bool themeSchema2 = std::find(lines.begin(), lines.end(), "themeSchema=2") != lines.end();
        for (auto const& line : lines)
        {
            if (line.rfind("theme=", 0) == 0)
            {
                int savedTheme = std::atoi(line.c_str() + 6);
                if (themeSchema2)
                {
                    g_theme = std::clamp(savedTheme, 0, 4);
                }
                else switch (savedTheme)
                {
                case 1: g_theme = 2; break; // 深色云母 -> 极夜黑
                case 2: g_theme = 4; break; // 极光 -> 极光
                case 3: g_theme = 3; break; // 石墨 -> 石墨黑
                case 4: g_theme = 1; break; // 护眼纸张 -> 护眼米白
                case 5: g_theme = 0; break; // 雾面云母 -> 云母白
                default: g_theme = 0; break;
                }
            }
            else if (line.rfind("sidebar=", 0) == 0)
            {
                int savedSidebar = std::clamp(std::atoi(line.c_str() + 8), 180, 520);
                if (savedSidebar == 280) savedSidebar = 520;
                g_sidebarWidth = g_sidebarTargetWidth = g_sidebarExpandedWidth = savedSidebar;
            }
            else if (line.rfind("outline=", 0) == 0)
            {
                int savedOutline = std::clamp(std::atoi(line.c_str() + 8), 9, 22);
                if (savedOutline == 11) savedOutline = 10;
                g_outlineFont = savedOutline;
            }
            else if (line.rfind("view=", 0) == 0) g_viewMode = std::clamp(std::atoi(line.c_str() + 5), 0, 2);
            else if (line.rfind("continuous=", 0) == 0) g_continuousRead = std::atoi(line.c_str() + 11) != 0;
            else if (line.rfind("statusPinned=", 0) == 0) g_statusPinned = std::atoi(line.c_str() + 13) != 0;
            else if (line.rfind("recent=", 0) == 0)
            {
                auto rest = line.substr(7);
                auto a = rest.find('\t');
                auto b = rest.find('\t', a == std::string::npos ? a : a + 1);
                if (a != std::string::npos && b != std::string::npos)
                {
                    RecentItem item;
                    item.openedAt = std::atoll(rest.substr(0, a).c_str());
                    item.page = std::atoi(rest.substr(a + 1, b - a - 1).c_str());
                    item.path = FromUtf8(rest.substr(b + 1));
                    item.name = std::filesystem::path(item.path).filename().wstring();
                    g_recent.push_back(item);
                }
            }
        }
        std::sort(g_recent.begin(), g_recent.end(), [](auto const& a, auto const& b) { return a.openedAt > b.openedAt; });
        if (g_recent.size() > 80) g_recent.resize(80);
    }

    void SaveState()
    {
        std::ofstream out(StatePath(), std::ios::binary | std::ios::trunc);
        out << "themeSchema=2\n";
        out << "theme=" << g_theme << "\n";
        out << "sidebar=" << std::max(g_sidebarExpandedWidth, g_sidebarWidth) << "\n";
        out << "outline=" << g_outlineFont << "\n";
        out << "view=" << g_viewMode << "\n";
        out << "continuous=" << (g_continuousRead ? 1 : 0) << "\n";
        out << "statusPinned=" << (g_statusPinned ? 1 : 0) << "\n";
        for (auto const& item : g_recent)
        {
            out << "recent=" << item.openedAt << '\t' << item.page << '\t' << ToUtf8(item.path) << "\n";
        }
    }

    void DropTab(PdfTab& tab)
    {
        if (tab.doc) g_pdf.dropDocument(tab.ctx, tab.doc);
        if (tab.ctx) g_pdf.dropContext(tab.ctx);
        tab.doc = nullptr;
        tab.ctx = nullptr;
    }

    bool ReadFileStamp(const std::wstring& path, std::filesystem::file_time_type& stamp)
    {
        try
        {
            stamp = std::filesystem::last_write_time(path);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void UpdateTabFileStamp(PdfTab& tab)
    {
        tab.hasFileStamp = ReadFileStamp(tab.path, tab.fileStamp);
    }

    std::wstring LastErrorText(DWORD error)
    {
        if (!error) return L"未知错误";
        wchar_t* buffer = nullptr;
        DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            reinterpret_cast<wchar_t*>(&buffer),
            0,
            nullptr);
        std::wstring text = length && buffer ? std::wstring(buffer, length) : (L"系统错误 " + std::to_wstring(error));
        if (buffer) LocalFree(buffer);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
        return text;
    }

    bool ReplacePdfOnDisk(const std::filesystem::path& source, const std::filesystem::path& temp, DWORD& lastError)
    {
        lastError = ERROR_SUCCESS;
        std::filesystem::path backup = source;
        backup += L".quietpdf-save.bak";
        DeleteFileW(backup.c_str());

        if (ReplaceFileW(source.c_str(), temp.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
        {
            DeleteFileW(backup.c_str());
            return true;
        }
        lastError = GetLastError();

        if (MoveFileExW(temp.c_str(), source.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(backup.c_str());
            return true;
        }
        lastError = GetLastError();

        if (CopyFileW(temp.c_str(), source.c_str(), FALSE))
        {
            DeleteFileW(temp.c_str());
            DeleteFileW(backup.c_str());
            return true;
        }
        lastError = GetLastError();
        return false;
    }

    void FileWatchThreadProc(HWND hwnd, HANDLE directoryHandle, HANDLE stopEvent, unsigned long generation)
    {
        std::vector<BYTE> buffer(64 * 1024);
        HANDLE events[2] = { stopEvent, nullptr };

        for (;;)
        {
            OVERLAPPED overlapped{};
            events[1] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!events[1]) break;
            overlapped.hEvent = events[1];

            DWORD notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_CREATION;
            BOOL queued = ReadDirectoryChangesW(
                directoryHandle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                FALSE,
                notifyFilter,
                nullptr,
                &overlapped,
                nullptr);

            if (!queued)
            {
                CloseHandle(events[1]);
                break;
            }

            DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0)
            {
                CancelIoEx(directoryHandle, &overlapped);
                DWORD ignored{};
                GetOverlappedResult(directoryHandle, &overlapped, &ignored, TRUE);
                CloseHandle(events[1]);
                break;
            }

            DWORD bytes = 0;
            bool changed = wait == WAIT_OBJECT_0 + 1 &&
                GetOverlappedResult(directoryHandle, &overlapped, &bytes, FALSE) &&
                bytes > 0;
            CloseHandle(events[1]);

            if (changed)
            {
                Sleep(180);
                PostMessageW(hwnd, WM_QUIETPDF_FILE_CHANGED, static_cast<WPARAM>(generation), 0);
            }
        }

        CloseHandle(directoryHandle);
        CloseHandle(stopEvent);
    }

    void StopFileWatcher()
    {
        std::thread oldThread;
        HANDLE stopEvent{};
        HANDLE directoryHandle{};
        {
            std::lock_guard<std::mutex> lock(g_fileWatchMutex);
            ++g_fileWatchGeneration;
            stopEvent = g_fileWatchStopEvent;
            directoryHandle = g_fileWatchDirHandle;
            if (stopEvent) SetEvent(stopEvent);
            if (directoryHandle) CancelIoEx(directoryHandle, nullptr);
            oldThread = std::move(g_fileWatchThread);
            g_fileWatchStopEvent = nullptr;
            g_fileWatchDirHandle = nullptr;
        }
        if (oldThread.joinable()) oldThread.join();
    }

    void StartActiveFileWatcher(HWND hwnd)
    {
        StopFileWatcher();

        PdfTab* tab = Active();
        if (!hwnd || !tab || tab->path.empty()) return;

        std::filesystem::path filePath(tab->path);
        std::filesystem::path directory = filePath.parent_path();
        if (directory.empty() || filePath.filename().wstring().empty()) return;

        HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent) return;

        HANDLE directoryHandle = CreateFileW(
            directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            CloseHandle(stopEvent);
            return;
        }

        unsigned long generation = ++g_fileWatchGeneration;
        std::thread thread;
        try
        {
            thread = std::thread(FileWatchThreadProc, hwnd, directoryHandle, stopEvent, generation);
        }
        catch (...)
        {
            CloseHandle(directoryHandle);
            CloseHandle(stopEvent);
            return;
        }

        std::lock_guard<std::mutex> lock(g_fileWatchMutex);
        g_fileWatchDirHandle = directoryHandle;
        g_fileWatchStopEvent = stopEvent;
        g_fileWatchThread = std::move(thread);
    }

    void SetTitle()
    {
        auto* tab = Active();
        std::wstring title = AppName;
        if (tab) title += L" - " + tab->name;
        SetWindowTextW(g_hwnd, title.c_str());
    }

    void ApplyWindowChrome()
    {
        if (!g_hwnd) return;
        BOOL dark = g_theme == 2 || g_theme == 3 || g_theme == 4;
        DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
        int backdrop = IsGlassTheme() ? 4 : 2;
        DwmSetWindowAttribute(g_hwnd, 38, &backdrop, sizeof(backdrop));
    }

    void UpdateRecent(const PdfTab& tab)
    {
        if (tab.path.empty()) return;
        g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(), [&](auto const& i) { return i.path == tab.path; }), g_recent.end());
        g_recent.insert(g_recent.begin(), RecentItem{ tab.path, tab.name, tab.currentPage + 1, NowMs() });
        if (g_recent.size() > 80) g_recent.resize(80);
        SaveState();
    }

    int RecentPageForPath(const std::wstring& path)
    {
        auto it = std::find_if(g_recent.begin(), g_recent.end(), [&](auto const& item) { return item.path == path; });
        return it == g_recent.end() ? 1 : std::max(1, it->page);
    }

    void TouchRecentProgress(const PdfTab& tab, bool saveNow)
    {
        if (tab.path.empty()) return;
        int page = std::max(1, tab.currentPage + 1);
        auto it = std::find_if(g_recent.begin(), g_recent.end(), [&](auto const& item) { return item.path == tab.path; });
        if (it == g_recent.end())
        {
            g_recent.insert(g_recent.begin(), RecentItem{ tab.path, tab.name, page, NowMs() });
            if (g_recent.size() > 80) g_recent.resize(80);
        }
        else
        {
            it->page = page;
        }
        if (saveNow) SaveState();
    }

    int PageW(const PdfTab& tab, int page)
    {
        return static_cast<int>(tab.pages[static_cast<size_t>(page)].widthPt * tab.zoom * g_dpi / 72.0);
    }

    int PageH(const PdfTab& tab, int page)
    {
        return static_cast<int>(tab.pages[static_cast<size_t>(page)].heightPt * tab.zoom * g_dpi / 72.0);
    }

    int PageGap()
    {
        return std::max(2, S(2));
    }

    PageImage RenderPageAtZoom(PdfTab& tab, int index, double zoom)
    {
        double renderScale = zoom * static_cast<double>(g_dpi) / 72.0;
        fz_matrix matrix = g_pdf.scale(static_cast<float>(renderScale), static_cast<float>(renderScale));
        fz_pixmap* pixmap = g_pdf.newPixmapFromPageNumber(tab.ctx, tab.doc, index, matrix, g_pdf.deviceRgb(tab.ctx), 0);
        PageImage image;
        image.width = g_pdf.pixmapWidth(tab.ctx, pixmap);
        image.height = g_pdf.pixmapHeight(tab.ctx, pixmap);
        int components = g_pdf.pixmapComponents(tab.ctx, pixmap);
        int stride = g_pdf.pixmapStride(tab.ctx, pixmap);
        auto* samples = g_pdf.pixmapSamples(tab.ctx, pixmap);
        image.bgra.assign(static_cast<size_t>(image.width) * image.height * 4, 255);
        for (int y = 0; y < image.height; ++y)
        {
            auto* dst = image.bgra.data() + static_cast<size_t>(y) * image.width * 4;
            auto* src = samples + static_cast<size_t>(y) * stride;
            for (int x = 0; x < image.width; ++x)
            {
                auto* px = src + static_cast<size_t>(x) * components;
                std::uint8_t b = components >= 3 ? px[2] : px[0];
                std::uint8_t g = components >= 3 ? px[1] : px[0];
                std::uint8_t r = px[0];
                ApplyPdfTheme(b, g, r);
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = 255;
            }
        }
        g_pdf.dropPixmap(tab.ctx, pixmap);
        return image;
    }

    PageImage* LoadHomeThumb(const RecentItem& item, int targetWidth)
    {
        if (targetWidth <= 0 || item.path.empty()) return nullptr;
        if (g_homeThumbWidth != targetWidth)
        {
            g_homeThumbCache.clear();
            g_homeThumbWidth = targetWidth;
        }

        std::wstring key = item.path + L"#" + std::to_wstring(targetWidth) + L"#" + std::to_wstring(g_theme);
        auto found = g_homeThumbCache.find(key);
        if (found != g_homeThumbCache.end()) return &found->second;

        auto store = [&](PageImage image) -> PageImage*
        {
            while (g_homeThumbCache.size() >= 40) g_homeThumbCache.erase(g_homeThumbCache.begin());
            auto inserted = g_homeThumbCache.emplace(key, std::move(image));
            return &inserted.first->second;
        };

        PageImage result;
        PdfTab temp;
        try
        {
            if (!std::filesystem::exists(item.path)) return store({});
            g_pdf.Load();
            temp.ctx = g_pdf.newContext(nullptr, nullptr, 32 << 20, "1.28.0");
            g_pdf.setAaLevel(temp.ctx, 8);
            g_pdf.registerHandlers(temp.ctx);
            auto utf8 = ToUtf8(item.path);
            temp.doc = g_pdf.openDocument(temp.ctx, utf8.c_str());
            temp.pageCount = g_pdf.countPages(temp.ctx, temp.doc);
            if (temp.pageCount <= 0)
            {
                DropTab(temp);
                return store({});
            }

            fz_page* page = g_pdf.loadPage(temp.ctx, temp.doc, 0);
            fz_rect bounds = g_pdf.boundPage(temp.ctx, page);
            g_pdf.dropPage(temp.ctx, page);
            double widthPt = std::max(1.0, static_cast<double>(bounds.x1 - bounds.x0));
            double pageWidthAtZoom1 = widthPt * static_cast<double>(g_dpi) / 72.0;
            double zoom = std::clamp(static_cast<double>(targetWidth) / std::max(1.0, pageWidthAtZoom1), 0.04, 0.35);
            result = RenderPageAtZoom(temp, 0, zoom);
        }
        catch (...)
        {
            result = {};
        }
        DropTab(temp);

        return store(std::move(result));
    }

    PageImage PixmapToImage(PdfTab& tab, fz_pixmap* pixmap)
    {
        PageImage image;
        if (!pixmap) return image;
        image.width = g_pdf.pixmapWidth(tab.ctx, pixmap);
        image.height = g_pdf.pixmapHeight(tab.ctx, pixmap);
        int components = g_pdf.pixmapComponents(tab.ctx, pixmap);
        int stride = g_pdf.pixmapStride(tab.ctx, pixmap);
        auto* samples = g_pdf.pixmapSamples(tab.ctx, pixmap);
        image.bgra.assign(static_cast<size_t>(image.width) * image.height * 4, 255);
        for (int y = 0; y < image.height; ++y)
        {
            auto* dst = image.bgra.data() + static_cast<size_t>(y) * image.width * 4;
            auto* src = samples + static_cast<size_t>(y) * stride;
            for (int x = 0; x < image.width; ++x)
            {
                auto* px = src + static_cast<size_t>(x) * components;
                std::uint8_t b = components >= 3 ? px[2] : px[0];
                std::uint8_t g = components >= 3 ? px[1] : px[0];
                std::uint8_t r = px[0];
                ApplyPdfTheme(b, g, r);
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = 255;
            }
        }
        return image;
    }

    PageImage RenderPageTile(PdfTab& tab, int index, RECT tile)
    {
        double renderScale = tab.zoom * static_cast<double>(g_dpi) / 72.0;
        fz_matrix matrix = g_pdf.scale(static_cast<float>(renderScale), static_cast<float>(renderScale));
        fz_pixmap* pixmap = g_pdf.newPixmapFromPageNumberClip(
            tab.ctx,
            tab.doc,
            index,
            matrix,
            g_pdf.deviceRgb(tab.ctx),
            0,
            tile.left,
            tile.top,
            tile.right,
            tile.bottom);
        PageImage image = PixmapToImage(tab, pixmap);
        if (pixmap) g_pdf.dropPixmap(tab.ctx, pixmap);
        return image;
    }

    PageImage& EnsureTile(PdfTab& tab, TileKey key)
    {
        if (tab.cachedZoom != tab.zoom || tab.cachedDpi != g_dpi || tab.cachedTheme != g_theme)
        {
            tab.cache.clear();
            tab.tileCache.clear();
            tab.cachedZoom = tab.zoom;
            tab.cachedDpi = g_dpi;
            tab.cachedTheme = g_theme;
        }

        auto it = tab.tileCache.find(key);
        if (it == tab.tileCache.end())
        {
            RECT tile{ key.x, key.y, key.x + key.w, key.y + key.h };
            it = tab.tileCache.emplace(key, RenderPageTile(tab, key.page, tile)).first;
            auto cacheBytes = [&]()
            {
                size_t total = 0;
                for (auto const& entry : tab.tileCache) total += entry.second.bgra.size();
                return total;
            };
            while (tab.tileCache.size() > 44 || cacheBytes() > 24ull * 1024ull * 1024ull)
            {
                auto victim = tab.tileCache.end();
                int worstDistance = -1;
                for (auto candidate = tab.tileCache.begin(); candidate != tab.tileCache.end(); ++candidate)
                {
                    if (!(candidate->first < key) && !(key < candidate->first) && tab.tileCache.size() > 1) continue;
                    int distance = std::abs(candidate->first.page - tab.currentPage);
                    if (distance > worstDistance)
                    {
                        worstDistance = distance;
                        victim = candidate;
                    }
                }
                if (victim == tab.tileCache.end() || (!(victim->first < key) && !(key < victim->first))) break;
                tab.tileCache.erase(victim);
            }
        }
        return it->second;
    }

    PageImage& EnsurePage(PdfTab& tab, int index)
    {
        if (tab.cachedZoom != tab.zoom || tab.cachedDpi != g_dpi || tab.cachedTheme != g_theme)
        {
            tab.cache.clear();
            tab.cachedZoom = tab.zoom;
            tab.cachedDpi = g_dpi;
            tab.cachedTheme = g_theme;
        }
        auto it = tab.cache.find(index);
        if (it == tab.cache.end())
        {
            it = tab.cache.emplace(index, RenderPageAtZoom(tab, index, tab.zoom)).first;
            auto cacheBytes = [&]()
            {
                size_t total = 0;
                for (auto const& entry : tab.cache) total += entry.second.bgra.size();
                return total;
            };
            size_t maxBytes = tab.zoom > 2.0 ? 10ull * 1024ull * 1024ull : 36ull * 1024ull * 1024ull;
            size_t maxPages = tab.zoom > 2.0 ? 1ull : 4ull;
            while (tab.cache.size() > maxPages || cacheBytes() > maxBytes)
            {
                auto victim = tab.cache.end();
                int worstDistance = -1;
                for (auto candidate = tab.cache.begin(); candidate != tab.cache.end(); ++candidate)
                {
                    if (candidate->first == index && tab.cache.size() > 1) continue;
                    int distance = std::abs(candidate->first - tab.currentPage);
                    if (distance > worstDistance)
                    {
                        worstDistance = distance;
                        victim = candidate;
                    }
                }
                if (victim == tab.cache.end() || victim->first == index) break;
                tab.cache.erase(victim);
            }
        }
        return it->second;
    }

    PageImage& EnsureThumb(PdfTab& tab, int index)
    {
        if (tab.cachedThumbSize != g_thumbSize || tab.cachedThumbTheme != g_theme)
        {
            tab.thumbCache.clear();
            tab.cachedThumbSize = g_thumbSize;
            tab.cachedThumbTheme = g_theme;
        }
        auto it = tab.thumbCache.find(index);
        if (it == tab.thumbCache.end())
        {
            double pageWidthPixelsAtZoom1 = tab.pages[static_cast<size_t>(index)].widthPt * static_cast<double>(g_dpi) / 72.0;
            double thumbZoom = std::max(0.04, static_cast<double>(g_thumbSize) / std::max(1.0, pageWidthPixelsAtZoom1));
            it = tab.thumbCache.emplace(index, RenderPageAtZoom(tab, index, thumbZoom)).first;
            while (tab.thumbCache.size() > 28) tab.thumbCache.erase(tab.thumbCache.begin());
        }
        return it->second;
    }

    void SchedulePagePrefetch(HWND hwnd)
    {
        UNREFERENCED_PARAMETER(hwnd);
    }

    bool PrefetchOnePage()
    {
        PdfTab* tab = Active();
        if (!tab || tab->pageCount <= 0) return false;
        if (tab->cachedZoom != tab->zoom || tab->cachedDpi != g_dpi || tab->cachedTheme != g_theme)
        {
            tab->cache.clear();
            tab->cachedZoom = tab->zoom;
            tab->cachedDpi = g_dpi;
            tab->cachedTheme = g_theme;
        }

        if (tab->zoom > 2.5 || tab->cache.size() >= 5) return false;

        int center = std::clamp(tab->currentPage, 0, tab->pageCount - 1);
        for (int radius = 0; radius <= 2; ++radius)
        {
            int candidates[2]{ center + radius, center - radius };
            int count = radius == 0 ? 1 : 2;
            for (int i = 0; i < count; ++i)
            {
                int page = candidates[i];
                if (page < 0 || page >= tab->pageCount) continue;
                if (tab->cache.find(page) == tab->cache.end())
                {
                    EnsurePage(*tab, page);
                    return true;
                }
            }
        }
        return false;
    }

    void CollectOutline(PdfTab& tab, fz_outline* item, int depth)
    {
        for (fz_outline* current = item; current; current = current->next)
        {
            int page = g_pdf.pageNumberFromLocation(tab.ctx, tab.doc, current->page);
            if (page >= 0 && page < tab.pageCount)
            {
                std::wstring title = current->title ? FromUtf8(current->title) : L"未命名目录";
                tab.outline.push_back(OutlineItem{ title, page, current->y, depth });
            }
            if (current->down)
            {
                CollectOutline(tab, current->down, depth + 1);
            }
        }
    }

    void ScrollToPage(PdfTab& tab, int page)
    {
        page = std::clamp(page, 0, tab.pageCount - 1);
        int scroll = S(28);
        for (int p = 0; p < page; ++p)
        {
            scroll += PageH(tab, p) + PageGap();
        }
        tab.currentPage = page;
        g_scrollY = scroll;
    }

    int PageTopScroll(PdfTab& tab, int page)
    {
        page = std::clamp(page, 0, tab.pageCount - 1);
        int scroll = S(28);
        for (int p = 0; p < page; ++p)
        {
            scroll += PageH(tab, p) + PageGap();
        }
        return scroll;
    }

    RECT SearchQuadRect(PdfTab const& tab, SearchResult const& result, RECT pageRect)
    {
        double scale = tab.zoom * static_cast<double>(g_dpi) / 72.0;
        float minX = std::min({ result.quad.ul.x, result.quad.ur.x, result.quad.ll.x, result.quad.lr.x });
        float maxX = std::max({ result.quad.ul.x, result.quad.ur.x, result.quad.ll.x, result.quad.lr.x });
        float minY = std::min({ result.quad.ul.y, result.quad.ur.y, result.quad.ll.y, result.quad.lr.y });
        float maxY = std::max({ result.quad.ul.y, result.quad.ur.y, result.quad.ll.y, result.quad.lr.y });
        RECT r{
            pageRect.left + static_cast<int>(std::floor(minX * scale)) - S(1),
            pageRect.top + static_cast<int>(std::floor(minY * scale)) - S(1),
            pageRect.left + static_cast<int>(std::ceil(maxX * scale)) + S(1),
            pageRect.top + static_cast<int>(std::ceil(maxY * scale)) + S(1)
        };
        return r;
    }

    void ScrollToSearchResult(PdfTab& tab, SearchResult const& result)
    {
        int page = std::clamp(result.page, 0, tab.pageCount - 1);
        tab.currentPage = page;
        int scroll = PageTopScroll(tab, page);
        if (result.hasQuad)
        {
            double scale = tab.zoom * static_cast<double>(g_dpi) / 72.0;
            float topPt = std::min({ result.quad.ul.y, result.quad.ur.y, result.quad.ll.y, result.quad.lr.y });
            if (g_continuousRead)
            {
                int target = scroll + static_cast<int>(topPt * scale + 0.5) - S(96);
                int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
                int content = S(28);
                for (int p = 0; p < tab.pageCount; ++p)
                {
                    content += PageH(tab, p) + PageGap();
                }
                content = std::max<int>(content + S(28), stageH);
                int maxScroll = std::max(0, content - stageH);
                g_scrollY = std::clamp(target, 0, maxScroll);
            }
            else
            {
                int target = static_cast<int>(topPt * scale + 0.5) - S(96);
                int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
                int maxScroll = std::max<int>(0, PageH(tab, page) + S(56) - stageH);
                g_scrollY = std::clamp(target, 0, maxScroll);
            }
        }
        else
        {
            g_scrollY = scroll;
        }
    }

    int ContinuousContentHeight(PdfTab const& tab)
    {
        int gap = PageGap();
        int content = S(28);
        for (int p = 0; p < tab.pageCount; ++p)
        {
            content += PageH(tab, p) + gap;
        }
        return std::max<int>(content + S(28), g_stage.bottom - g_stage.top);
    }

    int SinglePageContentHeight(PdfTab const& tab)
    {
        if (tab.pageCount <= 0) return std::max<int>(1, g_stage.bottom - g_stage.top);
        int page = std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        return std::max<int>(PageH(tab, page) + S(56), g_stage.bottom - g_stage.top);
    }

    int SinglePageMaxScroll(PdfTab const& tab)
    {
        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        return std::max<int>(0, SinglePageContentHeight(tab) - stageH);
    }

    bool PageRectAtPoint(PdfTab const& tab, int px, int py, int& page, RECT& pageRect)
    {
        if (!Hit(g_stage, px, py) || tab.pageCount <= 0) return false;
        int gap = g_continuousRead ? PageGap() : 0;
        int firstPage = g_continuousRead ? 0 : std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        int lastPage = g_continuousRead ? tab.pageCount : std::min(tab.pageCount, firstPage + 1);
        int y = g_stage.top + S(28) - g_scrollY;
        if (g_continuousRead)
        {
            for (int p = 0; p < firstPage; ++p) y += PageH(tab, p) + gap;
        }
        for (int i = firstPage; i < lastPage; ++i)
        {
            int pageW = PageW(tab, i);
            int pageH = PageH(tab, i);
            int x = g_stage.left + std::max<int>(S(28), ((g_stage.right - g_stage.left) - pageW) / 2);
            RECT r{ x, y, x + pageW, y + pageH };
            if (Hit(r, px, py))
            {
                page = i;
                pageRect = r;
                return true;
            }
            y += pageH + gap;
        }
        return false;
    }

    bool PageRectForPage(PdfTab const& tab, int targetPage, RECT& pageRect)
    {
        if (tab.pageCount <= 0 || targetPage < 0 || targetPage >= tab.pageCount) return false;
        int gap = g_continuousRead ? PageGap() : 0;
        int firstPage = g_continuousRead ? 0 : std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        int lastPage = g_continuousRead ? tab.pageCount : std::min(tab.pageCount, firstPage + 1);
        int y = g_stage.top + S(28) - g_scrollY;
        for (int i = firstPage; i < lastPage; ++i)
        {
            int pageW = PageW(tab, i);
            int pageH = PageH(tab, i);
            int x = g_stage.left + std::max<int>(S(28), ((g_stage.right - g_stage.left) - pageW) / 2);
            RECT r{ x, y, x + pageW, y + pageH };
            if (i == targetPage)
            {
                pageRect = r;
                return true;
            }
            y += pageH + gap;
        }
        return false;
    }

    fz_rect NormalizeSelectionRect(RECT pageRect, POINT a, POINT b)
    {
        double pageW = std::max<int>(1, pageRect.right - pageRect.left);
        double pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        int left = std::clamp(std::min(a.x, b.x), pageRect.left, pageRect.right);
        int right = std::clamp(std::max(a.x, b.x), pageRect.left, pageRect.right);
        int top = std::clamp(std::min(a.y, b.y), pageRect.top, pageRect.bottom);
        int bottom = std::clamp(std::max(a.y, b.y), pageRect.top, pageRect.bottom);
        return fz_rect{
            static_cast<float>((left - pageRect.left) / pageW),
            static_cast<float>((top - pageRect.top) / pageH),
            static_cast<float>((right - pageRect.left) / pageW),
            static_cast<float>((bottom - pageRect.top) / pageH)
        };
    }

    RECT QuadToRect(fz_quad const& quad, RECT pageRect)
    {
        int pageW = std::max<int>(1, pageRect.right - pageRect.left);
        int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        float minX = std::min({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
        float maxX = std::max({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
        float minY = std::min({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
        float maxY = std::max({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
        return RECT{
            pageRect.left + static_cast<int>(minX * pageW + 0.5f),
            pageRect.top + static_cast<int>(minY * pageH + 0.5f),
            pageRect.left + static_cast<int>(maxX * pageW + 0.5f),
            pageRect.top + static_cast<int>(maxY * pageH + 0.5f)
        };
    }

    RECT DenormalizeAnnotationRect(PdfAnnotation const& annotation, RECT pageRect)
    {
        int pageW = std::max<int>(1, pageRect.right - pageRect.left);
        int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        return RECT{
            pageRect.left + static_cast<int>(annotation.x0 * pageW + 0.5),
            pageRect.top + static_cast<int>(annotation.y0 * pageH + 0.5),
            pageRect.left + static_cast<int>(annotation.x1 * pageW + 0.5),
            pageRect.top + static_cast<int>(annotation.y1 * pageH + 0.5)
        };
    }

    RECT CurrentSelectionRect()
    {
        if (!g_hasContentSelection && !g_selectingContent) return RECT{};
        return RECT{
            std::min(g_selectionStart.x, g_selectionEnd.x),
            std::min(g_selectionStart.y, g_selectionEnd.y),
            std::max(g_selectionStart.x, g_selectionEnd.x),
            std::max(g_selectionStart.y, g_selectionEnd.y)
        };
    }

    bool RefreshTextSelection(PdfTab& tab)
    {
        g_selectionQuads.clear();
        g_selectionText.clear();
        if (!g_pdf.selectText || g_selectionPage < 0 || g_selectionPage >= tab.pageCount)
            return false;

        fz_rect rect = NormalizeSelectionRect(g_selectionPageRect, g_selectionStart, g_selectionEnd);
        if (std::abs(rect.x1 - rect.x0) < 0.001f || std::abs(rect.y1 - rect.y0) < 0.001f)
            return false;

        std::vector<fz_quad> quads(256);
        std::string text(65536, '\0');
        int count = g_pdf.selectText(
            tab.ctx,
            tab.doc,
            g_selectionPage,
            rect.x0,
            rect.y0,
            rect.x1,
            rect.y1,
            quads.data(),
            static_cast<int>(quads.size()),
            text.data(),
            static_cast<int>(text.size()));
        if (count <= 0)
            return false;

        quads.resize(static_cast<size_t>(count));
        g_selectionQuads = std::move(quads);
        g_selectionNorm = rect;
        g_selectionText = FromUtf8(text.c_str());
        return true;
    }

    void CopyTextToClipboard(HWND hwnd, std::wstring const& text)
    {
        if (text.empty() || !OpenClipboard(hwnd)) return;
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem)
        {
            void* dst = GlobalLock(mem);
            if (dst)
            {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
                mem = nullptr;
            }
        }
        if (mem) GlobalFree(mem);
        CloseClipboard();
    }

    void ClearTextSelection()
    {
        g_selectingContent = false;
        g_hasContentSelection = false;
        g_selectionPage = -1;
        g_selectionQuads.clear();
        g_selectionText.clear();
    }

    void ScrollToPage(PdfTab& tab, int page);

    void ClearAnnotationFocus()
    {
        g_activeAnnotation = -1;
        g_annotationToolbarOpen = false;
        g_annotationNoteOpen = false;
        g_annotationNoteFocused = false;
        g_annotationHitRect = RECT{};
        g_annotationToolbarRect = RECT{};
        g_annotationCopyRect = RECT{};
        g_annotationNoteRect = RECT{};
        g_annotationDeleteRect = RECT{};
        g_annotationFormatRect = RECT{};
        g_annotationNoteCloseRect = RECT{};
        g_annotationNotePanelRect = RECT{};
        g_annotationNoteInputRect = RECT{};
        g_annotationColorRects.clear();
    }

    bool ActiveAnnotation(PdfTab& tab, PdfAnnotation*& annotation)
    {
        if (g_activeAnnotation < 0 || g_activeAnnotation >= static_cast<int>(tab.annotations.size())) return false;
        annotation = &tab.annotations[static_cast<size_t>(g_activeAnnotation)];
        return true;
    }

    std::wstring ElideText(std::wstring text, size_t maxChars);

    void DeleteAnnotation(HWND hwnd, PdfTab& tab, int index)
    {
        if (index < 0 || index >= static_cast<int>(tab.annotations.size())) return;
        tab.annotations.erase(tab.annotations.begin() + index);
        tab.dirty = true;
        ClearAnnotationFocus();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void SelectAnnotation(HWND hwnd, PdfTab& tab, int index, bool jump)
    {
        if (index < 0 || index >= static_cast<int>(tab.annotations.size())) return;
        g_activeAnnotation = index;
        g_annotationToolbarOpen = true;
        g_annotationNoteFocused = false;
        if (jump) ScrollToPage(tab, tab.annotations[static_cast<size_t>(index)].page);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    int HitTestAnnotation(PdfTab const& tab, int x, int y, RECT* hitRect = nullptr)
    {
        int page = -1;
        RECT pageRect{};
        if (!PageRectAtPoint(tab, x, y, page, pageRect)) return -1;
        for (int i = static_cast<int>(tab.annotations.size()) - 1; i >= 0; --i)
        {
            PdfAnnotation const& annotation = tab.annotations[static_cast<size_t>(i)];
            if (annotation.page != page) continue;
            RECT mark = DenormalizeAnnotationRect(annotation, pageRect);
            InflateRect(&mark, S(3), S(5));
            if (Hit(mark, x, y))
            {
                if (hitRect) *hitRect = mark;
                return i;
            }
        }
        return -1;
    }

    RECT SidebarScrollRect();
    int ClampSidebarScrollValue(int value);
    void RequestSidebarRepaint(HWND hwnd);

    int PageFromScroll(PdfTab const& tab, int scrollY)
    {
        if (tab.pageCount <= 0) return 0;
        int gap = PageGap();
        int y = S(28);
        int probe = scrollY + S(32);
        int best = 0;
        for (int p = 0; p < tab.pageCount; ++p)
        {
            int pageH = PageH(tab, p);
            if (probe < y + pageH + gap / 2) return p;
            y += pageH + gap;
            best = p;
        }
        return best;
    }

    int OutlineTargetScrollY(PdfTab const& tab, OutlineItem const& item, int knownPage = -1, int knownPageTop = 0)
    {
        int page = std::clamp(item.page, 0, tab.pageCount - 1);
        int scroll = knownPage == page ? knownPageTop : S(28);
        if (knownPage != page)
        {
            for (int p = 0; p < page; ++p)
            {
                scroll += PageH(tab, p) + PageGap();
            }
        }
        int yOffset = item.yPt > 0 ? static_cast<int>(item.yPt * tab.zoom * static_cast<double>(g_dpi) / 72.0 + 0.5) : 0;
        return std::max(0, scroll + yOffset - S(24));
    }

    bool OutlineRowVisible(PdfTab const& tab, int row)
    {
        if (row < 0 || row >= static_cast<int>(tab.outline.size())) return false;
        int depth = tab.outline[static_cast<size_t>(row)].depth;
        for (int i = row - 1; i >= 0 && depth > 0; --i)
        {
            int parentDepth = tab.outline[static_cast<size_t>(i)].depth;
            if (parentDepth < depth)
            {
                if (g_collapsedOutlineRows.find(i) != g_collapsedOutlineRows.end()) return false;
                depth = parentDepth;
            }
        }
        return true;
    }

    int OutlineRowFromScroll(PdfTab const& tab, int scrollY)
    {
        if (tab.outline.empty() || tab.pageCount <= 0) return -1;
        int probe = scrollY + S(48);
        int best = -1;
        int pageTop = S(28);
        int page = 0;
        for (int i = 0; i < static_cast<int>(tab.outline.size()); ++i)
        {
            if (!OutlineRowVisible(tab, i)) continue;
            int itemPage = std::clamp(tab.outline[static_cast<size_t>(i)].page, 0, tab.pageCount - 1);
            if (itemPage < page)
            {
                pageTop = S(28);
                page = 0;
            }
            while (page < itemPage)
            {
                pageTop += PageH(tab, page) + PageGap();
                ++page;
            }
            int target = OutlineTargetScrollY(tab, tab.outline[static_cast<size_t>(i)], page, pageTop);
            if (target <= probe) best = i;
            else if (itemPage > tab.currentPage || target > probe + S(400)) break;
        }
        return best;
    }

    void KeepOutlineSelectionVisible(HWND hwnd, PdfTab const& tab)
    {
        if (!g_sidebarOpen || g_immersive || g_sideMode != 0 || g_selectedOutlineRow < 0) return;
        if (tab.outlineLayout.empty()) return;
        auto rowIt = std::find_if(
            tab.outlineLayout.begin(),
            tab.outlineLayout.end(),
            [](OutlineLayoutRow const& row)
            {
                return row.id == g_selectedOutlineRow;
            });
        if (rowIt == tab.outlineLayout.end()) return;
        RECT area = SidebarScrollRect();
        int viewH = std::max<int>(1, area.bottom - area.top);
        int margin = S(20);
        int newScroll = g_sidebarScrollY;
        if (rowIt->top < g_sidebarScrollY + margin)
        {
            newScroll = rowIt->top - margin;
        }
        else if (rowIt->top + rowIt->height > g_sidebarScrollY + viewH - margin)
        {
            newScroll = rowIt->top + rowIt->height - viewH + margin;
        }
        newScroll = ClampSidebarScrollValue(newScroll);
        if (newScroll != g_sidebarScrollY)
        {
            g_sidebarScrollY = newScroll;
        }
        RequestSidebarRepaint(hwnd);
    }

    void SyncOutlineSelectionFromScroll(HWND hwnd, PdfTab& tab)
    {
        int selected = OutlineRowFromScroll(tab, g_scrollY);
        if (selected == g_selectedOutlineRow) return;
        g_selectedOutlineRow = selected;
        KeepOutlineSelectionVisible(hwnd, tab);
    }

    void FocusPageInput(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab) return;
        g_searchInputFocused = false;
        g_pageInputFocused = true;
        g_pageCaretVisible = true;
        g_pageInputText = std::to_wstring(tab->currentPage + 1);
        SetTimer(hwnd, 10, 500, nullptr);
        RECT client{};
        GetClientRect(hwnd, &client);
        RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
        InvalidateRect(hwnd, &status, FALSE);
    }

    void CancelPageInput(HWND hwnd)
    {
        if (!g_pageInputFocused) return;
        g_pageInputFocused = false;
        g_pageInputText.clear();
        KillTimer(hwnd, 10);
        RECT client{};
        GetClientRect(hwnd, &client);
        RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
        InvalidateRect(hwnd, &status, FALSE);
    }

    void CommitPageInput(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (tab && !g_pageInputText.empty())
        {
            int page = std::clamp(_wtoi(g_pageInputText.c_str()), 1, tab->pageCount);
            ScrollToPage(*tab, page - 1);
        }
        CancelPageInput(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void ScrollToOutline(PdfTab& tab, int outlineRow)
    {
        if (outlineRow < 0 || outlineRow >= static_cast<int>(tab.outline.size()))
        {
            return;
        }
        auto const& item = tab.outline[static_cast<size_t>(outlineRow)];
        int page = std::clamp(item.page, 0, tab.pageCount - 1);
        int scroll = S(28);
        for (int p = 0; p < page; ++p)
        {
            scroll += PageH(tab, p) + PageGap();
        }
        int yOffset = item.yPt > 0 ? static_cast<int>(item.yPt * tab.zoom * static_cast<double>(g_dpi) / 72.0 + 0.5) : 0;
        tab.currentPage = page;
        g_scrollY = std::max(0, scroll + yOffset - S(24));
    }

    void RunSearch()
    {
        PdfTab* tab = Active();
        g_searchResults.clear();
        g_searchCurrent = 0;
        g_searchHasRun = true;
        g_searchLastQuery = g_searchQuery;
        if (!tab || g_searchQuery.empty())
        {
            return;
        }

        std::string needle = ToUtf8(g_searchQuery);
        int totalHits = 0;
        for (int page = 0; page < tab->pageCount; ++page)
        {
            int marks[32]{};
            fz_quad boxes[32]{};
            int count = g_pdf.searchPageNumber(tab->ctx, tab->doc, page, needle.c_str(), marks, boxes, 32);
            if (count > 0)
            {
                for (int hit = 0; hit < count && totalHits < 500; ++hit)
                {
                    ++totalHits;
                    std::wstring snippet = L"第 " + std::to_wstring(page + 1) + L" 页第 " + std::to_wstring(hit + 1) + L" 处“" + g_searchQuery + L"”";
                    int highlightStart = static_cast<int>(snippet.find(g_searchQuery));
                    if (highlightStart < 0) highlightStart = 0;
                    g_searchResults.push_back(SearchResult{
                        L"第 " + std::to_wstring(page + 1) + L" 页包含 \"" + g_searchQuery + L"\"",
                        snippet,
                        page,
                        1,
                        hit + 1,
                        boxes[hit],
                        true,
                        highlightStart,
                        static_cast<int>(g_searchQuery.size())
                    });
                }
            }
            if (totalHits >= 500)
            {
                break;
            }
        }
    }

    int SearchHitTotal()
    {
        int total = 0;
        for (auto const& result : g_searchResults) total += std::max(0, result.count);
        return total;
    }

    void OpenPdf(HWND hwnd, const std::wstring& path)
    {
        g_pdf.Load();
        auto tab = std::make_unique<PdfTab>();
        tab->ctx = g_pdf.newContext(nullptr, nullptr, 64 << 20, "1.28.0");
        g_pdf.setAaLevel(tab->ctx, 8);
        g_pdf.registerHandlers(tab->ctx);
        auto utf8 = ToUtf8(path);
        tab->doc = g_pdf.openDocument(tab->ctx, utf8.c_str());
        tab->pageCount = g_pdf.countPages(tab->ctx, tab->doc);
        tab->path = path;
        tab->name = std::filesystem::path(path).filename().wstring();
        UpdateTabFileStamp(*tab);
        int restorePage = RecentPageForPath(path);
        tab->pages.resize(static_cast<size_t>(tab->pageCount));
        int probe = std::min(tab->pageCount, 1000);
        for (int i = 0; i < probe; ++i)
        {
            fz_page* page = g_pdf.loadPage(tab->ctx, tab->doc, i);
            fz_rect b = g_pdf.boundPage(tab->ctx, page);
            tab->pages[static_cast<size_t>(i)] = PageInfo{ std::max(1.0, static_cast<double>(b.x1 - b.x0)), std::max(1.0, static_cast<double>(b.y1 - b.y0)) };
            g_pdf.dropPage(tab->ctx, page);
        }
        for (int i = probe; i < tab->pageCount; ++i) tab->pages[static_cast<size_t>(i)] = tab->pages[0];
        fz_outline* outline = g_pdf.loadOutline(tab->ctx, tab->doc);
        if (outline)
        {
            CollectOutline(*tab, outline, 0);
            g_pdf.dropOutline(tab->ctx, outline);
        }
        g_tabs.push_back(std::move(tab));
        g_active = static_cast<int>(g_tabs.size()) - 1;
        if (auto* active = Active())
        {
            ScrollToPage(*active, std::clamp(restorePage, 1, active->pageCount) - 1);
        }
        else
        {
            g_scrollY = 0;
        }
        g_selectedOutlineRow = -1;
        UpdateRecent(*Active());
        SetTitle();
        StartActiveFileWatcher(hwnd);
        SchedulePagePrefetch(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void OpenDialog(HWND hwnd)
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0所有文件 (*.*)\0*.*\0";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) OpenPdf(hwnd, file);
    }

    void ClampScroll(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
        }
        int maxScroll = std::max<int>(0, g_contentHeight - (g_stage.bottom - g_stage.top));
        g_scrollY = std::clamp(g_scrollY, 0, maxScroll);
        if (tab && g_continuousRead)
        {
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
            TouchRecentProgress(*tab, false);
            SyncOutlineSelectionFromScroll(hwnd, *tab);
        }
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = std::max<int>(0, g_contentHeight);
        si.nPage = static_cast<UINT>(std::max<int>(0, g_stage.bottom - g_stage.top));
        si.nPos = g_scrollY;
        SetScrollInfo(hwnd, SB_VERT, &si, FALSE);
    }

    void UpdateVertScrollBar(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
        }
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = std::max<int>(0, g_contentHeight);
        si.nPage = static_cast<UINT>(std::max<int>(0, g_stage.bottom - g_stage.top));
        si.nPos = g_scrollY;
        SetScrollInfo(hwnd, SB_VERT, &si, FALSE);
    }

    bool Hit(RECT r, int x, int y)
    {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }

    bool Intersects(RECT a, RECT b)
    {
        RECT out{};
        return IntersectRect(&out, &a, &b) != FALSE;
    }

    void ResizeSidebarNow(HWND hwnd, int width, bool finalPaint)
    {
        width = std::clamp(width, S(180), S(520));
        g_sidebarTargetWidth = width;
        if (!finalPaint)
        {
            constexpr ULONGLONG frameMs = 16;
            ULONGLONG now = GetTickCount64();
            if (now - g_lastSidebarResizeMs < frameMs)
            {
                if (!g_sidebarResizePending)
                {
                    g_sidebarResizePending = true;
                    SetTimer(hwnd, 1, static_cast<UINT>(frameMs - (now - g_lastSidebarResizeMs)), nullptr);
                }
                return;
            }
        }
        if (width == g_sidebarWidth && !finalPaint) return;

        RECT client{};
        GetClientRect(hwnd, &client);
        int oldWidth = g_sidebarWidth;
        g_sidebarWidth = g_sidebarTargetWidth;
        if (g_sidebarWidth >= S(180)) g_sidebarExpandedWidth = g_sidebarWidth;
        g_sidebarResizePending = false;
        g_lastSidebarResizeMs = GetTickCount64();
        KillTimer(hwnd, 1);

        int top = g_immersive ? 0 : S(76);
        int bottom = (g_immersive || !g_statusPinned) ? 0 : S(34);
        int oldRight = oldWidth + (oldWidth ? std::max(1, S(1)) : 0);
        int newRight = width + (width ? std::max(1, S(1)) : 0);
        RECT dirty{ 0, top, std::min<int>(client.right, std::max<int>(oldRight, newRight) + S(18)), client.bottom - bottom };
        if (finalPaint)
        {
            dirty.right = client.right;
        }
        InvalidateRect(hwnd, &dirty, FALSE);
    }

    RECT SidebarScrollRect()
    {
        int top = g_sidebar.top + S(g_sideMode == 1 ? 80 : (g_sideMode == 2 ? 146 : 44));
        return RECT{ g_sidebar.left, top, g_sidebar.right, g_sidebar.bottom };
    }

    void RequestStageRepaint(HWND hwnd, bool includeStatus)
    {
        constexpr ULONGLONG frameMs = 16;
        ULONGLONG now = GetTickCount64();
        auto repaint = [&]()
        {
            RECT client{};
            GetClientRect(hwnd, &client);
            InvalidateRect(hwnd, &g_stage, FALSE);
            if (!IsRectEmpty(&g_mainScrollTrackRect))
            {
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
            }
            if (includeStatus)
            {
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                InvalidateRect(hwnd, &status, FALSE);
            }
            g_lastStageRepaintMs = GetTickCount64();
            g_stageRepaintPending = false;
        };
        if (now - g_lastStageRepaintMs >= frameMs)
        {
            repaint();
        }
        else if (!g_stageRepaintPending)
        {
            g_stageRepaintPending = true;
            SetTimer(hwnd, 6, static_cast<UINT>(frameMs - (now - g_lastStageRepaintMs)), nullptr);
        }
    }

    void RequestSidebarRepaint(HWND hwnd)
    {
        constexpr ULONGLONG frameMs = 16;
        ULONGLONG now = GetTickCount64();
        if (now - g_lastSidebarRepaintMs >= frameMs)
        {
            RECT area = SidebarScrollRect();
            InvalidateRect(hwnd, &area, FALSE);
            g_lastSidebarRepaintMs = now;
            g_sidebarRepaintPending = false;
        }
        else if (!g_sidebarRepaintPending)
        {
            g_sidebarRepaintPending = true;
            SetTimer(hwnd, 7, static_cast<UINT>(frameMs - (now - g_lastSidebarRepaintMs)), nullptr);
        }
    }

    void ScrollStageTo(HWND hwnd, int newScrollY)
    {
        PdfTab* tab = Active();
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
        }
        int maxScroll = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
        int old = g_scrollY;
        g_scrollY = std::clamp(newScrollY, 0, maxScroll);
        if (tab && g_continuousRead)
        {
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
            TouchRecentProgress(*tab, false);
            SyncOutlineSelectionFromScroll(hwnd, *tab);
        }
        int dy = old - g_scrollY;
        if (dy == 0)
        {
            UpdateVertScrollBar(hwnd);
            return;
        }

        RequestStageRepaint(hwnd, false);
        UpdateVertScrollBar(hwnd);
        SchedulePagePrefetch(hwnd);
    }

    void ScrollSidebarTo(HWND hwnd, int newScrollY)
    {
        int old = g_sidebarScrollY;
        int visible = std::max<int>(1, g_sidebar.bottom - SidebarScrollRect().top);
        int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
        g_sidebarScrollY = std::clamp(newScrollY, 0, maxScroll);
        int dy = old - g_sidebarScrollY;
        if (dy == 0) return;

        RequestSidebarRepaint(hwnd);
    }

    int WheelPixels(int delta, int pixelsPerNotch, double& remainder)
    {
        double exact = remainder + static_cast<double>(delta) * static_cast<double>(pixelsPerNotch) / static_cast<double>(WHEEL_DELTA);
        int pixels = exact >= 0.0 ? static_cast<int>(std::floor(exact)) : static_cast<int>(std::ceil(exact));
        remainder = exact - static_cast<double>(pixels);
        return pixels;
    }

    bool ScrollSinglePageByPixels(HWND hwnd, PdfTab& tab, int deltaY)
    {
        if (deltaY == 0) return true;
        int maxScroll = SinglePageMaxScroll(tab);
        if (deltaY > 0)
        {
            if (maxScroll > 0 && g_scrollY < maxScroll)
            {
                ScrollStageTo(hwnd, std::min(maxScroll, g_scrollY + deltaY));
                return true;
            }
            if (tab.currentPage < tab.pageCount - 1)
            {
                ++tab.currentPage;
                g_scrollY = 0;
                g_contentHeight = SinglePageContentHeight(tab);
                TouchRecentProgress(tab, false);
                UpdateVertScrollBar(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                SchedulePagePrefetch(hwnd);
            }
            return true;
        }

        if (maxScroll > 0 && g_scrollY > 0)
        {
            ScrollStageTo(hwnd, std::max(0, g_scrollY + deltaY));
            return true;
        }
        if (tab.currentPage > 0)
        {
            --tab.currentPage;
            g_contentHeight = SinglePageContentHeight(tab);
            g_scrollY = SinglePageMaxScroll(tab);
            TouchRecentProgress(tab, false);
            UpdateVertScrollBar(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            SchedulePagePrefetch(hwnd);
        }
        return true;
    }

    bool ScrollByWheelDelta(HWND hwnd, POINT clientPt, int delta, bool ctrlDown)
    {
        PdfTab* tab = Active();
        if (ctrlDown && tab)
        {
            tab->zoom = std::clamp(tab->zoom + (delta > 0 ? 0.08 : -0.08), 0.25, 5.0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }

        if (g_sidebarOpen && !g_immersive && Hit(g_sidebar, clientPt.x, clientPt.y))
        {
            int pixels = WheelPixels(delta, S(52), g_sidebarWheelRemainder);
            if (pixels != 0)
            {
                g_smoothSidebarScroll = false;
                KillTimer(hwnd, 5);
                ScrollSidebarTo(hwnd, g_sidebarScrollY - pixels);
            }
            return true;
        }

        if (tab && !g_continuousRead)
        {
            int pixels = WheelPixels(delta, S(48), g_stageWheelRemainder);
            if (pixels != 0)
            {
                g_smoothStageScroll = false;
                KillTimer(hwnd, 4);
                ScrollSinglePageByPixels(hwnd, *tab, -pixels);
            }
            return true;
        }

        int pixels = WheelPixels(delta, S(48), g_stageWheelRemainder);
        if (pixels != 0)
        {
            g_smoothStageScroll = false;
            KillTimer(hwnd, 4);
            ScrollStageTo(hwnd, g_scrollY - pixels);
        }
        return true;
    }

    bool ScrollByPanPixels(HWND hwnd, POINT clientPt, int deltaY)
    {
        if (deltaY == 0) return true;
        if (g_sidebarOpen && !g_immersive && Hit(g_sidebar, clientPt.x, clientPt.y))
        {
            g_smoothSidebarScroll = false;
            KillTimer(hwnd, 5);
            ScrollSidebarTo(hwnd, g_sidebarScrollY - deltaY);
            return true;
        }

        PdfTab* tab = Active();
        if (tab && !g_continuousRead)
        {
            g_smoothStageScroll = false;
            KillTimer(hwnd, 4);
            return ScrollSinglePageByPixels(hwnd, *tab, -deltaY);
        }

        g_smoothStageScroll = false;
        KillTimer(hwnd, 4);
        ScrollStageTo(hwnd, g_scrollY - deltaY);
        return true;
    }

    int ClampStageScrollValue(int value)
    {
        PdfTab* tab = Active();
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
        }
        int maxScroll = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
        return std::clamp(value, 0, maxScroll);
    }

    int ClampSidebarScrollValue(int value)
    {
        int visible = std::max<int>(1, g_sidebar.bottom - SidebarScrollRect().top);
        int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
        return std::clamp(value, 0, maxScroll);
    }

    void StartSmoothStageScroll(HWND hwnd, int target)
    {
        g_stageScrollTarget = ClampStageScrollValue(target);
        g_smoothStageScroll = true;
        SetTimer(hwnd, 4, 12, nullptr);
    }

    void StartSmoothSidebarScroll(HWND hwnd, int target)
    {
        g_sidebarScrollTargetY = ClampSidebarScrollValue(target);
        g_smoothSidebarScroll = true;
        SetTimer(hwnd, 5, 12, nullptr);
    }

    void StartSidebarPanelAnimation(HWND hwnd, bool open)
    {
        bool wasOpen = g_sidebarOpen;
        if (open)
        {
            g_sidebarOpen = true;
            if (!wasOpen || g_sidebarWidth <= S(44)) g_sidebarWidth = 0;
            g_sidebarTargetWidth = std::clamp(g_sidebarExpandedWidth, S(220), S(520));
        }
        else
        {
            if (g_sidebarWidth > S(120)) g_sidebarExpandedWidth = g_sidebarWidth;
            g_sidebarTargetWidth = 0;
        }
        g_sidebarAnimating = true;
        g_sidebarAnimationStartWidth = g_sidebarWidth;
        g_sidebarAnimationStartMs = GetTickCount64();
        SetTimer(hwnd, 11, 16, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    bool HitSidebarSplitter(int x, int y)
    {
        return g_sidebarOpen && !g_immersive && Active() &&
            std::abs(x - g_sidebarWidth) <= S(5) &&
            y >= g_sidebar.top && y <= g_sidebar.bottom;
    }

    void ClampSidebarScroll()
    {
        int visible = std::max<int>(1, g_sidebar.bottom - SidebarScrollRect().top);
        int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
        g_sidebarScrollY = std::clamp(g_sidebarScrollY, 0, maxScroll);
    }

    void PaintToolbar(HDC dc, RECT client)
    {
        if (g_immersive) return;
        RECT bar{ 0, 0, client.right, S(38) };
        GlassSurface(dc, bar, g_statusPinned ? 154 : 188);
        HPEN pen = CreatePen(PS_SOLID, 1, T().line);
        auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, bar.bottom - 1, nullptr);
        LineTo(dc, client.right, bar.bottom - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        HFONT f = Font(9);
        auto oldFont = SelectObject(dc, f);
        int x = S(18);
        auto add = [&](int id, const wchar_t* text, int w, const wchar_t* tip)
        {
            RECT r{ x, S(4), x + S(w), S(34) };
            AddButton(id, r, text, tip);
            DrawFlatMenuItem(dc, { id, r, text, tip });
            x += S(w + 16);
        };
        add(CmdHome, L"首页", 42, L"");
        add(CmdOpen, L"打开", 42, L"");
        add(CmdSearch, L"查找", 42, L"");
        add(CmdBookmark, L"书签", 42, L"");
        add(CmdTheme, L"主题", 42, L"");
        add(CmdView, L"视图", 42, L"");
        add(CmdSettings, L"帮助", 42, L"");
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintTabs(HDC dc, RECT client)
    {
        g_tabRects.clear();
        if (g_immersive) return;
        RECT bar{ 0, S(38), client.right, S(76) };
        GlassSurface(dc, bar, 142);
        HPEN pen = CreatePen(PS_SOLID, 1, T().line);
        auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, bar.bottom - 1, nullptr);
        LineTo(dc, client.right, bar.bottom - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        HFONT f = Font(8);
        auto oldFont = SelectObject(dc, f);
        int x = S(8);
        for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        {
            RECT r{ x, S(46), x + S(190), S(71) };
            g_tabRects.push_back(r);
            bool active = i == g_active;
            bool hovered = i == g_hoverTab;
            if (IsGlassTheme() || IsAuroraTheme())
            {
                if (active || hovered)
                {
                    if (IsAuroraTheme())
                    {
                        AlphaFill(dc, r, active ? RGB(12, 56, 58) : RGB(9, 40, 44), active ? 196 : 142);
                        RoundFill(dc, r, S(9), active ? RGB(14, 55, 58) : RGB(9, 42, 46), active ? RGB(38, 132, 107) : RGB(28, 78, 76));
                        AlphaFill(dc, RECT{ r.left + S(1), r.top + S(1), r.right - S(1), r.top + S(8) }, RGB(64, 255, 155), 22);
                    }
                    else
                    {
                        AlphaFill(dc, r, RGB(255, 255, 255), active ? 168 : 112);
                        RoundFill(dc, r, S(9), active ? RGB(250, 252, 254) : RGB(255, 255, 255), active ? RGB(218, 224, 232) : RGB(230, 235, 241));
                        AlphaFill(dc, RECT{ r.left + S(1), r.top + S(1), r.right - S(1), r.top + S(8) }, RGB(255, 255, 255), 36);
                    }
                }
            }
            else
            {
                RoundFill(dc, r, S(9), (active || hovered) ? T().panel2 : T().panel, (active || hovered) ? T().line : T().panel);
            }
            bool dirty = g_tabs[static_cast<size_t>(i)]->dirty;
            RECT text{ r.left + S(10), r.top, r.right - S(dirty ? 50 : 36), r.bottom };
            Text(dc, g_tabs[static_cast<size_t>(i)]->name, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            if (dirty)
            {
                RECT mark{ r.right - S(49), r.top + S(3), r.right - S(32), r.bottom - S(3) };
                HFONT markFont = Font(11, FW_SEMIBOLD);
                HGDIOBJ oldMarkFont = SelectObject(dc, markFont);
                Text(dc, L"*", mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE, IsDarkTheme() || IsAuroraTheme() ? RGB(255, 210, 92) : RGB(216, 143, 35));
                SelectObject(dc, oldMarkFont);
                DeleteObject(markFont);
            }
            RECT close{ r.right - S(30), r.top + S(2), r.right - S(5), r.bottom - S(2) };
            bool closeHover = g_hoverTabClose == i;
            int closeCx = (close.left + close.right) / 2;
            int closeCy = (close.top + close.bottom) / 2;
            if (closeHover)
            {
                DrawSoftCircle(dc, closeCx, closeCy, S(8), RGB(255, 92, 92));
                HPEN xPen = CreatePen(PS_SOLID, std::max(1, S(1)), RGB(255, 255, 255));
                HGDIOBJ oldXPen = SelectObject(dc, xPen);
                MoveToEx(dc, closeCx - S(3), closeCy - S(3), nullptr);
                LineTo(dc, closeCx + S(3), closeCy + S(3));
                MoveToEx(dc, closeCx + S(3), closeCy - S(3), nullptr);
                LineTo(dc, closeCx - S(3), closeCy + S(3));
                SelectObject(dc, oldXPen);
                DeleteObject(xPen);
            }
            else if (active)
            {
                DrawSoftCircle(dc, closeCx, closeCy, S(3), T().accent);
            }
            if (false && closeHover)
            {
                RoundFill(dc, close, S(14), RGB(255, 92, 92), RGB(255, 92, 92));
                Text(dc, L"×", close, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255));
            }
            else if (false && active)
            {
                HBRUSH dot = CreateSolidBrush(RGB(35, 200, 75));
                auto oldBrush = SelectObject(dc, dot);
                auto oldDotPen = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, close.left + S(9), close.top + S(9), close.left + S(17), close.top + S(17));
                SelectObject(dc, oldDotPen);
                SelectObject(dc, oldBrush);
                DeleteObject(dot);
            }
            x += S(196);
        }
        RECT plus{ x, S(46), x + S(30), S(71) };
        AddButton(CmdOpen, plus, L"+", L"打开新文档");
        HFONT plusFont = Font(16);
        HGDIOBJ oldPlusFont = SelectObject(dc, plusFont);
        Text(dc, L"+", plus, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldPlusFont);
        DeleteObject(plusFont);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintHome(HDC dc, RECT client)
    {
        g_recentRects.clear();
        HFONT titleFont = Font(24, FW_SEMIBOLD);
        auto oldFont = SelectObject(dc, titleFont);
        RECT title{ S(42), S(118), client.right - S(42), S(164) };
        Text(dc, L"最近阅读", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);
        HFONT f = Font(10);
        oldFont = SelectObject(dc, f);
        RECT openCard{ S(42), S(188), S(212), S(398) };
        g_recentRects.push_back(openCard);
        RoundFill(dc, openCard, S(8), T().panel2, T().line);
        RECT openText{ openCard.left + S(16), openCard.top + S(72), openCard.right - S(16), openCard.bottom - S(20) };
        Text(dc, L"打开 PDF\n\n拖入文件或点击这里开始阅读", openText, DT_CENTER | DT_WORDBREAK, T().text);
        int x = S(238);
        int y = S(188);
        for (size_t i = 0; i < g_recent.size() && i < 12; ++i)
        {
            RECT card{ x, y, x + S(170), y + S(210) };
            g_recentRects.push_back(card);
            RoundFill(dc, card, S(8), T().panel, T().line);
            RECT cover{ card.left + S(42), card.top + S(20), card.right - S(42), card.top + S(134) };
            RoundFill(dc, cover, S(4), T().paper, T().line);
            Text(dc, L"PDF", cover, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().accent);
            RECT name{ card.left + S(12), card.top + S(146), card.right - S(12), card.top + S(174) };
            Text(dc, g_recent[i].name, name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            RECT meta{ card.left + S(12), card.top + S(174), card.right - S(12), card.bottom - S(10) };
            Text(dc, L"第 " + std::to_wstring(g_recent[i].page) + L" 页", meta, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            x += S(196);
            if (x + S(170) > client.right - S(36))
            {
                x = S(42);
                y += S(236);
            }
        }
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintHomeGrid(HDC dc, RECT client)
    {
        g_recentRects.clear();
        HFONT titleFont = Font(24, FW_SEMIBOLD);
        HGDIOBJ oldFont = SelectObject(dc, titleFont);
        RECT title{ S(42), S(118), client.right - S(42), S(164) };
        Text(dc, L"最近阅读", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);

        HFONT f = Font(10);
        oldFont = SelectObject(dc, f);
        int tileW = S(152);
        int coverH = S(198);
        int nameH = S(36);
        int tileH = coverH + nameH + S(10);
        int gapX = S(28);
        int gapY = S(28);
        int startX = S(42);
        int x = startX;
        int y = S(188);
        int maxRight = std::max<int>(startX + tileW, client.right - S(42));
        auto advance = [&]()
        {
            x += tileW + gapX;
            if (x + tileW > maxRight)
            {
                x = startX;
                y += tileH + gapY;
            }
        };
        auto drawCoverFrame = [&](RECT cover, bool active)
        {
            DrawContactShadow(dc, cover, active ? 38 : 28);
            DrawRoundedShadow(dc, cover, S(5.0f), S(3), S(3), S(4), active ? 26 : 18);
            RoundFill(dc, cover, S(5), T().paper, active ? T().accent : T().line);
        };

        RECT openTile{ x, y, x + tileW, y + tileH };
        g_recentRects.push_back(openTile);
        RECT openCover{ openTile.left, openTile.top, openTile.right, openTile.top + coverH };
        drawCoverFrame(openCover, false);
        HFONT plusFont = Font(30, FW_LIGHT);
        HGDIOBJ oldPlus = SelectObject(dc, plusFont);
        RECT plus{ openCover.left, openCover.top + S(42), openCover.right, openCover.top + S(92) };
        Text(dc, L"+", plus, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().accent);
        SelectObject(dc, oldPlus);
        DeleteObject(plusFont);
        RECT openMain{ openCover.left + S(10), openCover.top + S(100), openCover.right - S(10), openCover.top + S(128) };
        Text(dc, L"打开 PDF", openMain, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        RECT openSub{ openCover.left + S(14), openCover.top + S(132), openCover.right - S(14), openCover.bottom - S(18) };
        Text(dc, L"拖入文件或点击打开", openSub, DT_CENTER | DT_WORDBREAK, T().muted);
        RECT openName{ openTile.left, openCover.bottom + S(8), openTile.right, openTile.bottom };
        Text(dc, L"打开文档", openName, DT_CENTER | DT_TOP | DT_SINGLELINE, T().muted);
        advance();

        for (size_t i = 0; i < g_recent.size() && i < 12; ++i)
        {
            RECT tile{ x, y, x + tileW, y + tileH };
            g_recentRects.push_back(tile);
            RECT cover{ tile.left, tile.top, tile.right, tile.top + coverH };
            drawCoverFrame(cover, false);
            RECT inner{ cover.left + S(7), cover.top + S(7), cover.right - S(7), cover.bottom - S(7) };
            PageImage* thumb = LoadHomeThumb(g_recent[i], inner.right - inner.left);
            if (thumb && !thumb->bgra.empty())
            {
                DrawImageFit(dc, *thumb, inner);
            }
            else
            {
                HFONT pdfFont = Font(18, FW_SEMIBOLD);
                HGDIOBJ oldPdfFont = SelectObject(dc, pdfFont);
                Text(dc, L"PDF", inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().accent);
                SelectObject(dc, oldPdfFont);
                DeleteObject(pdfFont);
            }
            RECT name{ tile.left, cover.bottom + S(8), tile.right, tile.bottom };
            Text(dc, g_recent[i].name, name, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, T().text);
            advance();
        }

        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

#if 0
    void PaintSidebar(HDC dc, PdfTab& tab)
    {
        if (g_immersive || !g_sidebarOpen) return;
        GlassFill(dc, g_sidebar, T().panel, 214);
        g_sidebarRowPages.clear();
        g_sidebarRowRects.clear();
        HFONT f = Font(9);
        auto oldFont = SelectObject(dc, f);
        RECT tabBar{ g_sidebar.left + S(10), g_sidebar.top + S(10), g_sidebar.right - S(10), g_sidebar.top + S(42) };
        int tabWidth = (tabBar.right - tabBar.left - S(12)) / 3;
        const wchar_t* sideTabs[] = { L"目录", L"缩略图", L"书签" };
        for (int i = 0; i < 3; ++i)
        {
            RECT tabRect{ tabBar.left + i * (tabWidth + S(6)), tabBar.top, tabBar.left + i * (tabWidth + S(6)) + tabWidth, tabBar.bottom };
            RoundFill(dc, tabRect, S(8), i == 0 ? T().panel2 : T().panel, T().line);
            Text(dc, sideTabs[i], tabRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        }
        RECT sizeBar{ g_sidebar.left + S(12), g_sidebar.top + S(50), g_sidebar.right - S(12), g_sidebar.top + S(80) };
        Text(dc, L"文字大小", sizeBar, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
        RECT smallBtn{ sizeBar.right - S(78), sizeBar.top, sizeBar.right - S(42), sizeBar.bottom };
        RECT largeBtn{ sizeBar.right - S(36), sizeBar.top, sizeBar.right, sizeBar.bottom };
        AddButton(CmdOutlineSmall, smallBtn, L"A-");
        AddButton(CmdOutlineLarge, largeBtn, L"A+");
        DrawButton(dc, Button{ CmdOutlineSmall, smallBtn, L"A-" });
        DrawButton(dc, Button{ CmdOutlineLarge, largeBtn, L"A+" });
        HFONT outlineFont = Font(g_outlineFont);
        SelectObject(dc, outlineFont);
        int y = g_sidebar.top + S(90);
        int rows = tab.outline.empty() ? std::min(tab.pageCount, 300) : static_cast<int>(tab.outline.size());
        for (int i = 0; i < rows; ++i)
        {
            RECT row{ g_sidebar.left + S(10), y, g_sidebar.right - S(10), y + S(30) };
            int page = tab.outline.empty() ? i : tab.outline[static_cast<size_t>(i)].page;
            g_sidebarRowPages.push_back(page);
            if (page == tab.currentPage) RoundFill(dc, row, S(7), T().panel2, T().line);
            RECT tx{ row.left + S(8 + (tab.outline.empty() ? 0 : tab.outline[static_cast<size_t>(i)].depth * 14)), row.top, row.right - S(8), row.bottom };
            std::wstring label = tab.outline.empty() ? (L"第 " + std::to_wstring(i + 1) + L" 页") : tab.outline[static_cast<size_t>(i)].title;
            Text(dc, label, tx, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            y += S(32);
            if (y > g_sidebar.bottom - S(20)) break;
        }
        SelectObject(dc, oldFont);
        DeleteObject(outlineFont);
        DeleteObject(f);
    }

#endif

    void PaintSidebar(HDC dc, PdfTab& tab)
    {
        if (g_immersive || !g_sidebarOpen || g_sidebarWidth <= 0) return;
        RECT sidebarFull{ 0, g_sidebar.top, g_sidebar.right, g_sidebar.bottom };
        GlassSurface(dc, sidebarFull, 128);
        g_sidebarRowPages.clear();
        g_sidebarRowRects.clear();
        g_sidebarRowIds.clear();
        g_outlineToggleRects.clear();
        g_outlineToggleRowIds.clear();
        g_sideTabRects.clear();
        g_searchResultRects.clear();
        g_searchResultIds.clear();
        g_sidebarContentHeight = 0;

        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        int railW = S(44);
        RECT rail{ 0, g_sidebar.top, std::min<int>(railW, sidebarFull.right), g_sidebar.bottom };
        if (IsAuroraTheme()) AlphaFill(dc, rail, RGB(4, 26, 31), 174);
        else if (IsGlassTheme()) AlphaFill(dc, rail, RGB(247, 250, 253), 116);
        else Fill(dc, rail, T().panel);
        RECT railLine{ rail.right - std::max(1, S(1)), rail.top, rail.right, rail.bottom };
        GlassDivider(dc, railLine);
        RECT tabBar{ rail.left, g_sidebar.top + S(10), rail.right, g_sidebar.bottom };
        int tabWidth = S(34);
        int tabGap = S(5);
        int tabStart = tabBar.left + std::max<int>(0, (tabBar.right - tabBar.left - tabWidth) / 2);
        SideTabIcon sideIcons[] = { SideTabIcon::Outline, SideTabIcon::Thumbnails, SideTabIcon::Search, SideTabIcon::Notes };
        const wchar_t* sideTips[] = { L"目录", L"缩略图", L"查找" };
        for (int i = 0; i < 4; ++i)
        {
            RECT tabRect{ tabStart, tabBar.top + i * (tabWidth + tabGap), tabStart + tabWidth, tabBar.top + i * (tabWidth + tabGap) + tabWidth };
            g_sideTabRects.push_back(tabRect);
            AddButton(0, tabRect, L"", i == 0 ? L"目录" : (i == 1 ? L"缩略图" : (i == 2 ? L"查找" : L"注释")));
            bool hovered = g_mousePos.x >= tabRect.left && g_mousePos.x < tabRect.right && g_mousePos.y >= tabRect.top && g_mousePos.y < tabRect.bottom;
            bool active = i == g_sideMode;
            if (hovered || active)
            {
                if (IsAuroraTheme()) RoundFill(dc, tabRect, S(7), active ? RGB(16, 73, 64) : RGB(10, 48, 51), active ? RGB(16, 73, 64) : RGB(28, 79, 76));
                else RoundFill(dc, tabRect, S(7), active ? RGB(232, 238, 244) : T().panel2, active ? RGB(232, 238, 244) : T().line);
            }
            DrawSideTabIcon(dc, tabRect, sideIcons[i], active ? T().accent : SidebarIconColor());
        }

        auto visible = [&](RECT r)
        {
            return r.bottom >= g_sidebar.top + S(48) && r.top <= g_sidebar.bottom - S(10);
        };

        if (g_sideMode == 1)
        {
            RECT title{ g_sidebar.left + S(14), g_sidebar.top + S(8), g_sidebar.right - S(14), g_sidebar.top + S(34) };
            Text(dc, L"缩略图", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
            g_thumbSliderRect = RECT{ g_sidebar.left + S(58), g_sidebar.top + S(42), g_sidebar.right - S(58), g_sidebar.top + S(62) };
            HPEN track = CreatePen(PS_SOLID, S(3), T().line);
            HGDIOBJ oldPen = SelectObject(dc, track);
            int cy = (g_thumbSliderRect.top + g_thumbSliderRect.bottom) / 2;
            MoveToEx(dc, g_thumbSliderRect.left, cy, nullptr);
            LineTo(dc, g_thumbSliderRect.right, cy);
            SelectObject(dc, oldPen);
            DeleteObject(track);
            double ratio = static_cast<double>(g_thumbSize - ThumbSizeMin) / static_cast<double>(ThumbSizeMax - ThumbSizeMin);
            int knobX = g_thumbSliderRect.left + static_cast<int>((g_thumbSliderRect.right - g_thumbSliderRect.left) * std::clamp(ratio, 0.0, 1.0));
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float radius = S(6.4f);
                Gdiplus::SolidBrush glow(GdiColor(T().accent, IsDarkTheme() || IsAuroraTheme() ? 34 : 24));
                g.FillEllipse(&glow, knobX - radius - S(2.0f), cy - radius - S(1.0f), (radius + S(2.0f)) * 2.0f, (radius + S(2.0f)) * 2.0f);
                Gdiplus::SolidBrush knob(GdiColor(T().accent));
                g.FillEllipse(&knob, knobX - radius, cy - radius, radius * 2.0f, radius * 2.0f);
                Gdiplus::SolidBrush wash(GdiColor(RGB(255, 255, 255), IsDarkTheme() || IsAuroraTheme() ? 34 : 74));
                g.FillEllipse(&wash, knobX - radius * 0.48f, cy - radius * 0.58f, radius * 0.72f, radius * 0.52f);
            }

            int clipDc = SaveDC(dc);
            IntersectClipRect(dc, g_sidebar.left, g_sidebar.top + S(72), g_sidebar.right, g_sidebar.bottom);
            int y = g_sidebar.top + S(80) - g_sidebarScrollY;
            int content = S(80);
            for (int i = 0; i < tab.pageCount; ++i)
            {
                double pageWidthPixelsAtZoom1 = tab.pages[static_cast<size_t>(i)].widthPt * static_cast<double>(g_dpi) / 72.0;
                double ratioH = tab.pages[static_cast<size_t>(i)].heightPt / std::max(1.0, tab.pages[static_cast<size_t>(i)].widthPt);
                int thumbW = g_thumbSize;
                int thumbH = std::max<int>(S(56), static_cast<int>(thumbW * ratioH + 0.5));
                (void)pageWidthPixelsAtZoom1;
                int boxW = std::max(thumbW + S(36), S(160));
                int boxH = thumbH + S(58);
                RECT item{ g_sidebar.left + (g_sidebar.right - g_sidebar.left - boxW) / 2, y, g_sidebar.left + (g_sidebar.right - g_sidebar.left + boxW) / 2, y + boxH };
                if (visible(item))
                {
                    PageImage& thumb = EnsureThumb(tab, i);
                    thumbW = thumb.width;
                    thumbH = thumb.height;
                    boxW = std::max(thumbW + S(36), S(160));
                    boxH = thumbH + S(58);
                    item = RECT{ g_sidebar.left + (g_sidebar.right - g_sidebar.left - boxW) / 2, y, g_sidebar.left + (g_sidebar.right - g_sidebar.left + boxW) / 2, y + boxH };
                    g_sidebarRowPages.push_back(i);
                    g_sidebarRowRects.push_back(item);
                    g_sidebarRowIds.push_back(-1);
                    if (i == tab.currentPage) RoundFill(dc, item, S(6), T().panel2, T().accent);
                    RECT imageRect{ item.left + (boxW - thumbW) / 2, item.top + S(14), item.left + (boxW + thumbW) / 2, item.top + S(14) + thumbH };
                    DrawContactShadow(dc, imageRect, 16);
                    DrawRoundedShadow(dc, imageRect, S(3.0f), S(1), S(2), S(2), 12);
                    BITMAPINFO info{};
                    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth = thumb.width;
                    info.bmiHeader.biHeight = -thumb.height;
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    StretchDIBits(dc, imageRect.left, imageRect.top, thumbW, thumbH, 0, 0, thumb.width, thumb.height, thumb.bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
                    RECT pageLabel{ item.left, imageRect.bottom + S(8), item.right, item.bottom };
                    Text(dc, std::to_wstring(i + 1), pageLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
                }
                y += boxH + S(22);
                content += boxH + S(22);
            }
            RestoreDC(dc, clipDc);
            g_sidebarContentHeight = std::max(0, content - S(80));
        }
        else if (g_sideMode == 2)
        {
            g_searchInputRect = RECT{ g_sidebar.left + S(14), g_sidebar.top + S(8), g_sidebar.right - S(14), g_sidebar.top + S(48) };
            RoundFill(dc, g_searchInputRect, S(8), T().paper, g_searchInputFocused ? T().accent : T().line);
            RECT searchIcon{ g_searchInputRect.left + S(13), g_searchInputRect.top + S(7), g_searchInputRect.left + S(37), g_searchInputRect.bottom - S(7) };
            DrawSearchGlyph(dc, searchIcon, T().muted);
            RECT inputText{ g_searchInputRect.left + S(43), g_searchInputRect.top, g_searchInputRect.right - S(82), g_searchInputRect.bottom };
            Text(dc, g_searchQuery.empty() ? L"输入关键词" : g_searchQuery, inputText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_searchQuery.empty() ? T().muted : T().text);
            if (g_searchInputFocused && g_searchCaretVisible)
            {
                SIZE sz{};
                std::wstring caretText = g_searchQuery;
                GetTextExtentPoint32W(dc, caretText.c_str(), static_cast<int>(caretText.size()), &sz);
                int cx = std::clamp(inputText.left + sz.cx + S(2), inputText.left + S(2), inputText.right - S(4));
                HPEN caret = CreatePen(PS_SOLID, std::max(1, S(1)), T().accent);
                HGDIOBJ oldPen = SelectObject(dc, caret);
                MoveToEx(dc, cx, inputText.top + S(8), nullptr);
                LineTo(dc, cx, inputText.bottom - S(8));
                SelectObject(dc, oldPen);
                DeleteObject(caret);
            }
            RECT clearRect{ g_searchInputRect.right - S(77), g_searchInputRect.top + S(6), g_searchInputRect.right - S(49), g_searchInputRect.bottom - S(6) };
            if (Hit(clearRect, g_mousePos.x, g_mousePos.y))
            {
                RoundFill(dc, clearRect, S(6), T().panel2, T().line);
            }
            AddButton(CmdSearchClear, clearRect, L"×");
            DrawCloseGlyph(dc, clearRect, T().muted);
            RECT gearRect{ g_searchInputRect.right - S(38), g_searchInputRect.top + S(8), g_searchInputRect.right - S(14), g_searchInputRect.bottom - S(8) };
            RECT gearHotRect{ gearRect.left - S(3), gearRect.top - S(3), gearRect.right + S(3), gearRect.bottom + S(3) };
            if (Hit(gearHotRect, g_mousePos.x, g_mousePos.y))
            {
                RoundFill(dc, gearHotRect, S(6), T().panel2, T().line);
            }
            AddButton(0, gearHotRect, L"");
            DrawGearGlyph(dc, gearRect, T().muted);

            int rowLeft = g_sidebar.left + S(14);
            int rowRight = g_sidebar.right - S(14);
            int gap = S(8);
            int btnW = std::max(1, (rowRight - rowLeft - gap * 2) / 3);
            RECT prev{ rowLeft, g_sidebar.top + S(60), rowLeft + btnW, g_sidebar.top + S(94) };
            RECT next{ prev.right + gap, prev.top, prev.right + gap + btnW, prev.bottom };
            RECT run{ next.right + gap, prev.top, rowRight, prev.bottom };
            AddButton(CmdSearchPrev, prev, L"上一处");
            AddButton(CmdSearchNext, next, L"下一处");
            AddButton(CmdSearchRun, run, L"查找");
            DrawSearchActionButton(dc, CmdSearchPrev, prev, L"上一处");
            DrawSearchActionButton(dc, CmdSearchNext, next, L"下一处");
            DrawSearchActionButton(dc, CmdSearchRun, run, L"查找");

            RECT summary{ g_sidebar.left + S(14), g_sidebar.top + S(108), g_sidebar.right - S(14), g_sidebar.top + S(138) };
            std::wstring summaryText = g_searchHasRun ? (L"精确查找 (" + std::to_wstring(SearchHitTotal()) + L")") : L"输入关键词后点击查找";
            Text(dc, summaryText, summary, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);

            int clipDc = SaveDC(dc);
            IntersectClipRect(dc, g_sidebar.left, g_sidebar.top + S(142), g_sidebar.right, g_sidebar.bottom);
            int y = g_sidebar.top + S(146) - g_sidebarScrollY;
            int content = S(146);
            for (size_t i = 0; i < g_searchResults.size(); ++i)
            {
                RECT row{ g_sidebar.left + S(14), y, g_sidebar.right - S(14), y + S(54) };
                if (visible(row))
                {
                    g_searchResultRects.push_back(row);
                    g_searchResultIds.push_back(static_cast<int>(i));
                    g_sidebarRowPages.push_back(g_searchResults[i].page);
                    g_sidebarRowRects.push_back(row);
                    g_sidebarRowIds.push_back(-1);
                    bool selected = static_cast<int>(i) == g_searchCurrent;
                    bool hovered = static_cast<int>(i) == g_hoverSearchResult;
                    if (selected || hovered)
                    {
                        COLORREF fill = selected ? MixColor(T().panel2, T().accent, IsDarkTheme() || IsAuroraTheme() ? 0.20 : 0.10) : T().panel2;
                        COLORREF line = selected ? MixColor(T().line, T().accent, 0.45) : T().line;
                        RoundFill(dc, row, S(7), fill, line);
                    }
                    RECT text{ row.left + S(8), row.top, row.right - S(54), row.bottom };
                    DrawHighlightedSnippet(dc, g_searchResults[i], text, T().muted);
                    RECT page{ row.right - S(44), row.top, row.right - S(6), row.bottom };
                    Text(dc, std::to_wstring(g_searchResults[i].page + 1), page, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, T().muted);
                }
                y += S(56);
                content += S(56);
            }
            RestoreDC(dc, clipDc);
            g_sidebarContentHeight = std::max(0, content - S(146));
        }
        else if (g_sideMode == 3)
        {
            RECT title{ g_sidebar.left + S(14), g_sidebar.top + S(6), g_sidebar.right - S(14), g_sidebar.top + S(30) };
            Text(dc, L"\u6ce8\u91ca", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
            int buttonTop = g_sidebar.top + S(34);
            int buttonSize = S(25);
            int buttonGap = S(5);
            int buttonLeft = g_sidebar.left + S(14);
            struct NoteTool { int id; const wchar_t* text; const wchar_t* tip; };
            NoteTool tools[] = {
                { CmdNoteExpandAll, L"+", L"\u5c55\u5f00\u6240\u6709\u6ce8\u91ca" },
                { CmdNoteCollapseAll, L"-", L"\u6536\u8d77\u6240\u6709\u6ce8\u91ca" },
                { CmdNotePrev, L"<", L"\u4e0a\u4e00\u4e2a\u6ce8\u91ca" },
                { CmdNoteNext, L">", L"\u4e0b\u4e00\u4e2a\u6ce8\u91ca" },
                { CmdNoteDelete, L"x", L"\u5220\u9664\u5f53\u524d\u6ce8\u91ca" },
                { CmdNoteDeleteAll, L"\u6e05", L"\u5220\u9664\u6240\u6709\u6ce8\u91ca" }
            };
            for (int i = 0; i < static_cast<int>(std::size(tools)); ++i)
            {
                RECT r{ buttonLeft + i * (buttonSize + buttonGap), buttonTop, buttonLeft + i * (buttonSize + buttonGap) + buttonSize, buttonTop + buttonSize };
                AddButton(tools[i].id, r, tools[i].text, tools[i].tip);
                DrawStatusHotArea(dc, r, 6);
                Text(dc, tools[i].text, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
            }
            RECT summary{ g_sidebar.left + S(14), g_sidebar.top + S(64), g_sidebar.right - S(14), g_sidebar.top + S(88) };
            Text(dc, L"\u5171 " + std::to_wstring(tab.annotations.size()) + L" \u5904\u6ce8\u91ca", summary, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);

            int clipDc = SaveDC(dc);
            IntersectClipRect(dc, g_sidebar.left, g_sidebar.top + S(90), g_sidebar.right, g_sidebar.bottom);
            int y = g_sidebar.top + S(94) - g_sidebarScrollY;
            int content = S(94);
            int lastPage = -1;
            for (size_t i = 0; i < tab.annotations.size(); ++i)
            {
                PdfAnnotation const& annotation = tab.annotations[i];
                if (annotation.page != lastPage)
                {
                    lastPage = annotation.page;
                    RECT pageRow{ g_sidebar.left + S(14), y, g_sidebar.right - S(14), y + S(30) };
                    if (visible(pageRow))
                    {
                        g_sidebarRowPages.push_back(annotation.page);
                        g_sidebarRowRects.push_back(pageRow);
                        g_sidebarRowIds.push_back(-100000 - annotation.page);
                        bool collapsed = g_collapsedAnnotationPages.find(annotation.page) != g_collapsedAnnotationPages.end();
                        bool hovered = Hit(pageRow, g_mousePos.x, g_mousePos.y);
                        if (hovered) RoundFill(dc, pageRow, S(6), T().panel2, T().line);
                        RECT twist{ pageRow.left + S(2), pageRow.top + S(7), pageRow.left + S(18), pageRow.bottom - S(7) };
                        Text(dc, collapsed ? L"+" : L"-", twist, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
                        RECT pageText{ pageRow.left + S(24), pageRow.top, pageRow.right, pageRow.bottom };
                        Text(dc, L"\u7b2c " + std::to_wstring(annotation.page + 1) + L" \u9875", pageText, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
                    }
                    y += S(32);
                    content += S(32);
                }
                if (g_collapsedAnnotationPages.find(annotation.page) != g_collapsedAnnotationPages.end()) continue;

                RECT row{ g_sidebar.left + S(14), y, g_sidebar.right - S(14), y + S(86) };
                if (visible(row))
                {
                    g_sidebarRowPages.push_back(annotation.page);
                    g_sidebarRowRects.push_back(row);
                    g_sidebarRowIds.push_back(static_cast<int>(i));
                    bool selected = static_cast<int>(i) == g_activeAnnotation;
                    bool hovered = Hit(row, g_mousePos.x, g_mousePos.y);
                    if (hovered || selected)
                    {
                        COLORREF fill = selected ? MixColor(T().panel2, T().accent, IsDarkTheme() || IsAuroraTheme() ? 0.22 : 0.12) : T().panel2;
                        COLORREF line = selected ? T().accent : T().line;
                        RoundFill(dc, row, S(8), fill, line);
                    }
                    COLORREF markColor = AnnotationColor(annotation);
                    RECT swatch{ row.left + S(10), row.top + S(12), row.left + S(22), row.top + S(24) };
                    Gdiplus::Graphics g(dc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::SolidBrush brush(GdiColor(markColor));
                    g.FillEllipse(&brush, static_cast<float>(swatch.left), static_cast<float>(swatch.top), static_cast<float>(swatch.right - swatch.left), static_cast<float>(swatch.bottom - swatch.top));
                    RECT topText{ row.left + S(30), row.top + S(7), row.right - S(8), row.top + S(28) };
                    Text(dc, AnnotationKindName(annotation.kind) + L"  10726", topText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
                    RECT timeText{ row.left + S(30), row.top + S(27), row.right - S(8), row.top + S(45) };
                    Text(dc, FormatTimeMs(annotation.createdAt), timeText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().muted);
                    RECT preview{ row.left + S(10), row.top + S(48), row.right - S(10), row.bottom - S(8) };
                    std::wstring body = !annotation.note.empty() ? annotation.note : annotation.text;
                    Text(dc, body.empty() ? L"\u65e0\u6587\u672c\u5185\u5bb9" : ElideText(body, 48), preview, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS, T().muted);
                }
                y += S(92);
                content += S(92);
            }
            if (tab.annotations.empty())
            {
                RECT empty{ g_sidebar.left + S(14), g_sidebar.top + S(98), g_sidebar.right - S(14), g_sidebar.top + S(138) };
                Text(dc, L"\u6682\u65e0\u6ce8\u91ca", empty, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            }
            RestoreDC(dc, clipDc);
            g_sidebarContentHeight = std::max(0, content - S(94));
        }
        else
        {
            RECT sizeBar{ g_sidebar.left + S(12), g_sidebar.top + S(8), g_sidebar.right - S(12), g_sidebar.top + S(36) };
            Text(dc, L"文字大小", sizeBar, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            RECT smallBtn{ sizeBar.right - S(78), sizeBar.top, sizeBar.right - S(42), sizeBar.bottom };
            RECT largeBtn{ sizeBar.right - S(36), sizeBar.top, sizeBar.right, sizeBar.bottom };
            AddButton(CmdOutlineSmall, smallBtn, L"A-", L"缩小目录字体");
            AddButton(CmdOutlineLarge, largeBtn, L"A+", L"放大目录字体");
            DrawButton(dc, Button{ CmdOutlineSmall, smallBtn, L"A-", L"缩小目录字体" });
            DrawButton(dc, Button{ CmdOutlineLarge, largeBtn, L"A+", L"放大目录字体" });
            Fill(dc, sizeBar, T().panel);
            RECT toggleAll{ sizeBar.left, sizeBar.top, sizeBar.left + S(92), sizeBar.bottom };
            std::wstring toggleText = g_collapsedOutlineRows.empty() ? L"收起所有" : L"展开所有";
            AddButton(CmdOutlineToggleAll, toggleAll, toggleText);
            Text(dc, toggleText, toggleAll, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            RECT smallBtnNew{ sizeBar.right - S(64), sizeBar.top, sizeBar.right - S(34), sizeBar.bottom };
            RECT largeBtnNew{ sizeBar.right - S(30), sizeBar.top, sizeBar.right, sizeBar.bottom };
            AddButton(CmdOutlineSmall, smallBtnNew, L"A-", L"缩小目录字体");
            AddButton(CmdOutlineLarge, largeBtnNew, L"A+", L"放大目录字体");
            DrawStatusHotArea(dc, smallBtnNew, 5);
            DrawStatusHotArea(dc, largeBtnNew, 5);
            HFONT sizeFont = Font(10);
            HGDIOBJ oldSizeFont = SelectObject(dc, sizeFont);
            Text(dc, L"A-", smallBtnNew, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
            Text(dc, L"A+", largeBtnNew, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
            SelectObject(dc, oldSizeFont);
            DeleteObject(sizeFont);
            HFONT outlineFont = Font(EffectiveOutlineFont());
            SelectObject(dc, outlineFont);
            int clipDc = SaveDC(dc);
            RECT scrollArea{ g_sidebar.left, g_sidebar.top + S(44), g_sidebar.right, g_sidebar.bottom };
            IntersectClipRect(dc, scrollArea.left, scrollArea.top, scrollArea.right, scrollArea.bottom);
            int rows = tab.outline.empty() ? std::min(tab.pageCount, 300) : static_cast<int>(tab.outline.size());
            HPEN treePen = CreatePen(PS_DOT, 1, SidebarTreeColor());
            HGDIOBJ oldPen = SelectObject(dc, treePen);
            int sampleDepth = rows > 0 && !tab.outline.empty() ? tab.outline[0].depth : 0;
            int sampleLineX = g_sidebar.left + S(10 + sampleDepth * 18);
            int sampleIconLeft = sampleLineX + S(12);
            int sampleTextWidth = std::max<int>(24, static_cast<int>((g_sidebar.right - S(30)) - (sampleIconLeft + S(10) + S(5)) - S(4)));
            EnsureOutlineLayout(dc, tab, g_sidebar.right - g_sidebar.left);
            auto firstVisible = std::lower_bound(
                tab.outlineLayout.begin(),
                tab.outlineLayout.end(),
                g_sidebarScrollY,
                [](OutlineLayoutRow const& row, int scrollY)
                {
                    return row.top + row.height < scrollY;
                });
            for (auto it = firstVisible; it != tab.outlineLayout.end(); ++it)
            {
                if (it->top > g_sidebarScrollY + (scrollArea.bottom - scrollArea.top) + S(8)) break;
                int i = it->id;
                int depth = it->depth;
                int lineX = g_sidebar.left + S(10 + depth * 18);
                int iconH = S(14);
                int iconW = S(10);
                int iconLeft = lineX + S(12);
                int pageLeft = g_sidebar.right - S(30);
                int textLeft = iconLeft + iconW + S(5);
                int textWidth = std::max(24, pageLeft - textLeft - S(4));
                std::wstring label = tab.outline.empty() ? (L"第 " + std::to_wstring(i + 1) + L" 页") : tab.outline[static_cast<size_t>(i)].title;
                int textH = textWidth == sampleTextWidth ? it->textHeight : MeasureTextHeight(dc, label, textWidth, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                int rowH = it->height;
                int y = scrollArea.top + it->top - g_sidebarScrollY;
                RECT row{ g_sidebar.left + S(3), y, g_sidebar.right - S(3), y + rowH };
                int page = it->page;
                if (visible(row))
                {
                    g_sidebarRowPages.push_back(page);
                    g_sidebarRowRects.push_back(row);
                    g_sidebarRowIds.push_back(i);
                    TEXTMETRICW tm{};
                    GetTextMetricsW(dc, &tm);
                    int textTop = row.top + (rowH - textH) / 2;
                    int firstLineCenterY = textTop + std::max<int>(1, tm.tmHeight) / 2;
                    MoveToEx(dc, lineX, row.top - S(10), nullptr);
                    LineTo(dc, lineX, row.bottom - S(4));
                    MoveToEx(dc, lineX, firstLineCenterY, nullptr);
                    LineTo(dc, iconLeft - S(4), firstLineCenterY);
                    bool selected = tab.outline.empty() ? page == tab.currentPage : (g_selectedOutlineRow == i);
                    bool hovered = Hit(row, g_mousePos.x, g_mousePos.y);
                    if (hovered && !selected) RoundFill(dc, row, S(5), SidebarHoverFill(), SidebarHoverFill());
                    if (selected) RoundFill(dc, row, S(4), SidebarSelectedFill(), SidebarSelectedFill());

                    if (it->hasChildren)
                    {
                        RECT toggle{ lineX - S(5), firstLineCenterY - S(5), lineX + S(5), firstLineCenterY + S(5) };
                        g_outlineToggleRects.push_back(toggle);
                        g_outlineToggleRowIds.push_back(i);
                        bool toggleHover = Hit(toggle, g_mousePos.x, g_mousePos.y);
                        COLORREF toggleBg = SidebarToggleFill(toggleHover);
                        COLORREF toggleLine = SidebarToggleLine(toggleHover);
                        RoundFill(dc, toggle, S(2), toggleBg, toggleBg);
                        HPEN togglePen = CreatePen(PS_SOLID, std::max(1, S(1)), toggleLine);
                        HGDIOBJ oldTogglePen = SelectObject(dc, togglePen);
                        int cy = (toggle.top + toggle.bottom) / 2;
                        MoveToEx(dc, toggle.left + S(2), cy, nullptr);
                        LineTo(dc, toggle.right - S(2), cy);
                        if (g_collapsedOutlineRows.find(i) != g_collapsedOutlineRows.end())
                        {
                            int cx = (toggle.left + toggle.right) / 2;
                            MoveToEx(dc, cx, toggle.top + S(2), nullptr);
                            LineTo(dc, cx, toggle.bottom - S(2));
                        }
                        SelectObject(dc, oldTogglePen);
                        DeleteObject(togglePen);
                    }

                    RECT mark{ iconLeft, firstLineCenterY - iconH / 2, iconLeft + iconW, firstLineCenterY + (iconH + 1) / 2 };
                    DrawBookmarkIcon(dc, mark, selected ? ForegroundOn(SidebarSelectedFill()) : SidebarIconColor(), false);

                    RECT pageRect{ pageLeft, row.top, row.right - S(3), row.bottom };
                    RECT tx{ textLeft, textTop, pageRect.left - S(4), textTop + textH };
                    COLORREF rowText = selected ? ForegroundOn(SidebarSelectedFill()) : T().text;
                    COLORREF rowMuted = selected ? MixColor(ForegroundOn(SidebarSelectedFill()), T().paper, 0.18) : T().muted;
                    Text(dc, label, tx, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, rowText);
                    Text(dc, std::to_wstring(page + 1), pageRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, rowMuted);
                }
            }
            RestoreDC(dc, clipDc);
            SelectObject(dc, oldPen);
            DeleteObject(treePen);
            SelectObject(dc, f);
            DeleteObject(outlineFont);
        }
        ClampSidebarScroll();
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintPages(HDC dc, PdfTab& tab, RECT paintClip)
    {
        RECT stageDirty{};
        if (!IntersectRect(&stageDirty, &g_stage, &paintClip)) return;
        if (IsAuroraTheme())
        {
            PaintGlassBackdrop(dc, stageDirty);
            AlphaFill(dc, stageDirty, RGB(0, 8, 10), 36);
        }
        else if (IsGlassTheme())
        {
            Fill(dc, stageDirty, T().stage);
            RECT washTop{ stageDirty.left, stageDirty.top, stageDirty.right, stageDirty.top + std::max<int>(1, (stageDirty.bottom - stageDirty.top) / 3) };
            AlphaFill(dc, washTop, RGB(255, 255, 255), 38);
        }
        else Fill(dc, stageDirty, T().stage);
        int savedDc = SaveDC(dc);
        IntersectClipRect(dc, stageDirty.left, stageDirty.top, stageDirty.right, stageDirty.bottom);
        int gap = g_continuousRead ? PageGap() : 0;
        int content = g_continuousRead ? ContinuousContentHeight(tab) : SinglePageContentHeight(tab);
        int firstVisiblePage = 0;
        if (g_continuousRead)
        {
            int scan = S(28);
            int threshold = std::max(0, g_scrollY - S(96));
            for (int p = 0; p < tab.pageCount; ++p)
            {
                int next = scan + PageH(tab, p) + gap;
                if (next >= threshold) break;
                scan = next;
                firstVisiblePage = p + 1;
            }
        }
        int firstPage = g_continuousRead ? std::max(0, firstVisiblePage - 1) : std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        int y = g_continuousRead ? (g_stage.top + S(28) - g_scrollY) : (g_stage.top + S(28) - g_scrollY);
        for (int p = 0; g_continuousRead && p < firstPage; ++p)
        {
            y += PageH(tab, p) + gap;
        }
        int lastPage = g_continuousRead ? tab.pageCount : std::min(tab.pageCount, firstPage + 1);
        for (int i = firstPage; i < lastPage; ++i)
        {
            int pageW = PageW(tab, i);
            int pageH = PageH(tab, i);
            int x = g_stage.left + std::max<int>(S(28), ((g_stage.right - g_stage.left) - pageW) / 2);
            RECT pageRect{ x, y, x + pageW, y + pageH };
            if (g_continuousRead && pageRect.top > stageDirty.bottom + S(64)) break;
            if (Intersects(pageRect, stageDirty))
            {
                RECT shadowRight1{ pageRect.right, pageRect.top + S(1), pageRect.right + std::max(1, S(1)), pageRect.bottom + S(1) };
                RECT shadowRight2{ pageRect.right + std::max(1, S(1)), pageRect.top + S(2), pageRect.right + std::max(2, S(2)), pageRect.bottom + S(2) };
                RECT shadowBottom1{ pageRect.left + S(1), pageRect.bottom, pageRect.right + S(1), pageRect.bottom + std::max(1, S(1)) };
                RECT shadowBottom2{ pageRect.left + S(2), pageRect.bottom + std::max(1, S(1)), pageRect.right + S(2), pageRect.bottom + std::max(2, S(2)) };
                RECT innerTopWash{ pageRect.left, pageRect.top, pageRect.right, pageRect.top + std::max(1, S(1)) };
                RECT innerLeftWash{ pageRect.left, pageRect.top, pageRect.left + std::max(1, S(1)), pageRect.bottom };
                if (Intersects(shadowRight1, stageDirty)) AlphaFill(dc, shadowRight1, RGB(114, 126, 138), 13);
                if (Intersects(shadowRight2, stageDirty)) AlphaFill(dc, shadowRight2, RGB(114, 126, 138), 5);
                if (Intersects(shadowBottom1, stageDirty)) AlphaFill(dc, shadowBottom1, RGB(114, 126, 138), 13);
                if (Intersects(shadowBottom2, stageDirty)) AlphaFill(dc, shadowBottom2, RGB(114, 126, 138), 5);
                if (Intersects(innerTopWash, stageDirty)) AlphaFill(dc, innerTopWash, RGB(255, 255, 255), 20);
                if (Intersects(innerLeftWash, stageDirty)) AlphaFill(dc, innerLeftWash, RGB(255, 255, 255), 16);
                RECT drawRect{};
                if (IntersectRect(&drawRect, &pageRect, &stageDirty))
                {
                    int savedPageDc = SaveDC(dc);
                    IntersectClipRect(dc, pageRect.left, pageRect.top, pageRect.right, pageRect.bottom);
                    if (tab.zoom <= 2.0)
                    {
                        PageImage& image = EnsurePage(tab, i);
                        if (!image.bgra.empty())
                        {
                            BITMAPINFO info{};
                            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                            info.bmiHeader.biWidth = image.width;
                            info.bmiHeader.biHeight = -image.height;
                            info.bmiHeader.biPlanes = 1;
                            info.bmiHeader.biBitCount = 32;
                            info.bmiHeader.biCompression = BI_RGB;
                            int sx = std::clamp<int>(static_cast<int>((drawRect.left - pageRect.left) * image.width / std::max(1, pageW)), 0, image.width - 1);
                            int sy = std::clamp<int>(static_cast<int>((drawRect.top - pageRect.top) * image.height / std::max(1, pageH)), 0, image.height - 1);
                            int sw = std::max<int>(1, std::min<int>(image.width - sx, static_cast<int>((drawRect.right - drawRect.left) * image.width / std::max(1, pageW))));
                            int sh = std::max<int>(1, std::min<int>(image.height - sy, static_cast<int>((drawRect.bottom - drawRect.top) * image.height / std::max(1, pageH))));
                            int srcY = std::max<int>(0, image.height - sy - sh);
                            StretchDIBits(dc, drawRect.left, drawRect.top, drawRect.right - drawRect.left, drawRect.bottom - drawRect.top, sx, srcY, sw, sh, image.bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
                        }
                    }
                    else
                    {
                        int localLeft = drawRect.left - pageRect.left;
                        int localTop = drawRect.top - pageRect.top;
                        int localRight = drawRect.right - pageRect.left;
                        int localBottom = drawRect.bottom - pageRect.top;
                        int tileLeft = (localLeft / TileSize) * TileSize;
                        int tileTop = (localTop / TileSize) * TileSize;
                        for (int ty = tileTop; ty < localBottom; ty += TileSize)
                        {
                            int h = std::min(TileSize, pageH - ty);
                            if (h <= 0) continue;
                            for (int tx = tileLeft; tx < localRight; tx += TileSize)
                            {
                                int w = std::min(TileSize, pageW - tx);
                                if (w <= 0) continue;
                                TileKey key{ i, static_cast<int>(tab.zoom * 10000.0 + 0.5), g_theme, tx, ty, w, h };
                                PageImage& image = EnsureTile(tab, key);
                                if (image.bgra.empty()) continue;
                                BITMAPINFO info{};
                                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                                info.bmiHeader.biWidth = image.width;
                                info.bmiHeader.biHeight = -image.height;
                                info.bmiHeader.biPlanes = 1;
                                info.bmiHeader.biBitCount = 32;
                                info.bmiHeader.biCompression = BI_RGB;
                                RECT tileDest{ pageRect.left + tx, pageRect.top + ty, pageRect.left + tx + w, pageRect.top + ty + h };
                                StretchDIBits(dc, tileDest.left, tileDest.top, image.width, image.height, 0, 0, image.width, image.height, image.bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
                            }
                        }
                    }
                    if (g_searchCurrent >= 0 && g_searchCurrent < static_cast<int>(g_searchResults.size()))
                    {
                        SearchResult const& current = g_searchResults[static_cast<size_t>(g_searchCurrent)];
                        if (current.hasQuad && current.page == i)
                        {
                            RECT hitRect = SearchQuadRect(tab, current, pageRect);
                            RECT visibleHit{};
                            if (IntersectRect(&visibleHit, &hitRect, &pageRect))
                            {
                                AlphaFill(dc, visibleHit, RGB(255, 205, 64), IsDarkTheme() || IsAuroraTheme() ? 108 : 122);
                                RECT top{ visibleHit.left, visibleHit.top, visibleHit.right, visibleHit.top + 1 };
                                RECT bottom{ visibleHit.left, visibleHit.bottom - 1, visibleHit.right, visibleHit.bottom };
                                RECT left{ visibleHit.left, visibleHit.top, visibleHit.left + 1, visibleHit.bottom };
                                RECT right{ visibleHit.right - 1, visibleHit.top, visibleHit.right, visibleHit.bottom };
                                Fill(dc, top, RGB(234, 163, 38));
                                Fill(dc, bottom, RGB(234, 163, 38));
                                Fill(dc, left, RGB(234, 163, 38));
                                Fill(dc, right, RGB(234, 163, 38));
                            }
                        }
                    }
                    for (PdfAnnotation const& annotation : tab.annotations)
                    {
                        if (annotation.page != i) continue;
                        RECT mark = DenormalizeAnnotationRect(annotation, pageRect);
                        RECT visibleMark{};
                        if (!IntersectRect(&visibleMark, &mark, &pageRect)) continue;
                        COLORREF annotationColor = AnnotationColor(annotation);
                        if (annotation.kind == AnnotationKind::Highlight)
                        {
                            AlphaFill(dc, visibleMark, annotationColor, IsDarkTheme() || IsAuroraTheme() ? 82 : 112);
                        }
                        else
                        {
                            int lineY = annotation.kind == AnnotationKind::Underline ?
                                std::max(visibleMark.top, visibleMark.bottom - S(3)) :
                                (visibleMark.top + visibleMark.bottom) / 2;
                            HPEN markPen = CreatePen(PS_SOLID, std::max(1, S(2)), annotationColor);
                            HGDIOBJ oldMarkPen = SelectObject(dc, markPen);
                            MoveToEx(dc, visibleMark.left, lineY, nullptr);
                            LineTo(dc, visibleMark.right, lineY);
                            SelectObject(dc, oldMarkPen);
                            DeleteObject(markPen);
                        }
                    }
                    if ((g_hasContentSelection || g_selectingContent) && g_selectionPage == i)
                    {
                        if (!g_selectionQuads.empty())
                        {
                            for (fz_quad const& quad : g_selectionQuads)
                            {
                                RECT selection = QuadToRect(quad, pageRect);
                                RECT visibleSelection{};
                                if (IntersectRect(&visibleSelection, &selection, &pageRect))
                                {
                                    InflateRect(&visibleSelection, S(1), S(1));
                                    AlphaFill(dc, visibleSelection, RGB(118, 169, 230), IsDarkTheme() || IsAuroraTheme() ? 82 : 70);
                                }
                            }
                        }
                        else if (g_selectingContent)
                        {
                            RECT selection = CurrentSelectionRect();
                            RECT visibleSelection{};
                            if (IntersectRect(&visibleSelection, &selection, &pageRect))
                            {
                                AlphaFill(dc, visibleSelection, RGB(86, 156, 255), IsDarkTheme() || IsAuroraTheme() ? 32 : 24);
                            }
                        }
                    }
                    RestoreDC(dc, savedPageDc);
                }
            }
            y += pageH + gap;
        }
        RestoreDC(dc, savedDc);
        g_contentHeight = content;
    }

    std::wstring ElideText(std::wstring text, size_t maxChars)
    {
        if (text.size() <= maxChars) return text;
        if (maxChars <= 1) return text.substr(0, maxChars);
        return text.substr(0, maxChars - 1) + L"...";
    }

    void PaintAnnotationToolbar(HDC dc, PdfTab& tab, RECT client)
    {
        g_annotationToolbarRect = {};
        g_annotationCopyRect = {};
        g_annotationNoteRect = {};
        g_annotationDeleteRect = {};
        g_annotationFormatRect = {};
        g_annotationNoteCloseRect = {};
        g_annotationNotePanelRect = {};
        g_annotationNoteInputRect = {};
        g_annotationColorRects.clear();
        if (!g_annotationToolbarOpen) return;

        PdfAnnotation* annotation = nullptr;
        if (!ActiveAnnotation(tab, annotation)) return;

        RECT pageRect{};
        if (!PageRectForPage(tab, annotation->page, pageRect)) return;
        RECT mark = DenormalizeAnnotationRect(*annotation, pageRect);
        g_annotationHitRect = mark;

        int width = S(316);
        int height = S(42);
        int left = std::clamp(mark.left, g_stage.left + S(10), std::max(g_stage.left + S(10), g_stage.right - width - S(10)));
        int top = mark.top - height - S(10);
        if (top < g_stage.top + S(8)) top = std::min(mark.bottom + S(10), g_stage.bottom - height - S(8));
        g_annotationToolbarRect = RECT{ left, top, left + width, top + height };

        DrawRoundedShadow(dc, g_annotationToolbarRect, S(11.0f), S(1), S(2), S(4), IsDarkTheme() ? 36 : 28);
        GlassRoundedSurface(dc, g_annotationToolbarRect, IsDarkTheme() || IsAuroraTheme() ? 214 : 236, S(11.0f));

        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        auto drawTool = [&](RECT r, const wchar_t* label, bool active = false)
        {
            bool hover = Hit(r, g_mousePos.x, g_mousePos.y);
            if (hover || active) RoundFill(dc, r, S(7), hover ? T().panel2 : MixColor(T().panel2, T().accent, 0.10), hover ? T().line : T().panel2);
            Text(dc, label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        };

        int x = g_annotationToolbarRect.left + S(8);
        int y = g_annotationToolbarRect.top + S(7);
        int h = S(28);
        g_annotationCopyRect = RECT{ x, y, x + S(42), y + h };
        drawTool(g_annotationCopyRect, L"复制");
        x += S(46);
        g_annotationNoteRect = RECT{ x, y, x + S(42), y + h };
        drawTool(g_annotationNoteRect, L"便签", g_annotationNoteOpen);
        x += S(46);
        g_annotationDeleteRect = RECT{ x, y, x + S(42), y + h };
        drawTool(g_annotationDeleteRect, L"删除");
        x += S(49);
        g_annotationFormatRect = RECT{ x, y, x + S(42), y + h };
        drawTool(g_annotationFormatRect, L"格式");
        x += S(50);

        for (int i = 0; i < static_cast<int>(std::size(AnnotationPalette)); ++i)
        {
            RECT swatch{ x, y + S(5), x + S(18), y + S(23) };
            g_annotationColorRects.push_back(swatch);
            bool selected = i == annotation->colorIndex;
            bool hover = Hit(swatch, g_mousePos.x, g_mousePos.y);
            if (selected || hover)
            {
                RECT ring{ swatch.left - S(3), swatch.top - S(3), swatch.right + S(3), swatch.bottom + S(3) };
                RoundFill(dc, ring, S(10), selected ? MixColor(T().panel2, T().accent, 0.18) : T().panel2, selected ? T().accent : T().line);
            }
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush brush(GdiColor(AnnotationPalette[i]));
            g.FillEllipse(&brush, static_cast<float>(swatch.left), static_cast<float>(swatch.top), static_cast<float>(swatch.right - swatch.left), static_cast<float>(swatch.bottom - swatch.top));
            x += S(25);
        }
        SelectObject(dc, oldFont);
        DeleteObject(f);

        if (!g_annotationNoteOpen) return;
        int panelW = S(260);
        int panelH = S(214);
        int panelLeft = std::min<int>(static_cast<int>(g_annotationToolbarRect.right) - panelW, static_cast<int>(client.right) - panelW - S(14));
        panelLeft = std::max<int>(static_cast<int>(g_stage.left) + S(14), panelLeft);
        int panelTop = std::min<int>(static_cast<int>(g_annotationToolbarRect.bottom) + S(8), static_cast<int>(client.bottom) - panelH - S(46));
        panelTop = std::max<int>(static_cast<int>(g_stage.top) + S(14), panelTop);
        g_annotationNotePanelRect = RECT{ panelLeft, panelTop, panelLeft + panelW, panelTop + panelH };
        DrawRoundedShadow(dc, g_annotationNotePanelRect, S(12.0f), S(2), S(3), S(5), IsDarkTheme() ? 42 : 30);
        GlassRoundedSurface(dc, g_annotationNotePanelRect, IsDarkTheme() || IsAuroraTheme() ? 224 : 242, S(12.0f));

        HFONT titleFont = Font(10, FW_SEMIBOLD);
        oldFont = SelectObject(dc, titleFont);
        RECT title{ panelLeft + S(14), panelTop + S(10), panelLeft + S(160), panelTop + S(34) };
        Text(dc, L"注释", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        g_annotationNoteCloseRect = RECT{ panelLeft + panelW - S(34), panelTop + S(8), panelLeft + panelW - S(10), panelTop + S(32) };
        drawTool(g_annotationNoteCloseRect, L"x");
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);

        HFONT smallFont = Font(8);
        oldFont = SelectObject(dc, smallFont);
        RECT meta{ panelLeft + S(14), panelTop + S(36), panelLeft + panelW - S(14), panelTop + S(58) };
        std::wstring metaText = L"10726  " + FormatTimeMs(annotation->createdAt);
        Text(dc, metaText, meta, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(smallFont);

        g_annotationNoteInputRect = RECT{ panelLeft + S(14), panelTop + S(64), panelLeft + panelW - S(14), panelTop + panelH - S(14) };
        RoundFill(dc, g_annotationNoteInputRect, S(8), T().paper, g_annotationNoteFocused ? T().accent : T().line);
        RECT noteText{ g_annotationNoteInputRect.left + S(10), g_annotationNoteInputRect.top + S(8), g_annotationNoteInputRect.right - S(10), g_annotationNoteInputRect.bottom - S(8) };
        std::wstring note = annotation->note.empty() ? L"点击输入注释..." : annotation->note;
        Text(dc, note, noteText, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, annotation->note.empty() ? T().muted : T().text);
        if (g_annotationNoteFocused && g_annotationNoteCaretVisible)
        {
            SIZE size{};
            HFONT noteFont = Font(9);
            oldFont = SelectObject(dc, noteFont);
            GetTextExtentPoint32W(dc, annotation->note.c_str(), static_cast<int>(annotation->note.size()), &size);
            SelectObject(dc, oldFont);
            DeleteObject(noteFont);
            int caretX = std::min(noteText.right - S(2), noteText.left + size.cx + S(1));
            RECT caret{ caretX, noteText.top + S(2), caretX + std::max(1, S(1)), noteText.top + S(18) };
            Fill(dc, caret, T().accent);
        }
    }

    void PaintStatus(HDC dc, RECT client)
    {
        PdfTab* tab = Active();
        if (g_immersive)
        {
            RECT panel{ client.right - S(154), client.bottom - S(48), client.right - S(18), client.bottom - S(21) };
            PaintLiquidGlassPill(dc, panel);
            std::wstring text = tab ? std::to_wstring(tab->currentPage + 1) + L" / " + std::to_wstring(tab->pageCount) + L"    退出沉浸" : L"退出沉浸";
            AddButton(CmdImmersive, panel, text);
            HFONT immersiveFont = Font(9);
            HGDIOBJ oldImmersiveFont = SelectObject(dc, immersiveFont);
            COLORREF immersiveText = IsDarkTheme() ? MixColor(T().muted, RGB(255, 255, 255), 0.10) : MixColor(T().muted, RGB(70, 80, 92), 0.25);
            Text(dc, text, panel, DT_CENTER | DT_VCENTER | DT_SINGLELINE, immersiveText);
            SelectObject(dc, oldImmersiveFont);
            DeleteObject(immersiveFont);
            return;
        }
        int barH = S(34);
        if (!g_statusPinned && g_statusOffset >= barH) return;
        RECT bar{ 0, client.bottom - barH + g_statusOffset, client.right, client.bottom + g_statusOffset };
        GlassSurface(dc, bar, 154);
        RECT topLine{ bar.left, bar.top, bar.right, bar.top + std::max(1, S(1)) };
        GlassDivider(dc, topLine);
        HFONT f = Font(10);
        HGDIOBJ oldFont = SelectObject(dc, f);
        std::wstring pageSize = L"-- x -- 厘米";
        if (tab && !tab->pages.empty())
        {
            auto const& p = tab->pages[static_cast<size_t>(tab->currentPage)];
            wchar_t buf[80]{};
            swprintf_s(buf, L"%.1f x %.1f 厘米", p.widthPt * 2.54 / 72.0, p.heightPt * 2.54 / 72.0);
            pageSize = buf;
        }
        RECT left{ S(10), bar.top, S(184), bar.bottom };
        Text(dc, pageSize, left, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
        int x = std::max<int>(S(188), client.right / 2 - S(190));
        COLORREF iconColor = IsAuroraTheme() ? RGB(154, 223, 202) : RGB(128, 136, 145);
        auto addIcon = [&](int id, StatusIcon icon, const wchar_t* tip, COLORREF color = RGB(120, 126, 134))
        {
            RECT r{ x, bar.top + S(7), x + S(20), bar.bottom - S(7) };
            AddButton(id, r, L"", tip);
            DrawStatusHotArea(dc, r, 5);
            DrawStatusIcon(dc, r, icon, color);
            x += S(24);
        };
        addIcon(CmdFirst, StatusIcon::First, L"第一页", iconColor);
        addIcon(CmdPrev, StatusIcon::Prev, L"上一页", iconColor);

        std::wstring currentPageText = tab ? std::to_wstring(tab->currentPage + 1) : L"0";
        std::wstring totalPageText = tab ? (L"/" + std::to_wstring(tab->pageCount)) : L"/0";
        RECT pageBox{ x + S(4), bar.top + S(7), x + S(78), bar.bottom - S(7) };
        g_pageInputRect = pageBox;
        AddButton(CmdPageInput, pageBox, currentPageText, L"");
        RoundFill(dc, pageBox, S(3), T().paper, T().line);
        HFONT pageFont = Font(10);
        HGDIOBJ oldPageFont = SelectObject(dc, pageFont);
        std::wstring pageText = g_pageInputFocused ? g_pageInputText : currentPageText;
        Text(dc, pageText, pageBox, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        if (g_pageInputFocused && g_pageCaretVisible)
        {
            SIZE caretText{};
            GetTextExtentPoint32W(dc, pageText.c_str(), static_cast<int>(pageText.size()), &caretText);
            int caretX = pageBox.left + ((pageBox.right - pageBox.left) + caretText.cx) / 2 + S(2);
            int caretTop = pageBox.top + S(5);
            int caretBottom = pageBox.bottom - S(5);
            HPEN caretPen = CreatePen(PS_SOLID, std::max(1, S(1)), T().muted);
            HGDIOBJ oldCaretPen = SelectObject(dc, caretPen);
            MoveToEx(dc, caretX, caretTop, nullptr);
            LineTo(dc, caretX, caretBottom);
            SelectObject(dc, oldCaretPen);
            DeleteObject(caretPen);
        }
        SelectObject(dc, oldPageFont);
        DeleteObject(pageFont);
        x += S(82);
        RECT totalRect{ x, bar.top, x + S(50), bar.bottom };
        pageFont = Font(10);
        oldPageFont = SelectObject(dc, pageFont);
        Text(dc, totalPageText, totalRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldPageFont);
        DeleteObject(pageFont);
        x += S(50);
        addIcon(CmdNext, StatusIcon::Next, L"下一页", iconColor);
        addIcon(CmdLast, StatusIcon::Last, L"最后一页", iconColor);

        int rx = std::max<int>(x + S(36), client.right - S(384));
        auto separator = [&]()
        {
            RECT line{ rx + S(8), bar.top + S(7), rx + S(9), bar.bottom - S(7) };
            Fill(dc, line, T().line);
            rx += S(18);
        };
        auto addStatusIcon = [&](int id, StatusIcon icon, const std::wstring& tip, int w = 30, COLORREF color = RGB(120, 126, 134))
        {
            bool hasCaret = icon == StatusIcon::Theme || icon == StatusIcon::View || icon == StatusIcon::Actual;
            w = hasCaret ? std::min(w, 34) : std::min(w, 26);
            RECT r{ rx, bar.top + S(7), rx + S(w), bar.bottom - S(7) };
            AddButton(id, r, L"", tip);
            DrawStatusHotArea(dc, r, 5);
            DrawStatusIcon(dc, r, icon, color);
            rx += S(w + 3);
            if (id == CmdTheme || id == CmdView) rx += S(5);
        };
        addStatusIcon(CmdTheme, StatusIcon::Theme, L"主题：" + std::wstring(T().name), 34, iconColor);
        addStatusIcon(CmdView, StatusIcon::View, L"阅读视图", 34, iconColor);
        addStatusIcon(CmdZoomMenu, StatusIcon::Actual, L"页面缩放", 34, iconColor);
        separator();
        addStatusIcon(CmdZoomOut, StatusIcon::ZoomOut, L"缩小", 34, iconColor);
        std::wstring zoom = tab ? std::to_wstring(static_cast<int>(tab->zoom * 100.0 + 0.5)) + L"%" : L"100%";
        std::wstring zoomText = zoom == L"100%" ? L"实际大小" : zoom;
        RECT zr{ rx + S(3), bar.top + S(7), rx + S(108), bar.bottom - S(7) };
        AddButton(CmdZoomMenu, zr, zoomText, L"当前缩放比例");
        DrawStatusHotArea(dc, zr, 8);
        RoundFill(dc, zr, S(9), T().paper, T().line);
        RECT zoomLabel{ zr.left + S(14), zr.top, zr.right - S(24), zr.bottom };
        HFONT zoomFont = Font(9);
        HGDIOBJ oldZoomFont = SelectObject(dc, zoomFont);
        Text(dc, zoomText, zoomLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldZoomFont);
        DeleteObject(zoomFont);
        DrawDownCaret(dc, zr.right - S(18), (zr.top + zr.bottom) / 2, iconColor);
        rx += S(112);
        addStatusIcon(CmdZoomIn, StatusIcon::ZoomIn, L"放大", 34, iconColor);
        addStatusIcon(CmdImmersive, StatusIcon::Fullscreen, L"沉浸模式", 34, iconColor);
        addStatusIcon(CmdStatusPin, StatusIcon::Pin, g_statusPinned ? L"取消固定工具栏" : L"固定工具栏", 34, g_statusPinned ? T().accent : iconColor);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintReader(HDC dc, RECT client, RECT paintClip)
    {
        PdfTab* tab = Active();
        g_recentRects.clear();
        if (!tab)
        {
            PaintHomeGrid(dc, client);
            return;
        }
        int top = g_immersive ? 0 : S(76);
        int bottom = (g_immersive || !g_statusPinned) ? 0 : S(34);
        int side = (g_sidebarOpen && !g_immersive) ? g_sidebarWidth : 0;
        int rail = side ? std::min<int>(S(44), side) : 0;
        g_sidebar = RECT{ rail, top, side, client.bottom - bottom };
        int splitW = side ? std::max(1, S(1)) : 0;
        g_stage = RECT{ side + splitW, top, client.right, client.bottom - bottom };
        if (!side)
        {
            g_sidebarRowPages.clear();
            g_sidebarRowRects.clear();
            g_sideTabRects.clear();
            g_searchResultRects.clear();
            g_searchResultIds.clear();
        }
        bool paintedPages = false;
        RECT sidebarPaint{ 0, top, side, client.bottom - bottom };
        if (side && Intersects(paintClip, sidebarPaint))
        {
            PaintSidebar(dc, *tab);
        }
        if (side)
        {
            int lineW = std::max(1, S(1));
            RECT splitter{ side, top, side + lineW, client.bottom - bottom };
            if (Intersects(paintClip, splitter)) GlassDivider(dc, splitter);
        }
        if (Intersects(paintClip, g_stage))
        {
            PaintPages(dc, *tab, paintClip);
            paintedPages = true;
        }
        if (paintedPages) ClampScroll(g_hwnd);
        PaintMainScrollBar(dc, client, paintClip);
        if (Intersects(paintClip, g_stage) || Intersects(paintClip, g_annotationToolbarRect) || Intersects(paintClip, g_annotationNotePanelRect))
            PaintAnnotationToolbar(dc, *tab, client);
    }

    void ShowThemeMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(std::size(g_themes)); ++i)
            AppendMenuW(menu, MF_STRING | (i == g_theme ? MF_CHECKED : 0), 2200 + i, g_themes[i].name);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowViewMenuModern(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (g_viewMode == 0 ? MF_CHECKED : 0), 2300, L"单页视图");
        AppendMenuW(menu, MF_STRING | (g_viewMode == 1 ? MF_CHECKED : 0), 2301, L"双页视图");
        AppendMenuW(menu, MF_STRING | (g_viewMode == 2 ? MF_CHECKED : 0), 2302, L"书本视图");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (g_continuousRead ? MF_CHECKED : 0), 2303, L"连续阅读");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowViewMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (g_viewMode == 0 ? MF_CHECKED : 0), 2300, L"连续滚动");
        AppendMenuW(menu, MF_STRING | (g_viewMode == 1 ? MF_CHECKED : 0), 2301, L"单页阅读");
        AppendMenuW(menu, MF_STRING | (g_viewMode == 2 ? MF_CHECKED : 0), 2302, L"双页阅读");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowZoomMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        PdfTab* tab = Active();
        int current = tab ? static_cast<int>(tab->zoom * 100.0 + 0.5) : 100;
        AppendMenuW(menu, MF_STRING | (current == 100 ? MF_CHECKED : 0), 2400, L"实际大小");
        AppendMenuW(menu, MF_STRING, 2401, L"适合页面");
        AppendMenuW(menu, MF_STRING, 2402, L"适合宽度");
        AppendMenuW(menu, MF_STRING, 2403, L"适合内容");
        AppendMenuW(menu, MF_STRING, 2404, L"自定义缩放");
        struct ZoomItem { int id; double zoom; const wchar_t* label; };
        ZoomItem items[] = {
            { 2410, 64.0, L"6400%" },
            { 2411, 32.0, L"3200%" },
            { 2412, 16.0, L"1600%" },
            { 2413, 8.0, L"800%" },
            { 2414, 4.0, L"400%" },
            { 2415, 2.0, L"200%" },
            { 2416, 1.5, L"150%" },
            { 2417, 1.25, L"125%" },
            { 2418, 1.0, L"100%" },
            { 2419, 0.5, L"50%" },
            { 2420, 0.25, L"25%" },
            { 2421, 0.125, L"12.5%" },
            { 2422, 0.0833, L"8.33%" },
        };
        for (auto const& item : items)
        {
            int pct = static_cast<int>(item.zoom * 100.0 + 0.5);
            AppendMenuW(menu, MF_STRING | (current == pct ? MF_CHECKED : 0), item.id, item.label);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2430, L"设置默认缩放比例");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowTabMenu(HWND hwnd, int x, int y)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 2101, L"关闭");
        AppendMenuW(menu, MF_STRING, 2102, L"关闭其他标签");
        AppendMenuW(menu, MF_STRING, 2103, L"关闭右侧标签");
        AppendMenuW(menu, MF_STRING, 2104, L"关闭左侧标签");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2105, L"复制文件路径");
        AppendMenuW(menu, MF_STRING, 2106, L"打开所在文件夹");
        AppendMenuW(menu, MF_STRING, 2107, L"属性");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowSelectionMenu(HWND hwnd, int x, int y)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 2501, L"复制所选内容");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2502, L"高亮");
        AppendMenuW(menu, MF_STRING, 2503, L"下划线");
        AppendMenuW(menu, MF_STRING, 2504, L"删除线");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2505, L"添加便签");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void ShowSelectionMenuWithShortcuts(HWND hwnd, int x, int y)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 2501, L"复制选中内容\tCtrl + C");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2502, L"高亮\tCtrl + H");
        AppendMenuW(menu, MF_STRING, 2503, L"下划线\tCtrl + U");
        AppendMenuW(menu, MF_STRING, 2504, L"删除线\tCtrl + K");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2505, L"添加便签");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    void AddAnnotationFromSelection(HWND hwnd, AnnotationKind kind)
    {
        PdfTab* tab = Active();
        if (!tab || !g_hasContentSelection || g_selectionPage < 0 || g_selectionQuads.empty()) return;
        float minX = 1.0f;
        float minY = 1.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (fz_quad const& quad : g_selectionQuads)
        {
            minX = std::min({ minX, quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            minY = std::min({ minY, quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
            maxX = std::max({ maxX, quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            maxY = std::max({ maxY, quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
        }
        fz_rect r{ minX, minY, maxX, maxY };
        if (std::abs(r.x1 - r.x0) < 0.004f || std::abs(r.y1 - r.y0) < 0.004f) return;
        PdfAnnotation annotation;
        annotation.page = std::clamp(g_selectionPage, 0, tab->pageCount - 1);
        annotation.x0 = std::min(r.x0, r.x1);
        annotation.y0 = std::min(r.y0, r.y1);
        annotation.x1 = std::max(r.x0, r.x1);
        annotation.y1 = std::max(r.y0, r.y1);
        annotation.kind = kind;
        annotation.colorIndex = kind == AnnotationKind::Highlight ? 0 : (kind == AnnotationKind::Underline ? 1 : 3);
        annotation.createdAt = NowMs();
        annotation.text = g_selectionText;
        tab->annotations.push_back(annotation);
        g_activeAnnotation = static_cast<int>(tab->annotations.size()) - 1;
        g_annotationToolbarOpen = true;
        tab->dirty = true;
        g_hasContentSelection = false;
        g_selectionQuads.clear();
        g_selectionText.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    bool ReloadPdfTab(PdfTab& tab, const std::wstring& path, int page, double zoom)
    {
        DropTab(tab);
        tab.pages.clear();
        tab.outline.clear();
        tab.cache.clear();
        tab.tileCache.clear();
        tab.thumbCache.clear();
        tab.annotations.clear();
        tab.outlineLayout.clear();
        tab.outlineLayoutWidth = 0;
        tab.outlineLayoutFont = 0;
        tab.outlineLayoutRows = 0;
        tab.outlineLayoutContentHeight = 0;
        tab.dirty = false;

        try
        {
            g_pdf.Load();
            tab.ctx = g_pdf.newContext(nullptr, nullptr, 64 << 20, "1.28.0");
            g_pdf.setAaLevel(tab.ctx, 8);
            g_pdf.registerHandlers(tab.ctx);
            auto utf8 = ToUtf8(path);
            tab.doc = g_pdf.openDocument(tab.ctx, utf8.c_str());
            tab.pageCount = g_pdf.countPages(tab.ctx, tab.doc);
            tab.path = path;
            tab.name = std::filesystem::path(path).filename().wstring();
            UpdateTabFileStamp(tab);
            tab.currentPage = std::clamp(page, 0, std::max(0, tab.pageCount - 1));
            tab.zoom = zoom;
            tab.pages.resize(static_cast<size_t>(tab.pageCount));
            int probe = std::min(tab.pageCount, 1000);
            for (int i = 0; i < probe; ++i)
            {
                fz_page* pdfPage = g_pdf.loadPage(tab.ctx, tab.doc, i);
                fz_rect b = g_pdf.boundPage(tab.ctx, pdfPage);
                tab.pages[static_cast<size_t>(i)] = PageInfo{ std::max(1.0, static_cast<double>(b.x1 - b.x0)), std::max(1.0, static_cast<double>(b.y1 - b.y0)) };
                g_pdf.dropPage(tab.ctx, pdfPage);
            }
            for (int i = probe; i < tab.pageCount; ++i) tab.pages[static_cast<size_t>(i)] = tab.pages[0];
            fz_outline* outline = g_pdf.loadOutline(tab.ctx, tab.doc);
            if (outline)
            {
                CollectOutline(tab, outline, 0);
                g_pdf.dropOutline(tab.ctx, outline);
            }
            if (g_continuousRead) ScrollToPage(tab, tab.currentPage);
            else g_scrollY = 0;
            return true;
        }
        catch (...)
        {
            DropTab(tab);
            return false;
        }
    }

    void SaveAnnotationsToPdf(HWND hwnd, PdfTab& tab)
    {
        if (tab.path.empty() || !tab.dirty || tab.annotations.empty()) return;
        if (!g_pdf.saveAnnotations)
        {
            MessageBoxW(hwnd, L"当前 MuPDF 组件不支持写入 PDF 标注。", L"保存", MB_OK | MB_ICONWARNING);
            return;
        }

        std::filesystem::path source(tab.path);
        std::filesystem::path temp = source;
        temp += L".quietpdf-save.tmp.pdf";
        try { std::filesystem::remove(temp); } catch (...) {}

        std::vector<std::string> notes;
        std::vector<QuietPdfAnnotation> annotations;
        notes.reserve(tab.annotations.size());
        annotations.reserve(tab.annotations.size());
        for (auto const& item : tab.annotations)
        {
            notes.push_back(ToUtf8(item.note));
            QuietPdfAnnotation out;
            out.page = item.page;
            out.kind = static_cast<int>(item.kind);
            out.x0 = static_cast<float>(item.x0);
            out.y0 = static_cast<float>(item.y0);
            out.x1 = static_cast<float>(item.x1);
            out.y1 = static_cast<float>(item.y1);
            COLORREF color = AnnotationColor(item);
            out.colorR = static_cast<float>(GetRValue(color)) / 255.0f;
            out.colorG = static_cast<float>(GetGValue(color)) / 255.0f;
            out.colorB = static_cast<float>(GetBValue(color)) / 255.0f;
            out.opacity = item.kind == AnnotationKind::Highlight ? 0.45f : 1.0f;
            out.contents = notes.back().empty() ? nullptr : notes.back().c_str();
            annotations.push_back(out);
        }

        std::string tempUtf8 = ToUtf8(temp.wstring());
        int saved = g_pdf.saveAnnotations(tab.ctx, tab.doc, tempUtf8.c_str(), annotations.data(), static_cast<int>(annotations.size()));
        if (!saved || !std::filesystem::exists(temp))
        {
            try { std::filesystem::remove(temp); } catch (...) {}
            MessageBoxW(hwnd, L"PDF 标注写入失败，原文件未修改。", L"保存", MB_OK | MB_ICONWARNING);
            return;
        }

        int page = tab.currentPage;
        double zoom = tab.zoom;
        std::wstring originalPath = tab.path;
        auto pendingAnnotations = tab.annotations;
        bool pendingDirty = tab.dirty;
        DropTab(tab);
        Sleep(80);
        DWORD replaceError = ERROR_SUCCESS;
        bool replaced = ReplacePdfOnDisk(source, temp, replaceError);

        if (!replaced)
        {
            ReloadPdfTab(tab, originalPath, page, zoom);
            tab.annotations = std::move(pendingAnnotations);
            tab.dirty = pendingDirty;
            std::wstring msg = L"PDF 已生成临时保存文件，但替换原文件失败。\n\n原因：" + LastErrorText(replaceError) +
                L"\n\n请确认这个 PDF 没有被其它程序占用。\n临时文件：\n" + temp.wstring();
            MessageBoxW(hwnd, msg.c_str(), L"保存", MB_OK | MB_ICONWARNING);
            return;
        }

        if (!ReloadPdfTab(tab, source.wstring(), page, zoom))
        {
            MessageBoxW(hwnd, L"PDF 已保存，但重新载入失败。请重新打开文件。", L"保存", MB_OK | MB_ICONWARNING);
            return;
        }
        TouchRecentProgress(tab, true);
        SetTitle();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void RefreshActiveFileIfChanged(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab || tab->path.empty() || !tab->hasFileStamp) return;
        std::filesystem::file_time_type stamp{};
        if (!ReadFileStamp(tab->path, stamp) || stamp == tab->fileStamp) return;

        int page = tab->currentPage;
        double zoom = tab->zoom;
        int oldScroll = g_scrollY;
        std::wstring path = tab->path;
        bool hadPendingAnnotations = tab->dirty;
        auto pendingAnnotations = hadPendingAnnotations ? tab->annotations : std::vector<PdfAnnotation>{};
        if (ReloadPdfTab(*tab, path, page, zoom))
        {
            if (hadPendingAnnotations)
            {
                tab->annotations = std::move(pendingAnnotations);
                tab->dirty = true;
            }
            g_scrollY = oldScroll;
            g_searchResults.clear();
            g_searchCurrent = 0;
            g_searchHasRun = false;
            g_searchLastQuery.clear();
            ClearAnnotationFocus();
            ClearTextSelection();
            SetTitle();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void SaveAnnotationSidecar(HWND hwnd, PdfTab& tab)
    {
        if (tab.path.empty()) return;
        std::filesystem::path sidecar = std::filesystem::path(tab.path).replace_extension(L".quietpdf.annotations");
        std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            MessageBoxW(hwnd, L"标注旁路文件保存失败。", L"保存", MB_OK | MB_ICONWARNING);
            return;
        }
        out << "QuietPDFAnnotations\t1\n";
        for (auto const& annotation : tab.annotations)
        {
            out << annotation.page << '\t'
                << static_cast<int>(annotation.kind) << '\t'
                << annotation.x0 << '\t' << annotation.y0 << '\t'
                << annotation.x1 << '\t' << annotation.y1 << '\t'
                << ToUtf8(annotation.note) << '\n';
        }
        tab.dirty = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void ShowProperties(HWND hwnd, PdfTab const& tab)
    {
        std::uintmax_t size = 0;
        try { size = std::filesystem::file_size(tab.path); } catch (...) {}
        std::wstring msg = L"文件: " + tab.path +
            L"\n页数: " + std::to_wstring(tab.pageCount) +
            L"\n当前页: " + std::to_wstring(tab.currentPage + 1) +
            L"\n缩放: " + std::to_wstring(static_cast<int>(tab.zoom * 100.0 + 0.5)) + L"%" +
            L"\n文件大小: " + std::to_wstring(size / 1024) + L" KB";
        MessageBoxW(hwnd, msg.c_str(), L"属性", MB_OK | MB_ICONINFORMATION);
    }

    void ApplyImmersiveWindow(HWND hwnd, bool immersive)
    {
        if (immersive == g_immersive) return;
        if (immersive)
        {
            g_preImmersivePlacement.length = sizeof(WINDOWPLACEMENT);
            g_hasPreImmersivePlacement = GetWindowPlacement(hwnd, &g_preImmersivePlacement) != FALSE;
            g_preImmersiveStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            g_preImmersiveExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
            GetMonitorInfoW(monitor, &monitorInfo);

            LONG_PTR style = g_preImmersiveStyle;
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_preImmersiveExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            g_immersive = true;
            SetWindowPos(hwnd, HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtrW(hwnd, GWL_STYLE, g_preImmersiveStyle ? g_preImmersiveStyle : (WS_OVERLAPPEDWINDOW | WS_VSCROLL));
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_preImmersiveExStyle);
            g_immersive = false;
            if (g_hasPreImmersivePlacement)
            {
                SetWindowPlacement(hwnd, &g_preImmersivePlacement);
            }
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        ShowScrollBar(hwnd, SB_VERT, FALSE);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void CloseTab(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_tabs.size())) return;
        TouchRecentProgress(*g_tabs[static_cast<size_t>(index)], true);
        DropTab(*g_tabs[static_cast<size_t>(index)]);
        g_tabs.erase(g_tabs.begin() + index);
        if (g_tabs.empty()) g_active = -1;
        else g_active = std::clamp(g_active >= index ? g_active - 1 : g_active, 0, static_cast<int>(g_tabs.size()) - 1);
        g_scrollY = 0;
        SetTitle();
        StartActiveFileWatcher(g_hwnd);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void Command(HWND hwnd, int id)
    {
        PdfTab* tab = Active();
        if (id >= 2200 && id < 2200 + static_cast<int>(std::size(g_themes)))
        {
            g_theme = id - 2200;
            ApplyWindowChrome();
            SaveState();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id >= 2300 && id <= 2302)
        {
            g_viewMode = id - 2300;
            SaveState();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id == 2303)
        {
            g_continuousRead = !g_continuousRead;
            if (tab)
            {
                if (g_continuousRead) ScrollToPage(*tab, tab->currentPage);
                else g_scrollY = 0;
            }
            SaveState();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id >= 2400 && id <= 2430)
        {
            if (!tab) return;
            auto applyZoom = [&](double zoom)
            {
                tab->zoom = std::clamp(zoom, 0.0833, 64.0);
                InvalidateRect(hwnd, nullptr, FALSE);
            };
            if (id == 2400) applyZoom(1.0);
            else if (id == 2401 && !tab->pages.empty())
            {
                auto const& p = tab->pages[static_cast<size_t>(tab->currentPage)];
                double zw = static_cast<double>(std::max<int>(1, g_stage.right - g_stage.left - S(64))) * 72.0 / (p.widthPt * g_dpi);
                double zh = static_cast<double>(std::max<int>(1, g_stage.bottom - g_stage.top - S(64))) * 72.0 / (p.heightPt * g_dpi);
                applyZoom(std::min(zw, zh));
            }
            else if (id == 2402 && !tab->pages.empty())
            {
                auto const& p = tab->pages[static_cast<size_t>(tab->currentPage)];
                applyZoom(static_cast<double>(std::max<int>(1, g_stage.right - g_stage.left - S(64))) * 72.0 / (p.widthPt * g_dpi));
            }
            else if (id == 2403 && !tab->pages.empty())
            {
                auto const& p = tab->pages[static_cast<size_t>(tab->currentPage)];
                applyZoom(static_cast<double>(std::max<int>(1, g_stage.right - g_stage.left - S(96))) * 72.0 / (p.widthPt * g_dpi));
            }
            else if (id >= 2410 && id <= 2422)
            {
                double values[] = { 64.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.5, 1.25, 1.0, 0.5, 0.25, 0.125, 0.0833 };
                applyZoom(values[id - 2410]);
            }
            return;
        }
        if (id == 2101) { CloseTab(g_contextTab); return; }
        if (id == 2102 && g_contextTab >= 0)
        {
            for (int i = static_cast<int>(g_tabs.size()) - 1; i >= 0; --i) if (i != g_contextTab) CloseTab(i);
            g_active = 0;
            StartActiveFileWatcher(hwnd);
            return;
        }
        if (id == 2103 && g_contextTab >= 0) { while (static_cast<int>(g_tabs.size()) > g_contextTab + 1) CloseTab(static_cast<int>(g_tabs.size()) - 1); return; }
        if (id == 2104 && g_contextTab >= 0) { while (g_contextTab > 0) { CloseTab(0); --g_contextTab; } return; }
        if (id == 2105 && g_contextTab >= 0)
        {
            const auto& path = g_tabs[static_cast<size_t>(g_contextTab)]->path;
            if (OpenClipboard(hwnd))
            {
                EmptyClipboard();
                size_t bytes = (path.size() + 1) * sizeof(wchar_t);
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (mem)
                {
                    memcpy(GlobalLock(mem), path.c_str(), bytes);
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                }
                CloseClipboard();
            }
            return;
        }
        if (id == 2501 && !g_selectionText.empty())
        {
            CopyTextToClipboard(hwnd, g_selectionText);
            return;
        }
        if (id == 2106 && g_contextTab >= 0)
        {
            std::wstring args = L"/select,\"" + g_tabs[static_cast<size_t>(g_contextTab)]->path + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        if (id == 2107 && g_contextTab >= 0) { ShowProperties(hwnd, *g_tabs[static_cast<size_t>(g_contextTab)]); return; }
        if (id == 2501)
        {
            MessageBoxW(hwnd, L"当前版本还没有接入 MuPDF 文字提取接口，暂时不能复制选区文字。", L"复制", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (id == 2502) { AddAnnotationFromSelection(hwnd, AnnotationKind::Highlight); return; }
        if (id == 2503) { AddAnnotationFromSelection(hwnd, AnnotationKind::Underline); return; }
        if (id == 2504) { AddAnnotationFromSelection(hwnd, AnnotationKind::Strikeout); return; }
        if (id == 2505)
        {
            AddAnnotationFromSelection(hwnd, AnnotationKind::Highlight);
            if (tab && !tab->annotations.empty()) tab->annotations.back().note = L"";
            return;
        }

        switch (id)
        {
        case CmdHome: g_active = -1; SetTitle(); StartActiveFileWatcher(hwnd); InvalidateRect(hwnd, nullptr, FALSE); break;
        case CmdOpen: OpenDialog(hwnd); break;
        case CmdSidebar: StartSidebarPanelAnimation(hwnd, !g_sidebarOpen); break;
        case CmdSearch:
            g_sidebarOpen = true;
            g_sideMode = 2;
            g_searchInputFocused = true;
            g_searchCaretVisible = true;
            SetTimer(hwnd, 12, 500, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case CmdBookmark:
            if (g_sidebarOpen && g_sideMode == 0) StartSidebarPanelAnimation(hwnd, false);
            else
            {
                g_sideMode = 0;
                StartSidebarPanelAnimation(hwnd, true);
            }
            break;
        case CmdSettings: MessageBoxW(hwnd, L"主题、视图、缩放和沉浸模式已放在底部状态栏。", L"设置", MB_OK | MB_ICONINFORMATION); break;
        case CmdSearchRun:
            RunSearch();
            g_sidebarScrollY = 0;
            g_sidebarScrollTargetY = 0;
            if (!g_searchResults.empty() && tab)
            {
                g_searchCurrent = 0;
                ScrollToSearchResult(*tab, g_searchResults[0]);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case CmdSearchPrev:
            if (!g_searchResults.empty() && tab)
            {
                g_searchCurrent = (g_searchCurrent + static_cast<int>(g_searchResults.size()) - 1) % static_cast<int>(g_searchResults.size());
                ScrollToSearchResult(*tab, g_searchResults[static_cast<size_t>(g_searchCurrent)]);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdSearchNext:
            if (!g_searchResults.empty() && tab)
            {
                g_searchCurrent = (g_searchCurrent + 1) % static_cast<int>(g_searchResults.size());
                ScrollToSearchResult(*tab, g_searchResults[static_cast<size_t>(g_searchCurrent)]);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdSearchClear:
            g_searchQuery.clear();
            g_searchResults.clear();
            g_searchCurrent = 0;
            g_searchHasRun = false;
            g_searchLastQuery.clear();
            g_searchInputFocused = true;
            g_searchCaretVisible = true;
            SetTimer(hwnd, 12, 500, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case CmdNoteExpandAll:
            g_collapsedAnnotationPages.clear();
            InvalidateRect(hwnd, &g_sidebar, FALSE);
            break;
        case CmdNoteCollapseAll:
            if (tab)
            {
                g_collapsedAnnotationPages.clear();
                for (auto const& annotation : tab->annotations) g_collapsedAnnotationPages.insert(annotation.page);
                InvalidateRect(hwnd, &g_sidebar, FALSE);
            }
            break;
        case CmdNotePrev:
            if (tab && !tab->annotations.empty())
            {
                int index = g_activeAnnotation < 0 ? 0 : (g_activeAnnotation + static_cast<int>(tab->annotations.size()) - 1) % static_cast<int>(tab->annotations.size());
                SelectAnnotation(hwnd, *tab, index, true);
            }
            break;
        case CmdNoteNext:
            if (tab && !tab->annotations.empty())
            {
                int index = g_activeAnnotation < 0 ? 0 : (g_activeAnnotation + 1) % static_cast<int>(tab->annotations.size());
                SelectAnnotation(hwnd, *tab, index, true);
            }
            break;
        case CmdNoteDelete:
            if (tab && g_activeAnnotation >= 0) DeleteAnnotation(hwnd, *tab, g_activeAnnotation);
            break;
        case CmdNoteDeleteAll:
            if (tab && !tab->annotations.empty())
            {
                tab->annotations.clear();
                tab->dirty = true;
                ClearAnnotationFocus();
                g_collapsedAnnotationPages.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdTheme: ShowThemeMenu(hwnd); break;
        case CmdView: ShowViewMenuModern(hwnd); break;
        case CmdZoomMenu: ShowZoomMenu(hwnd); break;
        case CmdStatusPin:
            g_statusPinned = !g_statusPinned;
            g_statusReveal = g_statusPinned;
            g_statusTargetOffset = g_statusPinned ? 0 : S(34);
            SetTimer(hwnd, 9, 16, nullptr);
            SaveState();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case CmdImmersive: ApplyImmersiveWindow(hwnd, !g_immersive); break;
        case CmdZoomOut: if (tab) { tab->zoom = std::clamp(tab->zoom - 0.1, 0.25, 5.0); InvalidateRect(hwnd, nullptr, FALSE); } break;
        case CmdZoomIn: if (tab) { tab->zoom = std::clamp(tab->zoom + 0.1, 0.25, 5.0); InvalidateRect(hwnd, nullptr, FALSE); } break;
        case CmdFirst:
            if (tab)
            {
                if (g_continuousRead) ScrollStageTo(hwnd, 0);
                else { tab->currentPage = 0; g_scrollY = 0; InvalidateRect(hwnd, nullptr, FALSE); }
            }
            break;
        case CmdPrev:
            if (tab)
            {
                if (g_continuousRead) ScrollStageTo(hwnd, g_scrollY - std::max(S(60), PageH(*tab, tab->currentPage) + PageGap()));
                else tab->currentPage = std::max(0, tab->currentPage - 1);
                if (!g_continuousRead) InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdNext:
            if (tab)
            {
                if (g_continuousRead) ScrollStageTo(hwnd, g_scrollY + std::max(S(60), PageH(*tab, tab->currentPage) + PageGap()));
                else tab->currentPage = std::min(tab->pageCount - 1, tab->currentPage + 1);
                if (!g_continuousRead) InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdLast:
            if (tab)
            {
                if (g_continuousRead) ScrollStageTo(hwnd, ContinuousContentHeight(*tab));
                else tab->currentPage = tab->pageCount - 1;
                if (!g_continuousRead) InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdOutlineToggleAll:
            if (tab)
            {
                if (g_collapsedOutlineRows.empty())
                {
                    for (int i = 0; i + 1 < static_cast<int>(tab->outline.size()); ++i)
                    {
                        if (tab->outline[static_cast<size_t>(i + 1)].depth > tab->outline[static_cast<size_t>(i)].depth)
                        {
                            g_collapsedOutlineRows.insert(i);
                        }
                    }
                }
                else
                {
                    g_collapsedOutlineRows.clear();
                }
                ++g_outlineCollapseVersion;
                g_sidebarScrollY = ClampSidebarScrollValue(g_sidebarScrollY);
                InvalidateRect(hwnd, &g_sidebar, FALSE);
            }
            break;
        case CmdOutlineSmall: g_outlineFont = std::max(9, g_outlineFont - 1); SaveState(); InvalidateRect(hwnd, nullptr, FALSE); break;
        case CmdOutlineLarge: g_outlineFont = std::min(22, g_outlineFont + 1); SaveState(); InvalidateRect(hwnd, nullptr, FALSE); break;
        default: break;
        }
    }

    void Paint(HWND hwnd)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        RECT dirty = ps.rcPaint;
        if (IsRectEmpty(&dirty)) dirty = client;
        int dirtyW = std::max<int>(1, dirty.right - dirty.left);
        int dirtyH = std::max<int>(1, dirty.bottom - dirty.top);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, dirtyW, dirtyH);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        SetViewportOrgEx(mem, -dirty.left, -dirty.top, nullptr);
        if (UsesBackdropTheme()) PaintGlassBackdrop(mem, dirty);
        else Fill(mem, dirty, T().bg);
        RECT toolbarRect{ 0, 0, client.right, S(38) };
        RECT tabsRect{ 0, S(38), client.right, S(76) };
        RECT statusRect{ 0, client.bottom - S(34), client.right, client.bottom };
        bool uiDirty = Intersects(dirty, toolbarRect) || Intersects(dirty, tabsRect) || Intersects(dirty, statusRect);
        if (g_sidebarOpen)
        {
            RECT sidebarUi{ 0, g_sidebar.top, g_sidebar.right, g_sidebar.bottom };
            if (Intersects(dirty, sidebarUi)) uiDirty = true;
        }
        bool repaintUi = uiDirty || g_buttons.empty();
        if (repaintUi)
        {
            g_buttons.clear();
            PaintToolbar(mem, client);
            PaintTabs(mem, client);
        }
        PaintReader(mem, client, dirty);
        if (repaintUi) PaintStatus(mem, client);
        PaintHoverTip(mem, client);
        SetViewportOrgEx(mem, 0, 0, nullptr);
        BitBlt(dc, dirty.left, dirty.top, dirtyW, dirtyH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
            g_hwnd = hwnd;
            g_dpi = GetDpiForWindow(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            ShowScrollBar(hwnd, SB_VERT, FALSE);
            {
                GESTURECONFIG panConfig{};
                panConfig.dwID = GID_PAN;
                panConfig.dwWant = GC_PAN | GC_PAN_WITH_GUTTER | GC_PAN_WITH_INERTIA | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY;
                panConfig.dwBlock = 0;
                SetGestureConfig(hwnd, 0, 1, &panConfig, sizeof(panConfig));
            }
            return 0;
        case WM_ACTIVATEAPP:
            if (wParam) RefreshActiveFileIfChanged(hwnd);
            return 0;
        case WM_QUIETPDF_FILE_CHANGED:
            if (static_cast<unsigned long>(wParam) == g_fileWatchGeneration.load())
            {
                RefreshActiveFileIfChanged(hwnd);
            }
            return 0;
        case WM_DPICHANGED:
            g_dpi = HIWORD(wParam);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_SIZE:
            ShowScrollBar(hwnd, SB_VERT, FALSE);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_COMMAND:
            Command(hwnd, LOWORD(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
        {
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (g_resizingSidebar || HitSidebarSplitter(pt.x, pt.y))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_DROPFILES:
        {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, i, path, MAX_PATH))
                {
                    try { if (_wcsicmp(std::filesystem::path(path).extension().c_str(), L".pdf") == 0) OpenPdf(hwnd, path); }
                    catch (std::exception const& e) { MessageBoxA(hwnd, e.what(), "QingYuePDF", MB_ICONERROR); }
                }
            }
            DragFinish(drop);
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (PdfTab* tab = Active())
            {
                PdfAnnotation* annotation = nullptr;
                if (ActiveAnnotation(*tab, annotation))
                {
                    if (Hit(g_annotationCopyRect, x, y))
                    {
                        CopyTextToClipboard(hwnd, annotation->text);
                        return 0;
                    }
                    if (Hit(g_annotationNoteRect, x, y))
                    {
                        g_annotationNoteOpen = true;
                        g_annotationNoteFocused = true;
                        SetTimer(hwnd, 13, 500, nullptr);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (Hit(g_annotationDeleteRect, x, y))
                    {
                        DeleteAnnotation(hwnd, *tab, g_activeAnnotation);
                        return 0;
                    }
                    for (size_t i = 0; i < g_annotationColorRects.size(); ++i)
                    {
                        if (Hit(g_annotationColorRects[i], x, y))
                        {
                            annotation->colorIndex = static_cast<int>(i);
                            tab->dirty = true;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }
                    if (g_annotationNoteOpen)
                    {
                        if (Hit(g_annotationNoteCloseRect, x, y))
                        {
                            g_annotationNoteOpen = false;
                            g_annotationNoteFocused = false;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        if (Hit(g_annotationNoteInputRect, x, y))
                        {
                            g_annotationNoteFocused = true;
                            SetTimer(hwnd, 13, 500, nullptr);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }
                    if (Hit(g_annotationToolbarRect, x, y) || Hit(g_annotationNotePanelRect, x, y))
                        return 0;
                }
            }
            bool hadTextSelection = g_hasContentSelection || !g_selectionQuads.empty();
            bool clickedAnnotation = false;
            if (PdfTab* tab = Active())
            {
                RECT hitRect{};
                int annotationIndex = HitTestAnnotation(*tab, x, y, &hitRect);
                if (annotationIndex >= 0)
                {
                    ClearTextSelection();
                    g_activeAnnotation = annotationIndex;
                    g_annotationHitRect = hitRect;
                    g_annotationToolbarOpen = true;
                    g_annotationNoteFocused = false;
                    clickedAnnotation = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (g_annotationToolbarOpen && !clickedAnnotation)
            {
                ClearAnnotationFocus();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (hadTextSelection)
            {
                ClearTextSelection();
                InvalidateRect(hwnd, &g_stage, FALSE);
            }
            g_searchInputFocused = false;
            if (g_pageInputFocused && !Hit(g_pageInputRect, x, y))
            {
                CancelPageInput(hwnd);
            }
            if (HitSidebarSplitter(x, y))
            {
                g_resizingSidebar = true;
                g_sidebarTargetWidth = g_sidebarWidth;
                SetCapture(hwnd);
                return 0;
            }
            if (Active() && Hit(g_mainScrollThumbRect, x, y))
            {
                g_draggingMainScroll = true;
                g_mainScrollDragOffset = y - g_mainScrollThumbRect.top;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
                return 0;
            }
            if (Active() && Hit(g_mainScrollTrackRect, x, y))
            {
                int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_mainScrollThumbRect.bottom - g_mainScrollThumbRect.top);
                int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
                int maxScroll = std::max<int>(0, g_contentHeight - stageH);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(y - thumbH / 2, g_mainScrollTrackRect.top, g_mainScrollTrackRect.bottom - thumbH);
                int newScroll = MulDiv(thumbTop - g_mainScrollTrackRect.top, maxScroll, travel);
                ScrollStageTo(hwnd, newScroll);
                return 0;
            }
            for (size_t i = 0; i < g_sideTabRects.size(); ++i)
            {
                if (Hit(g_sideTabRects[i], x, y))
                {
                    g_sideMode = static_cast<int>(i);
                    g_sidebarScrollY = 0;
                    g_searchInputFocused = g_sideMode == 2;
                    if (g_searchInputFocused)
                    {
                        g_searchCaretVisible = true;
                        SetTimer(hwnd, 12, 500, nullptr);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (g_sideMode == 1 && Hit(g_thumbSliderRect, x, y))
            {
                g_draggingThumbSlider = true;
                SetCapture(hwnd);
                int width = std::max<int>(1, g_thumbSliderRect.right - g_thumbSliderRect.left);
                g_thumbSize = std::clamp<int>(ThumbSizeMin + (x - g_thumbSliderRect.left) * (ThumbSizeMax - ThumbSizeMin) / width, ThumbSizeMin, ThumbSizeMax);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_sideMode == 2 && Hit(g_searchInputRect, x, y))
            {
                RECT clearRect{ g_searchInputRect.right - S(77), g_searchInputRect.top + S(6), g_searchInputRect.right - S(49), g_searchInputRect.bottom - S(6) };
                if (Hit(clearRect, x, y))
                {
                    g_searchQuery.clear();
                    g_searchResults.clear();
                    g_searchCurrent = 0;
                    g_searchHasRun = false;
                    g_searchLastQuery.clear();
                }
                g_searchInputFocused = true;
                g_searchCaretVisible = true;
                SetTimer(hwnd, 12, 500, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_sideMode == 2)
            {
                for (size_t i = 0; i < g_searchResultRects.size() && i < g_searchResultIds.size(); ++i)
                {
                    if (Hit(g_searchResultRects[i], x, y))
                    {
                        PdfTab* tab = Active();
                        if (tab)
                        {
                            int resultId = std::clamp(g_searchResultIds[i], 0, static_cast<int>(g_searchResults.size()) - 1);
                            g_searchCurrent = resultId;
                            ScrollToSearchResult(*tab, g_searchResults[static_cast<size_t>(resultId)]);
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                }
            }
            for (size_t i = 0; i < g_tabRects.size(); ++i)
            {
                RECT close{ g_tabRects[i].right - S(32), g_tabRects[i].top, g_tabRects[i].right, g_tabRects[i].bottom };
                if (Hit(close, x, y)) { CloseTab(static_cast<int>(i)); return 0; }
                if (Hit(g_tabRects[i], x, y))
                {
                    g_active = static_cast<int>(i);
                    g_scrollY = 0;
                    g_selectedOutlineRow = -1;
                    SetTitle();
                    StartActiveFileWatcher(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (Active() && Hit(g_pageInputRect, x, y))
            {
                FocusPageInput(hwnd);
                return 0;
            }
            for (auto const& b : g_buttons) if (Hit(b.rect, x, y))
            {
                g_pressedButtonRect = b.rect;
                g_hasPressedButton = true;
                SetTimer(hwnd, 2, 90, nullptr);
                InvalidateRect(hwnd, &b.rect, FALSE);
                Command(hwnd, b.id);
                return 0;
            }
            if (!Active()) for (size_t i = 0; i < g_recentRects.size(); ++i)
            {
                if (!Hit(g_recentRects[i], x, y)) continue;
                if (i == 0) OpenDialog(hwnd);
                else if (i - 1 < g_recent.size() && std::filesystem::exists(g_recent[i - 1].path)) OpenPdf(hwnd, g_recent[i - 1].path);
                return 0;
            }
            if (PdfTab* tab = Active())
            {
                int page = -1;
                RECT pageRect{};
                if (PageRectAtPoint(*tab, x, y, page, pageRect))
                {
                    g_selectingContent = true;
                    g_hasContentSelection = false;
                    g_selectionPage = page;
                    g_selectionStart = POINT{ x, y };
                    g_selectionEnd = POINT{ x, y };
                    g_selectionPageRect = pageRect;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g_stage, FALSE);
                    return 0;
                }
            }
            if (g_sidebarOpen && !g_immersive && Hit(g_sidebar, x, y))
            {
                PdfTab* tab = Active();
                if (g_sideMode == 0)
                {
                    for (size_t i = 0; i < g_outlineToggleRects.size() && i < g_outlineToggleRowIds.size(); ++i)
                    {
                        if (Hit(g_outlineToggleRects[i], x, y))
                        {
                            int rowId = g_outlineToggleRowIds[i];
                            if (g_collapsedOutlineRows.find(rowId) != g_collapsedOutlineRows.end()) g_collapsedOutlineRows.erase(rowId);
                            else g_collapsedOutlineRows.insert(rowId);
                            ++g_outlineCollapseVersion;
                            g_sidebarScrollY = ClampSidebarScrollValue(g_sidebarScrollY);
                            InvalidateRect(hwnd, &g_sidebar, FALSE);
                            return 0;
                        }
                    }
                }
                for (size_t i = 0; tab && i < g_sidebarRowRects.size() && i < g_sidebarRowPages.size(); ++i)
                {
                    if (Hit(g_sidebarRowRects[i], x, y))
                    {
                        int page = std::clamp(g_sidebarRowPages[i], 0, tab->pageCount - 1);
                        if (g_sideMode == 3 && i < g_sidebarRowIds.size())
                        {
                            int rowId = g_sidebarRowIds[i];
                            if (rowId <= -100000)
                            {
                                int notePage = -100000 - rowId;
                                if (g_collapsedAnnotationPages.find(notePage) != g_collapsedAnnotationPages.end()) g_collapsedAnnotationPages.erase(notePage);
                                else g_collapsedAnnotationPages.insert(notePage);
                                InvalidateRect(hwnd, &g_sidebar, FALSE);
                                return 0;
                            }
                            if (rowId >= 0 && rowId < static_cast<int>(tab->annotations.size()))
                            {
                                SelectAnnotation(hwnd, *tab, rowId, true);
                                return 0;
                            }
                        }
                        if (g_sideMode == 0 && i < g_sidebarRowIds.size())
                        {
                            g_selectedOutlineRow = g_sidebarRowIds[i];
                            if (!tab->outline.empty() && g_selectedOutlineRow >= 0)
                            {
                                ScrollToOutline(*tab, g_selectedOutlineRow);
                                InvalidateRect(hwnd, nullptr, FALSE);
                                break;
                            }
                        }
                        ScrollToPage(*tab, page);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
                }
            }
            return 0;
        }
        case WM_RBUTTONUP:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (g_hasContentSelection && !g_selectionQuads.empty())
            {
                POINT pt{ x, y };
                ClientToScreen(hwnd, &pt);
                ShowSelectionMenuWithShortcuts(hwnd, pt.x, pt.y);
                return 0;
            }
            for (size_t i = 0; i < g_tabRects.size(); ++i)
            {
                if (Hit(g_tabRects[i], x, y))
                {
                    POINT pt{ x, y };
                    ClientToScreen(hwnd, &pt);
                    g_contextTab = static_cast<int>(i);
                    ShowTabMenu(hwnd, pt.x, pt.y);
                    return 0;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            g_mousePos = POINT{ mx, my };
            bool oldMainScrollHover = g_mainScrollHover;
            g_mainScrollHover = Hit(g_mainScrollTrackRect, mx, my);
            if (oldMainScrollHover != g_mainScrollHover)
            {
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
            }
            if (g_draggingMainScroll)
            {
                int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_mainScrollThumbRect.bottom - g_mainScrollThumbRect.top);
                int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
                int maxScroll = std::max<int>(0, g_contentHeight - stageH);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(my - g_mainScrollDragOffset, g_mainScrollTrackRect.top, g_mainScrollTrackRect.bottom - thumbH);
                int newScroll = MulDiv(thumbTop - g_mainScrollTrackRect.top, maxScroll, travel);
                ScrollStageTo(hwnd, newScroll);
                return 0;
            }
            if (g_resizingSidebar)
            {
                ResizeSidebarNow(hwnd, mx, false);
                return 0;
            }
            if (g_selectingContent)
            {
                g_selectionEnd = POINT{
                    static_cast<LONG>(std::clamp(mx, static_cast<int>(g_selectionPageRect.left), static_cast<int>(g_selectionPageRect.right))),
                    static_cast<LONG>(std::clamp(my, static_cast<int>(g_selectionPageRect.top), static_cast<int>(g_selectionPageRect.bottom)))
                };
                if (PdfTab* tab = Active()) RefreshTextSelection(*tab);
                InvalidateRect(hwnd, &g_stage, FALSE);
                return 0;
            }
            int oldHoverTabClose = g_hoverTabClose;
            int oldHoverTab = g_hoverTab;
            int oldHoverOutlineRow = g_hoverOutlineRow;
            int oldHoverOutlineToggle = g_hoverOutlineToggle;
            int oldHoverSearchResult = g_hoverSearchResult;
            bool oldHasHoverButton = g_hasHoverButton;
            RECT oldHoverButtonRect = g_hoverButtonRect;
            g_hoverTab = -1;
            g_hoverTabClose = -1;
            g_hoverOutlineRow = -1;
            g_hoverOutlineToggle = -1;
            g_hoverSearchResult = -1;
            g_hasHoverButton = false;
            g_hoverButtonRect = RECT{};
            for (size_t i = 0; i < g_tabRects.size(); ++i)
            {
                if (Hit(g_tabRects[i], mx, my)) g_hoverTab = static_cast<int>(i);
                RECT close{ g_tabRects[i].right - S(34), g_tabRects[i].top + S(3), g_tabRects[i].right - S(6), g_tabRects[i].bottom - S(3) };
                if (Hit(close, mx, my))
                {
                    g_hoverTabClose = static_cast<int>(i);
                    break;
                }
            }
            if (g_sidebarOpen && !g_immersive && g_sideMode == 0)
            {
                for (size_t i = 0; i < g_outlineToggleRects.size() && i < g_outlineToggleRowIds.size(); ++i)
                {
                    if (Hit(g_outlineToggleRects[i], mx, my))
                    {
                        g_hoverOutlineToggle = g_outlineToggleRowIds[i];
                        break;
                    }
                }
                for (size_t i = 0; i < g_sidebarRowRects.size() && i < g_sidebarRowIds.size(); ++i)
                {
                    if (g_sidebarRowIds[i] >= 0 && Hit(g_sidebarRowRects[i], mx, my))
                    {
                        g_hoverOutlineRow = g_sidebarRowIds[i];
                        break;
                    }
                }
            }
            if (g_sidebarOpen && !g_immersive && g_sideMode == 2)
            {
                for (size_t i = 0; i < g_searchResultRects.size() && i < g_searchResultIds.size(); ++i)
                {
                    if (Hit(g_searchResultRects[i], mx, my))
                    {
                        g_hoverSearchResult = g_searchResultIds[i];
                        break;
                    }
                }
            }
            bool hadTip = g_hasHoverTip;
            std::wstring oldTip = g_hoverTip;
            g_hasHoverTip = false;
            g_hoverTip.clear();
            for (auto const& b : g_buttons)
            {
                if (Hit(b.rect, mx, my))
                {
                    g_hasHoverButton = true;
                    g_hoverButtonRect = b.rect;
                }
                if (!b.tip.empty() && Hit(b.rect, mx, my))
                {
                    g_hasHoverTip = true;
                    g_hoverTip = b.tip;
                    g_hoverTipRect = b.rect;
                    break;
                }
            }
            if (hadTip != g_hasHoverTip || oldTip != g_hoverTip || oldHoverTabClose != g_hoverTabClose || oldHoverTab != g_hoverTab)
            {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (oldHoverOutlineRow != g_hoverOutlineRow || oldHoverOutlineToggle != g_hoverOutlineToggle)
            {
                InvalidateRect(hwnd, &g_sidebar, FALSE);
            }
            if (oldHoverSearchResult != g_hoverSearchResult)
            {
                InvalidateRect(hwnd, &g_sidebar, FALSE);
            }
            if (oldHasHoverButton != g_hasHoverButton || !EqualRect(&oldHoverButtonRect, &g_hoverButtonRect))
            {
                if (oldHasHoverButton) InvalidateRect(hwnd, &oldHoverButtonRect, FALSE);
                if (g_hasHoverButton) InvalidateRect(hwnd, &g_hoverButtonRect, FALSE);
            }
            if (!g_statusPinned && !g_immersive)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                bool reveal = my >= client.bottom - S(48);
                if (reveal != g_statusReveal)
                {
                    g_statusReveal = reveal;
                    g_statusTargetOffset = reveal ? 0 : S(34);
                    SetTimer(hwnd, 9, 16, nullptr);
                }
            }
            if (g_draggingThumbSlider)
            {
                int x = mx;
                int width = std::max<int>(1, g_thumbSliderRect.right - g_thumbSliderRect.left);
                g_thumbSize = std::clamp<int>(ThumbSizeMin + (x - g_thumbSliderRect.left) * (ThumbSizeMax - ThumbSizeMin) / width, ThumbSizeMin, ThumbSizeMax);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (g_selectingContent)
            {
                RECT selection = CurrentSelectionRect();
                if (PdfTab* tab = Active()) RefreshTextSelection(*tab);
                g_selectingContent = false;
                ReleaseCapture();
                g_hasContentSelection =
                    !g_selectionQuads.empty() &&
                    (selection.right - selection.left) >= S(8) &&
                    (selection.bottom - selection.top) >= S(6);
                if (!g_hasContentSelection)
                {
                    g_selectionQuads.clear();
                    g_selectionText.clear();
                }
                InvalidateRect(hwnd, &g_stage, FALSE);
                return 0;
            }
            if (g_draggingMainScroll)
            {
                g_draggingMainScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
                return 0;
            }
            if (g_draggingThumbSlider)
            {
                g_draggingThumbSlider = false;
                ReleaseCapture();
                return 0;
            }
            if (g_resizingSidebar)
            {
                g_resizingSidebar = false;
                ReleaseCapture();
                ResizeSidebarNow(hwnd, g_sidebarTargetWidth, true);
                SaveState();
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == 1)
            {
                KillTimer(hwnd, 1);
                g_sidebarResizePending = false;
                ResizeSidebarNow(hwnd, g_sidebarTargetWidth, false);
                return 0;
            }
            if (wParam == 2)
            {
                KillTimer(hwnd, 2);
                g_hasPressedButton = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == 3)
            {
                KillTimer(hwnd, 3);
                if (PrefetchOnePage())
                {
                    SetTimer(hwnd, 3, 80, nullptr);
                }
                return 0;
            }
            if (wParam == 4)
            {
                int delta = g_stageScrollTarget - g_scrollY;
                if (!g_smoothStageScroll || std::abs(delta) <= S(2))
                {
                    ScrollStageTo(hwnd, g_stageScrollTarget);
                    g_smoothStageScroll = false;
                    KillTimer(hwnd, 4);
                    return 0;
                }
                int step = delta / 3;
                if (step == 0) step = delta > 0 ? 1 : -1;
                ScrollStageTo(hwnd, g_scrollY + step);
                return 0;
            }
            if (wParam == 5)
            {
                int delta = g_sidebarScrollTargetY - g_sidebarScrollY;
                if (!g_smoothSidebarScroll || std::abs(delta) <= S(2))
                {
                    ScrollSidebarTo(hwnd, g_sidebarScrollTargetY);
                    g_smoothSidebarScroll = false;
                    KillTimer(hwnd, 5);
                    return 0;
                }
                int step = delta / 3;
                if (step == 0) step = delta > 0 ? 1 : -1;
                ScrollSidebarTo(hwnd, g_sidebarScrollY + step);
                return 0;
            }
            if (wParam == 6)
            {
                KillTimer(hwnd, 6);
                g_stageRepaintPending = false;
                RequestStageRepaint(hwnd, true);
                return 0;
            }
            if (wParam == 7)
            {
                KillTimer(hwnd, 7);
                g_sidebarRepaintPending = false;
                RequestSidebarRepaint(hwnd);
                return 0;
            }
            if (wParam == 8)
            {
                if (GetTickCount64() < g_deferPageRenderUntil)
                {
                    SetTimer(hwnd, 8, 80, nullptr);
                    return 0;
                }
                KillTimer(hwnd, 8);
                InvalidateRect(hwnd, &g_stage, FALSE);
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                InvalidateRect(hwnd, &status, FALSE);
                return 0;
            }
            if (wParam == 9)
            {
                int delta = g_statusTargetOffset - g_statusOffset;
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT area{ 0, client.bottom - S(92), client.right, client.bottom };
                if (std::abs(delta) <= S(2))
                {
                    g_statusOffset = g_statusTargetOffset;
                    KillTimer(hwnd, 9);
                }
                else
                {
                    int step = delta / 2;
                    int minStep = std::max(1, S(4));
                    if (std::abs(step) < minStep) step = delta > 0 ? minStep : -minStep;
                    if (std::abs(step) > std::abs(delta)) step = delta;
                    g_statusOffset += step;
                }
                InvalidateRect(hwnd, &area, FALSE);
                return 0;
            }
            if (wParam == 10)
            {
                g_pageCaretVisible = !g_pageCaretVisible;
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                InvalidateRect(hwnd, &status, FALSE);
                return 0;
            }
            if (wParam == 11)
            {
                constexpr double durationMs = 190.0;
                double t = std::clamp((GetTickCount64() - g_sidebarAnimationStartMs) / durationMs, 0.0, 1.0);
                double eased = 1.0 - std::pow(1.0 - t, 3.0);
                g_sidebarWidth = g_sidebarAnimationStartWidth +
                    static_cast<int>((g_sidebarTargetWidth - g_sidebarAnimationStartWidth) * eased + 0.5);
                if (t >= 1.0)
                {
                    g_sidebarWidth = g_sidebarTargetWidth;
                    g_sidebarAnimating = false;
                    KillTimer(hwnd, 11);
                    if (g_sidebarWidth <= 0)
                    {
                        g_sidebarOpen = false;
                        g_sidebarWidth = g_sidebarExpandedWidth;
                    }
                    SaveState();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == 12)
            {
                if (!g_searchInputFocused || g_sideMode != 2)
                {
                    g_searchCaretVisible = true;
                    KillTimer(hwnd, 12);
                    return 0;
                }
                g_searchCaretVisible = !g_searchCaretVisible;
                InvalidateRect(hwnd, &g_searchInputRect, FALSE);
                return 0;
            }
            if (wParam == 13)
            {
                if (!g_annotationNoteFocused)
                {
                    g_annotationNoteCaretVisible = true;
                    KillTimer(hwnd, 13);
                    return 0;
                }
                g_annotationNoteCaretVisible = !g_annotationNoteCaretVisible;
                InvalidateRect(hwnd, &g_annotationNotePanelRect, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            ScrollByWheelDelta(hwnd, pt, delta, (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
            return 0;
        }
        case WM_GESTURE:
        {
            GESTUREINFO info{};
            info.cbSize = sizeof(info);
            if (!GetGestureInfo(reinterpret_cast<HGESTUREINFO>(lParam), &info))
            {
                break;
            }
            POINT pt{ info.ptsLocation.x, info.ptsLocation.y };
            ScreenToClient(hwnd, &pt);
            if (info.dwID == GID_BEGIN)
            {
                g_gesturePanning = false;
                g_pageWheelDeltaAccum = 0;
            }
            else if (info.dwID == GID_END)
            {
                g_gesturePanning = false;
                g_gestureSidebar = false;
                g_pageWheelDeltaAccum = 0;
            }
            else if (info.dwID == GID_PAN)
            {
                if ((info.dwFlags & GF_BEGIN) != 0 || !g_gesturePanning)
                {
                    g_gesturePanning = true;
                    g_gestureSidebar = g_sidebarOpen && !g_immersive && Hit(g_sidebar, pt.x, pt.y);
                    g_gestureLastY = pt.y;
                }
                else
                {
                    int deltaY = pt.y - g_gestureLastY;
                    g_gestureLastY = pt.y;
                    POINT targetPt = pt;
                    if (g_gestureSidebar)
                    {
                        targetPt.x = std::clamp(targetPt.x, g_sidebar.left, std::max(g_sidebar.left, g_sidebar.right - 1));
                        targetPt.y = std::clamp(targetPt.y, g_sidebar.top, std::max(g_sidebar.top, g_sidebar.bottom - 1));
                    }
                    ScrollByPanPixels(hwnd, targetPt, deltaY);
                }
            }
            CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lParam));
            return 0;
        }
        case WM_VSCROLL:
        {
            PdfTab* tab = Active();
            int code = LOWORD(wParam);
            int newScroll = g_scrollY;
            if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION)
            {
                SCROLLINFO si{};
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                newScroll = si.nTrackPos;
            }
            else if (code == SB_LINEUP) newScroll -= S(40);
            else if (code == SB_LINEDOWN) newScroll += S(40);
            else if (code == SB_PAGEUP) newScroll -= g_stage.bottom - g_stage.top;
            else if (code == SB_PAGEDOWN) newScroll += g_stage.bottom - g_stage.top;
            else if (code == SB_TOP) newScroll = 0;
            else if (code == SB_BOTTOM) newScroll = g_contentHeight;
            g_smoothStageScroll = false;
            KillTimer(hwnd, 4);
            ScrollStageTo(hwnd, newScroll);
            return 0;
        }
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && g_hasContentSelection && !g_selectionQuads.empty())
            {
                if (wParam == 'C')
                {
                    CopyTextToClipboard(hwnd, g_selectionText);
                    return 0;
                }
                if (wParam == 'H')
                {
                    AddAnnotationFromSelection(hwnd, AnnotationKind::Highlight);
                    return 0;
                }
                if (wParam == 'U')
                {
                    AddAnnotationFromSelection(hwnd, AnnotationKind::Underline);
                    return 0;
                }
                if (wParam == 'K')
                {
                    AddAnnotationFromSelection(hwnd, AnnotationKind::Strikeout);
                    return 0;
                }
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'F')
            {
                g_sidebarOpen = true;
                g_sideMode = 2;
                g_searchInputFocused = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'O') { OpenDialog(hwnd); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'W') { CloseTab(g_active); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'S')
            {
                if (PdfTab* tab = Active()) SaveAnnotationsToPdf(hwnd, *tab);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_pageInputFocused) { CancelPageInput(hwnd); return 0; }
            if (wParam == VK_ESCAPE && g_hasContentSelection) { g_hasContentSelection = false; InvalidateRect(hwnd, &g_stage, FALSE); return 0; }
            if (wParam == VK_ESCAPE && g_immersive) { ApplyImmersiveWindow(hwnd, false); return 0; }
            if (wParam == VK_NEXT)
            {
                PdfTab* tab = Active();
                if (tab && !g_continuousRead) ScrollSinglePageByPixels(hwnd, *tab, std::max<int>(S(80), g_stage.bottom - g_stage.top - S(56)));
                else ScrollStageTo(hwnd, g_scrollY + g_stage.bottom - g_stage.top);
                return 0;
            }
            if (wParam == VK_PRIOR)
            {
                PdfTab* tab = Active();
                if (tab && !g_continuousRead) ScrollSinglePageByPixels(hwnd, *tab, -std::max<int>(S(80), g_stage.bottom - g_stage.top - S(56)));
                else ScrollStageTo(hwnd, g_scrollY - (g_stage.bottom - g_stage.top));
                return 0;
            }
            break;
        case WM_CHAR:
            if (g_annotationNoteFocused)
            {
                PdfTab* tab = Active();
                PdfAnnotation* annotation = nullptr;
                if (!tab || !ActiveAnnotation(*tab, annotation))
                {
                    g_annotationNoteFocused = false;
                    return 0;
                }
                bool changed = false;
                if (wParam == VK_BACK)
                {
                    if (!annotation->note.empty())
                    {
                        annotation->note.pop_back();
                        changed = true;
                    }
                }
                else if (wParam == VK_RETURN)
                {
                    if (annotation->note.size() < 1000)
                    {
                        annotation->note.push_back(L'\n');
                        changed = true;
                    }
                }
                else if (wParam >= 32 && annotation->note.size() < 1000)
                {
                    annotation->note.push_back(static_cast<wchar_t>(wParam));
                    changed = true;
                }
                if (changed)
                {
                    tab->dirty = true;
                    g_annotationNoteCaretVisible = true;
                    SetTimer(hwnd, 13, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g_pageInputFocused)
            {
                if (wParam == VK_BACK)
                {
                    if (!g_pageInputText.empty()) g_pageInputText.pop_back();
                }
                else if (wParam == VK_RETURN)
                {
                    CommitPageInput(hwnd);
                    return 0;
                }
                else if (wParam >= L'0' && wParam <= L'9' && g_pageInputText.size() < 6)
                {
                    if (g_pageInputText == L"0") g_pageInputText.clear();
                    g_pageInputText.push_back(static_cast<wchar_t>(wParam));
                }
                g_pageCaretVisible = true;
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                InvalidateRect(hwnd, &status, FALSE);
                return 0;
            }
            if (g_searchInputFocused && g_sideMode == 2)
            {
                bool changed = false;
                if (wParam == VK_BACK)
                {
                    if (!g_searchQuery.empty())
                    {
                        g_searchQuery.pop_back();
                        changed = true;
                    }
                }
                else if (wParam == VK_RETURN)
                {
                    return 0;
                }
                else if (wParam >= 32)
                {
                    g_searchQuery.push_back(static_cast<wchar_t>(wParam));
                    changed = true;
                }
                if (changed)
                {
                    g_searchResults.clear();
                    g_searchCurrent = 0;
                    g_searchHasRun = false;
                    g_searchLastQuery.clear();
                    g_searchCaretVisible = true;
                    SetTimer(hwnd, 12, 500, nullptr);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_immersive) ApplyImmersiveWindow(hwnd, false);
            StopFileWatcher();
            for (auto& tab : g_tabs) TouchRecentProgress(*tab, false);
            for (auto& tab : g_tabs) DropTab(*tab);
            if (g_glassBackdrop) DeleteObject(g_glassBackdrop);
            SaveState();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Gdiplus::GdiplusStartupInput gdiplusInput{};
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
        LoadState();
        g_statusOffset = g_statusPinned ? 0 : S(34);
        g_statusTargetOffset = g_statusOffset;
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"XiaoYuePDFWindow";
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, AppName, WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 840, nullptr, nullptr, instance, nullptr);
        if (!g_hwnd) return 1;
        ApplyWindowChrome();
        ShowWindow(g_hwnd, show == SW_SHOWMINIMIZED ? show : SW_SHOWMAXIMIZED);
        UpdateWindow(g_hwnd);
        int argc = 0;
        PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; ++i) if (std::filesystem::exists(argv[i])) OpenPdf(g_hwnd, argv[i]);
            LocalFree(argv);
        }
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return static_cast<int>(msg.wParam);
    }
    catch (std::exception const& e)
    {
        MessageBoxA(nullptr, e.what(), "QingYuePDF startup failed", MB_ICONERROR);
        return 1;
    }
}
