#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
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

    constexpr wchar_t const* AppName = L"\u8f7b\u9605PDF";

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
        mutable HBITMAP dib{};

        PageImage() = default;
        ~PageImage() { ResetDib(); }

        PageImage(PageImage const& other)
            : width(other.width), height(other.height), bgra(other.bgra)
        {
        }

        PageImage& operator=(PageImage const& other)
        {
            if (this != &other)
            {
                ResetDib();
                width = other.width;
                height = other.height;
                bgra = other.bgra;
            }
            return *this;
        }

        PageImage(PageImage&& other) noexcept
            : width(other.width), height(other.height), bgra(std::move(other.bgra)), dib(other.dib)
        {
            other.width = 0;
            other.height = 0;
            other.dib = nullptr;
        }

        PageImage& operator=(PageImage&& other) noexcept
        {
            if (this != &other)
            {
                ResetDib();
                width = other.width;
                height = other.height;
                bgra = std::move(other.bgra);
                dib = other.dib;
                other.width = 0;
                other.height = 0;
                other.dib = nullptr;
            }
            return *this;
        }

        void ResetDib() const
        {
            if (dib)
            {
                DeleteObject(dib);
                dib = nullptr;
            }
        }

        size_t PixelBytes() const
        {
            if (width <= 0 || height <= 0) return 0;
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 4ull;
        }

        size_t CacheBytes() const
        {
            return dib ? PixelBytes() : bgra.size();
        }

        void ReleasePixels()
        {
            std::vector<std::uint8_t>().swap(bgra);
        }
    };

    struct TileKey
    {
        int page{};
        int zoom{};
        int theme{};
        int level{};
        int x{};
        int y{};
        int w{};
        int h{};

        bool operator<(TileKey const& other) const
        {
            return std::tie(page, zoom, theme, level, x, y, w, h) <
                std::tie(other.page, other.zoom, other.theme, other.level, other.x, other.y, other.w, other.h);
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

    struct AnnotationSegment
    {
        double x0{};
        double y0{};
        double x1{};
        double y1{};
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
        bool persisted{};
        std::vector<AnnotationSegment> segments;
        std::wstring text;
        std::wstring note;
        std::wstring author{ L"10726" };
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
        long long createdAtMs{};
        const char* author{};
        const char* contents{};
    };

    struct QuietPdfAnnotationInfo
    {
        int page{};
        int kind{};
        float x0{};
        float y0{};
        float x1{};
        float y1{};
        float colorR{};
        float colorG{};
        float colorB{};
        float opacity{};
        long long createdAtMs{};
        char author[128]{};
        char contents[512]{};
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
        int savedScrollY{};
        double zoom{ 1.0 };
        double cachedZoom{};
        unsigned cachedDpi{};
        std::wstring path;
        std::wstring name;
        std::vector<PageInfo> pages;
        std::vector<OutlineItem> outline;
        std::map<int, PageImage> cache;
        std::map<int, PageImage> zoomFallbackCache;
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
        double zoomFallbackZoom{};
        unsigned zoomFallbackDpi{};
        int zoomFallbackTheme{ -1 };
        ULONGLONG zoomFallbackUntil{};
        int cachedThumbSize{};
        int cachedThumbTheme{ -1 };
        std::filesystem::file_time_type fileStamp{};
        bool hasFileStamp{};
        bool dirty{};
        unsigned long renderVersion{};
    };

    struct RecentItem
    {
        std::wstring path;
        std::wstring name;
        int page{ 1 };
        long long openedAt{};
        long long pinnedAt{};
        bool pinned{};
        bool favorite{};
    };

    struct LastSessionItem
    {
        std::wstring path;
        int page{ 1 };
        int scrollY{};
        double zoom{ 1.0 };
    };

    enum class HomeAction
    {
        Open = 0,
        Pin,
        Folder,
        Remove,
        Favorite,
        ToggleView,
        ClearAll,
        ConfirmClear,
        CancelClear,
        CloseDialog
    };

    struct HomeHit
    {
        RECT rect{};
        int recentIndex{ -1 };
        HomeAction action{ HomeAction::Open };
        std::wstring tip;
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
        CmdNoteDeleteAll,
        CmdHomeToggleView,
        CmdHomeClearRecords,
        CmdTabNavPrev,
        CmdTabNavNext,
        CmdTabListToggle,
        CmdSidebarPanelToggle
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

    constexpr wchar_t const* PdfProgId = L"QingYuePDF.Document";
    constexpr wchar_t const* PdfRegisteredApp = L"QingYuePDF";
    constexpr wchar_t const* UserChoiceExperienceFallback = L"User Choice set via Windows User Experience {D18B6DD5-6124-4341-9318-804003BAFA0B}";
    int g_defaultAssocStatus = 0;

    std::wstring LowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    }

    bool WriteRegSz(HKEY root, std::wstring const& path, wchar_t const* name, std::wstring const& value)
    {
        HKEY key{};
        if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    bool WriteRegDword(HKEY root, std::wstring const& path, wchar_t const* name, DWORD value)
    {
        HKEY key{};
        if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        LSTATUS status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&value), sizeof(value));
        RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    std::wstring ReadRegSz(HKEY root, std::wstring const& path, wchar_t const* name)
    {
        HKEY key{};
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return {};

        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t))
        {
            RegCloseKey(key);
            return {};
        }

        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            return {};
        }

        RegCloseKey(key);
        while (!value.empty() && value.back() == L'\0')
            value.pop_back();
        return value;
    }

    std::wstring PowerShellEscapeSingleQuoted(std::wstring value)
    {
        size_t pos = 0;
        while ((pos = value.find(L'\'', pos)) != std::wstring::npos)
        {
            value.replace(pos, 1, L"''");
            pos += 2;
        }
        return value;
    }

    bool RunHiddenProcess(std::wstring const& application, std::wstring commandLine, DWORD& exitCode)
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
        buffer.push_back(L'\0');
        if (!CreateProcessW(application.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;

        WaitForSingleObject(pi.hProcess, INFINITE);
        exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    bool VerifyPdfDefaultAssociation()
    {
        for (int i = 0; i < 20; ++i)
        {
            std::wstring progId = ReadRegSz(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice",
                L"ProgId");
            if (_wcsicmp(progId.c_str(), PdfProgId) == 0)
                return true;
            Sleep(100);
        }
        return false;
    }

    bool HasUserChoiceProtection()
    {
        HKEY key{};
        LSTATUS status = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\UCPD",
            0,
            KEY_QUERY_VALUE,
            &key);
        if (status == ERROR_SUCCESS)
            RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    bool DeleteRegTreeIfExists(HKEY root, std::wstring const& path)
    {
        LSTATUS status = RegDeleteTreeW(root, path.c_str());
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    }

    std::wstring CurrentUserSidString()
    {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return {};

        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<BYTE> buffer(bytes);
        if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes))
        {
            CloseHandle(token);
            return {};
        }
        CloseHandle(token);

        TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPWSTR sidText = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
            return {};
        std::wstring sid = sidText;
        LocalFree(sidText);
        return sid;
    }

    std::wstring CurrentAssocTimestampHex()
    {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        local.wSecond = 0;
        local.wMilliseconds = 0;
        FILETIME localFileTime{};
        FILETIME utcFileTime{};
        if (!SystemTimeToFileTime(&local, &localFileTime) || !LocalFileTimeToFileTime(&localFileTime, &utcFileTime))
            return {};

        wchar_t buffer[17]{};
        swprintf_s(buffer, L"%08lx%08lx", utcFileTime.dwHighDateTime, utcFileTime.dwLowDateTime);
        return buffer;
    }

    std::wstring DetectUserChoiceExperience()
    {
        wchar_t sysWow64[MAX_PATH]{};
        UINT len = GetSystemWow64DirectoryW(sysWow64, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            len = GetSystemDirectoryW(sysWow64, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                return UserChoiceExperienceFallback;
        }

        std::filesystem::path shell32 = std::filesystem::path(sysWow64) / L"Shell32.dll";
        std::ifstream in(shell32, std::ios::binary);
        if (!in)
            return UserChoiceExperienceFallback;
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() < sizeof(wchar_t) * 8)
            return UserChoiceExperienceFallback;

        std::wstring text(reinterpret_cast<wchar_t const*>(bytes.data()), bytes.size() / sizeof(wchar_t));
        std::wstring needle = L"User Choice set via Windows User Experience";
        size_t start = text.find(needle);
        if (start == std::wstring::npos)
            return UserChoiceExperienceFallback;
        size_t end = text.find(L'}', start);
        if (end == std::wstring::npos)
            return UserChoiceExperienceFallback;
        return text.substr(start, end - start + 1);
    }

    int32_t ReadInt32LE(std::vector<BYTE> const& bytes, size_t offset)
    {
        uint32_t value = static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        return static_cast<int32_t>(value);
    }

    int64_t ShiftRightSigned(int64_t value, int count)
    {
        if (value >= 0) return value >> count;
        return (value >> count) ^ (0xFFFF0000LL);
    }

    int32_t Wrap32(int64_t value)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(value & 0xFFFFFFFFu));
    }

    bool Md5Wide(std::wstring const& text, BYTE hash[16])
    {
        HCRYPTPROV provider{};
        HCRYPTHASH digest{};
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
            return false;
        if (!CryptCreateHash(provider, CALG_MD5, 0, 0, &digest))
        {
            CryptReleaseContext(provider, 0);
            return false;
        }
        DWORD byteCount = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
        bool ok = CryptHashData(digest, reinterpret_cast<BYTE const*>(text.c_str()), byteCount, 0) != 0;
        DWORD hashSize = 16;
        if (ok) ok = CryptGetHashParam(digest, HP_HASHVAL, hash, &hashSize, 0) != 0 && hashSize == 16;
        CryptDestroyHash(digest);
        CryptReleaseContext(provider, 0);
        return ok;
    }

    std::string Base64EncodeBytes(BYTE const* data, size_t size)
    {
        static char const* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((size + 2) / 3) * 4);
        for (size_t i = 0; i < size; i += 3)
        {
            uint32_t block = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < size) block |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < size) block |= static_cast<uint32_t>(data[i + 2]);
            out.push_back(alphabet[(block >> 18) & 63]);
            out.push_back(alphabet[(block >> 12) & 63]);
            out.push_back(i + 1 < size ? alphabet[(block >> 6) & 63] : '=');
            out.push_back(i + 2 < size ? alphabet[block & 63] : '=');
        }
        return out;
    }

    std::wstring ComputeUserChoiceHash(std::wstring const& extension, std::wstring const& sid, std::wstring const& progId, std::wstring const& timestampHex, std::wstring const& experience)
    {
        std::wstring baseInfo = LowerCopy(extension + sid + progId + timestampHex + experience);
        BYTE md5[16]{};
        if (!Md5Wide(baseInfo, md5))
            return {};

        std::vector<BYTE> baseBytes(reinterpret_cast<BYTE const*>(baseInfo.data()), reinterpret_cast<BYTE const*>(baseInfo.data()) + baseInfo.size() * sizeof(wchar_t));
        baseBytes.push_back(0);
        baseBytes.push_back(0);
        baseBytes.resize(baseBytes.size() + 16, 0);

        auto shift = [](int64_t value, int count) -> int64_t
        {
            if ((value & 0x80000000LL) != 0)
                return static_cast<int32_t>((static_cast<uint32_t>(value >> count)) ^ 0xFFFF0000u);
            return value >> count;
        };
        std::vector<BYTE> md5Bytes(md5, md5 + 16);
        int64_t lengthBase = static_cast<int64_t>(baseInfo.size() * sizeof(wchar_t) + sizeof(wchar_t));
        int64_t length = (((lengthBase & 4) <= 1) ? 1 : 0) + shift(lengthBase, 2) - 1;
        if (length <= 1)
            return {};

        BYTE outHash[16]{};
        int64_t md51 = (static_cast<int64_t>(ReadInt32LE(md5Bytes, 0)) | 1LL) + 0x69FB0000LL;
        int64_t md52 = (static_cast<int64_t>(ReadInt32LE(md5Bytes, 4)) | 1LL) + 0x13DB0000LL;
        int64_t counter = shift(length - 2, 1) + 1;
        size_t p = 0;
        int32_t cache = 0;
        int32_t out1 = 0;
        int32_t out2 = 0;
        while (counter-- > 0)
        {
            int32_t r0 = Wrap32(static_cast<int64_t>(ReadInt32LE(baseBytes, p)) + out1);
            int32_t r1 = ReadInt32LE(baseBytes, p + 4);
            p += 8;
            int32_t r2 = Wrap32((static_cast<int64_t>(r0) * md51) - (0x10FA9605LL * shift(r0, 16)));
            int32_t r2b = Wrap32((0x79F8A395LL * r2) + (0x689B6B9FLL * shift(r2, 16)));
            int32_t r3 = Wrap32((0xEA970001LL * r2b) - (0x3C101569LL * shift(r2b, 16)));
            int32_t r4 = Wrap32(static_cast<int64_t>(r3) + r1);
            int32_t r5 = Wrap32(static_cast<int64_t>(cache) + r3);
            int32_t r6 = Wrap32((static_cast<int64_t>(r4) * md52) - (0x3CE8EC25LL * shift(r4, 16)));
            int32_t r6b = Wrap32((0x59C3AF2DLL * r6) - (0x2232E0F1LL * shift(r6, 16)));
            out1 = Wrap32((0x1EC90001LL * r6b) + (0x35BD1EC9LL * shift(r6b, 16)));
            out2 = Wrap32(static_cast<int64_t>(r5) + out1);
            cache = out2;
        }
        *reinterpret_cast<int32_t*>(outHash + 0) = out1;
        *reinterpret_cast<int32_t*>(outHash + 4) = out2;

        md51 = static_cast<int64_t>(ReadInt32LE(md5Bytes, 0)) | 1LL;
        md52 = static_cast<int64_t>(ReadInt32LE(md5Bytes, 4)) | 1LL;
        counter = shift(length - 2, 1) + 1;
        p = 0;
        cache = 0;
        out1 = 0;
        out2 = 0;
        while (counter-- > 0)
        {
            int32_t r0 = Wrap32(static_cast<int64_t>(ReadInt32LE(baseBytes, p)) + out1);
            p += 8;
            int32_t r1 = Wrap32(static_cast<int64_t>(r0) * md51);
            int32_t r1b = Wrap32((0xB1110000LL * r1) - (0x30674EEFLL * shift(r1, 16)));
            int32_t r2 = Wrap32((0x5B9F0000LL * r1b) - (0x78F7A461LL * shift(r1b, 16)));
            int32_t r2b = Wrap32((0x12CEB96DLL * shift(r2, 16)) - (0x46930000LL * r2));
            int32_t r3 = Wrap32((0x1D830000LL * r2b) + (0x257E1D83LL * shift(r2b, 16)));
            int32_t r4 = Wrap32(md52 * (static_cast<int64_t>(r3) + ReadInt32LE(baseBytes, p - 4)));
            int32_t r4b = Wrap32((0x16F50000LL * r4) - (0x5D8BE90BLL * shift(r4, 16)));
            int32_t r5 = Wrap32((0x96FF0000LL * r4b) - (0x2C7C6901LL * shift(r4b, 16)));
            int32_t r5b = Wrap32((0x2B890000LL * r5) + (0x7C932B89LL * shift(r5, 16)));
            out1 = Wrap32((0x9F690000LL * r5b) - (0x405B6097LL * shift(r5b, 16)));
            out2 = Wrap32(static_cast<int64_t>(out1) + cache + r3);
            cache = out2;
        }
        *reinterpret_cast<int32_t*>(outHash + 8) = out1;
        *reinterpret_cast<int32_t*>(outHash + 12) = out2;

        uint32_t hash1 = static_cast<uint32_t>(ReadInt32LE(std::vector<BYTE>(outHash, outHash + 16), 8)) ^ static_cast<uint32_t>(ReadInt32LE(std::vector<BYTE>(outHash, outHash + 16), 0));
        uint32_t hash2 = static_cast<uint32_t>(ReadInt32LE(std::vector<BYTE>(outHash, outHash + 16), 12)) ^ static_cast<uint32_t>(ReadInt32LE(std::vector<BYTE>(outHash, outHash + 16), 4));
        BYTE out[8]{
            static_cast<BYTE>(hash1 & 0xFF), static_cast<BYTE>((hash1 >> 8) & 0xFF), static_cast<BYTE>((hash1 >> 16) & 0xFF), static_cast<BYTE>((hash1 >> 24) & 0xFF),
            static_cast<BYTE>(hash2 & 0xFF), static_cast<BYTE>((hash2 >> 8) & 0xFF), static_cast<BYTE>((hash2 >> 16) & 0xFF), static_cast<BYTE>((hash2 >> 24) & 0xFF)
        };
        return FromUtf8(Base64EncodeBytes(out, sizeof(out)));
    }

    bool RegisterPdfAssociationForCurrentUser()
    {
        std::wstring exe = (ExeDir() / AppName).replace_extension(L".exe").wstring();
        if (!std::filesystem::exists(exe))
            exe = (ExeDir() / L"\u8f7b\u9605PDF.exe").wstring();
        if (!std::filesystem::exists(exe))
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            exe = modulePath;
        }

        return
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\\u8f7b\u9605PDF.exe", L"FriendlyAppName", AppName) &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\\u8f7b\u9605PDF.exe\\DefaultIcon", nullptr, exe + L",0") &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\\u8f7b\u9605PDF.exe\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"") &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\\u8f7b\u9605PDF.exe\\SupportedTypes", L".pdf", L"") &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids", PdfProgId, L"") &&
            WriteRegSz(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + PdfProgId, nullptr, AppName) &&
            WriteRegSz(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + PdfProgId + L"\\DefaultIcon", nullptr, exe + L",0") &&
            WriteRegSz(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + PdfProgId + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"") &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\QingYuePDF\\Capabilities", L"ApplicationName", AppName) &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\QingYuePDF\\Capabilities", L"ApplicationDescription", AppName) &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\QingYuePDF\\Capabilities\\FileAssociations", L".pdf", PdfProgId) &&
            WriteRegSz(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", PdfRegisteredApp, L"Software\\QingYuePDF\\Capabilities");
    }

    bool TrySetDefaultPdfAssociationViaShell()
    {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool shouldUninit = SUCCEEDED(init);
        if (init == RPC_E_CHANGED_MODE)
            init = S_OK;
        if (FAILED(init))
        {
            g_defaultAssocStatus = 170;
            return false;
        }

        IApplicationAssociationRegistration* association = nullptr;
        HRESULT hr = CoCreateInstance(
            CLSID_ApplicationAssociationRegistration,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&association));
        if (SUCCEEDED(hr) && association)
        {
            hr = association->SetAppAsDefault(PdfRegisteredApp, L".pdf", AT_FILEEXTENSION);
            association->Release();
        }
        if (shouldUninit)
            CoUninitialize();

        if (FAILED(hr))
        {
            g_defaultAssocStatus = 180;
            return false;
        }

        SHChangeNotify(0x08000000, 0, nullptr, nullptr);
        bool verified = VerifyPdfDefaultAssociation();
        g_defaultAssocStatus = verified ? 0 : 190;
        return verified;
    }

    bool TrySetDefaultPdfAssociationViaLicensedHelper()
    {
        std::filesystem::path helper = ExeDir() / L"SetUserFTA.exe";
        if (!std::filesystem::exists(helper))
            return false;

        std::wstring commandLine =
            L"\"" + helper.wstring() + L"\" .pdf " + PdfProgId;
        DWORD exitCode = 1;
        if (!RunHiddenProcess(helper.wstring(), commandLine, exitCode) || exitCode != 0)
        {
            g_defaultAssocStatus = 200;
            return false;
        }

        SHChangeNotify(0x08000000, 0, nullptr, nullptr);
        bool verified = VerifyPdfDefaultAssociation();
        g_defaultAssocStatus = verified ? 0 : 210;
        return verified;
    }

    bool TrySetDefaultPdfAssociationDirect()
    {
        g_defaultAssocStatus = 100;
        if (!RegisterPdfAssociationForCurrentUser())
        {
            g_defaultAssocStatus = 110;
            return false;
        }
        if (TrySetDefaultPdfAssociationViaLicensedHelper())
            return true;
        if (TrySetDefaultPdfAssociationViaShell())
            return true;

        std::wstring sid = CurrentUserSidString();
        std::wstring timestampHex = CurrentAssocTimestampHex();
        std::wstring experience = DetectUserChoiceExperience();
        if (sid.empty() || timestampHex.empty() || experience.empty())
        {
            g_defaultAssocStatus = 120;
            return false;
        }

        std::wstring hash = ComputeUserChoiceHash(L".pdf", sid, PdfProgId, timestampHex, experience);
        if (hash.empty())
        {
            g_defaultAssocStatus = 130;
            return false;
        }

        WriteRegDword(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
            (std::wstring(PdfProgId) + L"_.pdf").c_str(),
            0);
        WriteRegDword(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
            L"Applications\\\u8f7b\u9605PDF.exe_.pdf",
            0);

        DeleteRegTreeIfExists(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice");
        if (!WriteRegSz(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice", L"Hash", hash))
        {
            g_defaultAssocStatus = 140;
            return false;
        }
        if (!WriteRegSz(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice", L"ProgId", PdfProgId))
        {
            g_defaultAssocStatus = 150;
            return false;
        }

        SHChangeNotify(0x08000000, 0, nullptr, nullptr);
        bool verified = VerifyPdfDefaultAssociation();
        g_defaultAssocStatus = verified ? 0 : 160;
        return verified;
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
        using LoadAnnotationsFn = int(__cdecl*)(fz_context*, fz_document*, QuietPdfAnnotationInfo*, int);
        using SelectTextFn = int(__cdecl*)(fz_context*, fz_document*, int, float, float, float, float, fz_quad*, int, char*, int);
        using SelectWordAtPointFn = int(__cdecl*)(fz_context*, fz_document*, int, float, float, fz_quad*, int, char*, int);
        using FindImageAtPointFn = int(__cdecl*)(fz_context*, fz_document*, int, float, float, fz_rect*);
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
            Get(loadAnnotations, "quiet_pdf_load_annotations");
            Get(selectText, "quiet_pdf_select_text");
            GetOptional(selectWordAtPoint, "quiet_pdf_select_word_at_point");
            GetOptional(findImageAtPoint, "quiet_pdf_find_image_at_point");
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
        LoadAnnotationsFn loadAnnotations{};
        SelectTextFn selectText{};
        SelectWordAtPointFn selectWordAtPoint{};
        FindImageAtPointFn findImageAtPoint{};
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

        template <typename T>
        void GetOptional(T& target, const char* name)
        {
            target = reinterpret_cast<T>(GetProcAddress(module, name));
        }
    };

    MuPdf g_pdf;
    HWND g_hwnd{};
    unsigned g_dpi{ 96 };
    std::vector<std::unique_ptr<PdfTab>> g_tabs;
    std::vector<RecentItem> g_recent;
    std::vector<LastSessionItem> g_lastSessionTabs;
    std::map<std::wstring, PageImage> g_homeThumbCache;
    std::vector<Button> g_buttons;
    std::vector<RECT> g_tabRects;
    std::vector<int> g_tabRectIds;
    std::vector<bool> g_tabTitleTruncated;
    std::vector<RECT> g_recentRects;
    std::vector<HomeHit> g_homeHits;
    std::vector<int> g_homeRecentIds;
    std::vector<int> g_sidebarRowPages;
    std::vector<RECT> g_sidebarRowRects;
    std::vector<int> g_sidebarRowIds;
    std::vector<RECT> g_outlineToggleRects;
    std::vector<int> g_outlineToggleRowIds;
    std::vector<RECT> g_sideTabRects;
    std::vector<RECT> g_searchResultRects;
    std::vector<int> g_searchResultIds;
    std::vector<RECT> g_tabListItemRects;
    std::vector<int> g_tabListItemIds;
    constexpr UINT WM_QUIETPDF_FILE_CHANGED = WM_APP + 41;
    constexpr UINT WM_QUIETPDF_DRAG_SCROLL = WM_APP + 42;
    constexpr UINT WM_QUIETPDF_PAGE_RENDERED = WM_APP + 43;

    struct PageRenderRequest
    {
        unsigned long long serial{};
        unsigned long documentVersion{};
        HWND target{};
        std::wstring path;
        int page{};
        bool tile{};
        int x{};
        int y{};
        int w{};
        int h{};
        double zoom{};
        double renderZoom{};
        int level{};
        unsigned long long viewportGeneration{};
        unsigned dpi{};
        int theme{};
        int priority{};
    };

    struct PageRenderResult
    {
        PageRenderRequest request;
        PageImage image;
    };

    std::mutex g_pageRenderMutex;
    std::condition_variable g_pageRenderCv;
    std::thread g_pageRenderThread;
    std::deque<PageRenderRequest> g_pageRenderQueue;
    PageRenderRequest g_pageRenderActiveRequest;
    bool g_pageRenderActive{};
    bool g_pageRenderStop{};
    unsigned long long g_pageRenderSerial{};
    std::atomic<unsigned long> g_nextRenderVersion{ 0 };
    std::mutex g_fileWatchMutex;
    std::thread g_fileWatchThread;
    HANDLE g_fileWatchDirHandle{};
    HANDLE g_fileWatchStopEvent{};
    std::atomic<unsigned long> g_fileWatchGeneration{ 0 };
    RECT g_mainScrollTrackRect{};
    RECT g_mainScrollThumbRect{};
    RECT g_mainHScrollTrackRect{};
    RECT g_mainHScrollThumbRect{};
    RECT g_homeScrollTrackRect{};
    RECT g_homeScrollThumbRect{};
    RECT g_sidebarScrollTrackRect{};
    RECT g_sidebarScrollThumbRect{};
    RECT g_stage{};
    RECT g_sidebar{};
    RECT g_searchInputRect{};
    RECT g_thumbSliderRect{};
    WINDOWPLACEMENT g_preImmersivePlacement{ sizeof(WINDOWPLACEMENT) };
    LONG_PTR g_preImmersiveStyle{};
    LONG_PTR g_preImmersiveExStyle{};
    int g_active{ -1 };
    int g_scrollY{};
    int g_scrollX{};
    int g_stageScrollTarget{};
    double g_stageScrollPos{};
    double g_stageScrollVelocity{};
    ULONGLONG g_stageScrollLastTick{};
    int g_contentHeight{};
    int g_contentWidth{};
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
    constexpr int ThumbSizeMax = 320;
    int g_homeThumbWidth{};
    int g_homeScrollY{};
    int g_homeContentHeight{};
    int g_theme{};
    int g_viewMode{};
    int g_contextTab{ -1 };
    int g_lastSessionActive{ -1 };
    int g_searchCurrent{};
    int g_selectedOutlineRow{ -1 };
    int g_tabVisibleStart{};
    int g_hoverTab{ -1 };
    int g_hoverTabClose{ -1 };
    int g_hoverOutlineRow{ -1 };
    int g_hoverOutlineToggle{ -1 };
    int g_hoverSearchResult{ -1 };
    int g_hoverHomeHit{ -1 };
    int g_hoverRecentRect{ -1 };
    int g_hoverTabListItem{ -1 };
    int g_outlineCollapseVersion{};
    RECT g_pressedButtonRect{};
    RECT g_hoverButtonRect{};
    RECT g_hoverTipRect{};
    RECT g_pageInputRect{};
    RECT g_homeSearchRect{};
    RECT g_homeSearchClearRect{};
    RECT g_tabNavPrevRect{};
    RECT g_tabNavNextRect{};
    RECT g_tabCountRect{};
    RECT g_tabListRect{};
    RECT g_tabListScrollTrackRect{};
    RECT g_tabListScrollThumbRect{};
    RECT g_sidebarPanelToggleRect{};
    POINT g_mousePos{};
    bool g_sidebarOpen{ true };
    bool g_immersive{};
    bool g_immersiveSidebarOpen{};
    bool g_hasPreImmersivePlacement{};
    bool g_resizingSidebar{};
    bool g_draggingThumbSlider{};
    bool g_draggingTab{};
    bool g_tabDetachArmed{};
    int g_draggingTabIndex{ -1 };
    POINT g_tabDragStart{};
    POINT g_tabDragOffset{};
    RECT g_tabDragSourceRect{};
    RECT g_tabDragCurrentRect{};
    bool g_tabDragFloating{};
    int g_tabDragInsertIndex{ -1 };
    bool g_tabListOpen{};
    bool g_draggingTabListScroll{};
    int g_tabListScrollY{};
    int g_tabListContentHeight{};
    int g_tabListDragOffset{};
    int g_dragScrollTarget{};
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
    bool g_draggingMainHScroll{};
    bool g_draggingHomeScroll{};
    bool g_draggingSidebarScroll{};
    bool g_dragScrollPosted{};
    bool g_mainScrollThumbLive{};
    bool g_mainScrollHover{};
    bool g_mainHScrollHover{};
    bool g_homeScrollHover{};
    bool g_sidebarScrollHover{};
    bool g_sidebarPanelToggleHover{};
    bool g_stageRepaintPending{};
    bool g_sidebarRepaintPending{};
    bool g_sidebarResizePending{};
    bool g_sidebarAnimating{};
    bool g_stageIdleRefreshPending{};
    unsigned long long g_viewportGeneration{};
    bool g_sidebarProgressRefreshPending{};
    bool g_gesturePanning{};
    bool g_gestureSidebar{};
    bool g_touchPanQueued{};
    bool g_touchMomentum{};
    bool g_pagePrefetchPending{};
    ULONGLONG g_lastStageRepaintMs{};
    ULONGLONG g_lastSidebarRepaintMs{};
    ULONGLONG g_lastSidebarResizeMs{};
    ULONGLONG g_lastTouchFollowPaintMs{};
    ULONGLONG g_stageLightPaintUntil{};
    ULONGLONG g_sidebarAnimationStartMs{};
    HBITMAP g_glassBackdrop{};
    HDC g_paintBufferDc{};
    HDC g_imageDrawDc{};
    HBITMAP g_paintBufferBitmap{};
    HGDIOBJ g_paintBufferOldBitmap{};
    int g_glassBackdropW{};
    int g_glassBackdropH{};
    int g_glassBackdropTheme{ -1 };
    int g_paintBufferW{};
    int g_paintBufferH{};
    int g_statusOffset{};
    int g_statusTargetOffset{};
    int g_pageWheelDeltaAccum{};
    int g_gestureLastX{};
    int g_gestureLastY{};
    int g_touchPanPendingX{};
    int g_touchPanPendingY{};
    int g_lastTouchFollowSign{};
    POINT g_touchPanPoint{};
    POINT g_touchMomentumPoint{};
    ULONGLONG g_touchMomentumLastInputMs{};
    ULONGLONG g_touchMomentumLastTickMs{};
    double g_touchMomentumX{};
    double g_touchMomentumY{};
    double g_touchMomentumRemainderX{};
    double g_touchMomentumRemainderY{};
    double g_touchMomentumBoost{};
    double g_stageWheelRemainder{};
    double g_stageHWheelRemainder{};
    double g_sidebarWheelRemainder{};
    double g_autoScrollXSpeed{};
    double g_autoScrollYSpeed{};
    double g_autoScrollXAccum{};
    double g_autoScrollYAccum{};
    ULONGLONG g_autoScrollLastTick{};
    POINT g_autoScrollAnchor{};
    bool g_autoScrollActive{};
    ULONG_PTR g_gdiplusToken{};
    std::wstring g_hoverTip;
    std::wstring g_searchQuery;
    std::wstring g_searchLastQuery;
    std::wstring g_pageInputText;
    std::wstring g_homeSearchQuery;
    std::vector<SearchResult> g_searchResults;
    std::set<int> g_collapsedOutlineRows;
    int g_mainScrollDragOffset{};
    int g_mainHScrollDragOffset{};
    int g_mainScrollDragStartY{};
    int g_mainScrollDragStartPos{};
    int g_mainHScrollDragStartX{};
    int g_mainHScrollDragStartPos{};
    int g_mainScrollThumbLiveTop{};
    int g_mainScrollThumbLiveHeight{};
    int g_homeScrollDragOffset{};
    int g_sidebarScrollDragOffset{};
    bool g_selectingContent{};
    bool g_hasContentSelection{};
    bool g_selectionIsImage{};
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
    bool g_homeListView{};
    bool g_homeSearchFocused{};
    bool g_homeSearchCaretVisible{ true };
    bool g_confirmClearRecent{};
    RECT g_clearDialogConfirmRect{};
    RECT g_clearDialogCancelRect{};
    RECT g_clearDialogCloseRect{};
    RECT g_zoomDialogRect{};
    RECT g_zoomDialogInputRect{};
    RECT g_zoomDialogConfirmRect{};
    RECT g_zoomDialogCancelRect{};
    constexpr int TileSize = 1024;
    constexpr double TileRenderZoomThreshold = 1.60;
    bool g_customZoomOpen{};
    bool g_customZoomFocused{};
    bool g_customZoomCaretVisible{ true };
    std::wstring g_customZoomText;
    struct ThemedMenuItem
    {
        std::wstring text;
        UINT id{};
        bool separator{};
        bool checked{};
        bool disabled{};
    };
    std::vector<std::unique_ptr<ThemedMenuItem>> g_themedMenuItems;
    HWND g_themedMenuPopup{};
    HWND g_themedMenuOwner{};
    std::vector<RECT> g_themedMenuItemRects;
    int g_themedMenuHover{ -1 };
    int g_themedMenuPressed{ -1 };
    Theme g_themes[] = {
        { L"\u4e91\u6bcd\u767d", RGB(244,247,250), RGB(255,255,255), RGB(250,252,254), RGB(238,243,247), RGB(255,255,255), RGB(31,35,40), RGB(105,113,125), RGB(210,217,226), RGB(59,170,124) },
        { L"\u62a4\u773c\u7c73\u767d", RGB(236,231,216), RGB(245,240,221), RGB(255,252,240), RGB(225,217,196), RGB(255,249,233), RGB(45,39,29), RGB(124,113,95), RGB(202,191,166), RGB(124,143,66) },
        { L"\u6781\u591c\u9ed1", RGB(12,14,20), RGB(25,29,38), RGB(38,44,56), RGB(8,10,15), RGB(30,34,43), RGB(243,246,251), RGB(169,178,191), RGB(58,65,78), RGB(126,162,255) },
        { L"\u77f3\u58a8\u9ed1", RGB(29,30,32), RGB(48,49,52), RGB(59,60,64), RGB(20,20,21), RGB(43,42,39), RGB(244,240,232), RGB(184,177,168), RGB(78,79,84), RGB(240,189,103) },
        { L"\u6781\u5149\u7eff", RGB(6,49,45), RGB(6,49,45), RGB(9,58,52), RGB(6,49,45), RGB(6,49,45), RGB(231,255,244), RGB(142,214,196), RGB(36,100,90), RGB(45,245,143) }
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

    bool IsBeigeTheme()
    {
        return g_theme == 1;
    }

    bool SidebarVisible()
    {
        return g_sidebarOpen && (!g_immersive || g_immersiveSidebarOpen);
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
        if (IsAuroraTheme()) return MixColor(T().panel, T().accent, 0.055);
        if (IsGlassTheme()) return MixColor(T().panel2, T().accent, 0.08);
        if (IsBeigeTheme()) return MixColor(RGB(246, 241, 231), RGB(0, 0, 0), 0.03);
        if (IsDarkTheme()) return MixColor(T().panel2, RGB(255, 255, 255), 0.04);
        return MixColor(T().panel2, T().accent, 0.10);
    }

    COLORREF SidebarSelectedFill()
    {
        if (IsAuroraTheme()) return MixColor(T().panel, T().accent, 0.075);
        if (IsGlassTheme()) return MixColor(T().accent, RGB(255, 255, 255), 0.10);
        if (IsBeigeTheme()) return T().panel2;
        if (IsDarkTheme()) return MixColor(T().accent, T().panel2, 0.18);
        return MixColor(T().accent, T().paper, 0.12);
    }

    COLORREF SidebarSelectedBorder()
    {
        if (IsAuroraTheme()) return MixColor(T().panel, T().accent, 0.055);
        if (IsDarkTheme() && !IsAuroraTheme()) return MixColor(T().accent, T().panel2, 0.25);
        if (IsBeigeTheme()) return T().line;
        return SidebarSelectedFill();
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
        if (IsBeigeTheme()) return hovered ? SidebarSelectedFill() : MixColor(RGB(251, 247, 239), RGB(0, 0, 0), 0.03);
        if (IsDarkTheme()) return hovered ? MixColor(T().panel2, T().text, 0.18) : MixColor(T().panel2, T().line, 0.12);
        return hovered ? MixColor(T().panel2, T().text, 0.12) : MixColor(T().panel2, T().line, 0.08);
    }

    COLORREF SidebarToggleLine(bool hovered)
    {
        if (IsAuroraTheme()) return hovered ? MixColor(T().accent, RGB(255, 255, 255), 0.18) : MixColor(T().accent, T().text, 0.20);
        if (IsBeigeTheme()) return hovered ? RGB(111, 155, 255) : RGB(111, 106, 99);
        return hovered ? MixColor(T().text, T().muted, 0.22) : MixColor(T().muted, T().text, 0.16);
    }

    COLORREF SidebarIconColor()
    {
        if (IsAuroraTheme()) return MixColor(T().muted, T().accent, 0.34);
        if (IsBeigeTheme()) return RGB(111, 106, 99);
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

    int AnnotationColorIndexFromRgb(float r, float g, float b)
    {
        int rr = std::clamp<int>(static_cast<int>(r * 255.0f + 0.5f), 0, 255);
        int gg = std::clamp<int>(static_cast<int>(g * 255.0f + 0.5f), 0, 255);
        int bb = std::clamp<int>(static_cast<int>(b * 255.0f + 0.5f), 0, 255);
        int best = 0;
        int bestDistance = INT_MAX;
        for (int i = 0; i < static_cast<int>(std::size(AnnotationPalette)); ++i)
        {
            COLORREF c = AnnotationPalette[i];
            int dr = rr - GetRValue(c);
            int dg = gg - GetGValue(c);
            int db = bb - GetBValue(c);
            int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
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

    COLORREF ChromeBarColor()
    {
        if (IsAuroraTheme()) return RGB(6, 49, 45);
        if (IsGlassTheme()) return RGB(244, 247, 250);
        if (IsDarkTheme()) return T().panel;
        return T().panel;
    }

    COLORREF ChromeDividerColor()
    {
        if (IsAuroraTheme()) return RGB(38, 114, 101);
        return T().line;
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

    void ApplyPdfThemeFor(int theme, std::uint8_t& b, std::uint8_t& g, std::uint8_t& r)
    {
        int clampedTheme = std::clamp(theme, 0, static_cast<int>(std::size(g_themes)) - 1);
        if (clampedTheme == 0) return;
        if (clampedTheme == 2 || clampedTheme == 3 || clampedTheme == 4) return;

        int lum = Luma(r, g, b);
        COLORREF paper = g_themes[clampedTheme].paper;

        if (lum > 185)
        {
            double t = static_cast<double>(lum - 185) / 70.0;
            r = MixByte(r, static_cast<std::uint8_t>(GetRValue(paper)), t);
            g = MixByte(g, static_cast<std::uint8_t>(GetGValue(paper)), t);
            b = MixByte(b, static_cast<std::uint8_t>(GetBValue(paper)), t);
        }
    }

    void ApplyPdfTheme(std::uint8_t& b, std::uint8_t& g, std::uint8_t& r)
    {
        ApplyPdfThemeFor(g_theme, b, g, r);
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
        lf.lfQuality = CLEARTYPE_NATURAL_QUALITY;
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
                for (int y = 0; y < fullH; ++y)
                {
                    double ty = static_cast<double>(y) / static_cast<double>(fullH);
                    COLORREF color = RGB(6, 49, 45);
                    UNREFERENCED_PARAMETER(ty);
                    RECT line{ 0, y, fullW, y + 1 };
                    Fill(mem, line, color);
                }
                AlphaFill(mem, full, RGB(40, 220, 145), 2);
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
            Fill(dc, r, T().panel);
            AlphaFill(dc, r, RGB(42, 245, 143), std::min<BYTE>(alpha / 22, 8));
            HPEN highlight = CreatePen(PS_SOLID, std::max(1, S(1)), ChromeDividerColor());
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
            Fill(dc, r, ChromeBarColor());
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
        Fill(dc, r, IsAuroraTheme() ? ChromeDividerColor() : (IsGlassTheme() ? RGB(211, 218, 226) : T().line));
    }

    RECT HomeContentViewportRect(RECT client);

    void UpdateMainScrollRects(RECT client)
    {
        g_mainScrollTrackRect = {};
        g_mainScrollThumbRect = {};
        g_mainHScrollTrackRect = {};
        g_mainHScrollThumbRect = {};
        if (g_immersive || g_active < 0) return;
        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        int stageW = std::max<int>(1, g_stage.right - g_stage.left);
        int trackW = S(12);
        int gutter = S(3);
        if (g_contentHeight > stageH + 1)
        {
            g_mainScrollTrackRect = RECT{ client.right - gutter - trackW, g_stage.top + S(8), client.right - gutter, g_stage.bottom - S(8) };
            int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
            int thumbH = std::clamp(MulDiv(stageH, trackH, std::max(1, g_contentHeight)), S(28), trackH);
            if (g_draggingMainScroll && g_mainScrollThumbLive && g_mainScrollThumbLiveHeight > 0)
                thumbH = std::clamp(g_mainScrollThumbLiveHeight, S(28), trackH);
            int maxScroll = std::max<int>(1, g_contentHeight - stageH);
            int thumbTravel = std::max<int>(0, trackH - thumbH);
            int thumbTop = g_mainScrollTrackRect.top + MulDiv(g_scrollY, thumbTravel, maxScroll);
            if (g_draggingMainScroll && g_mainScrollThumbLive)
                thumbTop = std::clamp<int>(g_mainScrollThumbLiveTop, g_mainScrollTrackRect.top, g_mainScrollTrackRect.bottom - thumbH);
            else if (g_scrollY >= maxScroll)
                thumbTop = g_mainScrollTrackRect.bottom - thumbH;
            g_mainScrollThumbRect = RECT{ g_mainScrollTrackRect.left, thumbTop, g_mainScrollTrackRect.right, thumbTop + thumbH };
        }
        if (g_contentWidth > stageW + 1)
        {
            int trackH = S(12);
            int rightInset = IsRectEmpty(&g_mainScrollTrackRect) ? S(16) : S(22);
            g_mainHScrollTrackRect = RECT{ g_stage.left + S(14), g_stage.bottom - gutter - trackH, g_stage.right - rightInset, g_stage.bottom - gutter };
            int trackLen = std::max<int>(1, g_mainHScrollTrackRect.right - g_mainHScrollTrackRect.left);
            int thumbW = std::clamp(MulDiv(stageW, trackLen, std::max(1, g_contentWidth)), S(40), trackLen);
            int maxScroll = std::max<int>(1, g_contentWidth - stageW);
            int thumbTravel = std::max<int>(0, trackLen - thumbW);
            int thumbLeft = g_mainHScrollTrackRect.left + MulDiv(g_scrollX, thumbTravel, maxScroll);
            if (g_scrollX >= maxScroll) thumbLeft = g_mainHScrollTrackRect.right - thumbW;
            g_mainHScrollThumbRect = RECT{ thumbLeft, g_mainHScrollTrackRect.top, thumbLeft + thumbW, g_mainHScrollTrackRect.bottom };
        }
    }

    void RedrawMainScrollBarNow(HWND hwnd)
    {
        RECT redraw = g_mainScrollTrackRect;
        if (!IsRectEmpty(&g_mainHScrollTrackRect))
        {
            RECT merged{};
            UnionRect(&merged, &redraw, &g_mainHScrollTrackRect);
            redraw = merged;
        }
        if (!IsRectEmpty(&redraw))
            RedrawWindow(hwnd, &redraw, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    void UpdateMainScrollDragStateFromPointer(HWND hwnd, int pointerY, bool redrawNow)
    {
        if (!g_draggingMainScroll || g_active < 0) return;

        RECT client{};
        GetClientRect(hwnd, &client);
        UpdateMainScrollRects(client);
        if (IsRectEmpty(&g_mainScrollTrackRect)) return;

        int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
        int thumbH = g_mainScrollThumbLiveHeight > 0 ? g_mainScrollThumbLiveHeight : std::max<int>(1, g_mainScrollThumbRect.bottom - g_mainScrollThumbRect.top);
        thumbH = std::clamp<int>(thumbH, S(28), trackH);

        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        int maxScroll = std::max<int>(0, g_contentHeight - stageH);
        int travel = std::max<int>(1, trackH - thumbH);
        int dragDelta = pointerY - g_mainScrollDragStartY;
        int newScroll = g_mainScrollDragStartPos;
        if (travel > 0 && maxScroll > 0)
            newScroll = g_mainScrollDragStartPos + MulDiv(dragDelta, maxScroll, travel);

        int liveTop = std::clamp<int>(pointerY - g_mainScrollDragOffset, g_mainScrollTrackRect.top, g_mainScrollTrackRect.bottom - thumbH);
        bool liveChanged = !g_mainScrollThumbLive || g_mainScrollThumbLiveTop != liveTop;
        g_mainScrollThumbLive = true;
        g_mainScrollThumbLiveTop = liveTop;
        g_mainScrollThumbLiveHeight = thumbH;
        g_dragScrollTarget = std::clamp<int>(newScroll, 0, maxScroll);

        if (redrawNow && liveChanged)
            RedrawMainScrollBarNow(hwnd);
    }

    void PaintMainScrollBar(HDC dc, RECT client, RECT paintClip)
    {
        UpdateMainScrollRects(client);
        if (!IsRectEmpty(&g_mainScrollTrackRect) && (Intersects(paintClip, g_mainScrollTrackRect) || Intersects(paintClip, g_mainScrollThumbRect)))
        {
            bool active = g_draggingMainScroll || Hit(g_mainScrollTrackRect, g_mousePos.x, g_mousePos.y);
            int centerX = (g_mainScrollTrackRect.left + g_mainScrollTrackRect.right) / 2;
            int trackW = active ? S(7) : std::max(2, S(3));
            RECT visualThumb{ centerX - trackW / 2, g_mainScrollThumbRect.top, centerX + (trackW + 1) / 2, g_mainScrollThumbRect.bottom };
            RoundFill(dc, visualThumb, S(5), ScrollThumbColor(active), ScrollThumbColor(active));
        }
        if (!IsRectEmpty(&g_mainHScrollTrackRect) && (Intersects(paintClip, g_mainHScrollTrackRect) || Intersects(paintClip, g_mainHScrollThumbRect)))
        {
            bool hActive = g_draggingMainHScroll || Hit(g_mainHScrollTrackRect, g_mousePos.x, g_mousePos.y);
            int centerY = (g_mainHScrollTrackRect.top + g_mainHScrollTrackRect.bottom) / 2;
            int h = hActive ? S(7) : std::max(2, S(3));
            RECT visualH{ g_mainHScrollThumbRect.left, centerY - h / 2, g_mainHScrollThumbRect.right, centerY + (h + 1) / 2 };
            RoundFill(dc, visualH, S(5), ScrollThumbColor(hActive), ScrollThumbColor(hActive));
        }
    }

    void UpdateHomeScrollRects(RECT client)
    {
        g_homeScrollTrackRect = {};
        g_homeScrollThumbRect = {};
        if (g_immersive || g_active >= 0) return;
        RECT viewport = HomeContentViewportRect(client);
        int visible = std::max<int>(1, viewport.bottom - viewport.top);
        if (g_homeContentHeight <= visible + 1) return;
        int trackW = S(12);
        int gutter = S(3);
        g_homeScrollTrackRect = RECT{ client.right - gutter - trackW, viewport.top + S(8), client.right - gutter, viewport.bottom - S(8) };
        int trackH = std::max<int>(1, g_homeScrollTrackRect.bottom - g_homeScrollTrackRect.top);
        int thumbH = std::clamp(MulDiv(visible, trackH, std::max(1, g_homeContentHeight)), S(28), trackH);
        int maxScroll = std::max<int>(1, g_homeContentHeight - visible);
        int thumbTravel = std::max<int>(0, trackH - thumbH);
        int thumbTop = g_homeScrollTrackRect.top + MulDiv(g_homeScrollY, thumbTravel, maxScroll);
        if (g_homeScrollY >= maxScroll) thumbTop = g_homeScrollTrackRect.bottom - thumbH;
        g_homeScrollThumbRect = RECT{ g_homeScrollTrackRect.left, thumbTop, g_homeScrollTrackRect.right, thumbTop + thumbH };
    }

    void PaintHomeScrollBar(HDC dc, RECT client, RECT paintClip)
    {
        UpdateHomeScrollRects(client);
        if (!IsRectEmpty(&g_homeScrollTrackRect) && (Intersects(paintClip, g_homeScrollTrackRect) || Intersects(paintClip, g_homeScrollThumbRect)))
        {
            bool active = g_draggingHomeScroll || Hit(g_homeScrollTrackRect, g_mousePos.x, g_mousePos.y);
            int centerX = (g_homeScrollTrackRect.left + g_homeScrollTrackRect.right) / 2;
            int trackW = active ? S(7) : std::max(2, S(3));
            RECT visualThumb{ centerX - trackW / 2, g_homeScrollThumbRect.top, centerX + (trackW + 1) / 2, g_homeScrollThumbRect.bottom };
            RoundFill(dc, visualThumb, S(5), ScrollThumbColor(active), ScrollThumbColor(active));
        }
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
                std::wstring label = tab.outline.empty() ? (L"\u7b2c" + std::to_wstring(i + 1) + L" \u9875") : tab.outline[static_cast<size_t>(i)].title;
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

    std::wstring FormatDate(long long ms)
    {
        std::time_t seconds = static_cast<std::time_t>(ms / 1000);
        std::tm local{};
        localtime_s(&local, &seconds);
        wchar_t buffer[24]{};
        wcsftime(buffer, std::size(buffer), L"%Y-%m-%d", &local);
        return buffer;
    }

    std::wstring FormatFileSize(std::wstring const& path)
    {
        try
        {
            auto bytes = std::filesystem::file_size(path);
            double value = static_cast<double>(bytes);
            const wchar_t* units[] = { L"B", L"K", L"M", L"G" };
            int unit = 0;
            while (value >= 1024.0 && unit < 3)
            {
                value /= 1024.0;
                ++unit;
            }
            wchar_t buffer[32]{};
            if (unit == 0) swprintf_s(buffer, L"%llu%s", static_cast<unsigned long long>(bytes), units[unit]);
            else swprintf_s(buffer, L"%.2f%s", value, units[unit]);
            return buffer;
        }
        catch (...)
        {
            return L"-";
        }
    }

    void DrawStarIcon(HDC dc, RECT r, COLORREF color, bool filled)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        auto rr = Rf(r);
        float cx = rr.X + rr.Width * 0.5f;
        float cy = rr.Y + rr.Height * 0.52f;
        float outer = std::min(rr.Width, rr.Height) * 0.44f;
        float inner = outer * 0.48f;
        Gdiplus::GraphicsPath path;
        Gdiplus::PointF points[10]{};
        for (int i = 0; i < 10; ++i)
        {
            double angle = -3.14159265358979323846 / 2.0 + i * 3.14159265358979323846 / 5.0;
            float radius = (i % 2 == 0) ? outer : inner;
            points[i] = Gdiplus::PointF(
                cx + static_cast<float>(std::cos(angle) * radius),
                cy + static_cast<float>(std::sin(angle) * radius));
        }
        path.AddLines(points, 10);
        path.CloseFigure();
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.25f, S(1.1f)));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(GdiColor(color));
        if (filled) g.FillPath(&brush, &path);
        g.DrawPath(&pen, &path);
    }

    void DrawHomeIcon(HDC dc, RECT r, HomeAction action, COLORREF color, bool active = false)
    {
        if (action == HomeAction::Favorite)
        {
            DrawStarIcon(dc, r, active ? RGB(246, 190, 58) : color, active);
            return;
        }

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.65f, S(1.35f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(GdiColor(color));
        auto rr = Rf(r);
        float x = rr.X;
        float y = rr.Y;
        float w = rr.Width;
        float h = rr.Height;

        if (action == HomeAction::Pin)
        {
            Gdiplus::GraphicsPath path;
            Gdiplus::PointF points[] = {
                { x + w * 0.32f, y + h * 0.12f },
                { x + w * 0.68f, y + h * 0.12f },
                { x + w * 0.60f, y + h * 0.36f },
                { x + w * 0.78f, y + h * 0.56f },
                { x + w * 0.58f, y + h * 0.56f },
                { x + w * 0.52f, y + h * 0.88f },
                { x + w * 0.48f, y + h * 0.88f },
                { x + w * 0.42f, y + h * 0.56f },
                { x + w * 0.22f, y + h * 0.56f },
                { x + w * 0.40f, y + h * 0.36f }
            };
            path.AddLines(points, static_cast<INT>(std::size(points)));
            path.CloseFigure();
            if (active) g.FillPath(&brush, &path);
            g.DrawPath(&pen, &path);
        }
        else if (action == HomeAction::Folder)
        {
            Gdiplus::RectF body(x + w * 0.13f, y + h * 0.34f, w * 0.74f, h * 0.48f);
            Gdiplus::GraphicsPath path;
            path.AddLine(x + w * 0.14f, y + h * 0.34f, x + w * 0.37f, y + h * 0.34f);
            path.AddLine(x + w * 0.43f, y + h * 0.25f, x + w * 0.58f, y + h * 0.25f);
            path.AddLine(x + w * 0.65f, y + h * 0.34f, x + w * 0.86f, y + h * 0.34f);
            path.AddLine(x + w * 0.86f, y + h * 0.82f, x + w * 0.14f, y + h * 0.82f);
            path.CloseFigure();
            g.DrawPath(&pen, &path);
            if (active) g.FillRectangle(&brush, body);
        }
        else if (action == HomeAction::Remove)
        {
            float d = std::min(w, h) * 0.78f;
            Gdiplus::RectF dot(x + (w - d) * 0.5f, y + (h - d) * 0.5f, d, d);
            Gdiplus::SolidBrush red(GdiColor(color));
            Gdiplus::Pen white(GdiColor(RGB(255, 255, 255)), std::max<Gdiplus::REAL>(1.7f, S(1.55f)));
            white.SetStartCap(Gdiplus::LineCapRound);
            white.SetEndCap(Gdiplus::LineCapRound);
            g.FillEllipse(&red, dot);
            g.DrawLine(&white, dot.X + dot.Width * 0.28f, dot.Y + dot.Height * 0.50f, dot.X + dot.Width * 0.72f, dot.Y + dot.Height * 0.50f);
        }
        else if (action == HomeAction::ClearAll)
        {
            g.DrawLine(&pen, x + w * 0.30f, y + h * 0.28f, x + w * 0.72f, y + h * 0.28f);
            g.DrawLine(&pen, x + w * 0.43f, y + h * 0.20f, x + w * 0.58f, y + h * 0.20f);
            g.DrawRectangle(&pen, x + w * 0.34f, y + h * 0.36f, w * 0.34f, h * 0.42f);
            g.DrawLine(&pen, x + w * 0.45f, y + h * 0.44f, x + w * 0.45f, y + h * 0.70f);
            g.DrawLine(&pen, x + w * 0.57f, y + h * 0.44f, x + w * 0.57f, y + h * 0.70f);
        }
        else if (action == HomeAction::ToggleView)
        {
            if (g_homeListView)
            {
                for (int row = 0; row < 3; ++row)
                {
                    float yy = y + h * (0.24f + row * 0.25f);
                    g.DrawLine(&pen, x + w * 0.22f, yy, x + w * 0.30f, yy);
                    g.DrawLine(&pen, x + w * 0.42f, yy, x + w * 0.82f, yy);
                }
            }
            else
            {
                for (int yy = 0; yy < 2; ++yy)
                    for (int xx = 0; xx < 2; ++xx)
                        g.DrawRectangle(&pen, x + w * (0.22f + xx * 0.32f), y + h * (0.22f + yy * 0.32f), w * 0.18f, h * 0.18f);
            }
        }
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
        bool hovered = g_hasHoverButton && EqualRect(&g_hoverButtonRect, &b.rect);
        bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &b.rect);
        if (IsAuroraTheme())
        {
            BYTE alpha = pressed ? 188 : (hovered ? 136 : 0);
            if (alpha > 0)
            {
                AlphaFill(dc, b.rect, RGB(10, 42, 46), alpha);
                RoundFill(dc, b.rect, S(8),
                    pressed ? RGB(12, 56, 58) : RGB(9, 42, 46),
                    pressed ? RGB(38, 132, 107) : RGB(28, 78, 76));
            }
        }
        else if (IsGlassTheme())
        {
            BYTE alpha = pressed ? 180 : (hovered ? 128 : 0);
            if (alpha > 0)
            {
                AlphaFill(dc, b.rect, RGB(255, 255, 255), alpha);
                RoundFill(dc, b.rect, S(8),
                    pressed ? RGB(248, 251, 254) : RGB(255, 255, 255),
                    pressed ? RGB(208, 217, 226) : RGB(228, 234, 241));
            }
        }
        else if (pressed || hovered)
        {
            COLORREF fill = pressed
                ? MixColor(T().panel2, T().accent, IsDarkTheme() ? 0.26 : 0.12)
                : MixColor(T().panel2, T().paper, IsDarkTheme() ? 0.18 : 0.50);
            COLORREF line = pressed
                ? MixColor(T().line, T().accent, 0.36)
                : MixColor(T().line, T().text, 0.08);
            RoundFill(dc, b.rect, S(8), fill, line);
        }
        Text(dc, b.text, b.rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, pressed ? T().text : MixColor(T().text, T().muted, 0.08));
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

    void DrawTabArrowGlyph(HDC dc, RECT r, bool right, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.4f, S(1.15f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float dir = right ? 1.0f : -1.0f;
        float dx = S(3.9f);
        float dy = S(4.8f);
        g.DrawLine(&pen, cx - dir * dx, cy - dy, cx + dir * dx, cy);
        g.DrawLine(&pen, cx + dir * dx, cy, cx - dir * dx, cy + dy);
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

    void DrawNoteToolIcon(HDC dc, RECT r, int id, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.15f, S(1.0f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(GdiColor(color));

        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float box = std::min<float>(static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
        float u = box / 28.0f;

        auto drawTree = [&](bool expand)
        {
            Gdiplus::RectF mark{ cx - 8.5f * u, cy - 8.5f * u, 17.0f * u, 17.0f * u };
            DrawGdiRoundRect(g, pen, mark, 3.0f * u);

            float dir = expand ? 1.0f : -1.0f;
            float topY = cy - 4.5f * u * dir;
            float midY = cy;
            float bottomY = cy + 4.5f * u * dir;
            float wing = 4.2f * u;

            g.DrawLine(&pen, cx - wing, topY, cx, midY);
            g.DrawLine(&pen, cx + wing, topY, cx, midY);
            g.DrawLine(&pen, cx - wing, midY, cx, bottomY);
            g.DrawLine(&pen, cx + wing, midY, cx, bottomY);
        };
        auto drawArrowBox = [&](bool next)
        {
            Gdiplus::RectF mark{ cx - 8.5f * u, cy - 8.5f * u, 17.0f * u, 17.0f * u };
            DrawGdiRoundRect(g, pen, mark, 3.0f * u);
            float dir = next ? 1.0f : -1.0f;
            float ax = cx + dir * 4.0f * u;
            float bx = cx - dir * 3.5f * u;
            g.DrawLine(&pen, ax, cy, bx, cy - 5.0f * u);
            g.DrawLine(&pen, ax, cy, bx, cy + 5.0f * u);
            g.DrawLine(&pen, ax, cy, cx - dir * 5.0f * u, cy);
        };

        auto drawTrash = [&](bool badge)
        {
            float x = cx - 6.8f * u;
            float y = cy - 5.2f * u;
            Gdiplus::RectF body{ x, y, 13.6f * u, 13.0f * u };
            DrawGdiRoundRect(g, pen, body, 2.3f * u);
            g.DrawLine(&pen, x - 1.4f * u, y - 3.0f * u, x + 15.0f * u, y - 3.0f * u);
            g.DrawLine(&pen, cx - 3.2f * u, y - 5.8f * u, cx + 3.2f * u, y - 5.8f * u);
            g.DrawLine(&pen, cx - 2.8f * u, y - 5.8f * u, cx - 4.4f * u, y - 3.0f * u);
            g.DrawLine(&pen, cx + 2.8f * u, y - 5.8f * u, cx + 4.4f * u, y - 3.0f * u);
            g.DrawLine(&pen, cx - 3.4f * u, y + 2.0f * u, cx - 3.4f * u, y + 9.2f * u);
            g.DrawLine(&pen, cx, y + 2.0f * u, cx, y + 9.2f * u);
            g.DrawLine(&pen, cx + 3.4f * u, y + 2.0f * u, cx + 3.4f * u, y + 9.2f * u);

            if (badge)
            {
                Gdiplus::SolidBrush red(GdiColor(RGB(220, 77, 77)));
                Gdiplus::Pen white(GdiColor(RGB(255, 255, 255)), std::max<Gdiplus::REAL>(1.1f, S(0.9f)));
                white.SetStartCap(Gdiplus::LineCapRound);
                white.SetEndCap(Gdiplus::LineCapRound);
                Gdiplus::RectF dot{ cx + 3.3f * u, cy - 10.4f * u, 8.4f * u, 8.4f * u };
                g.FillEllipse(&red, dot);
                g.DrawLine(&white, dot.X + 2.5f * u, dot.Y + 2.5f * u, dot.X + 5.9f * u, dot.Y + 5.9f * u);
                g.DrawLine(&white, dot.X + 5.9f * u, dot.Y + 2.5f * u, dot.X + 2.5f * u, dot.Y + 5.9f * u);
            }
        };

        if (id == CmdNoteExpandAll) drawTree(true);
        else if (id == CmdNoteCollapseAll) drawTree(false);
        else if (id == CmdNotePrev) drawArrowBox(false);
        else if (id == CmdNoteNext) drawArrowBox(true);
        else if (id == CmdNoteDelete) drawTrash(false);
        else if (id == CmdNoteDeleteAll) drawTrash(true);
    }

    void DrawFilledCircle(HDC dc, int cx, int cy, float radius, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush brush(GdiColor(color));
        float d = radius * 2.0f;
        g.FillEllipse(&brush, static_cast<float>(cx) - radius, static_cast<float>(cy) - radius, d, d);
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
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.22f, S(0.92f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float d = S(3.45f);
        g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
        g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d);
    }

    void DrawPlusGlyph(HDC dc, RECT r, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.6f, S(1.25f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        float d = S(5.4f);
        g.DrawLine(&pen, cx - d, cy, cx + d, cy);
        g.DrawLine(&pen, cx, cy - d, cx, cy + d);
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

    void SaveActiveTabProgress();
    void RestoreActiveTabPosition(HWND hwnd);
    void ActivateTab(HWND hwnd, int index);

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

    std::vector<std::string> SplitTabLine(std::string const& text)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;)
        {
            size_t tab = text.find('\t', start);
            if (tab == std::string::npos)
            {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, tab - start));
            start = tab + 1;
        }
        return parts;
    }

    void SortRecent()
    {
        std::stable_sort(g_recent.begin(), g_recent.end(), [](RecentItem const& a, RecentItem const& b)
        {
            if (a.pinned != b.pinned) return a.pinned;
            if (a.pinned && b.pinned && a.pinnedAt != b.pinnedAt) return a.pinnedAt > b.pinnedAt;
            return a.openedAt > b.openedAt;
        });
    }

    bool HomeRecentMatches(RecentItem const& item)
    {
        if (g_homeSearchQuery.empty()) return true;
        std::wstring query = LowerCopy(g_homeSearchQuery);
        std::wstring name = LowerCopy(item.name);
        std::wstring path = LowerCopy(item.path);
        return name.find(query) != std::wstring::npos || path.find(query) != std::wstring::npos;
    }

    std::vector<int> FilteredHomeRecentIds()
    {
        std::vector<int> ids;
        ids.reserve(g_recent.size());
        for (int i = 0; i < static_cast<int>(g_recent.size()); ++i)
        {
            if (HomeRecentMatches(g_recent[static_cast<size_t>(i)])) ids.push_back(i);
        }
        return ids;
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
                case 1: g_theme = 2; break; // legacy dark mica -> night black
                case 2: g_theme = 4; break; // legacy aurora -> aurora green
                case 3: g_theme = 3; break; // legacy graphite -> graphite black
                case 4: g_theme = 1; break; // 护眼纸张 -> 护眼米白
                case 5: g_theme = 0; break; // legacy frosted mica -> mica white
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
            else if (line.rfind("homeList=", 0) == 0) g_homeListView = std::atoi(line.c_str() + 9) != 0;
            else if (line.rfind("continuous=", 0) == 0) g_continuousRead = std::atoi(line.c_str() + 11) != 0;
            else if (line.rfind("statusPinned=", 0) == 0) g_statusPinned = std::atoi(line.c_str() + 13) != 0;
            else if (line.rfind("lastSessionActive=", 0) == 0) g_lastSessionActive = std::atoi(line.c_str() + 18);
            else if (line.rfind("lastSessionTab=", 0) == 0)
            {
                auto parts = SplitTabLine(line.substr(15));
                if (parts.size() >= 4)
                {
                    LastSessionItem item;
                    item.page = std::max(1, std::atoi(parts[0].c_str()));
                    item.scrollY = std::max(0, std::atoi(parts[1].c_str()));
                    item.zoom = std::clamp(std::atof(parts[2].c_str()), 0.0833, 64.0);
                    item.path = FromUtf8(parts[3]);
                    if (!item.path.empty()) g_lastSessionTabs.push_back(item);
                }
            }
            else if (line.rfind("recent=", 0) == 0)
            {
                auto parts = SplitTabLine(line.substr(7));
                if (parts.size() >= 3)
                {
                    RecentItem item;
                    item.openedAt = std::atoll(parts[0].c_str());
                    item.page = std::atoi(parts[1].c_str());
                    item.path = FromUtf8(parts[2]);
                    item.name = std::filesystem::path(item.path).filename().wstring();
                    if (parts.size() >= 4) item.favorite = std::atoi(parts[3].c_str()) != 0;
                    if (parts.size() >= 5) item.pinned = std::atoi(parts[4].c_str()) != 0;
                    if (parts.size() >= 6) item.pinnedAt = std::atoll(parts[5].c_str());
                    g_recent.push_back(item);
                }
            }
        }
        SortRecent();
        if (g_recent.size() > 80) g_recent.resize(80);
        if (g_lastSessionActive < 0 || g_lastSessionActive >= static_cast<int>(g_lastSessionTabs.size()))
            g_lastSessionActive = g_lastSessionTabs.empty() ? -1 : 0;
    }

    void SaveState()
    {
        std::ofstream out(StatePath(), std::ios::binary | std::ios::trunc);
        out << "themeSchema=2\n";
        out << "theme=" << g_theme << "\n";
        out << "sidebar=" << std::max(g_sidebarExpandedWidth, g_sidebarWidth) << "\n";
        out << "outline=" << g_outlineFont << "\n";
        out << "view=" << g_viewMode << "\n";
        out << "homeList=" << (g_homeListView ? 1 : 0) << "\n";
        out << "continuous=" << (g_continuousRead ? 1 : 0) << "\n";
        out << "statusPinned=" << (g_statusPinned ? 1 : 0) << "\n";
        out << "lastSessionActive=" << g_lastSessionActive << "\n";
        for (auto const& item : g_lastSessionTabs)
        {
            out << "lastSessionTab=" << item.page << '\t' << item.scrollY << '\t' << item.zoom << '\t' << ToUtf8(item.path) << "\n";
        }
        for (auto const& item : g_recent)
        {
            out << "recent=" << item.openedAt << '\t' << item.page << '\t' << ToUtf8(item.path) << '\t'
                << (item.favorite ? 1 : 0) << '\t' << (item.pinned ? 1 : 0) << '\t' << item.pinnedAt << "\n";
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
        COLORREF caption = ChromeBarColor();
        COLORREF captionText = T().text;
        COLORREF border = ChromeDividerColor();
        DwmSetWindowAttribute(g_hwnd, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(g_hwnd, 36, &captionText, sizeof(captionText));
        DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof(border));
    }

    void UpdateRecent(const PdfTab& tab)
    {
        if (tab.path.empty()) return;
        RecentItem item{ tab.path, tab.name, tab.currentPage + 1, NowMs() };
        auto it = std::find_if(g_recent.begin(), g_recent.end(), [&](auto const& i) { return i.path == tab.path; });
        if (it != g_recent.end())
        {
            item.favorite = it->favorite;
            item.pinned = it->pinned;
            item.pinnedAt = it->pinnedAt;
            g_recent.erase(it);
        }
        g_recent.insert(g_recent.begin(), item);
        SortRecent();
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
            SortRecent();
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

    int FacingPageGap()
    {
        return std::max(4, S(8));
    }

    int FirstPageInSpread(int page)
    {
        if (g_viewMode == 1) return (std::max(0, page) / 2) * 2;
        if (g_viewMode == 2)
        {
            if (page <= 0) return 0;
            return 1 + ((page - 1) / 2) * 2;
        }
        return std::max(0, page);
    }

    int NextSpreadStart(PdfTab const& tab, int start)
    {
        if (g_viewMode == 0) return start + 1;
        if (g_viewMode == 2 && start == 0) return std::min(1, tab.pageCount);
        return start + 2;
    }

    int SpreadPageCount(PdfTab const& tab, int start)
    {
        if (start < 0 || start >= tab.pageCount) return 0;
        if (g_viewMode == 0) return 1;
        if (g_viewMode == 2 && start == 0) return 1;
        return std::min(2, tab.pageCount - start);
    }

    int SpreadWidth(PdfTab const& tab, int start)
    {
        int count = SpreadPageCount(tab, start);
        if (count <= 0) return 0;
        int width = 0;
        for (int i = 0; i < count; ++i)
        {
            if (i) width += FacingPageGap();
            width += PageW(tab, start + i);
        }
        return width;
    }

    int SpreadHeight(PdfTab const& tab, int start)
    {
        int count = SpreadPageCount(tab, start);
        int height = 0;
        for (int i = 0; i < count; ++i)
            height = std::max(height, PageH(tab, start + i));
        return height;
    }

    int MaxSpreadWidth(PdfTab const& tab)
    {
        int maxWidth = 0;
        for (int start = 0; start < tab.pageCount; start = NextSpreadStart(tab, start))
            maxWidth = std::max(maxWidth, SpreadWidth(tab, start));
        return maxWidth;
    }

    int ReadingContentWidth(PdfTab const& tab)
    {
        int stageW = std::max<int>(1, g_stage.right - g_stage.left);
        return std::max<int>(stageW, MaxSpreadWidth(tab) + S(56));
    }

    int SpreadX(PdfTab const& tab, int start)
    {
        int stageW = std::max<int>(1, g_stage.right - g_stage.left);
        int spreadW = SpreadWidth(tab, start);
        int maxSpreadW = MaxSpreadWidth(tab);
        if (maxSpreadW + S(56) <= stageW)
            return g_stage.left + (stageW - spreadW) / 2;
        return g_stage.left + S(28) + (maxSpreadW - spreadW) / 2 - g_scrollX;
    }

    int SpreadTopScroll(PdfTab const& tab, int targetPage)
    {
        targetPage = std::clamp(targetPage, 0, std::max(0, tab.pageCount - 1));
        int targetStart = FirstPageInSpread(targetPage);
        int scroll = S(28);
        for (int start = 0; start < tab.pageCount && start < targetStart; start = NextSpreadStart(tab, start))
            scroll += SpreadHeight(tab, start) + PageGap();
        return scroll;
    }

    int SpreadVisualTop(PdfTab const& tab, int start)
    {
        return g_stage.top + SpreadTopScroll(tab, start) - g_scrollY;
    }

    int FirstVisibleSpreadStart(PdfTab const& tab)
    {
        int first = 0;
        int threshold = std::max(0, g_scrollY - S(96));
        for (int start = 0; start < tab.pageCount; start = NextSpreadStart(tab, start))
        {
            int nextStart = NextSpreadStart(tab, start);
            int nextTop = nextStart < tab.pageCount
                ? SpreadTopScroll(tab, nextStart)
                : SpreadTopScroll(tab, start) + SpreadHeight(tab, start) + PageGap();
            if (nextTop >= threshold) break;
            if (nextStart >= tab.pageCount)
            {
                first = start;
                break;
            }
            first = nextStart;
        }
        return first;
    }

    bool BuildSpreadPageRect(PdfTab const& tab, int start, int y, int page, RECT& pageRect)
    {
        int count = SpreadPageCount(tab, start);
        if (count <= 0 || page < start || page >= start + count) return false;
        int x = SpreadX(tab, start);
        int spreadH = SpreadHeight(tab, start);
        for (int i = 0; i < count; ++i)
        {
            int p = start + i;
            int pageW = PageW(tab, p);
            int pageH = PageH(tab, p);
            int pageY = y + (spreadH - pageH) / 2;
            RECT r{ x, pageY, x + pageW, pageY + pageH };
            if (p == page)
            {
                pageRect = r;
                return true;
            }
            x += pageW + FacingPageGap();
        }
        return false;
    }

    int ContinuousContentHeight(PdfTab const& tab);
    int SinglePageContentHeight(PdfTab const& tab);
    int SinglePageMaxScroll(PdfTab const& tab);

    PageImage RenderPageAtScale(PdfTab& tab, int index, double renderScale, bool applyTheme, int renderTheme = -1)
    {
        fz_matrix matrix = g_pdf.scale(static_cast<float>(renderScale), static_cast<float>(renderScale));
        fz_pixmap* pixmap = nullptr;
        if (g_pdf.newPixmapFromPageNumberClip)
        {
            fz_page* page = g_pdf.loadPage(tab.ctx, tab.doc, index);
            fz_rect bounds = g_pdf.boundPage(tab.ctx, page);
            g_pdf.dropPage(tab.ctx, page);
            int widthPx = std::max(1, static_cast<int>(std::ceil((bounds.x1 - bounds.x0) * renderScale)));
            int heightPx = std::max(1, static_cast<int>(std::ceil((bounds.y1 - bounds.y0) * renderScale)));
            pixmap = g_pdf.newPixmapFromPageNumberClip(
                tab.ctx,
                tab.doc,
                index,
                matrix,
                g_pdf.deviceRgb(tab.ctx),
                0,
                0,
                0,
                widthPx,
                heightPx);
        }
        if (!pixmap)
            pixmap = g_pdf.newPixmapFromPageNumber(tab.ctx, tab.doc, index, matrix, g_pdf.deviceRgb(tab.ctx), 0);
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
                if (applyTheme) ApplyPdfThemeFor(renderTheme >= 0 ? renderTheme : g_theme, b, g, r);
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = 255;
            }
        }
        g_pdf.dropPixmap(tab.ctx, pixmap);
        return image;
    }

    PageImage RenderPageAtZoom(PdfTab& tab, int index, double zoom)
    {
        return RenderPageAtScale(tab, index, zoom * static_cast<double>(g_dpi) / 72.0, true);
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

    PageImage PixmapToImageFor(PdfTab& tab, fz_pixmap* pixmap, int renderTheme)
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
                ApplyPdfThemeFor(renderTheme, b, g, r);
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = 255;
            }
        }
        return image;
    }

    PageImage PixmapToImage(PdfTab& tab, fz_pixmap* pixmap)
    {
        return PixmapToImageFor(tab, pixmap, g_theme);
    }

    PageImage RenderPageTileAtScale(PdfTab& tab, int index, RECT tile, double renderScale, int renderTheme)
    {
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
        PageImage image = PixmapToImageFor(tab, pixmap, renderTheme);
        if (pixmap) g_pdf.dropPixmap(tab.ctx, pixmap);
        return image;
    }

    PageImage RenderPageTile(PdfTab& tab, int index, RECT tile)
    {
        double renderScale = tab.zoom * static_cast<double>(g_dpi) / 72.0;
        return RenderPageTileAtScale(tab, index, tile, renderScale, g_theme);
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
                for (auto const& entry : tab.tileCache) total += entry.second.CacheBytes();
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

    void PruneZoomFallbackCache(PdfTab& tab)
    {
        if (tab.zoomFallbackCache.empty()) return;
        if (tab.zoomFallbackDpi != g_dpi || tab.zoomFallbackTheme != g_theme || GetTickCount64() > tab.zoomFallbackUntil)
        {
            tab.zoomFallbackCache.clear();
            tab.zoomFallbackZoom = 0.0;
            tab.zoomFallbackDpi = 0;
            tab.zoomFallbackTheme = -1;
            tab.zoomFallbackUntil = 0;
        }
    }

    void PreparePageCache(PdfTab& tab)
    {
        PruneZoomFallbackCache(tab);
        if (tab.cachedZoom != tab.zoom || tab.cachedDpi != g_dpi || tab.cachedTheme != g_theme)
        {
            tab.cache.clear();
            tab.tileCache.clear();
            tab.cachedZoom = tab.zoom;
            tab.cachedDpi = g_dpi;
            tab.cachedTheme = g_theme;
        }
    }

    PageImage* FindZoomFallbackPage(PdfTab& tab, int index)
    {
        PruneZoomFallbackCache(tab);
        if (tab.zoomFallbackCache.empty() || tab.zoomFallbackDpi != g_dpi || tab.zoomFallbackTheme != g_theme) return nullptr;
        auto found = tab.zoomFallbackCache.find(index);
        return found == tab.zoomFallbackCache.end() ? nullptr : &found->second;
    }

    void TrimPageCache(PdfTab& tab, int protectedPage)
    {
        auto cacheBytes = [&]()
        {
            size_t total = 0;
            for (auto const& entry : tab.cache) total += entry.second.CacheBytes();
            return total;
        };
        size_t maxBytes = tab.zoom > 2.0 ? 10ull * 1024ull * 1024ull : 48ull * 1024ull * 1024ull;
        size_t maxPages = tab.zoom > 2.0 ? 2ull : 6ull;
        while (tab.cache.size() > maxPages || cacheBytes() > maxBytes)
        {
            auto victim = tab.cache.end();
            int worstDistance = -1;
            for (auto candidate = tab.cache.begin(); candidate != tab.cache.end(); ++candidate)
            {
                if (candidate->first == protectedPage && tab.cache.size() > 1) continue;
                int distance = std::abs(candidate->first - tab.currentPage);
                if (distance > worstDistance)
                {
                    worstDistance = distance;
                    victim = candidate;
                }
            }
            if (victim == tab.cache.end() || victim->first == protectedPage) break;
            tab.cache.erase(victim);
        }
    }

    PageImage* FindCachedPage(PdfTab& tab, int index)
    {
        PreparePageCache(tab);
        auto found = tab.cache.find(index);
        return found == tab.cache.end() ? nullptr : &found->second;
    }

    bool SamePageRenderRequest(PageRenderRequest const& a, PageRenderRequest const& b)
    {
        return a.documentVersion == b.documentVersion &&
            a.page == b.page && a.tile == b.tile && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
            a.zoom == b.zoom && a.renderZoom == b.renderZoom && a.level == b.level &&
            a.dpi == b.dpi && a.theme == b.theme &&
            _wcsicmp(a.path.c_str(), b.path.c_str()) == 0;
    }

    void PageRenderWorkerMain()
    {
        fz_context* workerCtx = nullptr;
        fz_document* workerDoc = nullptr;
        std::wstring workerPath;
        unsigned long workerVersion = 0;

        auto closeWorkerDocument = [&]()
        {
            if (workerDoc) g_pdf.dropDocument(workerCtx, workerDoc);
            workerDoc = nullptr;
            workerPath.clear();
            workerVersion = 0;
        };

        for (;;)
        {
            PageRenderRequest request;
            {
                std::unique_lock<std::mutex> lock(g_pageRenderMutex);
                g_pageRenderCv.wait(lock, [] { return g_pageRenderStop || !g_pageRenderQueue.empty(); });
                if (g_pageRenderStop) break;
                request = g_pageRenderQueue.front();
                g_pageRenderQueue.pop_front();
                g_pageRenderActiveRequest = request;
                g_pageRenderActive = true;
            }

            auto result = std::make_unique<PageRenderResult>();
            result->request = request;
            try
            {
                if (!workerCtx)
                {
                    workerCtx = g_pdf.newContext(nullptr, nullptr, 16 << 20, "1.28.0");
                    g_pdf.setAaLevel(workerCtx, 8);
                    g_pdf.registerHandlers(workerCtx);
                }
                if (!workerDoc || workerVersion != request.documentVersion || _wcsicmp(workerPath.c_str(), request.path.c_str()) != 0)
                {
                    closeWorkerDocument();
                    std::string utf8 = ToUtf8(request.path);
                    workerDoc = g_pdf.openDocument(workerCtx, utf8.c_str());
                    workerPath = request.path;
                    workerVersion = request.documentVersion;
                }

                PdfTab renderTab;
                renderTab.ctx = workerCtx;
                renderTab.doc = workerDoc;
                renderTab.zoom = request.zoom;
                double renderZoom = request.renderZoom > 0.0 ? request.renderZoom : request.zoom;
                double renderScale = renderZoom * static_cast<double>(request.dpi) / 72.0;
                if (request.tile)
                {
                    RECT tile{ request.x, request.y, request.x + request.w, request.y + request.h };
                    result->image = RenderPageTileAtScale(renderTab, request.page, tile, renderScale, request.theme);
                }
                else
                {
                    result->image = RenderPageAtScale(renderTab, request.page, renderScale, true, request.theme);
                }
            }
            catch (...)
            {
                closeWorkerDocument();
                if (workerCtx) g_pdf.dropContext(workerCtx);
                workerCtx = nullptr;
                result->image = {};
            }

            bool stale = false;
            {
                std::lock_guard<std::mutex> lock(g_pageRenderMutex);
                g_pageRenderActive = false;
                stale = g_pageRenderStop;
            }
            if (!stale && !result->image.bgra.empty() && IsWindow(request.target))
            {
                if (!PostMessageW(request.target, WM_QUIETPDF_PAGE_RENDERED, 0, reinterpret_cast<LPARAM>(result.get())))
                    continue;
                result.release();
            }
        }

        closeWorkerDocument();
        if (workerCtx) g_pdf.dropContext(workerCtx);
    }

    void EnsurePageRenderWorker()
    {
        if (g_pageRenderThread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(g_pageRenderMutex);
            g_pageRenderStop = false;
            g_pageRenderQueue.clear();
            g_pageRenderActive = false;
        }
        g_pageRenderThread = std::thread(PageRenderWorkerMain);
    }

    void StopPageRenderWorker()
    {
        {
            std::lock_guard<std::mutex> lock(g_pageRenderMutex);
            g_pageRenderStop = true;
            g_pageRenderQueue.clear();
            ++g_pageRenderSerial;
        }
        g_pageRenderCv.notify_all();
        if (g_pageRenderThread.joinable()) g_pageRenderThread.join();
    }

    void QueuePageRender(HWND hwnd, PdfTab const& tab, int page, int priority = 0)
    {
        if (page < 0 || page >= tab.pageCount || tab.zoom >= TileRenderZoomThreshold) return;
        if (tab.cachedZoom == tab.zoom && tab.cachedDpi == g_dpi && tab.cachedTheme == g_theme && tab.cache.find(page) != tab.cache.end()) return;
        EnsurePageRenderWorker();
        PageRenderRequest request;
        request.documentVersion = tab.renderVersion;
        request.target = hwnd;
        request.path = tab.path;
        request.page = page;
        request.zoom = tab.zoom;
        request.dpi = g_dpi;
        request.theme = g_theme;
        request.priority = priority;
        {
            std::lock_guard<std::mutex> lock(g_pageRenderMutex);
            if (g_pageRenderActive && SamePageRenderRequest(g_pageRenderActiveRequest, request)) return;
            for (auto const& queued : g_pageRenderQueue)
            {
                if (SamePageRenderRequest(queued, request)) return;
            }
            request.serial = ++g_pageRenderSerial;
            auto insertAt = std::find_if(
                g_pageRenderQueue.begin(),
                g_pageRenderQueue.end(),
                [&](PageRenderRequest const& queued) { return queued.priority > priority; });
            g_pageRenderQueue.insert(insertAt, request);
            constexpr size_t maxQueuedPageRenders = 6;
            while (g_pageRenderQueue.size() > maxQueuedPageRenders)
            {
                auto victim = std::find_if(
                    g_pageRenderQueue.rbegin(),
                    g_pageRenderQueue.rend(),
                    [](PageRenderRequest const& queued) { return queued.priority > 0; });
                if (victim == g_pageRenderQueue.rend()) g_pageRenderQueue.pop_back();
                else g_pageRenderQueue.erase(std::next(victim).base());
            }
        }
        g_pageRenderCv.notify_one();
    }

    int TileZoomKey(double zoom)
    {
        return static_cast<int>(zoom * 10000.0 + 0.5);
    }

    int TileLevelScale(int level)
    {
        return 1 << std::clamp(level, 0, 6);
    }

    int TileLevelForPage(PdfTab const& tab, int page, int pageW, int pageH)
    {
        UNREFERENCED_PARAMETER(page);
        int stageW = std::max<int>(1, g_stage.right - g_stage.left);
        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        int longest = std::max(pageW, pageH);
        int viewportLongest = std::max(stageW, stageH);
        int level = 0;
        while (level < 3 && longest / TileLevelScale(level) > viewportLongest * 2)
            ++level;
        if (tab.zoom < 3.0) level = std::min(level, 1);
        return level;
    }

    int TileCoordAtLevel(int targetCoord, int level)
    {
        int scale = TileLevelScale(level);
        return targetCoord >= 0 ? targetCoord / scale : 0;
    }

    RECT TileScreenRect(RECT pageRect, TileKey const& key)
    {
        int scale = TileLevelScale(key.level);
        return RECT{
            pageRect.left + key.x * scale,
            pageRect.top + key.y * scale,
            pageRect.left + (key.x + key.w) * scale,
            pageRect.top + (key.y + key.h) * scale
        };
    }

    void TrimTileCache(PdfTab& tab, TileKey const& protectedKey)
    {
        auto cacheBytes = [&]()
        {
            size_t total = 0;
            for (auto const& entry : tab.tileCache) total += entry.second.CacheBytes();
            return total;
        };

        constexpr size_t maxTileBytes = 36ull * 1024ull * 1024ull;
        constexpr size_t maxTiles = 72;
        while (tab.tileCache.size() > maxTiles || cacheBytes() > maxTileBytes)
        {
            auto victim = tab.tileCache.end();
            int worstDistance = -1;
            for (auto candidate = tab.tileCache.begin(); candidate != tab.tileCache.end(); ++candidate)
            {
                bool isProtected = !(candidate->first < protectedKey) && !(protectedKey < candidate->first);
                if (isProtected && tab.tileCache.size() > 1) continue;
                int distance = std::abs(candidate->first.page - tab.currentPage);
                if (distance > worstDistance)
                {
                    worstDistance = distance;
                    victim = candidate;
                }
            }
            if (victim == tab.tileCache.end()) break;
            bool victimProtected = !(victim->first < protectedKey) && !(protectedKey < victim->first);
            if (victimProtected) break;
            tab.tileCache.erase(victim);
        }
    }

    PageImage* FindCachedTile(PdfTab& tab, TileKey const& key)
    {
        PreparePageCache(tab);
        auto found = tab.tileCache.find(key);
        return found == tab.tileCache.end() ? nullptr : &found->second;
    }

    void QueueTileRender(HWND hwnd, PdfTab& tab, TileKey const& key, int priority = 0)
    {
        if (key.page < 0 || key.page >= tab.pageCount || key.w <= 0 || key.h <= 0) return;
        PreparePageCache(tab);
        if (tab.tileCache.find(key) != tab.tileCache.end()) return;
        EnsurePageRenderWorker();

        PageRenderRequest request;
        request.documentVersion = tab.renderVersion;
        request.target = hwnd;
        request.path = tab.path;
        request.page = key.page;
        request.tile = true;
        request.x = key.x;
        request.y = key.y;
        request.w = key.w;
        request.h = key.h;
        request.zoom = tab.zoom;
        request.renderZoom = tab.zoom / static_cast<double>(TileLevelScale(key.level));
        request.level = key.level;
        request.viewportGeneration = g_viewportGeneration;
        request.dpi = g_dpi;
        request.theme = g_theme;
        request.priority = priority;
        {
            std::lock_guard<std::mutex> lock(g_pageRenderMutex);
            g_pageRenderQueue.erase(
                std::remove_if(
                    g_pageRenderQueue.begin(),
                    g_pageRenderQueue.end(),
                    [&](PageRenderRequest const& queued)
                    {
                        return queued.tile && queued.viewportGeneration + 1 < g_viewportGeneration &&
                            queued.documentVersion == tab.renderVersion && _wcsicmp(queued.path.c_str(), tab.path.c_str()) == 0;
                    }),
                g_pageRenderQueue.end());
            if (g_pageRenderActive && SamePageRenderRequest(g_pageRenderActiveRequest, request)) return;
            for (auto const& queued : g_pageRenderQueue)
            {
                if (SamePageRenderRequest(queued, request)) return;
            }
            request.serial = ++g_pageRenderSerial;
            auto insertAt = std::find_if(
                g_pageRenderQueue.begin(),
                g_pageRenderQueue.end(),
                [&](PageRenderRequest const& queued) { return queued.priority > priority; });
            g_pageRenderQueue.insert(insertAt, request);
            constexpr size_t maxQueuedTileRenders = 48;
            while (g_pageRenderQueue.size() > maxQueuedTileRenders)
            {
                auto victim = std::find_if(
                    g_pageRenderQueue.rbegin(),
                    g_pageRenderQueue.rend(),
                    [](PageRenderRequest const& queued) { return queued.priority > 0; });
                if (victim == g_pageRenderQueue.rend()) g_pageRenderQueue.pop_back();
                else g_pageRenderQueue.erase(std::next(victim).base());
            }
        }
        g_pageRenderCv.notify_one();
    }

    void QueueVisibleTilesForPageLevel(HWND hwnd, PdfTab& tab, int page, RECT pageRect, RECT clip, int level, int priority, int& budget)
    {
        RECT drawRect{};
        if (budget <= 0 || !IntersectRect(&drawRect, &pageRect, &clip)) return;

        int pageW = std::max<int>(1, pageRect.right - pageRect.left);
        int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        int scale = TileLevelScale(level);
        int levelPageW = std::max<int>(1, (pageW + scale - 1) / scale);
        int levelPageH = std::max<int>(1, (pageH + scale - 1) / scale);
        int localLeft = std::clamp<int>(drawRect.left - pageRect.left, 0, pageW);
        int localTop = std::clamp<int>(drawRect.top - pageRect.top, 0, pageH);
        int localRight = std::clamp<int>(drawRect.right - pageRect.left, 0, pageW);
        int localBottom = std::clamp<int>(drawRect.bottom - pageRect.top, 0, pageH);
        int levelLeft = TileCoordAtLevel(localLeft, level);
        int levelTop = TileCoordAtLevel(localTop, level);
        int levelRight = std::clamp<int>((localRight + scale - 1) / scale, levelLeft + 1, levelPageW);
        int levelBottom = std::clamp<int>((localBottom + scale - 1) / scale, levelTop + 1, levelPageH);
        int tileLeft = (levelLeft / TileSize) * TileSize;
        int tileTop = (levelTop / TileSize) * TileSize;
        for (int ty = tileTop; ty < levelBottom && budget > 0; ty += TileSize)
        {
            int h = std::min(TileSize, levelPageH - ty);
            if (h <= 0) continue;
            for (int tx = tileLeft; tx < levelRight && budget > 0; tx += TileSize)
            {
                int w = std::min(TileSize, levelPageW - tx);
                if (w <= 0) continue;
                TileKey key{ page, TileZoomKey(tab.zoom), g_theme, level, tx, ty, w, h };
                if (!FindCachedTile(tab, key))
                {
                    QueueTileRender(hwnd, tab, key, priority);
                    --budget;
                }
            }
        }
    }

    void QueueVisibleTilesForPage(HWND hwnd, PdfTab& tab, int page, RECT pageRect, RECT clip, int priority, int& budget)
    {
        int targetLevel = TileLevelForPage(tab, page, std::max<int>(1, pageRect.right - pageRect.left), std::max<int>(1, pageRect.bottom - pageRect.top));
        if (targetLevel > 0)
            QueueVisibleTilesForPageLevel(hwnd, tab, page, pageRect, clip, targetLevel, priority, budget);
        QueueVisibleTilesForPageLevel(hwnd, tab, page, pageRect, clip, 0, priority + 1, budget);
    }

    void ScheduleVisibleTilePrefetch(HWND hwnd, PdfTab& tab)
    {
        if (tab.pageCount <= 0) return;
        PreparePageCache(tab);
        bool rapidScroll = g_draggingMainScroll || (g_stageLightPaintUntil != 0 && GetTickCount64() < g_stageLightPaintUntil);
        int budget = rapidScroll ? 12 : 8;
        RECT visible = g_stage;
        visible.top -= S(32);
        visible.bottom += rapidScroll ? S(260) : S(120);

        int firstVisible = g_continuousRead ? FirstVisibleSpreadStart(tab) : FirstPageInSpread(std::clamp(tab.currentPage, 0, tab.pageCount - 1));
        for (int start = firstVisible; start < tab.pageCount && budget > 0; start = NextSpreadStart(tab, start))
        {
            int y = g_continuousRead ? SpreadVisualTop(tab, start) : g_stage.top + S(28) - g_scrollY;
            int spreadH = SpreadHeight(tab, start);
            RECT spreadRect{ SpreadX(tab, start), y, SpreadX(tab, start) + SpreadWidth(tab, start), y + spreadH };
            if (g_continuousRead && spreadRect.top > visible.bottom) break;
            if (!Intersects(spreadRect, visible) && (g_continuousRead || start != firstVisible)) continue;

            int count = SpreadPageCount(tab, start);
            for (int j = 0; j < count && budget > 0; ++j)
            {
                int page = start + j;
                RECT pageRect{};
                if (BuildSpreadPageRect(tab, start, y, page, pageRect))
                    QueueVisibleTilesForPage(hwnd, tab, page, pageRect, visible, 0, budget);
            }
            if (!g_continuousRead) break;
        }
    }
    bool TileRequestScreenRect(PdfTab const& tab, PageRenderRequest const& request, RECT& out)
    {
        if (!request.tile || request.page < 0 || request.page >= tab.pageCount) return false;
        int start = FirstPageInSpread(request.page);
        if (!g_continuousRead)
        {
            int currentStart = FirstPageInSpread(std::clamp(tab.currentPage, 0, std::max(0, tab.pageCount - 1)));
            if (start != currentStart) return false;
        }
        int y = g_continuousRead ? SpreadVisualTop(tab, start) : g_stage.top + S(28) - g_scrollY;
        RECT pageRect{};
        if (!BuildSpreadPageRect(tab, start, y, request.page, pageRect)) return false;
        TileKey key{ request.page, TileZoomKey(request.zoom), request.theme, request.level, request.x, request.y, request.w, request.h };
        RECT tileRect = TileScreenRect(pageRect, key);
        InflateRect(&tileRect, S(1), S(1));
        return IntersectRect(&out, &tileRect, &g_stage) != FALSE;
    }
    PageImage& EnsurePage(PdfTab& tab, int index)
    {
        PreparePageCache(tab);
        auto it = tab.cache.find(index);
        if (it == tab.cache.end())
        {
            it = tab.cache.emplace(index, RenderPageAtZoom(tab, index, tab.zoom)).first;
            TrimPageCache(tab, index);
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
            while (tab.thumbCache.size() > 28)
            {
                auto victim = tab.thumbCache.end();
                int worstDistance = -1;
                for (auto candidate = tab.thumbCache.begin(); candidate != tab.thumbCache.end(); ++candidate)
                {
                    if (candidate->first == index) continue;
                    int distance = std::abs(candidate->first - index);
                    if (distance > worstDistance)
                    {
                        worstDistance = distance;
                        victim = candidate;
                    }
                }
                if (victim == tab.thumbCache.end()) break;
                tab.thumbCache.erase(victim);
            }
        }
        return it->second;
    }

    void SchedulePagePrefetch(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab || tab->pageCount <= 0) return;
        if (tab->zoom >= TileRenderZoomThreshold)
        {
            ScheduleVisibleTilePrefetch(hwnd, *tab);
            return;
        }
        PreparePageCache(*tab);

        auto missing = [&](int page)
        {
            return page >= 0 && page < tab->pageCount && tab->cache.find(page) == tab->cache.end();
        };
        int enqueueBudget = (g_draggingMainScroll || (g_stageLightPaintUntil != 0 && GetTickCount64() < g_stageLightPaintUntil)) ? 6 : 4;
        auto enqueue = [&](int page, int priority)
        {
            if (enqueueBudget <= 0) return;
            if (missing(page))
            {
                QueuePageRender(hwnd, *tab, page, priority);
                --enqueueBudget;
            }
        };

        int firstVisible = g_continuousRead ? FirstVisibleSpreadStart(*tab) : FirstPageInSpread(std::clamp(tab->currentPage, 0, tab->pageCount - 1));
        int lastVisible = firstVisible;
        for (int start = firstVisible; start < tab->pageCount; start = NextSpreadStart(*tab, start))
        {
            int y = g_continuousRead ? SpreadVisualTop(*tab, start) : g_stage.top + S(28) - g_scrollY;
            int spreadH = SpreadHeight(*tab, start);
            RECT spreadRect{ SpreadX(*tab, start), y, SpreadX(*tab, start) + SpreadWidth(*tab, start), y + spreadH };
            if (g_continuousRead && spreadRect.top > g_stage.bottom + S(96)) break;
            if (!Intersects(spreadRect, g_stage) && (g_continuousRead || start != firstVisible)) continue;

            int count = SpreadPageCount(*tab, start);
            for (int j = 0; j < count; ++j)
                enqueue(start + j, 0);
            lastVisible = start;

            if (!g_continuousRead) break;
        }

        int center = std::clamp(tab->currentPage, 0, tab->pageCount - 1);
        bool rapidScroll = g_draggingMainScroll || (g_stageLightPaintUntil != 0 && GetTickCount64() < g_stageLightPaintUntil);
        int forward = rapidScroll ? 2 : 1;
        int backward = 1;
        int nextStart = NextSpreadStart(*tab, lastVisible);
        for (int start = nextStart, n = 0; start < tab->pageCount && n < forward; start = NextSpreadStart(*tab, start), ++n)
        {
            int count = SpreadPageCount(*tab, start);
            for (int j = 0; j < count; ++j)
                enqueue(start + j, 1 + n);
        }
        for (int page = center - 1, n = 0; page >= 0 && n < backward; --page, ++n)
            enqueue(page, 2 + n);
    }

    bool PrefetchOnePage()
    {
        SchedulePagePrefetch(g_hwnd);
        return false;
    }

    void CollectOutline(PdfTab& tab, fz_outline* item, int depth)
    {
        for (fz_outline* current = item; current; current = current->next)
        {
            int page = g_pdf.pageNumberFromLocation(tab.ctx, tab.doc, current->page);
            if (page >= 0 && page < tab.pageCount)
            {
                std::wstring title = current->title ? FromUtf8(current->title) : L"\u672a\u547d\u540d\u76ee\u5f55";
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
        tab.currentPage = page;
        g_scrollY = g_continuousRead ? SpreadTopScroll(tab, page) : 0;
    }

    int PageTopScroll(PdfTab& tab, int page)
    {
        return SpreadTopScroll(tab, page);
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
                int content = ContinuousContentHeight(tab);
                int maxScroll = std::max(0, content - stageH);
                g_scrollY = std::clamp(target, 0, maxScroll);
            }
            else
            {
                int target = static_cast<int>(topPt * scale + 0.5) - S(96);
                tab.currentPage = page;
                g_contentHeight = SinglePageContentHeight(tab);
                g_scrollY = std::clamp(target, 0, SinglePageMaxScroll(tab));
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
        for (int start = 0; start < tab.pageCount; start = NextSpreadStart(tab, start))
        {
            content += SpreadHeight(tab, start) + gap;
        }
        return std::max<int>(content + S(28), g_stage.bottom - g_stage.top);
    }

    int SinglePageContentHeight(PdfTab const& tab)
    {
        if (tab.pageCount <= 0) return std::max<int>(1, g_stage.bottom - g_stage.top);
        int page = std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        return std::max<int>(SpreadHeight(tab, FirstPageInSpread(page)) + S(56), g_stage.bottom - g_stage.top);
    }

    int SinglePageMaxScroll(PdfTab const& tab)
    {
        int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
        return std::max<int>(0, SinglePageContentHeight(tab) - stageH);
    }

    bool PageRectAtPoint(PdfTab const& tab, int px, int py, int& page, RECT& pageRect)
    {
        if (!Hit(g_stage, px, py) || tab.pageCount <= 0) return false;
        int firstPage = g_continuousRead ? FirstVisibleSpreadStart(tab) : FirstPageInSpread(std::clamp(tab.currentPage, 0, tab.pageCount - 1));
        int lastPage = g_continuousRead ? tab.pageCount : NextSpreadStart(tab, firstPage);
        for (int start = firstPage; start < lastPage; start = NextSpreadStart(tab, start))
        {
            int y = g_continuousRead ? SpreadVisualTop(tab, start) : g_stage.top + S(28) - g_scrollY;
            int count = SpreadPageCount(tab, start);
            for (int j = 0; j < count; ++j)
            {
                RECT r{};
                int p = start + j;
                if (BuildSpreadPageRect(tab, start, y, p, r) && Hit(r, px, py))
                {
                    page = p;
                    pageRect = r;
                    return true;
                }
            }
        }
        return false;
    }

    bool PageRectForPage(PdfTab const& tab, int targetPage, RECT& pageRect)
    {
        if (tab.pageCount <= 0 || targetPage < 0 || targetPage >= tab.pageCount) return false;
        int firstPage = g_continuousRead ? FirstPageInSpread(targetPage) : FirstPageInSpread(std::clamp(tab.currentPage, 0, tab.pageCount - 1));
        int lastPage = g_continuousRead ? NextSpreadStart(tab, firstPage) : NextSpreadStart(tab, firstPage);
        for (int start = firstPage; start < lastPage; start = NextSpreadStart(tab, start))
        {
            int y = g_continuousRead ? SpreadVisualTop(tab, start) : g_stage.top + S(28) - g_scrollY;
            if (BuildSpreadPageRect(tab, start, y, targetPage, pageRect))
            {
                return true;
            }
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

    fz_point NormalizeSelectionPoint(RECT pageRect, POINT p)
    {
        double pageW = std::max<int>(1, pageRect.right - pageRect.left);
        double pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        int x = std::clamp(p.x, pageRect.left, pageRect.right);
        int y = std::clamp(p.y, pageRect.top, pageRect.bottom);
        return fz_point{
            static_cast<float>((x - pageRect.left) / pageW),
            static_cast<float>((y - pageRect.top) / pageH)
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

    void FillTextSelectionPaperExact(HDC dc, RECT r)
    {
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits)
        {
            if (bitmap) DeleteObject(bitmap);
            Fill(dc, r, RGB(255, 241, 168));
            return;
        }

        HDC mem = CreateCompatibleDC(dc);
        if (!mem)
        {
            DeleteObject(bitmap);
            Fill(dc, r, RGB(255, 241, 168));
            return;
        }

        HGDIOBJ old = SelectObject(mem, bitmap);
        BitBlt(mem, 0, 0, w, h, dc, r.left, r.top, SRCCOPY);

        auto* px = static_cast<std::uint8_t*>(bits);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                std::uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 4;
                std::uint8_t b = p[0];
                std::uint8_t g = p[1];
                std::uint8_t red = p[2];
                if (Luma(red, g, b) >= 172)
                {
                    p[0] = 168;
                    p[1] = 241;
                    p[2] = 255;
                    p[3] = 255;
                }
            }
        }

        BitBlt(dc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
    }
    void DrawTextSelectionQuads(HDC dc, RECT pageRect, std::vector<fz_quad> const& quads)
    {
        struct SelectionLine
        {
            RECT bounds{};
            int centerY{};
            int height{};
        };

        std::vector<RECT> rects;
        rects.reserve(quads.size());
        for (fz_quad const& quad : quads)
        {
            RECT r = QuadToRect(quad, pageRect);
            RECT clipped{};
            if (!IntersectRect(&clipped, &r, &pageRect))
                continue;
            if (clipped.right <= clipped.left || clipped.bottom <= clipped.top)
                continue;
            rects.push_back(clipped);
        }
        if (rects.empty())
            return;

        std::sort(rects.begin(), rects.end(), [](RECT const& a, RECT const& b)
        {
            int ac = (a.top + a.bottom) / 2;
            int bc = (b.top + b.bottom) / 2;
            if (std::abs(ac - bc) > 2)
                return ac < bc;
            return a.left < b.left;
        });

        std::vector<SelectionLine> lines;
        for (RECT const& r : rects)
        {
            int center = (r.top + r.bottom) / 2;
            int height = std::max<int>(1, static_cast<int>(r.bottom - r.top));
            SelectionLine* best = nullptr;
            int bestDistance = 0x7fffffff;
            for (auto& line : lines)
            {
                int tolerance = std::max(S(4), std::max(line.height, height) / 2);
                int distance = std::abs(center - line.centerY);
                if (distance <= tolerance && distance < bestDistance)
                {
                    best = &line;
                    bestDistance = distance;
                }
            }

            if (!best)
            {
                lines.push_back(SelectionLine{ r, center, height });
                continue;
            }

            best->bounds.left = std::min(best->bounds.left, r.left);
            best->bounds.top = std::min(best->bounds.top, r.top);
            best->bounds.right = std::max(best->bounds.right, r.right);
            best->bounds.bottom = std::max(best->bounds.bottom, r.bottom);
            best->centerY = (best->bounds.top + best->bounds.bottom) / 2;
            best->height = std::max<int>(best->height, static_cast<int>(best->bounds.bottom - best->bounds.top));
        }

        for (SelectionLine const& line : lines)
        {
            RECT r = line.bounds;
            InflateRect(&r, S(1), S(1));
            r.left = std::clamp(r.left, pageRect.left, pageRect.right);
            r.right = std::clamp(r.right, pageRect.left, pageRect.right);
            r.top = std::clamp(r.top, pageRect.top, pageRect.bottom);
            r.bottom = std::clamp(r.bottom, pageRect.top, pageRect.bottom);
            FillTextSelectionPaperExact(dc, r);
        }
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

    RECT DenormalizeAnnotationSegment(AnnotationSegment const& segment, RECT pageRect)
    {
        int pageW = std::max<int>(1, pageRect.right - pageRect.left);
        int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        return RECT{
            pageRect.left + static_cast<int>(segment.x0 * pageW + 0.5),
            pageRect.top + static_cast<int>(segment.y0 * pageH + 0.5),
            pageRect.left + static_cast<int>(segment.x1 * pageW + 0.5),
            pageRect.top + static_cast<int>(segment.y1 * pageH + 0.5)
        };
    }

    bool NearlySameSegment(AnnotationSegment const& a, AnnotationSegment const& b)
    {
        constexpr double eps = 0.0015;
        return std::abs(a.x0 - b.x0) <= eps &&
            std::abs(a.y0 - b.y0) <= eps &&
            std::abs(a.x1 - b.x1) <= eps &&
            std::abs(a.y1 - b.y1) <= eps;
    }

    bool AppendUniqueSegment(std::vector<AnnotationSegment>& segments, AnnotationSegment segment)
    {
        if (segment.x1 - segment.x0 < 0.002 || segment.y1 - segment.y0 < 0.002)
            return false;
        for (auto const& existing : segments)
        {
            if (NearlySameSegment(existing, segment))
                return false;
        }
        segments.push_back(segment);
        return true;
    }

    bool NearlySameAnnotation(PdfAnnotation const& a, PdfAnnotation const& b)
    {
        if (a.page != b.page || a.kind != b.kind || a.colorIndex != b.colorIndex)
            return false;
        AnnotationSegment as{ a.x0, a.y0, a.x1, a.y1 };
        AnnotationSegment bs{ b.x0, b.y0, b.x1, b.y1 };
        return NearlySameSegment(as, bs);
    }

    bool CanMergeLoadedAnnotation(PdfAnnotation const& existing, PdfAnnotation const& incoming)
    {
        return existing.page == incoming.page &&
            existing.kind == incoming.kind &&
            existing.colorIndex == incoming.colorIndex &&
            existing.createdAt == incoming.createdAt &&
            existing.author == incoming.author &&
            existing.note == incoming.note;
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

    bool HasImageSelection()
    {
        return g_hasContentSelection && g_selectionIsImage;
    }

    bool CurrentSelectionPageRect(RECT& out)
    {
        RECT selection = CurrentSelectionRect();
        if (IsRectEmpty(&selection) || IsRectEmpty(&g_selectionPageRect)) return false;
        return IntersectRect(&out, &selection, &g_selectionPageRect) != FALSE;
    }

    bool RefreshTextSelection(PdfTab& tab)
    {
        g_selectionQuads.clear();
        g_selectionText.clear();
        if (!g_pdf.selectText || g_selectionPage < 0 || g_selectionPage >= tab.pageCount)
            return false;

        fz_point start = NormalizeSelectionPoint(g_selectionPageRect, g_selectionStart);
        fz_point end = NormalizeSelectionPoint(g_selectionPageRect, g_selectionEnd);
        if (std::abs(start.x - end.x) < 0.001f && std::abs(start.y - end.y) < 0.001f)
            return false;

        std::vector<fz_quad> quads(256);
        std::string text(65536, '\0');
        int count = g_pdf.selectText(
            tab.ctx,
            tab.doc,
            g_selectionPage,
            start.x,
            start.y,
            end.x,
            end.y,
            quads.data(),
            static_cast<int>(quads.size()),
            text.data(),
            static_cast<int>(text.size()));
        if (count <= 0)
            return false;

        quads.resize(static_cast<size_t>(count));
        g_selectionQuads = std::move(quads);
        g_selectionNorm = NormalizeSelectionRect(g_selectionPageRect, g_selectionStart, g_selectionEnd);
        g_selectionText = FromUtf8(text.c_str());
        return true;
    }

    bool TrySelectImageAtPoint(PdfTab& tab, int page, RECT pageRect, POINT clientPt)
    {
        if (!g_pdf.findImageAtPoint || page < 0 || page >= tab.pageCount || IsRectEmpty(&pageRect))
            return false;

        double pageW = std::max<int>(1, pageRect.right - pageRect.left);
        double pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        float x = std::clamp(static_cast<float>((clientPt.x - pageRect.left) / pageW), 0.0f, 1.0f);
        float y = std::clamp(static_cast<float>((clientPt.y - pageRect.top) / pageH), 0.0f, 1.0f);
        fz_rect imageRect{};
        if (g_pdf.findImageAtPoint(tab.ctx, tab.doc, page, x, y, &imageRect) <= 0)
            return false;

        RECT hitRect{
            pageRect.left + static_cast<int>(std::floor(imageRect.x0 * pageW)),
            pageRect.top + static_cast<int>(std::floor(imageRect.y0 * pageH)),
            pageRect.left + static_cast<int>(std::ceil(imageRect.x1 * pageW)),
            pageRect.top + static_cast<int>(std::ceil(imageRect.y1 * pageH))
        };
        hitRect.left = std::clamp(hitRect.left, pageRect.left, pageRect.right - 1);
        hitRect.top = std::clamp(hitRect.top, pageRect.top, pageRect.bottom - 1);
        hitRect.right = std::clamp(hitRect.right, hitRect.left + 1, pageRect.right);
        hitRect.bottom = std::clamp(hitRect.bottom, hitRect.top + 1, pageRect.bottom);

        g_selectionPage = page;
        g_selectionPageRect = pageRect;
        g_selectionNorm = imageRect;
        g_selectionStart = POINT{ hitRect.left, hitRect.top };
        g_selectionEnd = POINT{ hitRect.right, hitRect.bottom };
        g_selectionQuads.clear();
        g_selectionText.clear();
        g_selectionIsImage = true;
        g_hasContentSelection = true;
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

    bool CopyImageToClipboard(HWND hwnd, PageImage const& image)
    {
        if (image.bgra.empty() || image.width <= 0 || image.height <= 0) return false;
        if (!OpenClipboard(hwnd)) return false;
        EmptyClipboard();

        int stride = image.width * 4;
        size_t payloadBytes = static_cast<size_t>(stride) * image.height;
        size_t totalBytes = sizeof(BITMAPINFOHEADER) + payloadBytes;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
        if (!mem)
        {
            CloseClipboard();
            return false;
        }

        BYTE* dst = static_cast<BYTE*>(GlobalLock(mem));
        if (!dst)
        {
            GlobalFree(mem);
            CloseClipboard();
            return false;
        }

        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = image.width;
        bih.biHeight = image.height;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = static_cast<DWORD>(payloadBytes);
        memcpy(dst, &bih, sizeof(bih));

        BYTE* bits = dst + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < image.height; ++y)
        {
            BYTE const* srcRow = image.bgra.data() + static_cast<size_t>(image.height - 1 - y) * stride;
            memcpy(bits + static_cast<size_t>(y) * stride, srcRow, stride);
        }

        GlobalUnlock(mem);
        if (!SetClipboardData(CF_DIB, mem))
        {
            GlobalFree(mem);
            CloseClipboard();
            return false;
        }

        CloseClipboard();
        return true;
    }

    bool BuildSelectionImage(PdfTab& tab, PageImage& image)
    {
        RECT selected{};
        if (!HasImageSelection() || !CurrentSelectionPageRect(selected) || g_selectionPage < 0 || g_selectionPage >= tab.pageCount)
            return false;

        int localLeft = std::max<int>(0, selected.left - g_selectionPageRect.left);
        int localTop = std::max<int>(0, selected.top - g_selectionPageRect.top);
        int localRight = std::min<int>(g_selectionPageRect.right - g_selectionPageRect.left, selected.right - g_selectionPageRect.left);
        int localBottom = std::min<int>(g_selectionPageRect.bottom - g_selectionPageRect.top, selected.bottom - g_selectionPageRect.top);
        if (localRight <= localLeft || localBottom <= localTop)
            return false;

        RECT tile{ localLeft, localTop, localRight, localBottom };
        image = RenderPageTile(tab, g_selectionPage, tile);
        return !image.bgra.empty();
    }

    bool CopyCurrentSelectionToClipboard(HWND hwnd)
    {
        if (!g_hasContentSelection) return false;
        if (!g_selectionQuads.empty())
        {
            CopyTextToClipboard(hwnd, g_selectionText);
            return !g_selectionText.empty();
        }
        PdfTab* tab = Active();
        if (!tab) return false;
        PageImage image;
        if (!BuildSelectionImage(*tab, image)) return false;
        return CopyImageToClipboard(hwnd, image);
    }

    void ClearTextSelection()
    {
        g_selectingContent = false;
        g_hasContentSelection = false;
        g_selectionIsImage = false;
        g_selectionPage = -1;
        g_selectionPageRect = {};
        g_selectionNorm = {};
        g_selectionQuads.clear();
        g_selectionText.clear();
    }

    void ScrollToPage(PdfTab& tab, int page);
    void LoadAnnotationsFromPdf(PdfTab& tab);

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

    bool TrySelectWordAtPoint(HWND hwnd, PdfTab& tab, int page, RECT pageRect, POINT clientPt)
    {
        if (!g_pdf.selectWordAtPoint || page < 0 || page >= tab.pageCount || IsRectEmpty(&pageRect))
            return false;

        double pageW = std::max<int>(1, pageRect.right - pageRect.left);
        double pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        float x = std::clamp(static_cast<float>((clientPt.x - pageRect.left) / pageW), 0.0f, 1.0f);
        float y = std::clamp(static_cast<float>((clientPt.y - pageRect.top) / pageH), 0.0f, 1.0f);

        std::vector<fz_quad> quads(256);
        std::string text(65536, '\0');
        int count = g_pdf.selectWordAtPoint(
            tab.ctx,
            tab.doc,
            page,
            x,
            y,
            quads.data(),
            static_cast<int>(quads.size()),
            text.data(),
            static_cast<int>(text.size()));
        if (count <= 0)
            return false;

        quads.resize(static_cast<size_t>(count));
        bool haveUnion = false;
        RECT unionRect{};
        fz_rect norm{};
        for (fz_quad const& quad : quads)
        {
            float minX = std::min({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            float maxX = std::max({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            float minY = std::min({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
            float maxY = std::max({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
            minX = std::clamp(minX, 0.0f, 1.0f);
            maxX = std::clamp(maxX, 0.0f, 1.0f);
            minY = std::clamp(minY, 0.0f, 1.0f);
            maxY = std::clamp(maxY, 0.0f, 1.0f);
            RECT quadRect = QuadToRect(quad, pageRect);
            if (!haveUnion)
            {
                unionRect = quadRect;
                norm = fz_rect{ minX, minY, maxX, maxY };
                haveUnion = true;
            }
            else
            {
                UnionRect(&unionRect, &unionRect, &quadRect);
                norm.x0 = std::min(norm.x0, minX);
                norm.y0 = std::min(norm.y0, minY);
                norm.x1 = std::max(norm.x1, maxX);
                norm.y1 = std::max(norm.y1, maxY);
            }
        }
        if (!haveUnion)
            return false;

        ClearAnnotationFocus();
        g_selectingContent = false;
        g_hasContentSelection = true;
        g_selectionIsImage = false;
        g_selectionPage = page;
        g_selectionPageRect = pageRect;
        g_selectionStart = POINT{ unionRect.left, unionRect.top };
        g_selectionEnd = POINT{ unionRect.right, unionRect.bottom };
        g_selectionNorm = norm;
        g_selectionQuads = std::move(quads);
        g_selectionText = FromUtf8(text.c_str());
        InvalidateRect(hwnd, &g_stage, FALSE);
        return true;
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

    constexpr int MaxVisibleTabs = 10;

    RECT HomeContentViewportRect(RECT client)
    {
        return RECT{ 0, S(220), client.right, client.bottom };
    }

    int ComputeHomeGridContentHeight(RECT client, size_t recentCount)
    {
        int tileW = S(152);
        int coverH = S(198);
        int nameH = S(38);
        int tileH = coverH + nameH + S(10);
        int gapX = S(28);
        int gapY = S(28);
        int startX = S(42);
        int maxRight = std::max<int>(startX + tileW, client.right - S(42));
        int availableW = std::max<int>(tileW, maxRight - startX);
        int columns = std::max<int>(1, (availableW + gapX) / (tileW + gapX));
        size_t tileCount = 1 + recentCount;
        size_t rows = std::max<size_t>(1, (tileCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
        return S(16) + static_cast<int>(rows) * tileH + std::max<int>(0, static_cast<int>(rows) - 1) * gapY + S(24);
    }

    int ComputeHomeListContentHeight(size_t recentCount, bool showEmptySearch)
    {
        int content = static_cast<int>(recentCount) * S(48) + S(16);
        if (showEmptySearch && recentCount == 0)
            content = std::max<int>(content, S(80));
        return std::max<int>(0, content);
    }

    int ClampHomeScrollValue(RECT client, int value)
    {
        RECT viewport = HomeContentViewportRect(client);
        int visible = std::max<int>(1, viewport.bottom - viewport.top);
        int maxScroll = std::max<int>(0, g_homeContentHeight - visible);
        return std::clamp(value, 0, maxScroll);
    }

    void ScrollHomeTo(HWND hwnd, int value)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        int clamped = ClampHomeScrollValue(client, value);
        if (clamped == g_homeScrollY) return;
        g_homeScrollY = clamped;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void EnsureTabWindowVisible(int index)
    {
        int total = static_cast<int>(g_tabs.size());
        if (total <= MaxVisibleTabs)
        {
            g_tabVisibleStart = 0;
            return;
        }
        int maxStart = std::max(0, total - MaxVisibleTabs);
        if (index >= 0)
        {
            if (index < g_tabVisibleStart) g_tabVisibleStart = index;
            else if (index >= g_tabVisibleStart + MaxVisibleTabs) g_tabVisibleStart = index - MaxVisibleTabs + 1;
        }
        g_tabVisibleStart = std::clamp(g_tabVisibleStart, 0, maxStart);
    }

    int ClampTabListScrollValue(RECT client, int value)
    {
        int maxHeight = std::max<int>(S(120), static_cast<int>(client.bottom - g_tabCountRect.bottom - S(18)));
        int visible = std::min(maxHeight, std::max<int>(0, g_tabListContentHeight));
        int maxScroll = std::max<int>(0, g_tabListContentHeight - visible);
        return std::clamp(value, 0, maxScroll);
    }

    void ScrollTabListTo(HWND hwnd, int value)
    {
        if (!g_tabListOpen) return;
        RECT client{};
        GetClientRect(hwnd, &client);
        int clamped = ClampTabListScrollValue(client, value);
        if (clamped == g_tabListScrollY) return;
        g_tabListScrollY = clamped;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    int PageFromScroll(PdfTab const& tab, int scrollY)
    {
        if (tab.pageCount <= 0) return 0;
        int gap = PageGap();
        int y = S(28);
        int probe = scrollY + S(32);
        int best = 0;
        for (int start = 0; start < tab.pageCount; start = NextSpreadStart(tab, start))
        {
            int spreadH = SpreadHeight(tab, start);
            if (probe < y + spreadH + gap / 2) return start;
            y += spreadH + gap;
            best = start;
        }
        return best;
    }

    int OutlineTargetScrollY(PdfTab const& tab, OutlineItem const& item, int knownPage = -1, int knownPageTop = 0)
    {
        int page = std::clamp(item.page, 0, tab.pageCount - 1);
        int scroll = knownPage == page ? knownPageTop : SpreadTopScroll(tab, page);
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
        for (int i = 0; i < static_cast<int>(tab.outline.size()); ++i)
        {
            if (!OutlineRowVisible(tab, i)) continue;
            int itemPage = std::clamp(tab.outline[static_cast<size_t>(i)].page, 0, tab.pageCount - 1);
            int target = OutlineTargetScrollY(tab, tab.outline[static_cast<size_t>(i)]);
            if (target <= probe) best = i;
            else if (itemPage > tab.currentPage || target > probe + S(400)) break;
        }
        return best;
    }

    void KeepOutlineSelectionVisible(HWND hwnd, PdfTab const& tab)
    {
        if (!SidebarVisible() || g_sideMode != 0 || g_selectedOutlineRow < 0) return;
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

    int ThumbnailBoxHeight(PdfTab const& tab, int page)
    {
        page = std::clamp(page, 0, std::max(0, tab.pageCount - 1));
        double ratioH = tab.pages[static_cast<size_t>(page)].heightPt / std::max(1.0, tab.pages[static_cast<size_t>(page)].widthPt);
        int thumbH = std::max<int>(S(56), static_cast<int>(g_thumbSize * ratioH + 0.5));
        return thumbH + S(58);
    }

    void KeepThumbnailSelectionVisible(HWND hwnd, PdfTab const& tab)
    {
        if (!SidebarVisible() || g_sideMode != 1 || tab.pageCount <= 0) return;
        int page = std::clamp(tab.currentPage, 0, tab.pageCount - 1);
        int itemTop = S(80);
        for (int i = 0; i < page; ++i)
            itemTop += ThumbnailBoxHeight(tab, i) + S(22);
        int itemBottom = itemTop + ThumbnailBoxHeight(tab, page);

        int sidebarH = std::max<int>(1, g_sidebar.bottom - g_sidebar.top);
        int desired = g_sidebarScrollY;
        if (itemTop - g_sidebarScrollY < S(86))
            desired = itemTop - S(92);
        else if (itemBottom - g_sidebarScrollY > sidebarH - S(14))
            desired = itemBottom - sidebarH + S(14);

        desired = ClampSidebarScrollValue(desired);
        if (desired != g_sidebarScrollY)
        {
            g_sidebarScrollY = desired;
            RequestSidebarRepaint(hwnd);
        }
        else
        {
            RECT area = SidebarScrollRect();
            InvalidateRect(hwnd, &area, FALSE);
        }
    }

    void SyncOutlineSelectionFromScroll(HWND hwnd, PdfTab& tab)
    {
        int selected = OutlineRowFromScroll(tab, g_scrollY);
        if (selected == g_selectedOutlineRow) return;
        g_selectedOutlineRow = selected;
        KeepOutlineSelectionVisible(hwnd, tab);
    }

    void SyncSidebarProgressFromScroll(HWND hwnd, PdfTab& tab)
    {
        if (g_sideMode == 0) SyncOutlineSelectionFromScroll(hwnd, tab);
        else if (g_sideMode == 1) KeepThumbnailSelectionVisible(hwnd, tab);
    }

    void SyncSidebarProgressFromScrollFast(HWND hwnd, PdfTab& tab)
    {
        if (!SidebarVisible()) return;
        if (g_sideMode == 0)
        {
            int selected = OutlineRowFromScroll(tab, g_scrollY);
            if (selected != g_selectedOutlineRow)
            {
                g_selectedOutlineRow = selected;
                RequestSidebarRepaint(hwnd);
            }
        }
        else if (g_sideMode == 1)
        {
            RequestSidebarRepaint(hwnd);
        }
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
        int scroll = SpreadTopScroll(tab, page);
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
                    std::wstring snippet = L"\u7b2c" + std::to_wstring(page + 1) + L" \u9875\u7b2c " + std::to_wstring(hit + 1) + L" \u5904\uff1a" + g_searchQuery;
                    int highlightStart = static_cast<int>(snippet.find(g_searchQuery));
                    if (highlightStart < 0) highlightStart = 0;
                    g_searchResults.push_back(SearchResult{
                        L"\u7b2c" + std::to_wstring(page + 1) + L" \u9875\u5305\u542b \"" + g_searchQuery + L"\"",
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
        tab->renderVersion = ++g_nextRenderVersion;
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
        LoadAnnotationsFromPdf(*tab);
        g_tabs.push_back(std::move(tab));
        g_active = static_cast<int>(g_tabs.size()) - 1;
        EnsureTabWindowVisible(g_active);
        g_tabListOpen = false;
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

    bool SamePath(std::wstring const& a, std::wstring const& b)
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    int FindOpenTabByPath(std::wstring const& path)
    {
        for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        {
            if (SamePath(g_tabs[static_cast<size_t>(i)]->path, path))
                return i;
        }
        return -1;
    }

    void ApplySessionPosition(PdfTab& tab, LastSessionItem const& item)
    {
        tab.zoom = std::clamp(item.zoom, 0.0833, 64.0);
        ScrollToPage(tab, std::clamp(item.page, 1, tab.pageCount) - 1);
        tab.savedScrollY = std::max(0, item.scrollY);
        if (g_continuousRead)
        {
            g_contentHeight = ContinuousContentHeight(tab);
            int maxScroll = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
            g_scrollY = std::clamp(tab.savedScrollY, 0, maxScroll);
            tab.currentPage = PageFromScroll(tab, g_scrollY);
        }
        else
        {
            g_contentHeight = SinglePageContentHeight(tab);
            g_scrollY = std::clamp(tab.savedScrollY, 0, SinglePageMaxScroll(tab));
        }
        TouchRecentProgress(tab, false);
    }

    void CaptureLastSessionFromOpenTabs()
    {
        g_lastSessionTabs.clear();
        g_lastSessionActive = -1;
        for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        {
            auto const& tab = g_tabs[static_cast<size_t>(i)];
            if (!tab || tab->path.empty()) continue;
            LastSessionItem item;
            item.path = tab->path;
            item.page = std::max(1, tab->currentPage + 1);
            item.scrollY = (g_active == i) ? g_scrollY : tab->savedScrollY;
            item.zoom = tab->zoom;
            g_lastSessionTabs.push_back(item);
        }
        if (!g_lastSessionTabs.empty())
            g_lastSessionActive = std::clamp(g_active, 0, static_cast<int>(g_lastSessionTabs.size()) - 1);
    }

    bool HasOtherQingYueWindows(HWND current)
    {
        HWND found = nullptr;
        while ((found = FindWindowExW(nullptr, found, L"XiaoYuePDFWindow", nullptr)) != nullptr)
        {
            if (found != current)
                return true;
        }
        return false;
    }

    void RestoreLastSessionAndOpenLaunchFiles(HWND hwnd, std::vector<std::wstring> const& launchFiles)
    {
        std::vector<int> restoredIndices;
        std::vector<LastSessionItem> savedTabs = g_lastSessionTabs;
        int savedActive = g_lastSessionActive;

        for (auto const& item : savedTabs)
        {
            if (item.path.empty() || !std::filesystem::exists(item.path))
                continue;
            if (FindOpenTabByPath(item.path) >= 0)
                continue;
            try
            {
                OpenPdf(hwnd, item.path);
                if (PdfTab* tab = Active())
                {
                    ApplySessionPosition(*tab, item);
                    restoredIndices.push_back(g_active);
                }
            }
            catch (std::exception const&) {}
        }

        if (!restoredIndices.empty())
        {
            int restoredActive = std::clamp(savedActive, 0, static_cast<int>(restoredIndices.size()) - 1);
            g_active = restoredIndices[static_cast<size_t>(restoredActive)];
            RestoreActiveTabPosition(hwnd);
        }

        for (auto const& path : launchFiles)
        {
            int existing = FindOpenTabByPath(path);
            if (existing >= 0)
            {
                ActivateTab(hwnd, existing);
                continue;
            }
            try { OpenPdf(hwnd, path); }
            catch (std::exception const& e) { MessageBoxA(hwnd, e.what(), "QingYuePDF", MB_ICONERROR); }
        }

        SetTitle();
        StartActiveFileWatcher(hwnd);
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
        ofn.lpstrFilter = L"PDF \u6587\u4ef6 (*.pdf)\0*.pdf\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) OpenPdf(hwnd, file);
    }

    void ClampScroll(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
            g_contentWidth = ReadingContentWidth(*tab);
        }
        int maxScroll = std::max<int>(0, g_contentHeight - (g_stage.bottom - g_stage.top));
        int maxScrollX = std::max<int>(0, g_contentWidth - std::max<int>(1, g_stage.right - g_stage.left));
        g_scrollY = std::clamp(g_scrollY, 0, maxScroll);
        g_scrollX = std::clamp(g_scrollX, 0, maxScrollX);
        if (tab && g_continuousRead)
        {
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
            TouchRecentProgress(*tab, false);
            SyncSidebarProgressFromScroll(hwnd, *tab);
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
            g_contentWidth = ReadingContentWidth(*tab);
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

    RECT UnionRectSafe(RECT a, RECT b)
    {
        if (IsRectEmpty(&a)) return b;
        if (IsRectEmpty(&b)) return a;
        RECT out{};
        UnionRect(&out, &a, &b);
        return out;
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
        g_sidebarWidth = g_sidebarTargetWidth;
        if (g_sidebarWidth >= S(180)) g_sidebarExpandedWidth = g_sidebarWidth;
        g_sidebarResizePending = false;
        g_lastSidebarResizeMs = GetTickCount64();
        KillTimer(hwnd, 1);

        int top = g_immersive ? 0 : S(76);
        int bottom = (g_immersive || !g_statusPinned) ? 0 : S(34);
        RECT dirty{ 0, top, client.right, client.bottom - bottom };
        InvalidateRect(hwnd, &dirty, FALSE);
        InvalidateRect(hwnd, &dirty, FALSE);
    }

    RECT SidebarScrollRect()
    {
        int top = g_sidebar.top + S(g_sideMode == 1 ? 80 : (g_sideMode == 2 ? 146 : 44));
        return RECT{ g_sidebar.left, top, g_sidebar.right, g_sidebar.bottom };
    }

    bool SidebarContentNeedsScroll()
    {
        if (!SidebarVisible() || g_sidebarWidth <= 0) return false;
        if (g_sideMode != 0 && g_sideMode != 1) return false;
        RECT area = SidebarScrollRect();
        int visible = std::max<int>(1, area.bottom - area.top);
        return g_sidebarContentHeight > visible + S(1);
    }

    void UpdateSidebarScrollRects()
    {
        g_sidebarScrollTrackRect = {};
        g_sidebarScrollThumbRect = {};
        if (!SidebarContentNeedsScroll()) return;

        RECT area = SidebarScrollRect();
        int trackW = S(12);
        int gutter = S(2);
        g_sidebarScrollTrackRect = RECT{
            g_sidebar.right - gutter - trackW,
            area.top + S(6),
            g_sidebar.right - gutter,
            area.bottom - S(6)
        };

        int trackH = std::max<int>(1, g_sidebarScrollTrackRect.bottom - g_sidebarScrollTrackRect.top);
        int visible = std::max<int>(1, area.bottom - area.top);
        int thumbH = std::clamp(MulDiv(visible, trackH, std::max(1, g_sidebarContentHeight)), S(28), trackH);
        int maxScroll = std::max<int>(1, g_sidebarContentHeight - visible);
        int travel = std::max<int>(0, trackH - thumbH);
        int scrollY = std::clamp(g_sidebarScrollY, 0, maxScroll);
        int thumbTop = g_sidebarScrollTrackRect.top + MulDiv(scrollY, travel, maxScroll);
        if (scrollY >= maxScroll) thumbTop = g_sidebarScrollTrackRect.bottom - thumbH;
        g_sidebarScrollThumbRect = RECT{
            g_sidebarScrollTrackRect.left,
            thumbTop,
            g_sidebarScrollTrackRect.right,
            thumbTop + thumbH
        };
    }

    void PaintSidebarScrollBar(HDC dc)
    {
        UpdateSidebarScrollRects();
        if (IsRectEmpty(&g_sidebarScrollTrackRect)) return;

        RECT area = SidebarScrollRect();
        bool visible = g_draggingSidebarScroll || Hit(area, g_mousePos.x, g_mousePos.y);
        if (!visible) return;

        bool active = g_draggingSidebarScroll || Hit(g_sidebarScrollTrackRect, g_mousePos.x, g_mousePos.y);
        int centerX = (g_sidebarScrollTrackRect.left + g_sidebarScrollTrackRect.right) / 2;
        int trackW = active ? S(6) : std::max(2, S(3));
        RECT visualThumb{
            centerX - trackW / 2,
            g_sidebarScrollThumbRect.top,
            centerX + (trackW + 1) / 2,
            g_sidebarScrollThumbRect.bottom
        };
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        BYTE alpha = active ? 136 : 92;
        Gdiplus::SolidBrush brush(GdiColor(ScrollThumbColor(active), alpha));
        Gdiplus::RectF thumb = Rf(visualThumb);
        float cap = std::max(1.0f, thumb.Width * 0.5f);
        if (thumb.Height <= thumb.Width)
        {
            g.FillEllipse(&brush, thumb);
        }
        else
        {
            Gdiplus::GraphicsPath path;
            path.AddArc(thumb.X, thumb.Y, thumb.Width, thumb.Width, 180.0f, 180.0f);
            path.AddLine(thumb.X + thumb.Width, thumb.Y + cap, thumb.X + thumb.Width, thumb.Y + thumb.Height - cap);
            path.AddArc(thumb.X, thumb.Y + thumb.Height - thumb.Width, thumb.Width, thumb.Width, 0.0f, 180.0f);
            path.AddLine(thumb.X, thumb.Y + thumb.Height - cap, thumb.X, thumb.Y + cap);
            path.CloseFigure();
            g.FillPath(&brush, &path);
        }
    }

    void RequestStageRepaint(HWND hwnd, bool includeStatus)
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        InvalidateRect(hwnd, &g_stage, FALSE);
        if (!IsRectEmpty(&g_mainScrollTrackRect))
        {
            InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
        }
        if (!IsRectEmpty(&g_mainHScrollTrackRect))
        {
            InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
        }
        if (includeStatus)
        {
            RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
            InvalidateRect(hwnd, &status, FALSE);
        }
        g_lastStageRepaintMs = GetTickCount64();
        g_stageRepaintPending = false;
        KillTimer(hwnd, 6);
    }

    void ScrollStagePixels(HWND hwnd, int dx, int dy, bool includeStatus)
    {
        UNREFERENCED_PARAMETER(dx);
        UNREFERENCED_PARAMETER(dy);
        RequestStageRepaint(hwnd, includeStatus);
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

    bool StageLightPaintActive()
    {
        if (g_hasContentSelection || g_selectingContent || g_annotationToolbarOpen || g_annotationNoteOpen)
            return false;
        return g_stageLightPaintUntil != 0 && GetTickCount64() < g_stageLightPaintUntil;
    }    bool StageScrollPaintInProgress()
    {
        return StageLightPaintActive() || g_draggingMainScroll || g_draggingMainHScroll ||
            g_smoothStageScroll || g_touchPanQueued || g_touchMomentum || g_autoScrollActive;
    }

    void MarkStageScrolling(HWND hwnd)
    {
        ULONGLONG now = GetTickCount64();
        g_stageLightPaintUntil = now + 120;
        g_stageIdleRefreshPending = true;
        KillTimer(hwnd, 8);
        SetTimer(hwnd, 8, 140, nullptr);
    }

    void ScrollStageTo(HWND hwnd, int newScrollY, bool animatedFrame = false, bool immediateRepaint = false)
    {
        PdfTab* tab = Active();
        int oldPage = tab ? tab->currentPage : -1;
        if (tab)
        {
            g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
            g_contentWidth = ReadingContentWidth(*tab);
        }
        int maxScroll = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
        g_scrollX = std::clamp(g_scrollX, 0, std::max<int>(0, g_contentWidth - std::max<int>(1, g_stage.right - g_stage.left)));
        int old = g_scrollY;
        g_scrollY = std::clamp(newScrollY, 0, maxScroll);
        if (tab && g_continuousRead)
        {
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
            if (oldPage != tab->currentPage)
            {
                TouchRecentProgress(*tab, false);
                SyncSidebarProgressFromScrollFast(hwnd, *tab);
            }
            g_sidebarProgressRefreshPending = true;
        }
        int dy = old - g_scrollY;
        if (!animatedFrame)
        {
            g_stageScrollPos = static_cast<double>(g_scrollY);
            g_stageScrollVelocity = 0.0;
            g_stageScrollLastTick = GetTickCount64();
        }

        if (dy != 0) ++g_viewportGeneration;

        if (dy == 0)
        {
            UpdateVertScrollBar(hwnd);
            if (immediateRepaint)
            {
                if (!IsRectEmpty(&g_mainScrollTrackRect))
                    RedrawWindow(hwnd, &g_mainScrollTrackRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                if (!IsRectEmpty(&g_mainHScrollTrackRect))
                    RedrawWindow(hwnd, &g_mainHScrollTrackRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
            return;
        }

        if (!immediateRepaint)
            MarkStageScrolling(hwnd);

        bool pageChanged = tab && oldPage != tab->currentPage;
        if (immediateRepaint)
        {
            InvalidateRect(hwnd, &g_stage, FALSE);
            if (!IsRectEmpty(&g_mainScrollTrackRect))
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
            if (!IsRectEmpty(&g_mainHScrollTrackRect))
                InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
            if (pageChanged)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                InvalidateRect(hwnd, &status, FALSE);
            }
            g_lastStageRepaintMs = GetTickCount64();
            g_stageRepaintPending = false;
            if (!IsRectEmpty(&g_mainScrollTrackRect))
                RedrawWindow(hwnd, &g_mainScrollTrackRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            if (!IsRectEmpty(&g_mainHScrollTrackRect))
                RedrawWindow(hwnd, &g_mainHScrollTrackRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            if (pageChanged)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT status{ 0, client.bottom - S(34), client.right, client.bottom };
                RedrawWindow(hwnd, &status, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
        }
        else
        {
            ScrollStagePixels(hwnd, 0, dy, pageChanged);
        }
        UpdateVertScrollBar(hwnd);
        SchedulePagePrefetch(hwnd);
    }

    void FlushStageForTouch(HWND hwnd, int deltaY)
    {
        UNREFERENCED_PARAMETER(hwnd);
        UNREFERENCED_PARAMETER(deltaY);
    }

    void ScrollStageXTo(HWND hwnd, int newScrollX)
    {
        PdfTab* tab = Active();
        if (tab) g_contentWidth = ReadingContentWidth(*tab);
        int stageW = std::max<int>(1, g_stage.right - g_stage.left);
        int maxScroll = std::max<int>(0, g_contentWidth - stageW);
        int old = g_scrollX;
        g_scrollX = std::clamp(newScrollX, 0, maxScroll);
        if (old == g_scrollX)
        {
            UpdateVertScrollBar(hwnd);
            return;
        }
        MarkStageScrolling(hwnd);
        ScrollStagePixels(hwnd, old - g_scrollX, 0, false);
        UpdateVertScrollBar(hwnd);
    }

    void ScrollSidebarTo(HWND hwnd, int newScrollY)
    {
        int old = g_sidebarScrollY;
        int visible = std::max<int>(1, g_sidebar.bottom - SidebarScrollRect().top);
        int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
        g_sidebarScrollY = std::clamp(newScrollY, 0, maxScroll);
        if (old == g_sidebarScrollY) return;
        RequestSidebarRepaint(hwnd);
    }

    int WheelPixels(int delta, int pixelsPerNotch, double& remainder)
    {
        double exact = remainder + static_cast<double>(delta) * static_cast<double>(pixelsPerNotch) / static_cast<double>(WHEEL_DELTA);
        int pixels = exact >= 0.0 ? static_cast<int>(std::floor(exact)) : static_cast<int>(std::ceil(exact));
        remainder = exact - static_cast<double>(pixels);
        return pixels;
    }

    void StartSmoothStageScroll(HWND hwnd, int target);
    bool ScrollByPanPixels(HWND hwnd, POINT clientPt, int deltaY, int deltaX = 0);
    void StopAutoScroll(HWND hwnd);
    void QueueTouchPanPixels(HWND hwnd, POINT clientPt, int deltaY, int deltaX = 0);
    bool DrawCachedImage(HDC dc, PageImage& image, RECT dest);

    void StopTouchMomentum(HWND hwnd)
    {
        g_touchMomentum = false;
        g_touchMomentumX = 0.0;
        g_touchMomentumY = 0.0;
        g_touchMomentumRemainderX = 0.0;
        g_touchMomentumRemainderY = 0.0;
        g_touchMomentumBoost = 0.0;
        KillTimer(hwnd, 18);
    }

    void AddTouchMomentumImpulse(HWND hwnd, POINT clientPt, int deltaY, int deltaX)
    {
        constexpr bool touchMomentumEnabled = false;
        if (!touchMomentumEnabled)
        {
            UNREFERENCED_PARAMETER(hwnd);
            UNREFERENCED_PARAMETER(clientPt);
            UNREFERENCED_PARAMETER(deltaY);
            UNREFERENCED_PARAMETER(deltaX);
            return;
        }
        if (deltaY == 0 && deltaX == 0) return;
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedMs = g_touchMomentumLastInputMs ? now - g_touchMomentumLastInputMs : 32;
        double frames = std::clamp(static_cast<double>(std::max<ULONGLONG>(1, elapsedMs)) / 16.0, 0.45, 4.0);
        double sampleY = static_cast<double>(deltaY) / frames;
        double sampleX = static_cast<double>(deltaX) / frames;

        if (elapsedMs > 180)
            g_touchMomentumBoost = 0.0;
        g_touchMomentumBoost = std::min(1400.0, g_touchMomentumBoost * 0.72 + static_cast<double>(std::abs(deltaY) + std::abs(deltaX)) * 2.4);
        double multiplier = 1.35 + std::min(3.15, g_touchMomentumBoost / 500.0);

        g_touchMomentumY = std::clamp(sampleY * multiplier, -420.0, 420.0);
        g_touchMomentumX = std::clamp(sampleX * multiplier, -420.0, 420.0);
        g_touchMomentumPoint = clientPt;
        g_touchMomentumLastInputMs = now;
        g_touchMomentumLastTickMs = now;
        g_touchMomentum = std::abs(g_touchMomentumY) >= 0.8 || std::abs(g_touchMomentumX) >= 0.8;
        if (g_touchMomentum)
            SetTimer(hwnd, 18, 16, nullptr);
    }

    void StepTouchMomentum(HWND hwnd)
    {
        if (!g_touchMomentum)
        {
            StopTouchMomentum(hwnd);
            return;
        }

        ULONGLONG now = GetTickCount64();
        if (g_touchMomentumLastInputMs && now - g_touchMomentumLastInputMs < 42)
            return;
        double frames = std::clamp(static_cast<double>(std::max<ULONGLONG>(1, now - g_touchMomentumLastTickMs)) / 16.0, 0.5, 2.5);
        g_touchMomentumLastTickMs = now;

        double moveY = g_touchMomentumY * frames + g_touchMomentumRemainderY;
        double moveX = g_touchMomentumX * frames + g_touchMomentumRemainderX;
        int pixelsY = moveY >= 0.0 ? static_cast<int>(std::floor(moveY)) : static_cast<int>(std::ceil(moveY));
        int pixelsX = moveX >= 0.0 ? static_cast<int>(std::floor(moveX)) : static_cast<int>(std::ceil(moveX));
        g_touchMomentumRemainderY = moveY - static_cast<double>(pixelsY);
        g_touchMomentumRemainderX = moveX - static_cast<double>(pixelsX);

        if (pixelsY != 0 || pixelsX != 0)
            ScrollByPanPixels(hwnd, g_touchMomentumPoint, pixelsY, pixelsX);

        double decay = std::pow(0.955, frames);
        g_touchMomentumY *= decay;
        g_touchMomentumX *= decay;
        if (std::abs(g_touchMomentumY) < 0.32 && std::abs(g_touchMomentumX) < 0.32 &&
            std::abs(g_touchMomentumRemainderY) < 0.7 && std::abs(g_touchMomentumRemainderX) < 0.7)
        {
            StopTouchMomentum(hwnd);
        }
    }

    void CancelStageMotion(HWND hwnd)
    {
        g_smoothStageScroll = false;
        g_stageScrollTarget = g_scrollY;
        g_stageScrollPos = static_cast<double>(g_scrollY);
        g_stageScrollVelocity = 0.0;
        g_stageScrollLastTick = GetTickCount64();
        KillTimer(hwnd, 4);
        g_touchPanQueued = false;
        g_touchPanPendingX = 0;
        g_touchPanPendingY = 0;
        KillTimer(hwnd, 16);
        StopTouchMomentum(hwnd);
        g_stageWheelRemainder = 0.0;
        g_lastTouchFollowSign = 0;
        g_lastTouchFollowPaintMs = 0;
    }

    POINT CurrentZoomAnchorPoint()
    {
        if (Hit(g_stage, g_mousePos.x, g_mousePos.y))
            return g_mousePos;
        return POINT{
            (g_stage.left + g_stage.right) / 2,
            (g_stage.top + g_stage.bottom) / 2
        };
    }

    void ApplyZoomAtPoint(HWND hwnd, PdfTab& tab, POINT clientPt, double newZoom)
    {
        double clampedZoom = std::clamp(newZoom, 0.0833, 64.0);
        if (std::abs(clampedZoom - tab.zoom) < 0.0001) return;
        CancelStageMotion(hwnd);

        int anchorPage = std::clamp(tab.currentPage, 0, std::max(0, tab.pageCount - 1));
        double anchorY = 0.5;
        RECT pageRect{};
        int hoveredPage = -1;
        if (PageRectAtPoint(tab, clientPt.x, clientPt.y, hoveredPage, pageRect))
        {
            anchorPage = hoveredPage;
            int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
            anchorY = std::clamp(static_cast<double>(clientPt.y - pageRect.top) / static_cast<double>(pageH), 0.0, 1.0);
        }
        if (tab.cachedZoom == tab.zoom && tab.cachedDpi == g_dpi && tab.cachedTheme == g_theme &&
            tab.zoom < TileRenderZoomThreshold && clampedZoom < 3.25 && !tab.cache.empty())
        {
            tab.zoomFallbackCache = std::move(tab.cache);
            tab.zoomFallbackZoom = tab.cachedZoom;
            tab.zoomFallbackDpi = tab.cachedDpi;
            tab.zoomFallbackTheme = tab.cachedTheme;
            tab.zoomFallbackUntil = GetTickCount64() + 1800;
        }
        else if (!tab.zoomFallbackCache.empty() && tab.zoomFallbackDpi == g_dpi && tab.zoomFallbackTheme == g_theme)
        {
            tab.zoomFallbackUntil = GetTickCount64() + 1800;
        }

        tab.zoom = clampedZoom;
        ++g_viewportGeneration;
        if (!g_continuousRead) tab.currentPage = anchorPage;

        if (g_continuousRead)
        {
            int pageTop = PageTopScroll(tab, anchorPage);
            int pageH = PageH(tab, anchorPage);
            int desiredScroll = g_stage.top + pageTop + static_cast<int>(anchorY * pageH + 0.5) - clientPt.y;
            g_contentHeight = ContinuousContentHeight(tab);
            int maxScroll = std::max<int>(0, g_contentHeight - (g_stage.bottom - g_stage.top));
            g_scrollY = std::clamp(desiredScroll, 0, maxScroll);
            tab.currentPage = PageFromScroll(tab, g_scrollY);
            TouchRecentProgress(tab, false);
            SyncSidebarProgressFromScroll(hwnd, tab);
        }
        else
        {
            int pageH = PageH(tab, anchorPage);
            int desiredScroll = g_stage.top + S(28) + static_cast<int>(anchorY * pageH + 0.5) - clientPt.y;
            g_contentHeight = SinglePageContentHeight(tab);
            g_scrollY = std::clamp(desiredScroll, 0, SinglePageMaxScroll(tab));
            TouchRecentProgress(tab, false);
            KeepThumbnailSelectionVisible(hwnd, tab);
        }

        UpdateVertScrollBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        SchedulePagePrefetch(hwnd);
    }

    void CloseCustomZoomDialog(HWND hwnd)
    {
        g_customZoomOpen = false;
        g_customZoomFocused = false;
        g_customZoomText.clear();
        g_customZoomCaretVisible = true;
        KillTimer(hwnd, 17);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void OpenCustomZoomDialog(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab) return;
        int pct = static_cast<int>(tab->zoom * 100.0 + 0.5);
        g_customZoomText = std::to_wstring(std::clamp(pct, 1, 6400));
        g_customZoomOpen = true;
        g_customZoomFocused = true;
        g_customZoomCaretVisible = true;
        SetTimer(hwnd, 17, 500, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void CommitCustomZoomDialog(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab || g_customZoomText.empty())
        {
            CloseCustomZoomDialog(hwnd);
            return;
        }
        double pct = _wtof(g_customZoomText.c_str());
        pct = std::clamp(pct, 8.33, 6400.0);
        ApplyZoomAtPoint(hwnd, *tab, CurrentZoomAnchorPoint(), pct / 100.0);
        CloseCustomZoomDialog(hwnd);
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
                tab.currentPage = std::min(tab.pageCount - 1, NextSpreadStart(tab, FirstPageInSpread(tab.currentPage)));
                g_scrollY = 0;
                g_contentHeight = SinglePageContentHeight(tab);
                TouchRecentProgress(tab, false);
                KeepThumbnailSelectionVisible(hwnd, tab);
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
            int previous = 0;
            for (int start = 0; start < tab.currentPage; start = NextSpreadStart(tab, start))
                previous = start;
            tab.currentPage = previous;
            g_contentHeight = SinglePageContentHeight(tab);
            g_scrollY = SinglePageMaxScroll(tab);
            TouchRecentProgress(tab, false);
            KeepThumbnailSelectionVisible(hwnd, tab);
            UpdateVertScrollBar(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            SchedulePagePrefetch(hwnd);
        }
        return true;
    }

    bool ScrollByWheelDelta(HWND hwnd, POINT clientPt, int delta, bool ctrlDown)
    {
        PdfTab* tab = Active();
        if (g_tabListOpen && Hit(g_tabListRect, clientPt.x, clientPt.y))
        {
            int pixels = WheelPixels(delta, S(52), g_stageWheelRemainder);
            if (pixels != 0) ScrollTabListTo(hwnd, g_tabListScrollY - pixels);
            return true;
        }
        if (ctrlDown && tab)
        {
            StopTouchMomentum(hwnd);
            ApplyZoomAtPoint(hwnd, *tab, clientPt, tab->zoom + (delta > 0 ? 0.08 : -0.08));
            return true;
        }

        if (tab && (GetKeyState(VK_SHIFT) & 0x8000))
        {
            StopTouchMomentum(hwnd);
            int pixels = WheelPixels(delta, S(64), g_stageWheelRemainder);
            if (pixels != 0)
            {
                ScrollStageXTo(hwnd, g_scrollX - pixels);
            }
            return true;
        }

        if (SidebarVisible() && Hit(g_sidebar, clientPt.x, clientPt.y))
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

        if (tab && std::abs(delta) < WHEEL_DELTA)
        {
            StopTouchMomentum(hwnd);
            int pixels = WheelPixels(delta, S(48), g_stageWheelRemainder);
            if (pixels != 0)
            {
                g_smoothStageScroll = false;
                KillTimer(hwnd, 4);
                ScrollByPanPixels(hwnd, clientPt, pixels, 0);
            }
            return true;
        }

        if (tab && !g_continuousRead)
        {
            StopTouchMomentum(hwnd);
            int pixels = WheelPixels(delta, S(48), g_stageWheelRemainder);
            if (pixels != 0)
            {
                int maxScroll = SinglePageMaxScroll(*tab);
                int desired = g_scrollY - pixels;
                if (desired >= 0 && desired <= maxScroll)
                {
                    g_smoothStageScroll = false;
                    KillTimer(hwnd, 4);
                    ScrollStageTo(hwnd, desired);
                }
                else
                {
                    g_smoothStageScroll = false;
                    KillTimer(hwnd, 4);
                    ScrollSinglePageByPixels(hwnd, *tab, -pixels);
                }
            }
            return true;
        }

        if (!tab)
        {
            int pixels = WheelPixels(delta, S(52), g_stageWheelRemainder);
            if (pixels != 0) ScrollHomeTo(hwnd, g_homeScrollY - pixels);
            return true;
        }

        int pixels = WheelPixels(delta, S(48), g_stageWheelRemainder);
        if (pixels != 0)
        {
            StopTouchMomentum(hwnd);
            g_smoothStageScroll = false;
            KillTimer(hwnd, 4);
            ScrollStageTo(hwnd, g_scrollY - pixels);
        }
        return true;
    }

    bool ScrollByPanPixels(HWND hwnd, POINT clientPt, int deltaY, int deltaX)
    {
        if (deltaY == 0 && deltaX == 0) return true;
        if (g_tabListOpen && Hit(g_tabListRect, clientPt.x, clientPt.y))
        {
            if (deltaY != 0) ScrollTabListTo(hwnd, g_tabListScrollY - deltaY);
            return true;
        }
        if (SidebarVisible() && Hit(g_sidebar, clientPt.x, clientPt.y))
        {
            g_smoothSidebarScroll = false;
            KillTimer(hwnd, 5);
            if (deltaY != 0) ScrollSidebarTo(hwnd, g_sidebarScrollY - deltaY);
            return true;
        }

        PdfTab* tab = Active();
        if (!tab)
        {
            if (deltaY != 0) ScrollHomeTo(hwnd, g_homeScrollY - deltaY);
            return true;
        }
        if (tab && deltaX != 0)
        {
            ScrollStageXTo(hwnd, g_scrollX - deltaX);
        }
        if (tab && !g_continuousRead)
        {
            g_smoothStageScroll = false;
            KillTimer(hwnd, 4);
            if (deltaY != 0)
            {
                return ScrollSinglePageByPixels(hwnd, *tab, -deltaY);
            }
            return true;
        }

        g_smoothStageScroll = false;
        KillTimer(hwnd, 4);
        if (deltaY != 0)
        {
            ScrollStageTo(hwnd, g_scrollY - deltaY);
        }
        return true;
    }

    double AutoScrollAxisSpeed(int delta)
    {
        int deadZone = std::max(4, S(7));
        int distance = std::abs(delta);
        if (distance <= deadZone) return 0.0;
        double speed = static_cast<double>(distance - deadZone) / 10.0;
        speed = std::min(speed, 85.0);
        return delta < 0 ? -speed : speed;
    }

    void UpdateAutoScrollSpeed(HWND hwnd)
    {
        POINT pt{};
        if (!GetCursorPos(&pt)) return;
        ScreenToClient(hwnd, &pt);
        g_mousePos = pt;
        g_autoScrollXSpeed = AutoScrollAxisSpeed(pt.x - g_autoScrollAnchor.x);
        g_autoScrollYSpeed = AutoScrollAxisSpeed(pt.y - g_autoScrollAnchor.y);
    }

    void StopAutoScroll(HWND hwnd)
    {
        if (!g_autoScrollActive) return;
        g_autoScrollActive = false;
        g_autoScrollXSpeed = 0.0;
        g_autoScrollYSpeed = 0.0;
        g_autoScrollXAccum = 0.0;
        g_autoScrollYAccum = 0.0;
        g_autoScrollLastTick = 0;
        KillTimer(hwnd, 19);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    void ToggleAutoScroll(HWND hwnd, POINT anchor)
    {
        if (g_autoScrollActive)
        {
            StopAutoScroll(hwnd);
            return;
        }
        PdfTab* tab = Active();
        if (!tab || !Hit(g_stage, anchor.x, anchor.y)) return;
        CancelStageMotion(hwnd);
        ClearTextSelection();
        ClearAnnotationFocus();
        g_autoScrollActive = true;
        g_autoScrollAnchor = anchor;
        g_autoScrollXSpeed = 0.0;
        g_autoScrollYSpeed = 0.0;
        g_autoScrollXAccum = 0.0;
        g_autoScrollYAccum = 0.0;
        g_autoScrollLastTick = GetTickCount64();
        UpdateAutoScrollSpeed(hwnd);
        SetTimer(hwnd, 19, 10, nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }

    void StepAutoScroll(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!g_autoScrollActive || !tab)
        {
            StopAutoScroll(hwnd);
            return;
        }

        UpdateAutoScrollSpeed(hwnd);
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedMs = g_autoScrollLastTick ? now - g_autoScrollLastTick : 10;
        g_autoScrollLastTick = now;
        double scale = std::clamp(static_cast<double>(std::max<ULONGLONG>(1, elapsedMs)) / 20.0, 0.05, 3.0);
        g_autoScrollXAccum += g_autoScrollXSpeed * scale;
        g_autoScrollYAccum += g_autoScrollYSpeed * scale;
        int dx = g_autoScrollXAccum >= 0.0 ? static_cast<int>(std::floor(g_autoScrollXAccum)) : static_cast<int>(std::ceil(g_autoScrollXAccum));
        int dy = g_autoScrollYAccum >= 0.0 ? static_cast<int>(std::floor(g_autoScrollYAccum)) : static_cast<int>(std::ceil(g_autoScrollYAccum));
        g_autoScrollXAccum -= static_cast<double>(dx);
        g_autoScrollYAccum -= static_cast<double>(dy);

        if (dx != 0)
            ScrollStageXTo(hwnd, g_scrollX + dx);
        if (dy != 0)
        {
            if (g_continuousRead) ScrollStageTo(hwnd, g_scrollY + dy, true, false);
            else ScrollSinglePageByPixels(hwnd, *tab, dy);
        }
    }
    void QueueTouchPanPixels(HWND hwnd, POINT clientPt, int deltaY, int deltaX)
    {
        if (deltaY == 0 && deltaX == 0) return;
        g_touchPanPendingY += deltaY;
        g_touchPanPendingX += deltaX;
        g_touchPanPoint = clientPt;
        if (!g_touchPanQueued)
        {
            g_touchPanQueued = true;
            SetTimer(hwnd, 16, 8, nullptr);
        }
    }

    void FlushTouchPan(HWND hwnd)
    {
        if (!g_touchPanQueued) return;
        int dx = g_touchPanPendingX;
        int dy = g_touchPanPendingY;
        g_touchPanPendingX = 0;
        g_touchPanPendingY = 0;
        g_touchPanQueued = false;
        KillTimer(hwnd, 16);
        if (dx != 0 || dy != 0)
        {
            ScrollByPanPixels(hwnd, g_touchPanPoint, dy, dx);
            FlushStageForTouch(hwnd, dy);
            AddTouchMomentumImpulse(hwnd, g_touchPanPoint, dy, dx);
        }
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

    void SaveActiveTabProgress()
    {
        PdfTab* tab = Active();
        if (!tab) return;
        tab->savedScrollY = g_scrollY;
        if (g_continuousRead)
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
        TouchRecentProgress(*tab, false);
    }

    void RestoreActiveTabPosition(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab)
        {
            g_scrollY = 0;
            return;
        }

        if (g_continuousRead)
        {
            g_contentHeight = ContinuousContentHeight(*tab);
            int maxScroll = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
            g_scrollY = std::clamp(tab->savedScrollY, 0, maxScroll);
            tab->currentPage = PageFromScroll(*tab, g_scrollY);
        }
        else
        {
            g_contentHeight = SinglePageContentHeight(*tab);
            g_scrollY = std::clamp(tab->savedScrollY, 0, SinglePageMaxScroll(*tab));
        }
        UpdateVertScrollBar(hwnd);
        SchedulePagePrefetch(hwnd);
    }

    void ActivateTab(HWND hwnd, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_tabs.size()))
            return;
        if (g_active != index)
            SaveActiveTabProgress();
        g_active = index;
        EnsureTabWindowVisible(g_active);
        g_tabListOpen = false;
        g_selectedOutlineRow = -1;
        RestoreActiveTabPosition(hwnd);
        SetTitle();
        StartActiveFileWatcher(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void StartSmoothStageScroll(HWND hwnd, int target)
    {
        g_stageScrollTarget = ClampStageScrollValue(target);
        if (!g_smoothStageScroll)
        {
            g_stageScrollPos = static_cast<double>(g_scrollY);
            g_stageScrollVelocity = 0.0;
        }
        g_stageScrollLastTick = GetTickCount64();
        g_smoothStageScroll = true;
        SetTimer(hwnd, 4, 16, nullptr);
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
        return SidebarVisible() && Active() &&
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
        HPEN pen = CreatePen(PS_SOLID, 1, ChromeDividerColor());
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
        add(CmdHome, L"\u9996\u9875", 42, L"");
        add(CmdOpen, L"\u6253\u5f00", 42, L"");
        add(CmdSearch, L"\u67e5\u627e", 42, L"");
        add(CmdBookmark, L"\u4e66\u7b7e", 42, L"");
        add(CmdTheme, L"\u4e3b\u9898", 42, L"");
        add(CmdView, L"\u89c6\u56fe", 42, L"");
        add(CmdSettings, L"\u5e2e\u52a9", 42, L"");
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintTabs(HDC dc, RECT client)
    {
        g_tabRects.clear();
        g_tabRectIds.clear();
        g_tabTitleTruncated.clear();
        g_tabNavPrevRect = {};
        g_tabNavNextRect = {};
        g_tabCountRect = {};
        if (g_immersive) return;
        RECT bar{ 0, S(38), client.right, S(76) };
        GlassSurface(dc, bar, 142);
        HPEN pen = CreatePen(PS_SOLID, 1, ChromeDividerColor());
        auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, bar.bottom - 1, nullptr);
        LineTo(dc, client.right, bar.bottom - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        HFONT f = Font(9);
        auto oldFont = SelectObject(dc, f);
        int top = S(46);
        int bottom = S(71);
        int left = S(8);
        int gap = S(6);
        int countW = S(60);
        int plusW = S(26);
        int navW = S(24);
        g_tabCountRect = RECT{ client.right - S(10) - countW, top, client.right - S(10), bottom };
        bool showNav = static_cast<int>(g_tabs.size()) > MaxVisibleTabs;
        if (showNav)
        {
            g_tabNavNextRect = RECT{ g_tabCountRect.left - S(8) - navW, top, g_tabCountRect.left - S(8), bottom };
            g_tabNavPrevRect = RECT{ g_tabNavNextRect.left - S(4) - navW, top, g_tabNavNextRect.left - S(4), bottom };
        }
        int controlsLeft = showNav ? g_tabNavPrevRect.left : g_tabCountRect.left;
        int tabRight = controlsLeft - S(12) - plusW - gap;
        int totalTabs = static_cast<int>(g_tabs.size());
        int visibleCount = std::min(totalTabs, MaxVisibleTabs);
        EnsureTabWindowVisible(g_active);
        int start = std::clamp(g_tabVisibleStart, 0, std::max(0, totalTabs - visibleCount));
        int tabW = visibleCount > 0
            ? std::clamp((tabRight - left - gap * std::max(0, visibleCount - 1)) / visibleCount, S(98), S(170))
            : S(138);
        int plusLeft = visibleCount > 0 ? left + visibleCount * (tabW + gap) : left;
        plusLeft = std::clamp(plusLeft, left, controlsLeft - S(12) - plusW);
        RECT plusRect{ plusLeft, top, plusLeft + plusW, bottom };

        std::vector<int> tabSlots;
        bool draggingVisible = g_tabDragFloating &&
            g_draggingTabIndex >= start &&
            g_draggingTabIndex < start + visibleCount &&
            g_draggingTabIndex >= 0 &&
            g_draggingTabIndex < totalTabs;
        if (draggingVisible)
        {
            int insertAt = std::clamp(g_tabDragInsertIndex, start, start + visibleCount);
            for (int actual = start; actual < start + visibleCount && actual < totalTabs; ++actual)
            {
                if (actual == insertAt) tabSlots.push_back(-1);
                if (actual != g_draggingTabIndex) tabSlots.push_back(actual);
            }
            if (insertAt == start + visibleCount) tabSlots.push_back(-1);
            while (static_cast<int>(tabSlots.size()) > visibleCount) tabSlots.pop_back();
        }
        else
        {
            for (int actual = start; actual < start + visibleCount && actual < totalTabs; ++actual)
                tabSlots.push_back(actual);
        }

        for (int i = 0; i < visibleCount && i < static_cast<int>(tabSlots.size()); ++i)
        {
            int actualIndex = tabSlots[static_cast<size_t>(i)];
            int x = left + i * (tabW + gap);
            RECT r{ x, top, x + tabW, bottom };
            if (actualIndex < 0)
            {
                COLORREF gapFill = MixColor(T().panel, T().stage, IsDarkTheme() || IsAuroraTheme() ? 0.34 : 0.58);
                COLORREF gapLine = MixColor(T().line, T().panel, IsDarkTheme() || IsAuroraTheme() ? 0.30 : 0.42);
                RoundFill(dc, r, S(9), gapFill, gapLine);
                continue;
            }
            if (actualIndex >= totalTabs) break;
            g_tabRects.push_back(r);
            g_tabRectIds.push_back(actualIndex);
            bool active = actualIndex == g_active;
            bool hovered = actualIndex == g_hoverTab;
            bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &r);
            if (IsGlassTheme() || IsAuroraTheme())
            {
                if (active || hovered || pressed)
                {
                    if (IsAuroraTheme())
                    {
                        COLORREF tabFill = pressed ? MixColor(T().panel2, T().accent, 0.075) : (active ? MixColor(T().panel2, T().accent, 0.055) : SidebarHoverFill());
                        COLORREF tabLine = pressed ? MixColor(T().line, T().accent, 0.26) : (active ? MixColor(T().line, T().accent, 0.18) : MixColor(T().line, T().panel, 0.22));
                        RoundFill(dc, r, S(9), tabFill, tabLine);
                        UNREFERENCED_PARAMETER(pressed);
                    }
                    else
                    {
                        AlphaFill(dc, r, RGB(255, 255, 255), pressed ? 196 : (active ? 168 : 112));
                        RoundFill(dc, r, S(9),
                            pressed ? RGB(245, 249, 252) : (active ? RGB(250, 252, 254) : RGB(255, 255, 255)),
                            pressed ? RGB(198, 208, 220) : (active ? RGB(218, 224, 232) : RGB(230, 235, 241)));
                        AlphaFill(dc, RECT{ r.left + S(1), r.top + S(1), r.right - S(1), r.top + S(8) }, RGB(255, 255, 255), pressed ? 48 : 36);
                    }
                }
                else
                {
                    if (IsAuroraTheme())
                    {
                        RoundFill(dc, r, S(9), T().panel, MixColor(T().line, T().panel, 0.28));
                    }
                    else
                    {
                        AlphaFill(dc, r, RGB(255, 255, 255), 82);
                        RoundFill(dc, r, S(9), RGB(255, 255, 255), RGB(230, 235, 241));
                    }
                }
            }
            else
            {
                COLORREF fill = pressed
                    ? MixColor(T().panel2, T().accent, IsDarkTheme() ? 0.24 : 0.10)
                    : ((active || hovered) ? T().panel2 : T().panel);
                COLORREF line = pressed
                    ? MixColor(T().line, T().accent, 0.34)
                    : ((active || hovered) ? T().line : T().panel);
                RoundFill(dc, r, S(9), fill, line);
            }
            PdfTab const& tab = *g_tabs[static_cast<size_t>(actualIndex)];
            bool dirty = tab.dirty;
            RECT text{ r.left + S(11), r.top, r.right - S(dirty ? 42 : 28), r.bottom };
            Text(dc, tab.name, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                pressed ? MixColor(T().text, T().accent, 0.12) : T().text);
            SIZE nameSize{};
            GetTextExtentPoint32W(dc, tab.name.c_str(), static_cast<int>(tab.name.size()), &nameSize);
            g_tabTitleTruncated.push_back(nameSize.cx > std::max<int>(0, static_cast<int>(text.right - text.left)));
            if (dirty)
            {
                RECT mark{ r.right - S(42), r.top + S(2), r.right - S(27), r.bottom - S(2) };
                HFONT markFont = Font(10, FW_SEMIBOLD);
                HGDIOBJ oldMarkFont = SelectObject(dc, markFont);
                Text(dc, L"*", mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE, IsDarkTheme() || IsAuroraTheme() ? RGB(255, 210, 92) : RGB(216, 143, 35));
                SelectObject(dc, oldMarkFont);
                DeleteObject(markFont);
            }
            RECT close{ r.right - S(24), r.top + S(2), r.right - S(4), r.bottom - S(2) };
            bool closeHover = g_hoverTabClose == actualIndex;
            int closeCx = (close.left + close.right) / 2;
            int closeCy = (close.top + close.bottom) / 2;
            if (closeHover)
            {
                DrawFilledCircle(dc, closeCx, closeCy, S(6.7f), RGB(255, 92, 92));
                DrawCloseGlyph(dc, close, RGB(255, 255, 255));
            }
            else if (hovered)
            {
                DrawCloseGlyph(dc, close, MixColor(T().muted, T().text, 0.12));
            }
            else if (active)
            {
                DrawFilledCircle(dc, closeCx, closeCy, S(3.2f), T().accent);
            }
            if (false && closeHover)
            {
                RoundFill(dc, close, S(14), RGB(255, 92, 92), RGB(255, 92, 92));
                Text(dc, L"\u00d7", close, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255));
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
        }

        auto drawUtilityButton = [&](RECT r, bool active)
        {
            bool hover = Hit(r, g_mousePos.x, g_mousePos.y);
            bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &r);
            COLORREF fill = active ? MixColor(T().panel2, T().accent, IsDarkTheme() || IsAuroraTheme() ? 0.24 : 0.10) : T().panel;
            COLORREF line = active ? MixColor(T().line, T().accent, 0.40) : T().line;
            if (IsGlassTheme() || IsAuroraTheme())
            {
                BYTE alpha = active ? 176 : (hover ? 140 : 88);
                if (IsAuroraTheme())
                {
                    COLORREF utilFill = active ? MixColor(T().panel2, T().accent, 0.06) : (hover ? SidebarHoverFill() : T().panel);
                    COLORREF utilLine = active ? MixColor(T().line, T().accent, 0.20) : MixColor(T().line, T().panel, 0.22);
                    RoundFill(dc, r, S(9), utilFill, utilLine);
                }
                else
                {
                    AlphaFill(dc, r, RGB(255, 255, 255), alpha);
                    RoundFill(dc, r, S(9), active ? RGB(252, 254, 255) : RGB(255, 255, 255), active ? RGB(212, 220, 228) : RGB(228, 233, 239));
                }
            }
            else
            {
                RoundFill(dc, r, S(9), pressed ? T().panel2 : (hover ? T().panel2 : fill), hover ? MixColor(line, T().text, 0.08) : line);
            }
        };

        AddButton(CmdOpen, plusRect, L"+", L"\u6253\u5f00\u65b0\u6587\u6863");
        drawUtilityButton(plusRect, false);
        DrawPlusGlyph(dc, plusRect, T().muted);

        if (showNav)
        {
            AddButton(CmdTabNavPrev, g_tabNavPrevRect, L"", L"\u4e0a\u4e00\u4e2a\u6807\u7b7e\u9875");
            AddButton(CmdTabNavNext, g_tabNavNextRect, L"", L"\u4e0b\u4e00\u4e2a\u6807\u7b7e\u9875");
            drawUtilityButton(g_tabNavPrevRect, false);
            drawUtilityButton(g_tabNavNextRect, false);
            DrawTabArrowGlyph(dc, g_tabNavPrevRect, false, T().muted);
            DrawTabArrowGlyph(dc, g_tabNavNextRect, true, T().muted);
        }

        AddButton(CmdTabListToggle, g_tabCountRect, L"", L"\u5168\u90e8\u6807\u7b7e\u9875");
        drawUtilityButton(g_tabCountRect, g_tabListOpen);
        RECT countText{ g_tabCountRect.left + S(6), g_tabCountRect.top, g_tabCountRect.right - S(18), g_tabCountRect.bottom };
        HFONT countFont = Font(9);
        HGDIOBJ oldCountFont = SelectObject(dc, countFont);
        Text(dc, std::to_wstring(totalTabs), countText, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldCountFont);
        DeleteObject(countFont);
        DrawDownCaret(dc, g_tabCountRect.right - S(12), (g_tabCountRect.top + g_tabCountRect.bottom) / 2, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }


    void PaintTabDragOverlay(HDC dc)
    {
        int totalTabs = static_cast<int>(g_tabs.size());
        if (!(g_tabDragFloating && g_draggingTabIndex >= 0 && g_draggingTabIndex < totalTabs))
            return;

        RECT r = g_tabDragCurrentRect;
        OffsetRect(&r, 0, g_tabDetachArmed ? S(2) : 0);
        DrawRoundedShadow(dc, r, S(10.0f), S(2), S(5), S(8), IsDarkTheme() || IsAuroraTheme() ? 54 : 32);
        COLORREF floatFill = IsAuroraTheme() ? MixColor(T().panel2, T().accent, 0.06) : T().panel2;
        COLORREF floatLine = MixColor(T().line, T().accent, g_tabDetachArmed ? 0.46 : 0.22);
        RoundFill(dc, r, S(9), floatFill, floatLine);
        PdfTab const& tab = *g_tabs[static_cast<size_t>(g_draggingTabIndex)];
        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        RECT textRect{ r.left + S(11), r.top, r.right - S(tab.dirty ? 42 : 28), r.bottom };
        Text(dc, tab.name, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
        if (tab.dirty)
        {
            RECT mark{ r.right - S(42), r.top + S(2), r.right - S(27), r.bottom - S(2) };
            HFONT markFont = Font(10, FW_SEMIBOLD);
            HGDIOBJ oldMarkFont = SelectObject(dc, markFont);
            Text(dc, L"*", mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE, IsDarkTheme() || IsAuroraTheme() ? RGB(255, 210, 92) : RGB(216, 143, 35));
            SelectObject(dc, oldMarkFont);
            DeleteObject(markFont);
        }
        RECT close{ r.right - S(24), r.top + S(2), r.right - S(4), r.bottom - S(2) };
        DrawSoftCircle(dc, (close.left + close.right) / 2, (close.top + close.bottom) / 2, S(3), T().accent);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintTabListOverlay(HDC dc, RECT client)
    {
        g_tabListItemRects.clear();
        g_tabListItemIds.clear();
        g_tabListRect = {};
        g_tabListScrollTrackRect = {};
        g_tabListScrollThumbRect = {};
        g_tabListContentHeight = 0;
        if (!g_tabListOpen || g_immersive || g_tabs.empty()) return;

        int rowH = S(31);
        int panelW = std::clamp<int>(client.right / 3, S(240), S(332));
        int maxHeight = std::max<int>(S(140), static_cast<int>(client.bottom - g_tabCountRect.bottom - S(18)));
        g_tabListContentHeight = static_cast<int>(g_tabs.size()) * rowH + S(12);
        int panelH = std::min(maxHeight, std::max<int>(S(56), g_tabListContentHeight));
        int left = std::max<int>(S(8), static_cast<int>(g_tabCountRect.right - panelW));
        g_tabListRect = RECT{ left, g_tabCountRect.bottom + S(6), left + panelW, g_tabCountRect.bottom + S(6) + panelH };
        g_tabListScrollY = ClampTabListScrollValue(client, g_tabListScrollY);

        DrawRoundedShadow(dc, g_tabListRect, S(10.0f), S(1), S(4), S(5), IsDarkTheme() || IsAuroraTheme() ? 42 : 22);
        GlassRoundedSurface(dc, g_tabListRect, IsDarkTheme() || IsAuroraTheme() ? 224 : 242, S(11.0f));

        RECT content{ g_tabListRect.left + S(8), g_tabListRect.top + S(6), g_tabListRect.right - S(8), g_tabListRect.bottom - S(6) };
        bool needsScroll = g_tabListContentHeight > (content.bottom - content.top);
        if (needsScroll) content.right -= S(10);

        int saved = SaveDC(dc);
        IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
        HFONT font = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, font);
        int y = content.top - g_tabListScrollY;
        for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        {
            RECT row{ content.left, y, content.right, y + rowH - S(2) };
            if (row.bottom >= content.top - rowH && row.top <= content.bottom)
            {
                g_tabListItemRects.push_back(row);
                g_tabListItemIds.push_back(i);
                bool active = i == g_active;
                bool hover = i == g_hoverTabListItem;
                if (active || hover)
                {
                    COLORREF fill = active ? MixColor(T().panel2, T().accent, IsDarkTheme() || IsAuroraTheme() ? 0.22 : 0.10) : SidebarHoverFill();
                    COLORREF line = active ? MixColor(T().line, T().accent, 0.36) : fill;
                    RoundFill(dc, row, S(8), fill, line);
                }
                RECT mark{ row.left + S(10), row.top + S(8), row.left + S(20), row.bottom - S(8) };
                if (active) DrawSoftCircle(dc, (mark.left + mark.right) / 2, (mark.top + mark.bottom) / 2, S(3), T().accent);
                RECT text{ row.left + S(24), row.top, row.right - S(12), row.bottom };
                Text(dc, g_tabs[static_cast<size_t>(i)]->name, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            }
            y += rowH;
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        RestoreDC(dc, saved);

        if (needsScroll)
        {
            g_tabListScrollTrackRect = RECT{ g_tabListRect.right - S(10), content.top, g_tabListRect.right - S(6), content.bottom };
            int trackH = std::max<int>(1, g_tabListScrollTrackRect.bottom - g_tabListScrollTrackRect.top);
            int thumbH = std::clamp(MulDiv(content.bottom - content.top, trackH, std::max(1, g_tabListContentHeight)), S(28), trackH);
            int maxScroll = std::max<int>(1, g_tabListContentHeight - (content.bottom - content.top));
            int travel = std::max<int>(0, trackH - thumbH);
            int thumbTop = g_tabListScrollTrackRect.top + MulDiv(g_tabListScrollY, travel, maxScroll);
            g_tabListScrollThumbRect = RECT{ g_tabListScrollTrackRect.left, thumbTop, g_tabListScrollTrackRect.right, thumbTop + thumbH };
            bool active = g_draggingTabListScroll || Hit(g_tabListRect, g_mousePos.x, g_mousePos.y);
            int centerX = (g_tabListScrollTrackRect.left + g_tabListScrollTrackRect.right) / 2;
            int visualW = active ? S(4) : std::max(2, S(2));
            RECT thumb{ centerX - visualW / 2, g_tabListScrollThumbRect.top, centerX + (visualW + 1) / 2, g_tabListScrollThumbRect.bottom };
            RoundFill(dc, thumb, S(5), ScrollThumbColor(active), ScrollThumbColor(active));
        }
    }

    void PaintHome(HDC dc, RECT client)
    {
        g_recentRects.clear();
        HFONT titleFont = Font(24, FW_SEMIBOLD);
        auto oldFont = SelectObject(dc, titleFont);
        RECT title{ S(42), S(118), client.right - S(42), S(164) };
        Text(dc, L"\u6700\u8fd1\u9605\u8bfb", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);
        HFONT f = Font(10);
        oldFont = SelectObject(dc, f);
        RECT openCard{ S(42), S(188), S(212), S(398) };
        g_recentRects.push_back(openCard);
        RoundFill(dc, openCard, S(8), T().panel2, T().line);
        RECT openText{ openCard.left + S(16), openCard.top + S(72), openCard.right - S(16), openCard.bottom - S(20) };
        Text(dc, L"\u6253\u5f00 PDF\n\n\u62d6\u5165\u6587\u4ef6\u6216\u70b9\u51fb\u8fd9\u91cc\u5f00\u59cb\u9605\u8bfb", openText, DT_CENTER | DT_WORDBREAK, T().text);
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
            Text(dc, L"\u7b2c" + std::to_wstring(g_recent[i].page) + L" \u9875", meta, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
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

    void AddHomeHit(RECT r, int recentIndex, HomeAction action, std::wstring tip)
    {
        g_homeHits.push_back(HomeHit{ r, recentIndex, action, std::move(tip) });
    }

    bool HomeHitHovered(size_t index)
    {
        return g_hoverHomeHit == static_cast<int>(index);
    }

    void DrawHomeIconButton(HDC dc, RECT hot, HomeAction action, bool active, size_t hitIndex, bool visible = false, int iconInset = 6, bool hoverBackground = true)
    {
        bool hover = HomeHitHovered(hitIndex);
        bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &hot);
        if (!visible && !hover && !active && !pressed) return;
        COLORREF fill = pressed ? MixColor(T().accent, T().paper, IsDarkTheme() ? 0.34 : 0.70)
            : (active ? MixColor(T().accent, T().paper, IsDarkTheme() ? 0.24 : 0.78) : (hover ? SidebarHoverFill() : T().panel2));
        COLORREF line = fill;
        if (action == HomeAction::Remove)
        {
            fill = pressed ? RGB(255, 224, 224) : (hover ? RGB(255, 238, 238) : T().panel2);
            line = fill;
        }
        if (pressed || active || (hover && hoverBackground)) RoundFill(dc, hot, S(7), fill, line);
        RECT icon{ hot.left + S(iconInset), hot.top + S(iconInset), hot.right - S(iconInset), hot.bottom - S(iconInset) };
        COLORREF color = action == HomeAction::Remove
            ? (hover || pressed ? RGB(238, 73, 78) : RGB(225, 92, 92))
            : (pressed || hover || active ? T().accent : SidebarIconColor());
        DrawHomeIcon(dc, icon, action, color, active);
    }

    void DrawHomeTextButton(HDC dc, RECT r, HomeAction action, const std::wstring& label, size_t hitIndex)
    {
        bool hover = HomeHitHovered(hitIndex);
        if (hover) RoundFill(dc, r, S(8), SidebarHoverFill(), SidebarHoverFill());
        RECT icon{ r.left + S(8), r.top + S(7), r.left + S(26), r.bottom - S(7) };
        DrawHomeIcon(dc, icon, action, SidebarIconColor());
        RECT text{ r.left + S(31), r.top, r.right - S(8), r.bottom };
        Text(dc, label, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().muted);
    }

    void PaintHomeHeader(HDC dc, RECT client)
    {
        HFONT titleFont = Font(22, FW_SEMIBOLD);
        HGDIOBJ oldFont = SelectObject(dc, titleFont);
        RECT title{ S(42), S(112), client.right - S(42), S(154) };
        Text(dc, L"\u6700\u8fd1\u9605\u8bfb", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);

        HFONT f = Font(9);
        oldFont = SelectObject(dc, f);
        g_homeSearchRect = RECT{ S(42), S(158), std::min<int>(client.right - S(42), S(560)), S(204) };
        RoundFill(dc, g_homeSearchRect, S(10), T().paper, g_homeSearchFocused ? T().accent : T().line);
        RECT searchIcon{ g_homeSearchRect.left + S(14), g_homeSearchRect.top + S(13), g_homeSearchRect.left + S(31), g_homeSearchRect.bottom - S(13) };
        DrawSideTabIcon(dc, searchIcon, SideTabIcon::Search, MixColor(T().muted, T().text, 0.06));
        g_homeSearchClearRect = RECT{ g_homeSearchRect.right - S(36), g_homeSearchRect.top + S(9), g_homeSearchRect.right - S(8), g_homeSearchRect.bottom - S(9) };
        RECT inputText{ g_homeSearchRect.left + S(40), g_homeSearchRect.top, g_homeSearchRect.right - (g_homeSearchQuery.empty() ? S(14) : S(42)), g_homeSearchRect.bottom };
        HFONT searchFont = Font(11);
        HGDIOBJ oldSearchFont = SelectObject(dc, searchFont);
        std::wstring shown = g_homeSearchQuery.empty() ? L"\u641c\u7d22\u6587\u6863" : g_homeSearchQuery;
        Text(dc, shown, inputText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
            g_homeSearchQuery.empty() ? MixColor(T().muted, T().paper, 0.16) : T().text);
        if (g_homeSearchFocused && g_homeSearchCaretVisible)
        {
            SIZE caretSize{};
            GetTextExtentPoint32W(dc, g_homeSearchQuery.c_str(), static_cast<int>(g_homeSearchQuery.size()), &caretSize);
            int caretX = std::min(inputText.right - S(2), inputText.left + caretSize.cx + S(2));
            HPEN caretPen = CreatePen(PS_SOLID, std::max(1, S(1)), T().text);
            HGDIOBJ oldCaret = SelectObject(dc, caretPen);
            MoveToEx(dc, caretX, g_homeSearchRect.top + S(11), nullptr);
            LineTo(dc, caretX, g_homeSearchRect.bottom - S(11));
            SelectObject(dc, oldCaret);
            DeleteObject(caretPen);
        }
        if (!g_homeSearchQuery.empty())
        {
            if (Hit(g_homeSearchClearRect, g_mousePos.x, g_mousePos.y)) RoundFill(dc, g_homeSearchClearRect, S(7), SidebarHoverFill(), SidebarHoverFill());
            Text(dc, L"\u00d7", g_homeSearchClearRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        }
        SelectObject(dc, oldSearchFont);
        DeleteObject(searchFont);

        int right = client.right - S(54);
        RECT clear{ right - S(112), S(168), right, S(200) };
        AddHomeHit(clear, -1, HomeAction::ClearAll, L"清除记录");
        size_t clearIndex = g_homeHits.size() - 1;
        DrawHomeTextButton(dc, clear, HomeAction::ClearAll, L"清除记录", clearIndex);
        RECT view{ clear.left - S(126), S(168), clear.left - S(12), S(200) };
        AddHomeHit(view, -1, HomeAction::ToggleView, g_homeListView ? L"\u5207\u6362\u89c6\u56fe" : L"\u5207\u6362\u5217\u8868");
        size_t viewIndex = g_homeHits.size() - 1;
        DrawHomeTextButton(dc, view, HomeAction::ToggleView, g_homeListView ? L"\u5207\u6362\u89c6\u56fe" : L"\u5207\u6362\u5217\u8868", viewIndex);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintHomeGrid(HDC dc, RECT client)
    {
        g_recentRects.clear();
        g_homeHits.clear();
        g_homeRecentIds = FilteredHomeRecentIds();
        g_homeContentHeight = ComputeHomeGridContentHeight(client, g_homeRecentIds.size());
        g_homeScrollY = ClampHomeScrollValue(client, g_homeScrollY);
        PaintHomeHeader(dc, client);
        RECT viewport = HomeContentViewportRect(client);

        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        int tileW = S(152);
        int coverH = S(198);
        int nameH = S(38);
        int tileH = coverH + nameH + S(10);
        int gapX = S(28);
        int gapY = S(28);
        int startX = S(42);
        int x = startX;
        int y = viewport.top + S(16) - g_homeScrollY;
        int lastBottom = y;
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
            DrawContactShadow(dc, cover, active ? 30 : 22);
            DrawRoundedShadow(dc, cover, S(5.0f), S(1), S(2), S(3), active ? 22 : 14);
            RoundFill(dc, cover, S(5), T().paper, active ? T().accent : T().line);
        };

        int saved = SaveDC(dc);
        IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);

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
        Text(dc, L"\u6253\u5f00\u6587\u6863", openName, DT_CENTER | DT_TOP | DT_SINGLELINE, T().muted);
        lastBottom = std::max(lastBottom, static_cast<int>(openTile.bottom));
        advance();

        for (size_t displayIndex = 0; displayIndex < g_homeRecentIds.size(); ++displayIndex)
        {
            int recentIndex = g_homeRecentIds[displayIndex];
            RecentItem const& item = g_recent[static_cast<size_t>(recentIndex)];
            RECT tile{ x, y, x + tileW, y + tileH };
            g_recentRects.push_back(tile);
            RECT cover{ tile.left, tile.top, tile.right, tile.top + coverH };
            bool hovered = Hit(cover, g_mousePos.x, g_mousePos.y);
            drawCoverFrame(cover, hovered || item.pinned);
            RECT inner{ cover.left + S(7), cover.top + S(7), cover.right - S(7), cover.bottom - S(7) };
            PageImage* thumb = LoadHomeThumb(item, inner.right - inner.left);
            if (thumb && !thumb->bgra.empty()) DrawImageFit(dc, *thumb, inner);
            else
            {
                HFONT pdfFont = Font(18, FW_SEMIBOLD);
                HGDIOBJ oldPdfFont = SelectObject(dc, pdfFont);
                Text(dc, L"PDF", inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().accent);
                SelectObject(dc, oldPdfFont);
                DeleteObject(pdfFont);
            }

            RECT starHot{ cover.right - S(39), cover.top + S(9), cover.right - S(9), cover.top + S(39) };
            AddHomeHit(starHot, recentIndex, HomeAction::Favorite, item.favorite ? L"取消收藏" : L"收藏");
            DrawHomeIconButton(dc, starHot, HomeAction::Favorite, item.favorite, g_homeHits.size() - 1, hovered, 5, false);

            RECT dock{ cover.left + S(17), cover.bottom - S(45), cover.left + S(120), cover.bottom - S(13) };
            if (hovered || item.pinned)
            {
                GlassRoundedSurface(dc, dock, IsDarkTheme() || IsAuroraTheme() ? 196 : 208, S(11.0f));
            }
            HomeAction actions[] = { HomeAction::Pin, HomeAction::Folder, HomeAction::Remove };
            const wchar_t* tips[] = { item.pinned ? L"\u53d6\u6d88\u7f6e\u9876" : L"\u7f6e\u9876", L"\u6253\u5f00\u6587\u4ef6\u5939", L"\u5220\u9664\u8bb0\u5f55" };
            for (int a = 0; a < 3; ++a)
            {
                RECT hot{ dock.left + S(5) + a * S(32), dock.top + S(3), dock.left + S(5) + a * S(32) + S(26), dock.top + S(29) };
                AddHomeHit(hot, recentIndex, actions[a], tips[a]);
                DrawHomeIconButton(dc, hot, actions[a], actions[a] == HomeAction::Pin && item.pinned, g_homeHits.size() - 1, hovered, 4, false);
            }

            RECT name{ tile.left, cover.bottom + S(8), tile.right, tile.bottom };
            Text(dc, item.name, name, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, T().text);
            lastBottom = std::max(lastBottom, static_cast<int>(tile.bottom));
            advance();
        }
        if (!g_homeSearchQuery.empty() && g_homeRecentIds.empty())
        {
            RECT empty{ S(58), viewport.top + S(32) - g_homeScrollY, client.right - S(58), viewport.top + S(90) - g_homeScrollY };
            Text(dc, L"\u6ca1\u6709\u627e\u5230\u5339\u914d\u7684\u6587\u6863", empty, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            lastBottom = std::max(lastBottom, static_cast<int>(empty.bottom));
        }
        RestoreDC(dc, saved);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintHomeList(HDC dc, RECT client)
    {
        g_recentRects.clear();
        g_homeHits.clear();
        g_homeRecentIds = FilteredHomeRecentIds();
        g_homeContentHeight = ComputeHomeListContentHeight(g_homeRecentIds.size(), !g_homeSearchQuery.empty() && g_homeRecentIds.empty());
        g_homeScrollY = ClampHomeScrollValue(client, g_homeScrollY);
        PaintHomeHeader(dc, client);
        RECT viewport = HomeContentViewportRect(client);

        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        int left = S(54);
        int headerTop = viewport.top + S(8);
        int rowH = S(48);
        int actionX = client.right - S(188);
        int sizeX = actionX - S(126);
        int nameW = std::max<int>(S(300), (sizeX - left) * 34 / 100);
        int dateX = left + nameW;
        int dirX = dateX + S(180);
        Text(dc, L"\u6587\u6863\u540d\u79f0", RECT{ left, headerTop, dateX - S(20), headerTop + S(28) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        Text(dc, L"\u9605\u8bfb\u65f6\u95f4", RECT{ dateX, headerTop, dirX - S(20), headerTop + S(28) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        Text(dc, L"\u6240\u5728\u76ee\u5f55", RECT{ dirX, headerTop, sizeX - S(20), headerTop + S(28) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        Text(dc, L"\u5927\u5c0f", RECT{ sizeX, headerTop, actionX - S(16), headerTop + S(28) }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        RECT rowsViewport{ viewport.left, headerTop + S(34), viewport.right, viewport.bottom };
        int y = rowsViewport.top - g_homeScrollY;
        int lastBottom = y;
        g_recentRects.push_back(RECT{});
        int saved = SaveDC(dc);
        IntersectClipRect(dc, rowsViewport.left, rowsViewport.top, rowsViewport.right, rowsViewport.bottom);
        for (size_t displayIndex = 0; displayIndex < g_homeRecentIds.size(); ++displayIndex)
        {
            int recentIndex = g_homeRecentIds[displayIndex];
            RecentItem const& item = g_recent[static_cast<size_t>(recentIndex)];
            RECT row{ left - S(10), y, client.right - S(44), y + rowH };
            g_recentRects.push_back(row);
            bool rowHover = Hit(row, g_mousePos.x, g_mousePos.y);
            if (rowHover) RoundFill(dc, row, S(7), SidebarHoverFill(), SidebarHoverFill());
            RECT name{ left, row.top, dateX - S(20), row.bottom };
            Text(dc, item.name, name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            Text(dc, FormatDate(item.openedAt), RECT{ dateX, row.top, dirX - S(20), row.bottom }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            Text(dc, std::filesystem::path(item.path).parent_path().wstring(), RECT{ dirX, row.top, sizeX - S(20), row.bottom }, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().muted);
            Text(dc, FormatFileSize(item.path), RECT{ sizeX, row.top, actionX - S(16), row.bottom }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            HomeAction actions[] = { HomeAction::Pin, HomeAction::Folder, HomeAction::Favorite, HomeAction::Remove };
            const wchar_t* tips[] = { item.pinned ? L"\u53d6\u6d88\u7f6e\u9876" : L"\u7f6e\u9876", L"\u6253\u5f00\u6587\u4ef6\u5939", item.favorite ? L"\u53d6\u6d88\u6536\u85cf" : L"\u6536\u85cf", L"\u5220\u9664\u8bb0\u5f55" };
            for (int a = 0; a < 4; ++a)
            {
                RECT hot{ actionX + a * S(38), row.top + S(8), actionX + a * S(38) + S(30), row.top + S(38) };
                AddHomeHit(hot, recentIndex, actions[a], tips[a]);
                bool active = (actions[a] == HomeAction::Pin && item.pinned) || (actions[a] == HomeAction::Favorite && item.favorite);
                DrawHomeIconButton(dc, hot, actions[a], active, g_homeHits.size() - 1, rowHover, 5, true);
            }
            lastBottom = std::max(lastBottom, static_cast<int>(row.bottom));
            y += rowH;
        }
        if (!g_homeSearchQuery.empty() && g_homeRecentIds.empty())
        {
            RECT empty{ left, rowsViewport.top + S(8) - g_homeScrollY, client.right - S(54), rowsViewport.top + S(64) - g_homeScrollY };
            Text(dc, L"\u6ca1\u6709\u627e\u5230\u5339\u914d\u7684\u6587\u6863", empty, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            lastBottom = std::max(lastBottom, static_cast<int>(empty.bottom));
        }
        RestoreDC(dc, saved);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    void PaintClearRecentDialog(HDC dc, RECT client)
    {
        if (!g_confirmClearRecent) return;
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush scrim(GdiColor(RGB(20, 24, 28), IsDarkTheme() ? 128 : 82));
        g.FillRectangle(&scrim, 0, 0, client.right, client.bottom);

        int w = S(420);
        int h = S(214);
        RECT panel{ (client.right - w) / 2, (client.bottom - h) / 2, (client.right + w) / 2, (client.bottom + h) / 2 };
        DrawRoundedShadow(dc, panel, S(14.0f), S(1), S(5), S(7), IsDarkTheme() ? 58 : 38);
        GlassRoundedSurface(dc, panel, IsDarkTheme() || IsAuroraTheme() ? 232 : 248, S(14.0f));

        g_clearDialogCloseRect = RECT{ panel.right - S(42), panel.top + S(14), panel.right - S(14), panel.top + S(42) };
        if (Hit(g_clearDialogCloseRect, g_mousePos.x, g_mousePos.y)) RoundFill(dc, g_clearDialogCloseRect, S(7), SidebarHoverFill(), SidebarHoverFill());
        HFONT closeFont = Font(18, FW_LIGHT);
        HGDIOBJ oldClose = SelectObject(dc, closeFont);
        Text(dc, L"\u00d7", g_clearDialogCloseRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldClose);
        DeleteObject(closeFont);

        RECT icon{ panel.left + S(36), panel.top + S(70), panel.left + S(86), panel.top + S(120) };
        Gdiplus::SolidBrush iconBg(GdiColor(MixColor(T().accent, RGB(255, 255, 255), IsDarkTheme() ? 0.18 : 0.36), 210));
        g.FillEllipse(&iconBg, Rf(icon));
        HFONT bangFont = Font(26, FW_SEMIBOLD);
        HGDIOBJ oldBang = SelectObject(dc, bangFont);
        Text(dc, L"!", icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE, ForegroundOn(T().accent));
        SelectObject(dc, oldBang);
        DeleteObject(bangFont);

        HFONT titleFont = Font(8, FW_SEMIBOLD);
        HGDIOBJ oldFont = SelectObject(dc, titleFont);
        RECT title{ panel.left + S(112), panel.top + S(60), panel.right - S(34), panel.top + S(92) };
        Text(dc, L"\u786e\u5b9a\u8981\u6e05\u7a7a\u6700\u8fd1\u4f7f\u7528\u8bb0\u5f55\u5417\uff1f", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);
        HFONT bodyFont = Font(9);
        oldFont = SelectObject(dc, bodyFont);
        RECT body{ panel.left + S(112), panel.top + S(92), panel.right - S(34), panel.top + S(130) };
        Text(dc, L"\u8fd9\u4f1a\u5220\u9664\u9996\u9875\u8bb0\u5f55\u3001\u9605\u8bfb\u8fdb\u5ea6\u548c\u5c01\u9762\u7f13\u5b58\uff0c\u4e0d\u4f1a\u5220\u9664 PDF \u6587\u4ef6\u3002", body, DT_LEFT | DT_WORDBREAK, T().muted);

        g_clearDialogConfirmRect = RECT{ panel.right - S(198), panel.bottom - S(58), panel.right - S(106), panel.bottom - S(24) };
        g_clearDialogCancelRect = RECT{ panel.right - S(94), panel.bottom - S(58), panel.right - S(24), panel.bottom - S(24) };
        bool confirmHover = Hit(g_clearDialogConfirmRect, g_mousePos.x, g_mousePos.y);
        bool cancelHover = Hit(g_clearDialogCancelRect, g_mousePos.x, g_mousePos.y);
        RoundFill(dc, g_clearDialogConfirmRect, S(8), confirmHover ? MixColor(T().accent, RGB(255, 255, 255), 0.16) : T().accent, T().accent);
        Text(dc, L"\u786e\u5b9a", g_clearDialogConfirmRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, ForegroundOn(T().accent));
        RoundFill(dc, g_clearDialogCancelRect, S(8), cancelHover ? SidebarHoverFill() : T().panel2, T().line);
        Text(dc, L"\u53d6\u6d88", g_clearDialogCancelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(bodyFont);
    }

    void PaintCustomZoomDialog(HDC dc, RECT client)
    {
        g_zoomDialogRect = {};
        g_zoomDialogInputRect = {};
        g_zoomDialogConfirmRect = {};
        g_zoomDialogCancelRect = {};
        if (!g_customZoomOpen) return;

        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush scrim(GdiColor(RGB(18, 22, 28), IsDarkTheme() ? 116 : 72));
        g.FillRectangle(&scrim, Rf(client));

        int panelW = S(202);
        int panelH = S(114);
        int left = client.left + (client.right - client.left - panelW) / 2;
        int top = client.top + (client.bottom - client.top - panelH) / 2;
        g_zoomDialogRect = RECT{ left, top, left + panelW, top + panelH };

        DrawRoundedShadow(dc, g_zoomDialogRect, S(14.0f), S(1), S(3), S(5), IsDarkTheme() ? 52 : 28);
        GlassRoundedSurface(dc, g_zoomDialogRect, IsDarkTheme() || IsAuroraTheme() ? 226 : 238, S(14.0f));

        RECT shine{ g_zoomDialogRect.left + S(16), g_zoomDialogRect.top + S(7), g_zoomDialogRect.right - S(48), g_zoomDialogRect.top + S(12) };
        Gdiplus::SolidBrush shineBrush(GdiColor(RGB(255, 255, 255), IsDarkTheme() ? 24 : 82));
        FillGdiRoundRect(g, shineBrush, Rf(shine), S(4.0f));

        HFONT titleFont = Font(13, FW_SEMIBOLD);
        HGDIOBJ oldFont = SelectObject(dc, titleFont);
        RECT title{ g_zoomDialogRect.left + S(14), g_zoomDialogRect.top + S(10), g_zoomDialogRect.right - S(14), g_zoomDialogRect.top + S(30) };
        Text(dc, L"\u81ea\u5b9a\u4e49\u7f29\u653e", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);

        HFONT hintFont = Font(6);
        oldFont = SelectObject(dc, hintFont);
        RECT hint{ g_zoomDialogRect.left + S(14), g_zoomDialogRect.top + S(30), g_zoomDialogRect.right - S(14), g_zoomDialogRect.top + S(44) };
        Text(dc, L"\u8f93\u5165\u9875\u9762\u7f29\u653e\u767e\u5206\u6bd4", hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(hintFont);

        g_zoomDialogInputRect = RECT{ g_zoomDialogRect.left + S(14), g_zoomDialogRect.top + S(47), g_zoomDialogRect.right - S(14), g_zoomDialogRect.top + S(74) };
        PaintLiquidGlassPill(dc, g_zoomDialogInputRect);
        RECT inputTextRect{ g_zoomDialogInputRect.left + S(13), g_zoomDialogInputRect.top, g_zoomDialogInputRect.right - S(38), g_zoomDialogInputRect.bottom };
        HFONT inputFont = Font(8, FW_SEMIBOLD);
        oldFont = SelectObject(dc, inputFont);
        std::wstring inputText = g_customZoomText.empty() ? L"" : g_customZoomText;
        Text(dc, inputText, inputTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        SIZE textSize{};
        GetTextExtentPoint32W(dc, inputText.c_str(), static_cast<int>(inputText.size()), &textSize);
        if (g_customZoomFocused && g_customZoomCaretVisible)
        {
            int caretX = std::clamp(inputTextRect.left + textSize.cx + S(2), inputTextRect.left, inputTextRect.right);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), T().accent);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, caretX, g_zoomDialogInputRect.top + S(6), nullptr);
            LineTo(dc, caretX, g_zoomDialogInputRect.bottom - S(6));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        SelectObject(dc, oldFont);
        DeleteObject(inputFont);

        HFONT pctFont = Font(7);
        oldFont = SelectObject(dc, pctFont);
        RECT pctRect{ g_zoomDialogInputRect.right - S(34), g_zoomDialogInputRect.top, g_zoomDialogInputRect.right - S(13), g_zoomDialogInputRect.bottom };
        Text(dc, L"%", pctRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(pctFont);

        g_zoomDialogCancelRect = RECT{ g_zoomDialogRect.right - S(112), g_zoomDialogRect.bottom - S(31), g_zoomDialogRect.right - S(62), g_zoomDialogRect.bottom - S(10) };
        g_zoomDialogConfirmRect = RECT{ g_zoomDialogRect.right - S(54), g_zoomDialogRect.bottom - S(31), g_zoomDialogRect.right - S(14), g_zoomDialogRect.bottom - S(10) };
        auto drawDialogButton = [&](RECT r, const wchar_t* label, bool primary)
        {
            bool hover = Hit(r, g_mousePos.x, g_mousePos.y);
            bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &r);
            COLORREF fill = primary
                ? (pressed ? MixColor(T().accent, RGB(0, 0, 0), 0.18) : (hover ? MixColor(T().accent, RGB(255, 255, 255), 0.20) : T().accent))
                : (pressed ? MixColor(T().panel2, T().accent, 0.22) : (hover ? MixColor(T().panel2, T().accent, 0.10) : T().panel2));
            COLORREF line = primary ? T().accent : T().line;
            Gdiplus::SolidBrush fillBrush(GdiColor(fill, 246));
            Gdiplus::Pen borderPen(GdiColor(line, hover || pressed ? 190 : 145), std::max<Gdiplus::REAL>(1.0f, S(1.0f)));
            FillGdiRoundRect(g, fillBrush, Rf(r), S(5.0f));
            if (hover)
            {
                Gdiplus::SolidBrush wash(GdiColor(RGB(255, 255, 255), primary ? 36 : 28));
                RECT topWash{ r.left + S(1), r.top + S(1), r.right - S(1), r.top + (r.bottom - r.top) / 2 };
                FillGdiRoundRect(g, wash, Rf(topWash), S(4.0f));
            }
            DrawGdiRoundRect(g, borderPen, Rf(r), S(5.0f));
            g.Flush();
            HFONT buttonFont = Font(7, FW_SEMIBOLD);
            HGDIOBJ oldButtonFont = SelectObject(dc, buttonFont);
            Text(dc, label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, primary ? ForegroundOn(T().accent) : T().muted);
            SelectObject(dc, oldButtonFont);
            DeleteObject(buttonFont);
        };
        drawDialogButton(g_zoomDialogCancelRect, L"\u53d6\u6d88", false);
        drawDialogButton(g_zoomDialogConfirmRect, L"\u786e\u5b9a", true);
    }

#if 0
    void PaintSidebar(HDC dc, PdfTab& tab, RECT paintClip)
    {
        if (g_immersive || !g_sidebarOpen) return;
        GlassFill(dc, g_sidebar, T().panel, 214);
        g_sidebarRowPages.clear();
        g_sidebarRowRects.clear();
        HFONT f = Font(9);
        auto oldFont = SelectObject(dc, f);
        RECT tabBar{ g_sidebar.left + S(10), g_sidebar.top + S(10), g_sidebar.right - S(10), g_sidebar.top + S(42) };
        int tabWidth = (tabBar.right - tabBar.left - S(12)) / 3;
        const wchar_t* sideTabs[] = { L"\u76ee\u5f55", L"\u7f29\u7565\u56fe", L"\u4e66\u7b7e" };
        for (int i = 0; i < 3; ++i)
        {
            RECT tabRect{ tabBar.left + i * (tabWidth + S(6)), tabBar.top, tabBar.left + i * (tabWidth + S(6)) + tabWidth, tabBar.bottom };
            RoundFill(dc, tabRect, S(8), i == 0 ? T().panel2 : T().panel, T().line);
            Text(dc, sideTabs[i], tabRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, T().text);
        }
        RECT sizeBar{ g_sidebar.left + S(12), g_sidebar.top + S(50), g_sidebar.right - S(12), g_sidebar.top + S(80) };
        Text(dc, L"\u6587\u5b57\u5927\u5c0f", sizeBar, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
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
                std::wstring label = tab.outline.empty() ? (L"\u7b2c" + std::to_wstring(i + 1) + L" \u9875") : tab.outline[static_cast<size_t>(i)].title;
            Text(dc, label, tx, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
            y += S(32);
            if (y > g_sidebar.bottom - S(20)) break;
        }
        SelectObject(dc, oldFont);
        DeleteObject(outlineFont);
        DeleteObject(f);
    }

#endif

    void PaintSidebar(HDC dc, PdfTab& tab, RECT paintClip)
    {
        if (!SidebarVisible() || g_sidebarWidth <= 0) return;
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
        RECT sidebarControls{ 0, g_sidebar.top, g_sidebar.right, g_sidebar.top + S(86) };
        bool registerSidebarButtons = Intersects(paintClip, sidebarControls) || g_buttons.empty();

        HFONT f = Font(9);
        HGDIOBJ oldFont = SelectObject(dc, f);
        int railW = S(44);
        RECT rail{ 0, g_sidebar.top, std::min<int>(railW, sidebarFull.right), g_sidebar.bottom };
        if (IsAuroraTheme()) Fill(dc, rail, T().panel);
        else if (IsGlassTheme()) AlphaFill(dc, rail, RGB(247, 250, 253), 116);
        else Fill(dc, rail, T().panel);
        RECT railLine{ rail.right - std::max(1, S(1)), rail.top, rail.right, rail.bottom };
        GlassDivider(dc, railLine);
        RECT tabBar{ rail.left, g_sidebar.top + S(10), rail.right, g_sidebar.bottom };
        int tabWidth = S(34);
        int tabGap = S(5);
        int tabStart = tabBar.left + std::max<int>(0, (tabBar.right - tabBar.left - tabWidth) / 2);
        SideTabIcon sideIcons[] = { SideTabIcon::Outline, SideTabIcon::Thumbnails, SideTabIcon::Search, SideTabIcon::Notes };
        const wchar_t* sideTips[] = { L"\u76ee\u5f55", L"\u7f29\u7565\u56fe", L"\u67e5\u627e" };
        for (int i = 0; i < 4; ++i)
        {
            RECT tabRect{ tabStart, tabBar.top + i * (tabWidth + tabGap), tabStart + tabWidth, tabBar.top + i * (tabWidth + tabGap) + tabWidth };
            g_sideTabRects.push_back(tabRect);
            if (registerSidebarButtons) AddButton(0, tabRect, L"", i == 0 ? L"\u76ee\u5f55" : (i == 1 ? L"\u7f29\u7565\u56fe" : (i == 2 ? L"\u67e5\u627e" : L"\u6ce8\u91ca")));
            bool hovered = g_mousePos.x >= tabRect.left && g_mousePos.x < tabRect.right && g_mousePos.y >= tabRect.top && g_mousePos.y < tabRect.bottom;
            bool active = i == g_sideMode;
            if (hovered || active)
            {
                if (IsAuroraTheme()) RoundFill(dc, tabRect, S(7), active ? MixColor(T().panel, T().accent, 0.075) : SidebarHoverFill(), active ? MixColor(T().line, T().accent, 0.16) : SidebarHoverFill());
                else if (IsBeigeTheme()) RoundFill(dc, tabRect, S(7), active ? SidebarSelectedFill() : SidebarHoverFill(), active ? SidebarSelectedBorder() : SidebarHoverFill());
                else if (IsDarkTheme()) RoundFill(dc, tabRect, S(7), active ? SidebarSelectedFill() : SidebarHoverFill(), active ? SidebarSelectedBorder() : SidebarHoverFill());
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
            Text(dc, L"\u7f29\u7565\u56fe", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
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
                    DrawCachedImage(dc, thumb, imageRect);
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
            Text(dc, g_searchQuery.empty() ? L"\u8f93\u5165\u5173\u952e\u8bcd" : g_searchQuery, inputText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_searchQuery.empty() ? T().muted : T().text);
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
            AddButton(CmdSearchClear, clearRect, L"\u00d7");
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
            AddButton(CmdSearchPrev, prev, L"\u4e0a\u4e00\u9875");
            AddButton(CmdSearchNext, next, L"\u4e0b\u4e00\u9875");
            AddButton(CmdSearchRun, run, L"\u67e5\u627e");
            DrawSearchActionButton(dc, CmdSearchPrev, prev, L"\u4e0a\u4e00\u9875");
            DrawSearchActionButton(dc, CmdSearchNext, next, L"\u4e0b\u4e00\u9875");
            DrawSearchActionButton(dc, CmdSearchRun, run, L"\u67e5\u627e");

            RECT summary{ g_sidebar.left + S(14), g_sidebar.top + S(108), g_sidebar.right - S(14), g_sidebar.top + S(138) };
            std::wstring summaryText = g_searchHasRun ? (L"\u7cbe\u786e\u67e5\u627e (" + std::to_wstring(SearchHitTotal()) + L")") : L"\u8f93\u5165\u5173\u952e\u8bcd\u540e\u70b9\u51fb\u67e5\u627e";
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
            int buttonTop = g_sidebar.top + S(33);
            int buttonSize = S(30);
            int buttonGap = S(3);
            int groupGap = S(11);
            int buttonLeft = g_sidebar.left + S(10);
            int available = std::max<int>(1, static_cast<int>(g_sidebar.right - g_sidebar.left) - S(20));
            int desired = buttonSize * 6 + buttonGap * 3 + groupGap * 2 + S(2);
            if (desired > available)
            {
                buttonSize = std::max(S(24), (available - buttonGap * 3 - groupGap * 2 - S(2)) / 6);
            }
            struct NoteTool { int id; const wchar_t* tip; };
            NoteTool tools[] = {
                { CmdNoteExpandAll, L"\u5c55\u5f00\u6240\u6709\u6ce8\u91ca" },
                { CmdNoteCollapseAll, L"\u6536\u8d77\u6240\u6709\u6ce8\u91ca" },
                { CmdNotePrev, L"\u4e0a\u4e00\u4e2a\u6ce8\u91ca" },
                { CmdNoteNext, L"\u4e0b\u4e00\u4e2a\u6ce8\u91ca" },
                { CmdNoteDelete, L"\u5220\u9664\u5f53\u524d\u6ce8\u91ca" },
                { CmdNoteDeleteAll, L"\u5220\u9664\u6240\u6709\u6ce8\u91ca" }
            };
            COLORREF noteIcon = MixColor(T().muted, T().text, IsDarkTheme() || IsAuroraTheme() ? 0.14 : 0.08);
            HPEN sepPen = CreatePen(PS_SOLID, std::max(1, S(1)), MixColor(T().line, T().panel, 0.25));
            HGDIOBJ oldPen = SelectObject(dc, sepPen);
            int x = buttonLeft;
            for (int i = 0; i < static_cast<int>(std::size(tools)); ++i)
            {
                RECT r{ x, buttonTop, x + buttonSize, buttonTop + buttonSize };
                AddButton(tools[i].id, r, L"", tools[i].tip);
                if (Hit(r, g_mousePos.x, g_mousePos.y))
                    RoundFill(dc, r, S(6), SidebarHoverFill(), SidebarHoverFill());
                RECT icon{ r.left + S(3), r.top + S(3), r.right - S(3), r.bottom - S(3) };
                DrawNoteToolIcon(dc, icon, tools[i].id, noteIcon);
                x += buttonSize;
                if (i == 1 || i == 3)
                {
                    x += buttonGap;
                    int sepX = x + groupGap / 2;
                    MoveToEx(dc, sepX, buttonTop + S(6), nullptr);
                    LineTo(dc, sepX, buttonTop + buttonSize - S(6));
                    x += groupGap;
                }
                else x += buttonGap;
            }
            SelectObject(dc, oldPen);
            DeleteObject(sepPen);
            RECT summary{ g_sidebar.left + S(14), g_sidebar.top + S(70), g_sidebar.right - S(14), g_sidebar.top + S(92) };
            Text(dc, L"\u5171 " + std::to_wstring(tab.annotations.size()) + L" \u5904\u6ce8\u91ca", summary, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);

            int clipDc = SaveDC(dc);
            IntersectClipRect(dc, g_sidebar.left, g_sidebar.top + S(96), g_sidebar.right, g_sidebar.bottom);
            int y = g_sidebar.top + S(100) - g_sidebarScrollY;
            int content = S(100);
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
                    Text(dc, AnnotationKindName(annotation.kind) + L"  " + (annotation.author.empty() ? L"10726" : annotation.author), topText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().text);
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
            Text(dc, L"\u6587\u5b57\u5927\u5c0f", sizeBar, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().muted);
            RECT smallBtn{ sizeBar.right - S(78), sizeBar.top, sizeBar.right - S(42), sizeBar.bottom };
            RECT largeBtn{ sizeBar.right - S(36), sizeBar.top, sizeBar.right, sizeBar.bottom };
            AddButton(CmdOutlineSmall, smallBtn, L"A-", L"\u51cf\u5c0f\u76ee\u5f55\u5b57\u53f7");
            AddButton(CmdOutlineLarge, largeBtn, L"A+", L"\u589e\u5927\u76ee\u5f55\u5b57\u53f7");
            DrawButton(dc, Button{ CmdOutlineSmall, smallBtn, L"A-", L"\u51cf\u5c0f\u76ee\u5f55\u5b57\u53f7" });
            DrawButton(dc, Button{ CmdOutlineLarge, largeBtn, L"A+", L"\u589e\u5927\u76ee\u5f55\u5b57\u53f7" });
            Fill(dc, sizeBar, T().panel);
            RECT toggleAll{ sizeBar.left, sizeBar.top, sizeBar.left + S(92), sizeBar.bottom };
            std::wstring toggleText = g_collapsedOutlineRows.empty() ? L"\u6536\u8d77\u6240\u6709" : L"\u5c55\u5f00\u6240\u6709";
            if (registerSidebarButtons) AddButton(CmdOutlineToggleAll, toggleAll, toggleText);
            bool toggleHovered = g_hasHoverButton && EqualRect(&g_hoverButtonRect, &toggleAll);
            bool togglePressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &toggleAll);
            if (toggleHovered || togglePressed)
            {
                COLORREF fill{};
                COLORREF line{};
                if (IsGlassTheme())
                {
                    fill = togglePressed ? RGB(232, 236, 241) : RGB(255, 255, 255);
                    line = togglePressed ? RGB(202, 210, 220) : RGB(228, 234, 241);
                }
                else
                {
                    fill = togglePressed
                        ? MixColor(T().panel2, T().line, IsDarkTheme() || IsAuroraTheme() ? 0.34 : 0.48)
                        : MixColor(T().panel2, T().paper, IsDarkTheme() || IsAuroraTheme() ? 0.18 : 0.50);
                    line = togglePressed ? T().line : MixColor(T().line, T().text, 0.08);
                }
                RoundFill(dc, toggleAll, S(5), fill, line);
            }
            COLORREF toggleColor = togglePressed ? T().text : (toggleHovered ? MixColor(T().text, T().muted, 0.08) : T().muted);
            Text(dc, toggleText, toggleAll, DT_CENTER | DT_VCENTER | DT_SINGLELINE, toggleColor);
            RECT largeBtnNew{ sizeBar.right - S(30), sizeBar.top, sizeBar.right, sizeBar.bottom };
            RECT smallBtnNew{ sizeBar.right - S(64), sizeBar.top, sizeBar.right - S(34), sizeBar.bottom };
            if (registerSidebarButtons) AddButton(CmdOutlineSmall, smallBtnNew, L"A-", L"\u51cf\u5c0f\u76ee\u5f55\u5b57\u53f7");
            if (registerSidebarButtons) AddButton(CmdOutlineLarge, largeBtnNew, L"A+", L"\u589e\u5927\u76ee\u5f55\u5b57\u53f7");
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
                std::wstring label = tab.outline.empty() ? (L"\u7b2c" + std::to_wstring(i + 1) + L" \u9875") : tab.outline[static_cast<size_t>(i)].title;
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
                    if (selected) RoundFill(dc, row, S(4), SidebarSelectedFill(), SidebarSelectedBorder());

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

                    RECT pageRect{ pageLeft, row.top, row.right - S(3), row.bottom };
                    RECT tx{ textLeft, textTop, pageRect.left - S(4), textTop + textH };
                    COLORREF rowText = (selected && IsGlassTheme()) ? RGB(255, 255, 255) : T().text;
                    COLORREF rowAux = selected ? rowText : T().muted;
                    COLORREF markColor = selected ? rowText : SidebarIconColor();

                    RECT mark{ iconLeft, firstLineCenterY - iconH / 2, iconLeft + iconW, firstLineCenterY + (iconH + 1) / 2 };
                    DrawBookmarkIcon(dc, mark, markColor, false);
                    Text(dc, label, tx, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, rowText);
                    Text(dc, std::to_wstring(page + 1), pageRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, rowAux);
                }
            }
            RestoreDC(dc, clipDc);
            SelectObject(dc, oldPen);
            DeleteObject(treePen);
            SelectObject(dc, f);
            DeleteObject(outlineFont);
        }
        ClampSidebarScroll();
        PaintSidebarScrollBar(dc);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    COLORREF PdfOverlayColor()
    {
        if (IsAuroraTheme()) return RGB(0, 42, 30);
        if (g_theme == 3) return RGB(28, 25, 22);
        return RGB(7, 10, 20);
    }

    BYTE PdfOverlayAlpha()
    {
        if (IsAuroraTheme()) return 42;
        if (g_theme == 3) return 40;
        return 46;
    }

    bool EnsureImageDib(PageImage& image)
    {
        if (image.dib) return true;
        if (image.bgra.empty() || image.width <= 0 || image.height <= 0) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits)
        {
            if (dib) DeleteObject(dib);
            return false;
        }

        memcpy(bits, image.bgra.data(), image.bgra.size());
        image.dib = dib;
        image.ReleasePixels();
        return true;
    }

    bool DrawCachedImagePart(HDC dc, PageImage& image, RECT dest, int sx, int sy, int sw, int sh)
    {
        if (dest.right <= dest.left || dest.bottom <= dest.top) return false;
        if (sw <= 0 || sh <= 0) return false;
        if (!EnsureImageDib(image))
        {
            if (image.bgra.empty() || image.width <= 0 || image.height <= 0) return false;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = image.width;
            info.bmiHeader.biHeight = -image.height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(
                dc,
                dest.left,
                dest.top,
                dest.right - dest.left,
                dest.bottom - dest.top,
                sx,
                sy,
                sw,
                sh,
                image.bgra.data(),
                &info,
                DIB_RGB_COLORS,
                SRCCOPY);
            return true;
        }

        if (!g_imageDrawDc)
            g_imageDrawDc = CreateCompatibleDC(dc);
        HDC src = g_imageDrawDc;
        if (!src) return false;
        HGDIOBJ old = SelectObject(src, image.dib);
        int oldMode = SetStretchBltMode(dc, COLORONCOLOR);
        bool ok = StretchBlt(
            dc,
            dest.left,
            dest.top,
            dest.right - dest.left,
            dest.bottom - dest.top,
            src,
            sx,
            sy,
            sw,
            sh,
            SRCCOPY) != 0;
        SetStretchBltMode(dc, oldMode);
        SelectObject(src, old);
        return ok;
    }

    bool DrawCachedImage(HDC dc, PageImage& image, RECT dest)
    {
        return DrawCachedImagePart(dc, image, dest, 0, 0, image.width, image.height);
    }
    bool DrawTileLevel(HDC dc, PdfTab& tab, int page, RECT pageRect, RECT drawRect, int level, bool renderMissing, bool& missingVisiblePage)
    {
        int pageW = std::max<int>(1, pageRect.right - pageRect.left);
        int pageH = std::max<int>(1, pageRect.bottom - pageRect.top);
        int scale = TileLevelScale(level);
        int levelPageW = std::max<int>(1, (pageW + scale - 1) / scale);
        int levelPageH = std::max<int>(1, (pageH + scale - 1) / scale);
        int localLeft = std::clamp<int>(drawRect.left - pageRect.left, 0, pageW);
        int localTop = std::clamp<int>(drawRect.top - pageRect.top, 0, pageH);
        int localRight = std::clamp<int>(drawRect.right - pageRect.left, 0, pageW);
        int localBottom = std::clamp<int>(drawRect.bottom - pageRect.top, 0, pageH);
        int levelLeft = TileCoordAtLevel(localLeft, level);
        int levelTop = TileCoordAtLevel(localTop, level);
        int levelRight = std::clamp<int>((localRight + scale - 1) / scale, levelLeft + 1, levelPageW);
        int levelBottom = std::clamp<int>((localBottom + scale - 1) / scale, levelTop + 1, levelPageH);
        int tileLeft = (levelLeft / TileSize) * TileSize;
        int tileTop = (levelTop / TileSize) * TileSize;
        bool painted = false;
        for (int ty = tileTop; ty < levelBottom; ty += TileSize)
        {
            int h = std::min(TileSize, levelPageH - ty);
            if (h <= 0) continue;
            for (int tx = tileLeft; tx < levelRight; tx += TileSize)
            {
                int w = std::min(TileSize, levelPageW - tx);
                if (w <= 0) continue;
                TileKey key{ page, TileZoomKey(tab.zoom), g_theme, level, tx, ty, w, h };
                PageImage* image = FindCachedTile(tab, key);
                if (!image)
                {
                    if (renderMissing)
                    {
                        missingVisiblePage = true;
                        QueueTileRender(g_hwnd, tab, key, 0);
                    }
                    continue;
                }
                if ((!image->dib && image->bgra.empty()) || image->width <= 0 || image->height <= 0)
                {
                    if (renderMissing) missingVisiblePage = true;
                    continue;
                }
                RECT tileDest = TileScreenRect(pageRect, key);
                RECT clippedDest{};
                if (!IntersectRect(&clippedDest, &tileDest, &pageRect)) continue;
                if (DrawCachedImage(dc, *image, tileDest)) painted = true;
            }
        }
        return painted;
    }

    bool PaintPages(HDC dc, PdfTab& tab, RECT paintClip)
    {
        RECT stageDirty{};
        if (!IntersectRect(&stageDirty, &g_stage, &paintClip)) return true;
        bool lightPaint = StageLightPaintActive();
        bool hadVisiblePage = false;
        bool paintedRenderable = false;
        bool missingVisiblePage = false;
        if (lightPaint)
        {
            Fill(dc, stageDirty, T().stage);
        }
        else if (IsAuroraTheme())
        {
            Fill(dc, stageDirty, T().stage);
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
        int content = g_continuousRead ? ContinuousContentHeight(tab) : SinglePageContentHeight(tab);
        int firstVisibleSpread = g_continuousRead ? FirstVisibleSpreadStart(tab) : 0;
        int firstPage = g_continuousRead ? std::max(0, firstVisibleSpread) : FirstPageInSpread(std::clamp(tab.currentPage, 0, tab.pageCount - 1));
        int lastPage = g_continuousRead ? tab.pageCount : NextSpreadStart(tab, firstPage);
        for (int start = firstPage; start < lastPage; start = NextSpreadStart(tab, start))
        {
            int y = g_continuousRead ? SpreadVisualTop(tab, start) : g_stage.top + S(28) - g_scrollY;
            int spreadH = SpreadHeight(tab, start);
            RECT spreadRect{ SpreadX(tab, start), y, SpreadX(tab, start) + SpreadWidth(tab, start), y + spreadH };
            if (g_continuousRead && spreadRect.top > stageDirty.bottom + S(64)) break;
            int count = SpreadPageCount(tab, start);
            for (int j = 0; j < count; ++j)
            {
                int i = start + j;
                int pageW = PageW(tab, i);
                int pageH = PageH(tab, i);
                RECT pageRect{};
                if (!BuildSpreadPageRect(tab, start, y, i, pageRect)) continue;
                if (!Intersects(pageRect, stageDirty)) continue;
                hadVisiblePage = true;
                if (!lightPaint)
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
                }
                RECT drawRect{};
                if (IntersectRect(&drawRect, &pageRect, &stageDirty))
                {
                    int savedPageDc = SaveDC(dc);
                    IntersectClipRect(dc, pageRect.left, pageRect.top, pageRect.right, pageRect.bottom);
                    if (tab.zoom < TileRenderZoomThreshold)
                    {
                        PageImage* image = lightPaint ? FindCachedPage(tab, i) : &EnsurePage(tab, i);
                        bool usingZoomFallback = false;
                        if (!image && lightPaint)
                        {
                            image = FindZoomFallbackPage(tab, i);
                            usingZoomFallback = image != nullptr;
                        }
                        if (!image && lightPaint)
                        {
                            missingVisiblePage = true;
                            QueuePageRender(g_hwnd, tab, i, 0);
                        }
                        if (image && (image->dib || !image->bgra.empty()) && image->width > 0 && image->height > 0)
                        {
                            Fill(dc, drawRect, RGB(255, 255, 255));
                            int sx = std::clamp<int>(static_cast<int>(std::floor((drawRect.left - pageRect.left) * static_cast<double>(image->width) / std::max(1, pageW))), 0, image->width - 1);
                            int sy = std::clamp<int>(static_cast<int>(std::floor((drawRect.top - pageRect.top) * static_cast<double>(image->height) / std::max(1, pageH))), 0, image->height - 1);
                            int ex = std::clamp<int>(static_cast<int>(std::ceil((drawRect.right - pageRect.left) * static_cast<double>(image->width) / std::max(1, pageW))), sx + 1, image->width);
                            int ey = std::clamp<int>(static_cast<int>(std::ceil((drawRect.bottom - pageRect.top) * static_cast<double>(image->height) / std::max(1, pageH))), sy + 1, image->height);
                            if (DrawCachedImagePart(dc, *image, drawRect, sx, sy, ex - sx, ey - sy))
                            {
                                paintedRenderable = true;
                                if (usingZoomFallback) QueuePageRender(g_hwnd, tab, i, 0);
                            }
                        }
                    }
                    else
                    {
                        Fill(dc, drawRect, RGB(255, 255, 255));
                        bool fallbackPainted = false;
                        int targetLevel = TileLevelForPage(tab, i, pageW, pageH);
                        for (int level = targetLevel; level >= 1; --level)
                        {
                            bool ignoredMissing = false;
                            if (DrawTileLevel(dc, tab, i, pageRect, drawRect, level, level == targetLevel, ignoredMissing))
                                fallbackPainted = true;
                        }
                        if (!fallbackPainted)
                        {
                            if (PageImage* fallback = FindZoomFallbackPage(tab, i))
                            {
                                if ((fallback->dib || !fallback->bgra.empty()) && fallback->width > 0 && fallback->height > 0)
                                {
                                    int fsx = std::clamp<int>(static_cast<int>(std::floor((drawRect.left - pageRect.left) * static_cast<double>(fallback->width) / std::max(1, pageW))), 0, fallback->width - 1);
                                    int fsy = std::clamp<int>(static_cast<int>(std::floor((drawRect.top - pageRect.top) * static_cast<double>(fallback->height) / std::max(1, pageH))), 0, fallback->height - 1);
                                    int fex = std::clamp<int>(static_cast<int>(std::ceil((drawRect.right - pageRect.left) * static_cast<double>(fallback->width) / std::max(1, pageW))), fsx + 1, fallback->width);
                                    int fey = std::clamp<int>(static_cast<int>(std::ceil((drawRect.bottom - pageRect.top) * static_cast<double>(fallback->height) / std::max(1, pageH))), fsy + 1, fallback->height);
                                    fallbackPainted = DrawCachedImagePart(dc, *fallback, drawRect, fsx, fsy, fex - fsx, fey - fsy);
                                }
                            }
                        }
                        bool targetMissing = false;
                        bool targetPainted = DrawTileLevel(dc, tab, i, pageRect, drawRect, 0, true, targetMissing);
                        if (fallbackPainted || targetPainted) paintedRenderable = true;
                        if (targetMissing && !fallbackPainted && !targetPainted) missingVisiblePage = true;
                    }
                    if (IsDarkTheme())
                    {
                        AlphaFill(dc, pageRect, PdfOverlayColor(), PdfOverlayAlpha());
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
                        COLORREF annotationColor = AnnotationColor(annotation);
                        auto drawSegment = [&](RECT mark)
                        {
                            RECT visibleMark{};
                            if (!IntersectRect(&visibleMark, &mark, &pageRect)) return;
                            if (annotation.kind == AnnotationKind::Highlight)
                            {
                                AlphaFill(dc, visibleMark, annotationColor, IsDarkTheme() || IsAuroraTheme() ? 76 : 132);
                            }
                            else
                            {
                                int lineY = annotation.kind == AnnotationKind::Underline ?
                                    std::min(pageRect.bottom - 1, visibleMark.bottom + S(1)) :
                                    (visibleMark.top + visibleMark.bottom) / 2;
                                HPEN markPen = CreatePen(PS_SOLID, std::max(1, S(annotation.kind == AnnotationKind::Underline ? 1 : 2)), annotationColor);
                                HGDIOBJ oldMarkPen = SelectObject(dc, markPen);
                                MoveToEx(dc, visibleMark.left, lineY, nullptr);
                                LineTo(dc, visibleMark.right, lineY);
                                SelectObject(dc, oldMarkPen);
                                DeleteObject(markPen);
                            }
                        };
                        if (!annotation.segments.empty())
                        {
                            for (auto const& segment : annotation.segments)
                                drawSegment(DenormalizeAnnotationSegment(segment, pageRect));
                        }
                        else drawSegment(DenormalizeAnnotationRect(annotation, pageRect));
                    }
                    if ((g_hasContentSelection || g_selectingContent) && g_selectionPage == i)
                    {
                        if (!g_selectionQuads.empty())
                        {
                            DrawTextSelectionQuads(dc, pageRect, g_selectionQuads);
                        }
                        else if (g_selectingContent || g_selectionIsImage)
                        {
                            RECT selection = CurrentSelectionRect();
                            RECT visibleSelection{};
                            if (IntersectRect(&visibleSelection, &selection, &pageRect))
                            {
                                AlphaFill(dc, visibleSelection, RGB(86, 156, 255), IsDarkTheme() || IsAuroraTheme() ? 42 : 32);
                                RECT top{ visibleSelection.left, visibleSelection.top, visibleSelection.right, visibleSelection.top + std::max(1, S(1)) };
                                RECT bottom{ visibleSelection.left, visibleSelection.bottom - std::max(1, S(1)), visibleSelection.right, visibleSelection.bottom };
                                RECT left{ visibleSelection.left, visibleSelection.top, visibleSelection.left + std::max(1, S(1)), visibleSelection.bottom };
                                RECT right{ visibleSelection.right - std::max(1, S(1)), visibleSelection.top, visibleSelection.right, visibleSelection.bottom };
                                Fill(dc, top, MixColor(T().accent, RGB(255, 255, 255), 0.10));
                                Fill(dc, bottom, MixColor(T().accent, RGB(255, 255, 255), 0.10));
                                Fill(dc, left, MixColor(T().accent, RGB(255, 255, 255), 0.10));
                                Fill(dc, right, MixColor(T().accent, RGB(255, 255, 255), 0.10));
                            }
                        }
                    }
                    RestoreDC(dc, savedPageDc);
                }
            }
        }
        RestoreDC(dc, savedDc);
        g_contentHeight = content;
        g_contentWidth = ReadingContentWidth(tab);
        return !hadVisiblePage || paintedRenderable || !missingVisiblePage;
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

        int width = S(360);
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

        int x = g_annotationToolbarRect.left + S(9);
        int y = g_annotationToolbarRect.top + S(7);
        int h = S(28);
        g_annotationCopyRect = RECT{ x, y, x + S(40), y + h };
        drawTool(g_annotationCopyRect, L"\u590d\u5236");
        x += S(45);
        g_annotationNoteRect = RECT{ x, y, x + S(40), y + h };
        drawTool(g_annotationNoteRect, L"\u4fbf\u7b7e", g_annotationNoteOpen);
        x += S(45);
        g_annotationDeleteRect = RECT{ x, y, x + S(40), y + h };
        drawTool(g_annotationDeleteRect, L"\u5220\u9664");
        x += S(50);

        for (int i = 0; i < static_cast<int>(std::size(AnnotationPalette)); ++i)
        {
            RECT swatch{ x, y + S(6), x + S(16), y + S(22) };
            g_annotationColorRects.push_back(swatch);
            bool selected = i == annotation->colorIndex;
            bool hover = Hit(swatch, g_mousePos.x, g_mousePos.y);
            if (selected || hover)
            {
                RECT ring{ swatch.left - S(4), swatch.top - S(4), swatch.right + S(4), swatch.bottom + S(4) };
                RoundFill(dc, ring, S(10), selected ? MixColor(T().panel2, T().accent, 0.18) : T().panel2, selected ? T().accent : T().line);
            }
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush brush(GdiColor(AnnotationPalette[i]));
            g.FillEllipse(&brush, static_cast<float>(swatch.left), static_cast<float>(swatch.top), static_cast<float>(swatch.right - swatch.left), static_cast<float>(swatch.bottom - swatch.top));
            x += S(22);
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
        Text(dc, L"\u4fbf\u7b7e", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, T().text);
        g_annotationNoteCloseRect = RECT{ panelLeft + panelW - S(34), panelTop + S(8), panelLeft + panelW - S(10), panelTop + S(32) };
        drawTool(g_annotationNoteCloseRect, L"x");
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);

        HFONT smallFont = Font(8);
        oldFont = SelectObject(dc, smallFont);
        RECT meta{ panelLeft + S(14), panelTop + S(36), panelLeft + panelW - S(14), panelTop + S(58) };
        std::wstring metaText = (annotation->author.empty() ? L"10726" : annotation->author) + L"  " + FormatTimeMs(annotation->createdAt);
        Text(dc, metaText, meta, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, T().muted);
        SelectObject(dc, oldFont);
        DeleteObject(smallFont);

        g_annotationNoteInputRect = RECT{ panelLeft + S(14), panelTop + S(64), panelLeft + panelW - S(14), panelTop + panelH - S(14) };
        RoundFill(dc, g_annotationNoteInputRect, S(8), T().paper, g_annotationNoteFocused ? T().accent : T().line);
        RECT noteText{ g_annotationNoteInputRect.left + S(10), g_annotationNoteInputRect.top + S(8), g_annotationNoteInputRect.right - S(10), g_annotationNoteInputRect.bottom - S(8) };
        std::wstring note = annotation->note.empty() ? L"\u70b9\u51fb\u8f93\u5165\u6ce8\u91ca..." : annotation->note;
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

    void DrawSidebarPanelToggleGlyph(HDC dc, RECT r, bool open, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color), std::max<Gdiplus::REAL>(1.2f, S(1.15f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        float cx = (static_cast<float>(r.left + r.right)) * 0.5f;
        float cy = (static_cast<float>(r.top + r.bottom)) * 0.5f;
        float dx = S(3.4f);
        float dy = S(5.0f);
        if (open)
        {
            Gdiplus::PointF pts[3] = { { cx + dx, cy - dy }, { cx - dx, cy }, { cx + dx, cy + dy } };
            g.DrawLines(&pen, pts, 3);
        }
        else
        {
            Gdiplus::PointF pts[3] = { { cx - dx, cy - dy }, { cx + dx, cy }, { cx - dx, cy + dy } };
            g.DrawLines(&pen, pts, 3);
        }
    }

    void PaintSidebarPanelToggle(HDC dc, RECT client)
    {
        g_sidebarPanelToggleRect = {};
        if (!Active()) return;
        int bottomInset = (!g_immersive && g_statusPinned) ? S(34) : 0;
        int size = S(24);
        int bottom = g_immersive ? client.bottom - S(21) : client.bottom - bottomInset - S(8);
        int railRight = S(44);
        int left = std::max<int>(S(7), (railRight - size) / 2);
        RECT r{ left, bottom - size, left + size, bottom };
        g_sidebarPanelToggleRect = r;

        bool open = SidebarVisible();
        bool hovered = Hit(r, g_mousePos.x, g_mousePos.y);
        bool pressed = g_hasPressedButton && EqualRect(&g_pressedButtonRect, &r);
        COLORREF base = IsAuroraTheme() ? T().panel : T().panel2;
        COLORREF fill = base;
        COLORREF line = T().line;
        COLORREF glyph = T().muted;
        if (hovered)
        {
            fill = SidebarHoverFill();
            line = MixColor(T().line, T().accent, IsAuroraTheme() ? 0.12 : 0.22);
            glyph = MixColor(T().muted, T().text, 0.20);
        }
        if (pressed)
        {
            fill = MixColor(fill, T().accent, 0.10);
            line = MixColor(line, T().accent, 0.30);
        }
        DrawRoundedShadow(dc, r, S(8.0f), S(1), S(3), S(3), IsDarkTheme() || IsAuroraTheme() ? 20 : 12);
        RoundFill(dc, r, S(8), fill, line);
        DrawSidebarPanelToggleGlyph(dc, r, open, glyph);
    }
    void PaintStatus(HDC dc, RECT client)
    {
        PdfTab* tab = Active();
        if (!tab && !g_immersive) return;
        if (g_immersive)
        {
            RECT panel{ client.right - S(154), client.bottom - S(48), client.right - S(18), client.bottom - S(21) };
            PaintLiquidGlassPill(dc, panel);
            std::wstring text = tab ? std::to_wstring(tab->currentPage + 1) + L" / " + std::to_wstring(tab->pageCount) + L"    \u9000\u51fa\u6c89\u6d78" : L"\u9000\u51fa\u6c89\u6d78";
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
        std::wstring pageSize = L"-- x -- \u5398\u7c73";
        if (tab && !tab->pages.empty())
        {
            auto const& p = tab->pages[static_cast<size_t>(tab->currentPage)];
            wchar_t buf[80]{};
            swprintf_s(buf, L"%.1f x %.1f \u5398\u7c73", p.widthPt * 2.54 / 72.0, p.heightPt * 2.54 / 72.0);
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
        addIcon(CmdFirst, StatusIcon::First, L"\u7b2c\u4e00\u9875", iconColor);
        addIcon(CmdPrev, StatusIcon::Prev, L"\u4e0a\u4e00\u9875", iconColor);

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
        addIcon(CmdNext, StatusIcon::Next, L"\u4e0b\u4e00\u9875", iconColor);
        addIcon(CmdLast, StatusIcon::Last, L"\u6700\u540e\u4e00\u9875", iconColor);

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
        addStatusIcon(CmdTheme, StatusIcon::Theme, L"\u4e3b\u9898\uff1a" + std::wstring(T().name), 34, iconColor);
        addStatusIcon(CmdView, StatusIcon::View, L"\u9605\u8bfb\u89c6\u56fe", 34, iconColor);
        addStatusIcon(CmdZoomMenu, StatusIcon::Actual, L"\u9875\u9762\u7f29\u653e", 34, iconColor);
        separator();
        addStatusIcon(CmdZoomOut, StatusIcon::ZoomOut, L"\u7f29\u5c0f", 34, iconColor);
        std::wstring zoom = tab ? std::to_wstring(static_cast<int>(tab->zoom * 100.0 + 0.5)) + L"%" : L"100%";
        std::wstring zoomText = zoom == L"100%" ? L"\u5b9e\u9645\u5927\u5c0f" : zoom;
        RECT zr{ rx + S(3), bar.top + S(7), rx + S(108), bar.bottom - S(7) };
        AddButton(CmdZoomMenu, zr, zoomText, L"\u5f53\u524d\u7f29\u653e\u6bd4\u4f8b");
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
        addStatusIcon(CmdZoomIn, StatusIcon::ZoomIn, L"\u653e\u5927", 34, iconColor);
        addStatusIcon(CmdImmersive, StatusIcon::Fullscreen, L"\u6c89\u6d78\u6a21\u5f0f", 34, iconColor);
        addStatusIcon(CmdStatusPin, StatusIcon::Pin, g_statusPinned ? L"\u53d6\u6d88\u56fa\u5b9a\u5de5\u5177\u680f" : L"\u56fa\u5b9a\u5de5\u5177\u680f", 34, g_statusPinned ? T().accent : iconColor);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    }

    bool PaintReader(HDC dc, RECT client, RECT paintClip)
    {
        PdfTab* tab = Active();
        g_recentRects.clear();
        if (!tab)
        {
            g_sidebar = {};
            g_stage = RECT{ 0, S(76), client.right, client.bottom };
            g_mainScrollTrackRect = {};
            g_mainScrollThumbRect = {};
            g_mainHScrollTrackRect = {};
            g_mainHScrollThumbRect = {};
            g_sidebarRowPages.clear();
            g_sidebarRowRects.clear();
            g_sideTabRects.clear();
            g_searchResultRects.clear();
            g_searchResultIds.clear();
            if (g_homeListView) PaintHomeList(dc, client);
            else PaintHomeGrid(dc, client);
            PaintHomeScrollBar(dc, client, paintClip);
            return true;
        }
        int top = g_immersive ? 0 : S(76);
        int bottom = (g_immersive || !g_statusPinned) ? 0 : S(34);
        int side = (SidebarVisible()) ? g_sidebarWidth : 0;
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
        bool canFlush = true;
        RECT sidebarPaint{ 0, top, side, client.bottom - bottom };
        if (side && Intersects(paintClip, sidebarPaint))
        {
            PaintSidebar(dc, *tab, paintClip);
        }
        if (side)
        {
            int lineW = std::max(1, S(1));
            RECT splitter{ side, top, side + lineW, client.bottom - bottom };
            if (Intersects(paintClip, splitter)) GlassDivider(dc, splitter);
        }
        if (Intersects(paintClip, g_stage))
        {
            canFlush = PaintPages(dc, *tab, paintClip);
            paintedPages = true;
        }
        if (paintedPages) ClampScroll(g_hwnd);
        PaintMainScrollBar(dc, client, paintClip);
        if (Intersects(paintClip, g_stage) || Intersects(paintClip, g_annotationToolbarRect) || Intersects(paintClip, g_annotationNotePanelRect))
            PaintAnnotationToolbar(dc, *tab, client);
        return canFlush;
    }

    COLORREF MenuPopupFill()
    {
        if (IsAuroraTheme()) return T().panel;
        if (IsBeigeTheme()) return RGB(251, 247, 239);
        if (IsGlassTheme()) return MixColor(T().panel2, T().bg, 0.20);
        if (IsDarkTheme()) return T().panel;
        return T().panel;
    }

    COLORREF MenuPopupLine()
    {
        if (IsAuroraTheme()) return MixColor(T().line, T().accent, 0.12);
        if (IsBeigeTheme()) return RGB(222, 217, 208);
        if (IsDarkTheme()) return MixColor(T().line, T().muted, 0.12);
        return T().line;
    }

    COLORREF MenuPopupBorder()
    {
        COLORREF fill = MenuPopupFill();
        if (IsAuroraTheme()) return MixColor(fill, T().accent, 0.26);
        if (IsBeigeTheme()) return RGB(211, 206, 198);
        if (IsDarkTheme()) return MixColor(fill, RGB(255, 255, 255), 0.20);
        return MixColor(fill, RGB(86, 96, 108), 0.24);
    }

    COLORREF MenuPopupHoverFill()
    {
        if (IsAuroraTheme()) return MixColor(T().panel, T().accent, 0.085);
        if (IsBeigeTheme()) return MixColor(RGB(251, 247, 239), RGB(0, 0, 0), 0.035);
        if (IsGlassTheme()) return MixColor(T().panel2, T().accent, 0.085);
        if (IsDarkTheme()) return MixColor(T().panel2, RGB(255, 255, 255), 0.045);
        return SidebarHoverFill();
    }

    COLORREF MenuPopupPressedFill()
    {
        if (IsAuroraTheme()) return MixColor(T().panel, T().accent, 0.13);
        if (IsBeigeTheme()) return MixColor(RGB(251, 247, 239), RGB(111, 155, 255), 0.16);
        if (IsDarkTheme()) return MixColor(T().panel2, T().accent, 0.12);
        return MixColor(T().panel2, T().accent, 0.13);
    }

    COLORREF MenuPopupText(bool disabled)
    {
        if (disabled) return MixColor(T().muted, MenuPopupFill(), IsDarkTheme() || IsAuroraTheme() ? 0.38 : 0.28);
        return T().text;
    }

    COLORREF MenuPopupShortcutText(bool disabled)
    {
        if (disabled) return MixColor(T().muted, MenuPopupFill(), IsDarkTheme() || IsAuroraTheme() ? 0.46 : 0.36);
        if (IsAuroraTheme()) return MixColor(T().muted, T().accent, 0.20);
        return T().muted;
    }

    ThemedMenuItem* AddThemedMenuStorage(std::wstring text, bool separator)
    {
        auto item = std::make_unique<ThemedMenuItem>();
        item->text = std::move(text);
        item->separator = separator;
        ThemedMenuItem* raw = item.get();
        g_themedMenuItems.push_back(std::move(item));
        return raw;
    }

    void AppendThemedMenuItem(HMENU menu, UINT id, const wchar_t* text, bool checked = false, bool disabled = false)
    {
        ThemedMenuItem* item = AddThemedMenuStorage(text ? text : L"", false);
        item->id = id;
        item->checked = checked;
        item->disabled = disabled;
        UINT flags = MF_OWNERDRAW | (checked ? MF_CHECKED : 0) | (disabled ? (MF_GRAYED | MF_DISABLED) : 0);
        AppendMenuW(menu, flags, id, reinterpret_cast<LPCWSTR>(item));
    }

    void AppendThemedMenuSeparator(HMENU menu)
    {
        ThemedMenuItem* item = AddThemedMenuStorage(L"", true);
        AppendMenuW(menu, MF_OWNERDRAW | MF_SEPARATOR, 0, reinterpret_cast<LPCWSTR>(item));
    }

    void ShowCustomThemedPopupMenu(HWND hwnd, HMENU menu, int x, int y);

    void ShowThemedPopupMenu(HWND hwnd, HMENU menu, int x, int y)
    {
        ShowCustomThemedPopupMenu(hwnd, menu, x, y);
    }


    int ThemedMenuFontPt(ThemedMenuItem const* item)
    {
        if (item && item->id >= 2410 && item->id <= 2422) return 10;
        return 9;
    }
    bool MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure)
    {
        if (!measure || measure->CtlType != ODT_MENU) return false;
        auto* item = reinterpret_cast<ThemedMenuItem*>(measure->itemData);
        if (!item || item->separator)
        {
            measure->itemWidth = S(112);
            measure->itemHeight = S(6);
            return true;
        }

        HDC dc = GetDC(g_hwnd);
        HFONT font = Font(ThemedMenuFontPt(item));
        HGDIOBJ old = SelectObject(dc, font);
        SIZE mainSize{};
        SIZE shortcutSize{};
        std::wstring mainText = item->text;
        std::wstring shortcut;
        size_t tab = mainText.find(L'\t');
        if (tab != std::wstring::npos)
        {
            shortcut = mainText.substr(tab + 1);
            mainText = mainText.substr(0, tab);
        }
        GetTextExtentPoint32W(dc, mainText.c_str(), static_cast<int>(mainText.size()), &mainSize);
        if (!shortcut.empty()) GetTextExtentPoint32W(dc, shortcut.c_str(), static_cast<int>(shortcut.size()), &shortcutSize);
        SelectObject(dc, old);
        DeleteObject(font);
        ReleaseDC(g_hwnd, dc);

        int textW = mainSize.cx + (shortcut.empty() ? 0 : shortcutSize.cx + S(30));
        measure->itemWidth = std::max(S(112), textW + (shortcut.empty() ? S(54) : S(62)));
        measure->itemHeight = S(25);
        return true;
    }

    void DrawThemedMenuCheck(HDC dc, RECT r, COLORREF color)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GdiColor(color, 240), std::max<Gdiplus::REAL>(1.65f, S(1.15f)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        float w = static_cast<float>(std::max<int>(1, static_cast<int>(r.right - r.left)));
        float h = static_cast<float>(std::max<int>(1, static_cast<int>(r.bottom - r.top)));
        float x1 = static_cast<float>(r.left) + w * 0.16f;
        float y1 = static_cast<float>(r.top) + h * 0.53f;
        float x2 = static_cast<float>(r.left) + w * 0.42f;
        float y2 = static_cast<float>(r.top) + h * 0.76f;
        float x3 = static_cast<float>(r.left) + w * 0.86f;
        float y3 = static_cast<float>(r.top) + h * 0.24f;
        g.DrawLine(&pen, x1, y1, x2, y2);
        g.DrawLine(&pen, x2, y2, x3, y3);
    }

    bool DrawThemedMenuItem(DRAWITEMSTRUCT* draw)
    {
        if (!draw || draw->CtlType != ODT_MENU) return false;
        auto* item = reinterpret_cast<ThemedMenuItem*>(draw->itemData);
        RECT r = draw->rcItem;
        Fill(draw->hDC, r, MenuPopupFill());
        if (!item) return true;

        if (item->separator)
        {
            int y = (r.top + r.bottom) / 2;
            RECT line{ r.left + S(13), y, r.right - S(13), y + std::max(1, S(1)) };
            Fill(draw->hDC, line, MenuPopupLine());
            return true;
        }

        bool selected = (draw->itemState & ODS_SELECTED) != 0;
        bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        bool checked = (draw->itemState & ODS_CHECKED) != 0;
        if (selected && !disabled)
        {
            RECT hot{ r.left + S(3), r.top + S(2), r.right - S(3), r.bottom - S(2) };
            COLORREF fill = (draw->itemState & ODS_HOTLIGHT) ? MenuPopupPressedFill() : MenuPopupHoverFill();
            RoundFill(draw->hDC, hot, S(6), fill, fill);
        }

        if (checked)
        {
            RECT check{ r.left + S(10), r.top + S(6), r.left + S(23), r.bottom - S(6) };
            DrawThemedMenuCheck(draw->hDC, check, disabled ? MenuPopupShortcutText(true) : T().accent);
        }

        std::wstring mainText = item->text;
        std::wstring shortcut;
        size_t tab = mainText.find(L'\t');
        if (tab != std::wstring::npos)
        {
            shortcut = mainText.substr(tab + 1);
            mainText = mainText.substr(0, tab);
        }

        HFONT font = Font(ThemedMenuFontPt(item));
        HGDIOBJ old = SelectObject(draw->hDC, font);
        RECT textRect{ r.left + S(28), r.top, r.right - S(10), r.bottom };
        if (!shortcut.empty()) textRect.right -= S(76);
        Text(draw->hDC, mainText, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, MenuPopupText(disabled));
        if (!shortcut.empty())
        {
            RECT shortcutRect{ std::max(textRect.right + S(6), r.right - S(70)), r.top, r.right - S(10), r.bottom };
            Text(draw->hDC, shortcut, shortcutRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, MenuPopupShortcutText(disabled));
        }
        SelectObject(draw->hDC, old);
        DeleteObject(font);
        return true;
    }
    int ThemedMenuHitTest(POINT pt)
    {
        for (int i = 0; i < static_cast<int>(g_themedMenuItemRects.size()); ++i)
        {
            if (Hit(g_themedMenuItemRects[static_cast<size_t>(i)], pt.x, pt.y))
                return i;
        }
        return -1;
    }

    void DrawThemedMenuEntry(HDC dc, ThemedMenuItem const& item, RECT r, bool hover, bool pressed)
    {
        if (item.separator)
        {
            int y = (r.top + r.bottom) / 2;
            RECT line{ r.left + S(13), y, r.right - S(13), y + std::max(1, S(1)) };
            Fill(dc, line, MenuPopupLine());
            return;
        }

        if (hover && !item.disabled)
        {
            RECT hot{ r.left + S(3), r.top + S(2), r.right - S(3), r.bottom - S(2) };
            COLORREF fill = pressed ? MenuPopupPressedFill() : MenuPopupHoverFill();
            RoundFill(dc, hot, S(6), fill, fill);
        }

        if (item.checked)
        {
            RECT check{ r.left + S(10), r.top + S(6), r.left + S(23), r.bottom - S(6) };
            DrawThemedMenuCheck(dc, check, item.disabled ? MenuPopupShortcutText(true) : T().accent);
        }

        std::wstring mainText = item.text;
        std::wstring shortcut;
        size_t tab = mainText.find(L'\t');
        if (tab != std::wstring::npos)
        {
            shortcut = mainText.substr(tab + 1);
            mainText = mainText.substr(0, tab);
        }

        HFONT font = Font(ThemedMenuFontPt(&item));
        HGDIOBJ old = SelectObject(dc, font);
        RECT textRect{ r.left + S(28), r.top, r.right - S(10), r.bottom };
        if (!shortcut.empty()) textRect.right -= S(76);
        Text(dc, mainText, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, MenuPopupText(item.disabled));
        if (!shortcut.empty())
        {
            RECT shortcutRect{ std::max(textRect.right + S(6), r.right - S(70)), r.top, r.right - S(10), r.bottom };
            Text(dc, shortcut, shortcutRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, MenuPopupShortcutText(item.disabled));
        }
        SelectObject(dc, old);
        DeleteObject(font);
    }

    void PaintThemedPopupMenu(HWND hwnd)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        int w = std::max<int>(1, client.right - client.left);
        int h = std::max<int>(1, client.bottom - client.top);

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ oldBitmap = SelectObject(mem, bitmap);

        RECT local{ 0, 0, w, h };
        Fill(mem, local, MenuPopupFill());
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        Gdiplus::SolidBrush fill(GdiColor(MenuPopupFill()));
        FillGdiRoundRect(g, fill, Rf(local), S(6.0f));
        Gdiplus::Pen border(GdiColor(MenuPopupBorder()), std::max<Gdiplus::REAL>(0.85f, S(1.0f)));
        float borderInset = S(0.75f);
        Gdiplus::RectF borderRect{
            borderInset,
            borderInset,
            std::max(1.0f, static_cast<float>(w) - borderInset * 2.0f),
            std::max(1.0f, static_cast<float>(h) - borderInset * 2.0f) };
        DrawGdiRoundRect(g, border, borderRect, S(5.25f));

        for (int i = 0; i < static_cast<int>(g_themedMenuItems.size()) && i < static_cast<int>(g_themedMenuItemRects.size()); ++i)
        {
            DrawThemedMenuEntry(mem, *g_themedMenuItems[static_cast<size_t>(i)], g_themedMenuItemRects[static_cast<size_t>(i)], i == g_themedMenuHover, i == g_themedMenuPressed);
        }

        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }
    void DestroyThemedPopupWindowOnly()
    {
        HWND popup = g_themedMenuPopup;
        if (popup)
        {
            g_themedMenuPopup = nullptr;
            if (GetCapture() == popup) ReleaseCapture();
            DestroyWindow(popup);
        }
        g_themedMenuOwner = nullptr;
        g_themedMenuHover = -1;
        g_themedMenuPressed = -1;
    }

    void CloseThemedPopupMenu()
    {
        HWND popup = g_themedMenuPopup;
        if (popup)
        {
            g_themedMenuPopup = nullptr;
            if (GetCapture() == popup) ReleaseCapture();
            DestroyWindow(popup);
        }
        g_themedMenuOwner = nullptr;
        g_themedMenuItemRects.clear();
        g_themedMenuItems.clear();
        g_themedMenuHover = -1;
        g_themedMenuPressed = -1;
    }

    LRESULT CALLBACK ThemedMenuPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintThemedPopupMenu(hwnd);
            return 0;
        case WM_MOUSEMOVE:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hover = ThemedMenuHitTest(pt);
            if (hover >= 0 && g_themedMenuItems[static_cast<size_t>(hover)]->separator) hover = -1;
            if (hover != g_themedMenuHover)
            {
                g_themedMenuHover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hitIndex = ThemedMenuHitTest(pt);
            if (hitIndex < 0)
            {
                HWND owner = g_themedMenuOwner;
                POINT screenPt{};
                GetCursorPos(&screenPt);
                CloseThemedPopupMenu();
                if (msg == WM_RBUTTONDOWN && owner && IsWindow(owner))
                {
                    POINT clientPt = screenPt;
                    ScreenToClient(owner, &clientPt);
                    PostMessageW(owner, WM_RBUTTONDOWN, 0, MAKELPARAM(clientPt.x, clientPt.y));
                }
                return 0;
            }
            ThemedMenuItem const& item = *g_themedMenuItems[static_cast<size_t>(hitIndex)];
            g_themedMenuPressed = (!item.separator && !item.disabled) ? hitIndex : -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hitIndex = ThemedMenuHitTest(pt);
            int pressed = g_themedMenuPressed;
            g_themedMenuPressed = -1;
            if (pressed >= 0 && pressed == hitIndex && pressed < static_cast<int>(g_themedMenuItems.size()))
            {
                UINT id = g_themedMenuItems[static_cast<size_t>(pressed)]->id;
                HWND owner = g_themedMenuOwner;
                CloseThemedPopupMenu();
                if (owner && id) SendMessageW(owner, WM_COMMAND, id, 0);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd && g_themedMenuPopup == hwnd)
                CloseThemedPopupMenu();
            return 0;
        case WM_NCDESTROY:
        {
            bool wasActivePopup = g_themedMenuPopup == hwnd;
            if (wasActivePopup) g_themedMenuPopup = nullptr;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (wasActivePopup)
            {
                g_themedMenuOwner = nullptr;
                g_themedMenuItemRects.clear();
                g_themedMenuItems.clear();
                g_themedMenuHover = -1;
                g_themedMenuPressed = -1;
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    ATOM EnsureThemedMenuPopupClass()
    {
        static ATOM atom{};
        if (atom) return atom;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = 0;
        wc.lpfnWndProc = ThemedMenuPopupProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuietPDF.ThemedPopupMenu";
        atom = RegisterClassExW(&wc);
        return atom;
    }

    void ShowCustomThemedPopupMenu(HWND hwnd, HMENU menu, int x, int y)
    {
        DestroyMenu(menu);
        DestroyThemedPopupWindowOnly();
        if (g_themedMenuItems.empty()) return;
        if (!EnsureThemedMenuPopupClass())
        {
            g_themedMenuItems.clear();
            return;
        }

        HDC dc = GetDC(hwnd);
        HFONT font = Font(8);
        HGDIOBJ old = SelectObject(dc, font);
        int width = S(112);
        int height = S(6);
        std::vector<int> heights;
        heights.reserve(g_themedMenuItems.size());
        for (auto const& item : g_themedMenuItems)
        {
            int rowH = item->separator ? S(6) : S(25);
            heights.push_back(rowH);
            if (!item->separator)
            {
                std::wstring mainText = item->text;
                std::wstring shortcut;
                size_t tab = mainText.find(L'\t');
                if (tab != std::wstring::npos)
                {
                    shortcut = mainText.substr(tab + 1);
                    mainText = mainText.substr(0, tab);
                }
                HFONT rowFont = Font(ThemedMenuFontPt(item.get()));
                HGDIOBJ oldRowFont = SelectObject(dc, rowFont);
                SIZE mainSize{};
                SIZE shortcutSize{};
                GetTextExtentPoint32W(dc, mainText.c_str(), static_cast<int>(mainText.size()), &mainSize);
                if (!shortcut.empty()) GetTextExtentPoint32W(dc, shortcut.c_str(), static_cast<int>(shortcut.size()), &shortcutSize);
                SelectObject(dc, oldRowFont);
                DeleteObject(rowFont);
                int textW = mainSize.cx + (shortcut.empty() ? 0 : shortcutSize.cx + S(30));
                width = std::max(width, textW + (shortcut.empty() ? S(54) : S(62)));
            }
            height += rowH;
        }
        height += S(6);
        SelectObject(dc, old);
        DeleteObject(font);
        ReleaseDC(hwnd, dc);

        int popupWidth = width;
        int popupHeight = height;

        g_themedMenuItemRects.clear();
        int rowTop = S(3);
        for (int h : heights)
        {
            g_themedMenuItemRects.push_back(RECT{ S(3), rowTop, width - S(3), rowTop + h });
            rowTop += h;
        }

        POINT pt{ x, y };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        if (x + popupWidth > mi.rcWork.right - S(4)) x = mi.rcWork.right - popupWidth - S(4);
        if (y + popupHeight > mi.rcWork.bottom - S(4)) y = mi.rcWork.bottom - popupHeight - S(4);
        x = std::max<int>(static_cast<int>(mi.rcWork.left) + S(4), x);
        y = std::max<int>(static_cast<int>(mi.rcWork.top) + S(4), y);

        g_themedMenuOwner = hwnd;
        g_themedMenuHover = -1;
        g_themedMenuPressed = -1;
        HWND popup = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"QuietPDF.ThemedPopupMenu",
            L"",
            WS_POPUP,
            x,
            y,
            popupWidth,
            popupHeight,
            hwnd,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (!popup)
        {
            CloseThemedPopupMenu();
            return;
        }
        g_themedMenuPopup = popup;
        int cornerPreference = 3;
        DwmSetWindowAttribute(popup, 33, &cornerPreference, sizeof(cornerPreference));
        DWORD borderColor = 0xFFFFFFFE;
        DwmSetWindowAttribute(popup, 34, &borderColor, sizeof(borderColor));
        int ncRenderingPolicy = 1;
        DwmSetWindowAttribute(popup, 2, &ncRenderingPolicy, sizeof(ncRenderingPolicy));
        HRGN popupRegion = CreateRoundRectRgn(0, 0, popupWidth + 1, popupHeight + 1, S(12), S(12));
        SetWindowRgn(popup, popupRegion, FALSE);
        ShowWindow(popup, SW_SHOWNOACTIVATE);
        UpdateWindow(popup);
        SetCapture(popup);
    }
    void ShowThemeMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(std::size(g_themes)); ++i)
            AppendThemedMenuItem(menu, 2200 + i, g_themes[i].name, i == g_theme);
        ShowThemedPopupMenu(hwnd, menu, pt.x, pt.y);
    }

    void ShowViewMenuModern(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2300, L"\u5355\u9875\u89c6\u56fe", g_viewMode == 0);
        AppendThemedMenuItem(menu, 2301, L"\u53cc\u9875\u89c6\u56fe", g_viewMode == 1);
        AppendThemedMenuItem(menu, 2302, L"\u4e66\u672c\u89c6\u56fe", g_viewMode == 2);
        AppendThemedMenuSeparator(menu);
        AppendThemedMenuItem(menu, 2303, L"\u8fde\u7eed\u9605\u8bfb", g_continuousRead);
        ShowThemedPopupMenu(hwnd, menu, pt.x, pt.y);
    }
    void ShowViewMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2300, L"\u8fde\u7eed\u6eda\u52a8", g_viewMode == 0);
        AppendThemedMenuItem(menu, 2301, L"\u5355\u9875\u9605\u8bfb", g_viewMode == 1);
        AppendThemedMenuItem(menu, 2302, L"\u53cc\u9875\u9605\u8bfb", g_viewMode == 2);
        ShowThemedPopupMenu(hwnd, menu, pt.x, pt.y);
    }
    void ShowZoomMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        PdfTab* tab = Active();
        int current = tab ? static_cast<int>(tab->zoom * 100.0 + 0.5) : 100;
        AppendThemedMenuItem(menu, 2400, L"\u5b9e\u9645\u5927\u5c0f", current == 100);
        AppendThemedMenuItem(menu, 2401, L"\u9002\u5408\u9875\u9762");
        AppendThemedMenuItem(menu, 2402, L"\u9002\u5408\u5bbd\u5ea6");
        AppendThemedMenuItem(menu, 2403, L"\u9002\u5408\u5185\u5bb9");
        AppendThemedMenuItem(menu, 2404, L"\u81ea\u5b9a\u4e49\u7f29\u653e");
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
            AppendThemedMenuItem(menu, item.id, item.label, current == pct);
        }
        AppendThemedMenuSeparator(menu);
        AppendThemedMenuItem(menu, 2430, L"\u8bbe\u7f6e\u9ed8\u8ba4\u7f29\u653e\u6bd4\u4f8b");
        ShowThemedPopupMenu(hwnd, menu, pt.x, pt.y);
    }

    void ShowHelpMenu(HWND hwnd)
    {
        RECT anchor{};
        for (auto const& button : g_buttons) if (button.id == CmdSettings) anchor = button.rect;
        POINT pt{ anchor.left, anchor.bottom };
        ClientToScreen(hwnd, &pt);
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2601, L"\u6253\u5370", false, !Active());
        AppendThemedMenuSeparator(menu);
        AppendThemedMenuItem(menu, 2600, L"\u8bbe\u7f6e\u4e3a\u9ed8\u8ba4 PDF \u9605\u8bfb\u5668");
        ShowThemedPopupMenu(hwnd, menu, pt.x, pt.y);
    }

    bool LaunchOpenWithForDefaultPdf(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab || tab->path.empty() || !std::filesystem::exists(tab->path))
            return false;

        MessageBoxW(
            hwnd,
            L"\u5c06\u6253\u5f00 Windows \u9ed8\u8ba4\u5e94\u7528\u8bbe\u7f6e\u9875\uff0c\u8bf7\u5728 PDF \u6587\u4ef6\u7c7b\u578b\u4e2d\u9009\u62e9\u8f7b\u9605PDF\u3002",
            AppName,
            MB_OK | MB_ICONINFORMATION);

        std::wstring args = L"shell32.dll,OpenAs_RunDLL \"" + tab->path + L"\"";
        auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(hwnd, L"open", L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
        return result > 32;
    }

    void OpenDefaultPdfReaderSettings(HWND hwnd)
    {
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd,
            L"open",
            L"ms-settings:defaultapps?registeredAppUser=QingYuePDF",
            nullptr,
            nullptr,
            SW_SHOWNORMAL));
        if (result <= 32)
        {
            result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL));
        }
        if (result <= 32)
        {
            result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", L"control.exe", L"/name Microsoft.DefaultPrograms", nullptr, SW_SHOWNORMAL));
        }
    }

    bool PrintPageImage(HDC printerDc, PdfTab& tab, int pageIndex)
    {
        if (pageIndex < 0 || pageIndex >= static_cast<int>(tab.pages.size())) return false;
        int printableW = GetDeviceCaps(printerDc, HORZRES);
        int printableH = GetDeviceCaps(printerDc, VERTRES);
        int offsetX = GetDeviceCaps(printerDc, PHYSICALOFFSETX);
        int offsetY = GetDeviceCaps(printerDc, PHYSICALOFFSETY);
        int physicalW = GetDeviceCaps(printerDc, PHYSICALWIDTH);
        int physicalH = GetDeviceCaps(printerDc, PHYSICALHEIGHT);
        if (printableW <= 0 || printableH <= 0) return false;

        PageInfo const& page = tab.pages[static_cast<size_t>(pageIndex)];
        double pageWPt = std::max(1.0, page.widthPt);
        double pageHPt = std::max(1.0, page.heightPt);
        double renderScale = std::min(
            static_cast<double>(printableW) / pageWPt,
            static_cast<double>(printableH) / pageHPt);
        int destW = std::max(1, static_cast<int>(pageWPt * renderScale + 0.5));
        int destH = std::max(1, static_cast<int>(pageHPt * renderScale + 0.5));
        int destX = (physicalW - destW) / 2 - offsetX;
        int destY = (physicalH - destH) / 2 - offsetY;

        PageImage image = RenderPageAtScale(tab, pageIndex, renderScale, false);
        if (image.bgra.empty()) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        int oldMode = SetStretchBltMode(printerDc, HALFTONE);
        POINT oldBrush{};
        SetBrushOrgEx(printerDc, 0, 0, &oldBrush);
        int ok = StretchDIBits(
            printerDc,
            destX,
            destY,
            destW,
            destH,
            0,
            0,
            image.width,
            image.height,
            image.bgra.data(),
            &info,
            DIB_RGB_COLORS,
            SRCCOPY);
        SetBrushOrgEx(printerDc, oldBrush.x, oldBrush.y, nullptr);
        SetStretchBltMode(printerDc, oldMode);
        return ok != GDI_ERROR;
    }

    void PrintActiveDocument(HWND hwnd)
    {
        PdfTab* tab = Active();
        if (!tab || tab->pageCount <= 0)
        {
            MessageBoxW(hwnd, L"\u5f53\u524d\u6ca1\u6709\u6253\u5f00\u4efb\u4f55 PDF \u6587\u6863\u3002", AppName, MB_OK | MB_ICONINFORMATION);
            return;
        }

        PRINTDLGW pd{};
        pd.lStructSize = sizeof(pd);
        pd.hwndOwner = hwnd;
        pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION;
        pd.nMinPage = 1;
        pd.nMaxPage = static_cast<WORD>(std::min(tab->pageCount, 65535));
        pd.nFromPage = 1;
        pd.nToPage = pd.nMaxPage;
        if (!PrintDlgW(&pd)) return;
        if (!pd.hDC) return;

        int fromPage = 1;
        int toPage = tab->pageCount;
        if ((pd.Flags & PD_PAGENUMS) != 0)
        {
            fromPage = std::clamp<int>(pd.nFromPage, 1, tab->pageCount);
            toPage = std::clamp<int>(pd.nToPage, fromPage, tab->pageCount);
        }
        int copies = (pd.Flags & PD_USEDEVMODECOPIESANDCOLLATE) ? std::max<int>(1, pd.nCopies) : 1;

        DOCINFOW doc{};
        doc.cbSize = sizeof(doc);
        std::wstring docName = tab->name.empty() ? std::filesystem::path(tab->path).filename().wstring() : tab->name;
        doc.lpszDocName = docName.empty() ? AppName : docName.c_str();

        bool failed = false;
        if (StartDocW(pd.hDC, &doc) <= 0)
        {
            failed = true;
        }
        else
        {
            for (int copy = 0; copy < copies && !failed; ++copy)
            {
                for (int pageNo = fromPage; pageNo <= toPage; ++pageNo)
                {
                    int pageIndex = pageNo - 1;
                    if (StartPage(pd.hDC) <= 0)
                    {
                        failed = true;
                        break;
                    }
                    if (!PrintPageImage(pd.hDC, *tab, pageIndex))
                    {
                        failed = true;
                    }
                    if (EndPage(pd.hDC) <= 0)
                    {
                        failed = true;
                        break;
                    }
                }
            }
            if (failed) AbortDoc(pd.hDC);
            else EndDoc(pd.hDC);
        }

        DeleteDC(pd.hDC);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        if (failed)
            MessageBoxW(hwnd, L"\u6253\u5370\u5931\u8d25\u3002", AppName, MB_OK | MB_ICONERROR);
    }

    void ShowTabMenu(HWND hwnd, int x, int y)
    {
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2101, L"\u5173\u95ed");
        AppendThemedMenuItem(menu, 2102, L"\u5173\u95ed\u5176\u4ed6\u6807\u7b7e");
        AppendThemedMenuItem(menu, 2103, L"\u5173\u95ed\u53f3\u4fa7\u6807\u7b7e");
        AppendThemedMenuItem(menu, 2104, L"\u5173\u95ed\u5de6\u4fa7\u6807\u7b7e");
        AppendThemedMenuSeparator(menu);
        AppendThemedMenuItem(menu, 2105, L"\u590d\u5236\u6587\u4ef6\u8def\u5f84");
        AppendThemedMenuItem(menu, 2106, L"\u6253\u5f00\u6240\u5728\u6587\u4ef6\u5939");
        AppendThemedMenuItem(menu, 2107, L"\u5c5e\u6027");
        ShowThemedPopupMenu(hwnd, menu, x, y);
    }

    void ShowSelectionMenu(HWND hwnd, int x, int y, bool imageOnly)
    {
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2501, imageOnly ? L"\u590d\u5236\u56fe\u7247" : L"\u590d\u5236\u9009\u62e9\u5185\u5bb9");
        if (!imageOnly)
        {
            AppendThemedMenuSeparator(menu);
            AppendThemedMenuItem(menu, 2502, L"\u9ad8\u4eae");
            AppendThemedMenuItem(menu, 2503, L"\u4e0b\u5212\u7ebf");
            AppendThemedMenuItem(menu, 2504, L"\u5220\u9664\u7ebf");
            AppendThemedMenuSeparator(menu);
            AppendThemedMenuItem(menu, 2505, L"\u6dfb\u52a0\u4fbf\u7b7e");
        }
        ShowThemedPopupMenu(hwnd, menu, x, y);
    }

    void ShowSelectionMenuWithShortcuts(HWND hwnd, int x, int y, bool imageOnly)
    {
        HMENU menu = CreatePopupMenu();
        AppendThemedMenuItem(menu, 2501, imageOnly ? L"\u590d\u5236\u56fe\u7247\tCtrl + C" : L"\u590d\u5236\u9009\u62e9\u5185\u5bb9\tCtrl + C");
        if (!imageOnly)
        {
            AppendThemedMenuSeparator(menu);
            AppendThemedMenuItem(menu, 2502, L"\u9ad8\u4eae\tCtrl + H");
            AppendThemedMenuItem(menu, 2503, L"\u4e0b\u5212\u7ebf\tCtrl + U");
            AppendThemedMenuItem(menu, 2504, L"\u5220\u9664\u7ebf\tCtrl + K");
            AppendThemedMenuSeparator(menu);
            AppendThemedMenuItem(menu, 2505, L"\u6dfb\u52a0\u4fbf\u7b7e");
        }
        ShowThemedPopupMenu(hwnd, menu, x, y);
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
        annotation.author = L"10726";
        annotation.text = g_selectionText;
        for (fz_quad const& quad : g_selectionQuads)
        {
            double sx0 = std::min({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            double sy0 = std::min({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
            double sx1 = std::max({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x });
            double sy1 = std::max({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y });
            AppendUniqueSegment(annotation.segments, AnnotationSegment{ sx0, sy0, sx1, sy1 });
        }
        if (annotation.segments.empty())
            annotation.segments.push_back(AnnotationSegment{ annotation.x0, annotation.y0, annotation.x1, annotation.y1 });
        tab->annotations.push_back(annotation);
        g_activeAnnotation = static_cast<int>(tab->annotations.size()) - 1;
        g_annotationToolbarOpen = true;
        tab->dirty = true;
        ClearTextSelection();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void LoadAnnotationsFromPdf(PdfTab& tab)
    {
        tab.annotations.clear();
        if (!g_pdf.loadAnnotations || !tab.ctx || !tab.doc || tab.pageCount <= 0) return;

        constexpr int MaxPdfAnnotations = 4096;
        std::vector<QuietPdfAnnotationInfo> items(MaxPdfAnnotations);
        int count = g_pdf.loadAnnotations(tab.ctx, tab.doc, items.data(), static_cast<int>(items.size()));
        if (count <= 0) return;
        count = std::min<int>(count, static_cast<int>(items.size()));
        tab.annotations.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            QuietPdfAnnotationInfo const& item = items[static_cast<size_t>(i)];
            PdfAnnotation annotation;
            annotation.page = std::clamp(item.page, 0, tab.pageCount - 1);
            annotation.x0 = std::clamp<double>(item.x0, 0.0, 1.0);
            annotation.y0 = std::clamp<double>(item.y0, 0.0, 1.0);
            annotation.x1 = std::clamp<double>(item.x1, 0.0, 1.0);
            annotation.y1 = std::clamp<double>(item.y1, 0.0, 1.0);
            annotation.kind = item.kind == 1 ? AnnotationKind::Underline : (item.kind == 2 ? AnnotationKind::Strikeout : AnnotationKind::Highlight);
            annotation.colorIndex = AnnotationColorIndexFromRgb(item.colorR, item.colorG, item.colorB);
            annotation.createdAt = item.createdAtMs > 0 ? item.createdAtMs : NowMs();
            annotation.persisted = true;
            AppendUniqueSegment(annotation.segments, AnnotationSegment{ annotation.x0, annotation.y0, annotation.x1, annotation.y1 });
            annotation.author = FromUtf8(item.author);
            if (annotation.author.empty()) annotation.author = L"10726";
            annotation.note = FromUtf8(item.contents);
            annotation.text = annotation.note;
            bool merged = false;
            for (auto& existing : tab.annotations)
            {
                if (CanMergeLoadedAnnotation(existing, annotation))
                {
                    AppendUniqueSegment(existing.segments, AnnotationSegment{ annotation.x0, annotation.y0, annotation.x1, annotation.y1 });
                    existing.x0 = std::min(existing.x0, annotation.x0);
                    existing.y0 = std::min(existing.y0, annotation.y0);
                    existing.x1 = std::max(existing.x1, annotation.x1);
                    existing.y1 = std::max(existing.y1, annotation.y1);
                    merged = true;
                    break;
                }
            }
            if (merged)
                continue;
            tab.annotations.push_back(std::move(annotation));
        }
    }

    bool ReloadPdfTab(PdfTab& tab, const std::wstring& path, int page, double zoom)
    {
        tab.renderVersion = ++g_nextRenderVersion;
        DropTab(tab);
        tab.pages.clear();
        tab.outline.clear();
        tab.cache.clear();
        tab.zoomFallbackCache.clear();
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
            LoadAnnotationsFromPdf(tab);
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

    void RestoreScrollAfterReload(HWND hwnd, PdfTab& tab, int oldScroll)
    {
        g_contentHeight = g_continuousRead ? ContinuousContentHeight(tab) : SinglePageContentHeight(tab);
        int maxScroll = std::max<int>(0, g_contentHeight - (g_stage.bottom - g_stage.top));
        g_scrollY = std::clamp(oldScroll, 0, maxScroll);
        if (g_continuousRead)
        {
            tab.currentPage = PageFromScroll(tab, g_scrollY);
            SyncSidebarProgressFromScroll(hwnd, tab);
        }
        else KeepThumbnailSelectionVisible(hwnd, tab);
        UpdateVertScrollBar(hwnd);
    }

    void SaveAnnotationsToPdf(HWND hwnd, PdfTab& tab)
    {
        if (tab.path.empty() || !tab.dirty) return;
        if (!g_pdf.saveAnnotations)
        {
            MessageBoxW(hwnd, L"\u5f53\u524d MuPDF \u7ec4\u4ef6\u4e0d\u652f\u6301\u5199\u5165 PDF \u6807\u6ce8\u3002", L"\u4fdd\u5b58", MB_OK | MB_ICONWARNING);
            return;
        }

        std::filesystem::path source(tab.path);
        std::filesystem::path temp = source;
        temp += L".quietpdf-save.tmp.pdf";
        try { std::filesystem::remove(temp); } catch (...) {}

        std::vector<std::string> notes;
        std::vector<std::string> authors;
        std::vector<QuietPdfAnnotation> annotations;
        notes.reserve(tab.annotations.size());
        authors.reserve(tab.annotations.size());
        annotations.reserve(tab.annotations.size());
        for (auto const& item : tab.annotations)
        {
            notes.push_back(ToUtf8(item.note));
            authors.push_back(ToUtf8(item.author.empty() ? L"10726" : item.author));
            COLORREF color = AnnotationColor(item);
            auto pushAnnotation = [&](double x0, double y0, double x1, double y1)
            {
                QuietPdfAnnotation out;
                out.page = item.page;
                out.kind = static_cast<int>(item.kind);
                out.x0 = static_cast<float>(x0);
                out.y0 = static_cast<float>(y0);
                out.x1 = static_cast<float>(x1);
                out.y1 = static_cast<float>(y1);
                out.colorR = static_cast<float>(GetRValue(color)) / 255.0f;
                out.colorG = static_cast<float>(GetGValue(color)) / 255.0f;
                out.colorB = static_cast<float>(GetBValue(color)) / 255.0f;
                out.opacity = item.kind == AnnotationKind::Highlight ? 0.45f : 1.0f;
                out.createdAtMs = item.createdAt > 0 ? item.createdAt : NowMs();
                out.author = authors.back().empty() ? "10726" : authors.back().c_str();
                out.contents = notes.back().empty() ? nullptr : notes.back().c_str();
                annotations.push_back(out);
            };
            if (!item.segments.empty())
            {
                std::vector<AnnotationSegment> uniqueSegments;
                uniqueSegments.reserve(item.segments.size());
                for (auto const& segment : item.segments)
                    AppendUniqueSegment(uniqueSegments, segment);
                for (auto const& segment : uniqueSegments)
                    pushAnnotation(segment.x0, segment.y0, segment.x1, segment.y1);
            }
            else pushAnnotation(item.x0, item.y0, item.x1, item.y1);
        }

        std::string tempUtf8 = ToUtf8(temp.wstring());
        int saved = g_pdf.saveAnnotations(tab.ctx, tab.doc, tempUtf8.c_str(), annotations.empty() ? nullptr : annotations.data(), static_cast<int>(annotations.size()));
        if (!saved || !std::filesystem::exists(temp))
        {
            try { std::filesystem::remove(temp); } catch (...) {}
            MessageBoxW(hwnd, L"PDF \u6807\u6ce8\u5199\u5165\u5931\u8d25\uff0c\u539f\u6587\u4ef6\u672a\u4fee\u6539\u3002", L"\u4fdd\u5b58", MB_OK | MB_ICONWARNING);
            return;
        }

        int page = tab.currentPage;
        double zoom = tab.zoom;
        int oldScroll = g_scrollY;
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
            RestoreScrollAfterReload(hwnd, tab, oldScroll);
            std::wstring msg = L"PDF \u5df2\u751f\u6210\u4e34\u65f6\u4fdd\u5b58\u6587\u4ef6\uff0c\u4f46\u66ff\u6362\u539f\u6587\u4ef6\u5931\u8d25\u3002\n\n\u539f\u56e0\uff1a" + LastErrorText(replaceError) +
                L"\n\n\u8bf7\u786e\u8ba4\u8fd9\u4e2a PDF \u6ca1\u6709\u88ab\u5176\u5b83\u7a0b\u5e8f\u5360\u7528\u3002\n\u4e34\u65f6\u6587\u4ef6\uff1a\n" + temp.wstring();
            MessageBoxW(hwnd, msg.c_str(), L"\u4fdd\u5b58", MB_OK | MB_ICONWARNING);
            return;
        }

        if (!ReloadPdfTab(tab, source.wstring(), page, zoom))
        {
            MessageBoxW(hwnd, L"PDF \u5df2\u4fdd\u5b58\uff0c\u4f46\u91cd\u65b0\u8f7d\u5165\u5931\u8d25\u3002\u8bf7\u91cd\u65b0\u6253\u5f00\u6587\u4ef6\u3002", L"\u4fdd\u5b58", MB_OK | MB_ICONWARNING);
            return;
        }
        RestoreScrollAfterReload(hwnd, tab, oldScroll);
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
            MessageBoxW(hwnd, L"\u6807\u6ce8\u65c1\u8def\u6587\u4ef6\u4fdd\u5b58\u5931\u8d25\u3002", L"\u4fdd\u5b58", MB_OK | MB_ICONWARNING);
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
        std::wstring msg = L"\u6587\u4ef6: " + tab.path +
            L"\n\u9875\u6570: " + std::to_wstring(tab.pageCount) +
            L"\n\u5f53\u524d\u9875\uff1a " + std::to_wstring(tab.currentPage + 1) +
            L"\n\u7f29\u653e: " + std::to_wstring(static_cast<int>(tab.zoom * 100.0 + 0.5)) + L"%" +
            L"\n\u6587\u4ef6\u5927\u5c0f: " + std::to_wstring(size / 1024) + L" KB";
        MessageBoxW(hwnd, msg.c_str(), L"\u5c5e\u6027", MB_OK | MB_ICONINFORMATION);
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
            g_immersiveSidebarOpen = false;
            SetWindowPos(hwnd, HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtrW(hwnd, GWL_STYLE, g_preImmersiveStyle ? g_preImmersiveStyle : WS_OVERLAPPEDWINDOW);
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
        ShowScrollBar(hwnd, SB_HORZ, FALSE);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    bool ConfirmCloseDirtyTab(HWND hwnd, PdfTab& tab)
    {
        if (!tab.dirty) return true;
        std::wstring name = tab.name.empty() ? L"\u5f53\u524d\u6587\u6863" : tab.name;
        std::wstring message = L"\u5173\u95ed\u524d\u662f\u5426\u4fdd\u5b58\u5bf9\u201c" + name + L"\u201d\u7684\u4fee\u6539\uff1f";
        int choice = MessageBoxW(hwnd, message.c_str(), AppName, MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON1);
        if (choice == IDCANCEL || choice == 0) return false;
        if (choice == IDYES)
        {
            SaveAnnotationsToPdf(hwnd, tab);
            return !tab.dirty;
        }
        tab.dirty = false;
        return true;
    }

    bool CloseTab(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_tabs.size())) return false;
        PdfTab& tab = *g_tabs[static_cast<size_t>(index)];
        if (!ConfirmCloseDirtyTab(g_hwnd, tab)) return false;
        TouchRecentProgress(tab, true);
        DropTab(tab);
        g_tabs.erase(g_tabs.begin() + index);
        if (g_tabs.empty()) g_active = -1;
        else g_active = std::clamp(g_active >= index ? g_active - 1 : g_active, 0, static_cast<int>(g_tabs.size()) - 1);
        EnsureTabWindowVisible(g_active);
        if (g_tabs.empty()) g_tabListOpen = false;
        RestoreActiveTabPosition(g_hwnd);
        SetTitle();
        StartActiveFileWatcher(g_hwnd);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
    }

    std::wstring JoinCopyDataPaths(std::vector<std::wstring> const& paths)
    {
        std::wstring payload;
        for (auto const& path : paths)
        {
            if (payload.empty()) payload = path;
            else payload += L"\n" + path;
        }
        return payload;
    }

    bool SendOpenFilesToExistingWindow(HWND target, std::vector<std::wstring> const& paths)
    {
        if (!target || paths.empty()) return false;
        std::wstring payload = JoinCopyDataPaths(paths);
        COPYDATASTRUCT cds{};
        cds.dwData = 0x51595044;
        cds.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
        cds.lpData = payload.data();
        return SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds)) != 0;
    }

    void OpenFilesFromCopyData(HWND hwnd, COPYDATASTRUCT const* cds)
    {
        if (!cds || cds->dwData != 0x51595044 || !cds->lpData || cds->cbData < sizeof(wchar_t))
            return;

        std::wstring payload(static_cast<wchar_t const*>(cds->lpData), cds->cbData / sizeof(wchar_t));
        while (!payload.empty() && payload.back() == L'\0') payload.pop_back();
        size_t start = 0;
        while (start < payload.size())
        {
            size_t end = payload.find(L'\n', start);
            std::wstring path = payload.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!path.empty() && std::filesystem::exists(path))
            {
                int existing = FindOpenTabByPath(path);
                if (existing >= 0)
                {
                    ActivateTab(hwnd, existing);
                }
                else
                {
                    try { OpenPdf(hwnd, path); }
                    catch (std::exception const& e) { MessageBoxA(hwnd, e.what(), "QingYuePDF", MB_ICONERROR); }
                }
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        SetForegroundWindow(hwnd);
    }

    int ComputeTabInsertIndexFromPoint(int x)
    {
        int total = static_cast<int>(g_tabs.size());
        if (total <= 0) return 0;
        int insert = total;
        for (size_t i = 0; i < g_tabRects.size() && i < g_tabRectIds.size(); ++i)
        {
            int id = g_tabRectIds[i];
            if (id == g_draggingTabIndex) continue;
            RECT r = g_tabRects[i];
            int mid = (r.left + r.right) / 2;
            if (x < mid)
            {
                insert = id;
                break;
            }
            insert = id + 1;
        }
        return std::clamp(insert, 0, total);
    }

    void MoveTabToInsertIndex(HWND hwnd, int from, int insertIndex)
    {
        int total = static_cast<int>(g_tabs.size());
        if (from < 0 || from >= total) return;
        int to = std::clamp(insertIndex, 0, total);
        if (to > from) --to;
        to = std::clamp(to, 0, total - 1);
        if (to == from)
        {
            EnsureTabWindowVisible(g_active);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        auto moving = std::move(g_tabs[static_cast<size_t>(from)]);
        g_tabs.erase(g_tabs.begin() + from);
        g_tabs.insert(g_tabs.begin() + to, std::move(moving));
        g_active = to;
        EnsureTabWindowVisible(g_active);
        g_tabListOpen = false;
        SetTitle();
        StartActiveFileWatcher(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    bool DetachTabToNewWindow(HWND hwnd, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_tabs.size()))
            return false;
        std::wstring path = g_tabs[static_cast<size_t>(index)]->path;
        if (path.empty() || !std::filesystem::exists(path))
            return false;

        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring args = L"--new-window \"" + path + L"\"";
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", modulePath, args.c_str(), nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            return false;
        CloseTab(index);
        return true;
    }

    void DropHomeThumbsForPath(std::wstring const& path)
    {
        for (auto it = g_homeThumbCache.begin(); it != g_homeThumbCache.end(); )
        {
            if (it->first.rfind(path + L"#", 0) == 0) it = g_homeThumbCache.erase(it);
            else ++it;
        }
    }

    void RemoveRecentAt(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_recent.size())) return;
        std::wstring path = g_recent[static_cast<size_t>(index)].path;
        g_recent.erase(g_recent.begin() + index);
        DropHomeThumbsForPath(path);
        SaveState();
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void ToggleRecentPinned(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_recent.size())) return;
        auto& item = g_recent[static_cast<size_t>(index)];
        item.pinned = !item.pinned;
        item.pinnedAt = item.pinned ? NowMs() : 0;
        SortRecent();
        SaveState();
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void ToggleRecentFavorite(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_recent.size())) return;
        auto& item = g_recent[static_cast<size_t>(index)];
        item.favorite = !item.favorite;
        SaveState();
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void OpenRecentFolder(HWND hwnd, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_recent.size())) return;
        std::wstring args = L"/select,\"" + g_recent[static_cast<size_t>(index)].path + L"\"";
        ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void ClearRecentRecords()
    {
        g_recent.clear();
        g_homeThumbCache.clear();
        SaveState();
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    bool HandleHomeAction(HWND hwnd, HomeAction action, int recentIndex)
    {
        switch (action)
        {
        case HomeAction::Pin:
            ToggleRecentPinned(recentIndex);
            return true;
        case HomeAction::Folder:
            OpenRecentFolder(hwnd, recentIndex);
            return true;
        case HomeAction::Remove:
            RemoveRecentAt(recentIndex);
            return true;
        case HomeAction::Favorite:
            ToggleRecentFavorite(recentIndex);
            return true;
        case HomeAction::ToggleView:
            g_homeListView = !g_homeListView;
            SaveState();
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        case HomeAction::ClearAll:
            g_confirmClearRecent = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        default:
            return false;
        }
    }

    void Command(HWND hwnd, int id)
    {
        PdfTab* tab = Active();
        if (id >= 2200 && id < 2200 + static_cast<int>(std::size(g_themes)))
        {
            g_theme = id - 2200;
            SaveState();
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
            ApplyWindowChrome();
            DwmFlush();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id >= 2300 && id <= 2302)
        {
            int oldPage = tab ? tab->currentPage : 0;
            g_viewMode = id - 2300;
            if (tab)
            {
                tab->currentPage = std::clamp(oldPage, 0, tab->pageCount - 1);
                g_contentHeight = g_continuousRead ? ContinuousContentHeight(*tab) : SinglePageContentHeight(*tab);
                g_contentWidth = ReadingContentWidth(*tab);
                int maxY = std::max<int>(0, g_contentHeight - std::max<int>(1, g_stage.bottom - g_stage.top));
                int maxX = std::max<int>(0, g_contentWidth - std::max<int>(1, g_stage.right - g_stage.left));
                g_scrollY = std::clamp(g_continuousRead ? SpreadTopScroll(*tab, oldPage) : g_scrollY, 0, maxY);
                g_scrollX = std::clamp(g_scrollX, 0, maxX);
                UpdateVertScrollBar(hwnd);
            }
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
                ApplyZoomAtPoint(hwnd, *tab, CurrentZoomAnchorPoint(), zoom);
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
            else if (id == 2404)
            {
                OpenCustomZoomDialog(hwnd);
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
            for (int i = static_cast<int>(g_tabs.size()) - 1; i >= 0; --i)
            {
                if (i != g_contextTab && !CloseTab(i)) return;
            }
            ActivateTab(hwnd, std::clamp(g_contextTab, 0, static_cast<int>(g_tabs.size()) - 1));
            return;
        }
        if (id == 2103 && g_contextTab >= 0) { while (static_cast<int>(g_tabs.size()) > g_contextTab + 1) { if (!CloseTab(static_cast<int>(g_tabs.size()) - 1)) return; } return; }
        if (id == 2104 && g_contextTab >= 0) { while (g_contextTab > 0) { if (!CloseTab(0)) return; --g_contextTab; } return; }
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
        if (id == 2501)
        {
            CopyCurrentSelectionToClipboard(hwnd);
            return;
        }
        if (id == 2106 && g_contextTab >= 0)
        {
            std::wstring args = L"/select,\"" + g_tabs[static_cast<size_t>(g_contextTab)]->path + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        if (id == 2107 && g_contextTab >= 0) { ShowProperties(hwnd, *g_tabs[static_cast<size_t>(g_contextTab)]); return; }
        if (id == 2600) { OpenDefaultPdfReaderSettings(hwnd); return; }
        if (id == 2601) { PrintActiveDocument(hwnd); return; }
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
        case CmdHome: g_active = -1; g_tabListOpen = false; SetTitle(); StartActiveFileWatcher(hwnd); InvalidateRect(hwnd, nullptr, FALSE); break;
        case CmdOpen: OpenDialog(hwnd); break;
        case CmdTabNavPrev:
            if (g_active > 0) ActivateTab(hwnd, g_active - 1);
            else
            {
                g_tabVisibleStart = std::max(0, g_tabVisibleStart - 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_tabListOpen = false;
            break;
        case CmdTabNavNext:
            if (g_active >= 0 && g_active + 1 < static_cast<int>(g_tabs.size())) ActivateTab(hwnd, g_active + 1);
            else
            {
                g_tabVisibleStart = std::min(std::max(0, static_cast<int>(g_tabs.size()) - MaxVisibleTabs), g_tabVisibleStart + 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_tabListOpen = false;
            break;
        case CmdSidebarPanelToggle:
            if (g_immersive)
            {
                g_immersiveSidebarOpen = !g_immersiveSidebarOpen;
                if (g_immersiveSidebarOpen)
                {
                    g_sidebarOpen = true;
                    if (g_sidebarWidth <= S(120)) g_sidebarWidth = std::clamp(g_sidebarExpandedWidth, S(220), S(520));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else StartSidebarPanelAnimation(hwnd, !g_sidebarOpen);
            break;
        case CmdTabListToggle:
            g_tabListOpen = !g_tabListOpen;
            if (g_tabListOpen)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                g_tabListContentHeight = static_cast<int>(g_tabs.size()) * S(31) + S(12);
                if (g_active >= 0)
                {
                    int desired = g_active * S(31) - S(20);
                    g_tabListScrollY = std::max(0, desired);
                }
                g_tabListScrollY = ClampTabListScrollValue(client, g_tabListScrollY);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case CmdSidebar: Command(hwnd, CmdSidebarPanelToggle); break;
        case CmdSearch:
            g_sidebarOpen = true;
            if (g_immersive) g_immersiveSidebarOpen = true;
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
                if (g_immersive)
                {
                    g_sidebarOpen = true;
                    g_immersiveSidebarOpen = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else StartSidebarPanelAnimation(hwnd, true);
            }
            break;
        case CmdSettings: ShowHelpMenu(hwnd); break;
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
            if (tab)
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
        case CmdZoomOut: if (tab) ApplyZoomAtPoint(hwnd, *tab, CurrentZoomAnchorPoint(), tab->zoom - 0.1); break;
        case CmdZoomIn: if (tab) ApplyZoomAtPoint(hwnd, *tab, CurrentZoomAnchorPoint(), tab->zoom + 0.1); break;
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
                else
                {
                    int previous = 0;
                    for (int start = 0; start < tab->currentPage; start = NextSpreadStart(*tab, start))
                        previous = start;
                    tab->currentPage = previous;
                    g_scrollY = 0;
                }
                if (!g_continuousRead) InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CmdNext:
            if (tab)
            {
                if (g_continuousRead) ScrollStageTo(hwnd, g_scrollY + std::max(S(60), PageH(*tab, tab->currentPage) + PageGap()));
                else
                {
                    tab->currentPage = std::min(tab->pageCount - 1, NextSpreadStart(*tab, FirstPageInSpread(tab->currentPage)));
                    g_scrollY = 0;
                }
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

    bool EnsurePaintBuffer(HDC targetDc, int width, int height)
    {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!g_paintBufferDc)
            g_paintBufferDc = CreateCompatibleDC(targetDc);
        if (!g_paintBufferDc) return false;
        if (g_paintBufferBitmap && g_paintBufferW == width && g_paintBufferH == height)
            return true;

        HBITMAP bitmap = CreateCompatibleBitmap(targetDc, width, height);
        if (!bitmap) return false;
        HGDIOBJ previous = SelectObject(g_paintBufferDc, bitmap);
        if (!g_paintBufferOldBitmap)
            g_paintBufferOldBitmap = previous;
        else if (previous && previous != g_paintBufferOldBitmap)
            DeleteObject(previous);

        g_paintBufferBitmap = bitmap;
        g_paintBufferW = width;
        g_paintBufferH = height;
        return true;
    }

    void ReleasePaintBuffer()
    {
        if (g_paintBufferDc)
        {
            if (g_paintBufferOldBitmap)
                SelectObject(g_paintBufferDc, g_paintBufferOldBitmap);
            if (g_paintBufferBitmap)
                DeleteObject(g_paintBufferBitmap);
            DeleteDC(g_paintBufferDc);
        }
        g_paintBufferDc = nullptr;
        g_paintBufferBitmap = nullptr;
        g_paintBufferOldBitmap = nullptr;
        g_paintBufferW = 0;
        g_paintBufferH = 0;
        if (g_imageDrawDc)
        {
            DeleteDC(g_imageDrawDc);
            g_imageDrawDc = nullptr;
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
        if (!EnsurePaintBuffer(dc, std::max<int>(1, client.right), std::max<int>(1, client.bottom)))
        {
            EndPaint(hwnd, &ps);
            return;
        }
        HDC mem = g_paintBufferDc;
        if (UsesBackdropTheme() && !StageLightPaintActive()) PaintGlassBackdrop(mem, dirty);
        else Fill(mem, dirty, T().bg);
        RECT toolbarRect{ 0, 0, client.right, S(38) };
        RECT tabsRect{ 0, S(38), client.right, S(76) };
        RECT statusRect{ 0, client.bottom - S(34), client.right, client.bottom };
        bool uiDirty = Intersects(dirty, toolbarRect) || Intersects(dirty, tabsRect) || Intersects(dirty, statusRect);
        if (g_sidebarOpen)
        {
            RECT sidebarControls{ 0, g_sidebar.top, g_sidebar.right, g_sidebar.top + S(86) };
            if (Intersects(dirty, sidebarControls)) uiDirty = true;
        }
        bool repaintUi = uiDirty || g_buttons.empty();
        if (repaintUi)
        {
            g_buttons.clear();
            PaintToolbar(mem, client);
            PaintTabs(mem, client);
        }
        bool shouldFlush = PaintReader(mem, client, dirty);
        PaintSidebarPanelToggle(mem, client);
        if (repaintUi) PaintStatus(mem, client);
        PaintTabListOverlay(mem, client);
        PaintClearRecentDialog(mem, client);
        PaintCustomZoomDialog(mem, client);
        if (!g_customZoomOpen) PaintHoverTip(mem, client);
        PaintTabDragOverlay(mem);
        if (shouldFlush) BitBlt(dc, dirty.left, dirty.top, dirtyW, dirtyH, mem, dirty.left, dirty.top, SRCCOPY);
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
            ShowScrollBar(hwnd, SB_HORZ, FALSE);
            {
                GESTURECONFIG panConfig{};
                panConfig.dwID = GID_PAN;
                panConfig.dwWant = GC_PAN | GC_PAN_WITH_GUTTER | GC_PAN_WITH_INERTIA | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY;
                panConfig.dwBlock = 0;
                SetGestureConfig(hwnd, 0, 1, &panConfig, sizeof(panConfig));
            }
            return 0;
        case WM_ACTIVATEAPP:
            if (!wParam) StopAutoScroll(hwnd);
            if (wParam) RefreshActiveFileIfChanged(hwnd);
            return 0;
        case WM_QUIETPDF_FILE_CHANGED:
            if (static_cast<unsigned long>(wParam) == g_fileWatchGeneration.load())
            {
                RefreshActiveFileIfChanged(hwnd);
            }
            return 0;
        case WM_QUIETPDF_DRAG_SCROLL:
            g_dragScrollPosted = false;
            if (g_draggingMainScroll && g_active >= 0)
            {
                POINT cursor{};
                if (GetCursorPos(&cursor))
                {
                    ScreenToClient(hwnd, &cursor);
                    g_mousePos = cursor;
                    UpdateMainScrollDragStateFromPointer(hwnd, cursor.y, true);
                }
                ScrollStageTo(hwnd, g_dragScrollTarget, true, false);
            }
            return 0;
        case WM_QUIETPDF_PAGE_RENDERED:
        {
            std::unique_ptr<PageRenderResult> result(reinterpret_cast<PageRenderResult*>(lParam));
            if (!result) return 0;
            for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
            {
                PdfTab& tab = *g_tabs[static_cast<size_t>(i)];
                PageRenderRequest const& request = result->request;
                if (_wcsicmp(tab.path.c_str(), request.path.c_str()) != 0 ||
                    tab.renderVersion != request.documentVersion || tab.zoom != request.zoom ||
                    request.dpi != g_dpi || request.theme != g_theme)
                    continue;

                PreparePageCache(tab);
                bool inserted = false;
                RECT tileDirty{};
                if (request.tile)
                {
                    TileKey key{ request.page, TileZoomKey(request.zoom), request.theme, request.level, request.x, request.y, request.w, request.h };
                    if (tab.tileCache.find(key) == tab.tileCache.end())
                    {
                        tab.tileCache.emplace(key, std::move(result->image));
                        TrimTileCache(tab, key);
                        inserted = true;
                        TileRequestScreenRect(tab, request, tileDirty);
                    }
                }
                else if (tab.cache.find(request.page) == tab.cache.end())
                {
                    tab.cache.emplace(request.page, std::move(result->image));
                    TrimPageCache(tab, request.page);
                    inserted = true;
                }
                if (inserted && i == g_active)
                {
                    if (request.tile)
                    {
                        if (!IsRectEmpty(&tileDirty))
                            InvalidateRect(hwnd, &tileDirty, FALSE);
                        if (StageScrollPaintInProgress())
                            g_stageIdleRefreshPending = true;
                    }
                    else
                    {
                        InvalidateRect(hwnd, &g_stage, FALSE);
                    }
                }
                break;
            }
            return 0;
        }
        case WM_DPICHANGED:
            g_dpi = HIWORD(wParam);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_SIZE:
            ShowScrollBar(hwnd, SB_VERT, FALSE);
            ShowScrollBar(hwnd, SB_HORZ, FALSE);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_COMMAND:
            Command(hwnd, LOWORD(wParam));
            return 0;
        case WM_MEASUREITEM:
            if (MeasureThemedMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_DRAWITEM:
            if (DrawThemedMenuItem(reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
        {
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (g_autoScrollActive)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
                if (g_resizingSidebar || HitSidebarSplitter(pt.x, pt.y))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_COPYDATA:
            OpenFilesFromCopyData(hwnd, reinterpret_cast<COPYDATASTRUCT const*>(lParam));
            return TRUE;
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
        case WM_MBUTTONDOWN:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            ToggleAutoScroll(hwnd, POINT{ x, y });
            return 0;
        }
        case WM_MBUTTONUP:
            return 0;        case WM_LBUTTONDBLCLK:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (g_customZoomOpen || g_confirmClearRecent)
                return 0;
            if (PdfTab* tab = Active())
            {
                int page = -1;
                RECT pageRect{};
                if (PageRectAtPoint(*tab, x, y, page, pageRect))
                {
                    if (g_selectingContent)
                    {
                        g_selectingContent = false;
                        ReleaseCapture();
                    }
                    if (TrySelectWordAtPoint(hwnd, *tab, page, pageRect, POINT{ x, y }))
                        return 0;
                    ClearTextSelection();
                    InvalidateRect(hwnd, &g_stage, FALSE);
                    return 0;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (g_autoScrollActive)
            {
                StopAutoScroll(hwnd);
                return 0;
            }
            if (g_customZoomOpen)
            {
                if (Hit(g_zoomDialogInputRect, x, y))
                {
                    g_customZoomFocused = true;
                    g_customZoomCaretVisible = true;
                    SetTimer(hwnd, 17, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (Hit(g_zoomDialogConfirmRect, x, y))
                {
                    g_pressedButtonRect = g_zoomDialogConfirmRect;
                    g_hasPressedButton = true;
                    SetTimer(hwnd, 2, 90, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    CommitCustomZoomDialog(hwnd);
                    return 0;
                }
                if (Hit(g_zoomDialogCancelRect, x, y) || !Hit(g_zoomDialogRect, x, y))
                {
                    if (Hit(g_zoomDialogCancelRect, x, y))
                    {
                        g_pressedButtonRect = g_zoomDialogCancelRect;
                        g_hasPressedButton = true;
                        SetTimer(hwnd, 2, 90, nullptr);
                    }
                    CloseCustomZoomDialog(hwnd);
                    return 0;
                }
                return 0;
            }
            if (g_confirmClearRecent)
            {
                if (Hit(g_clearDialogConfirmRect, x, y))
                {
                    g_confirmClearRecent = false;
                    ClearRecentRecords();
                    return 0;
                }
                if (Hit(g_clearDialogCancelRect, x, y) || Hit(g_clearDialogCloseRect, x, y))
                {
                    g_confirmClearRecent = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }
            if (g_tabListOpen)
            {
                if (Hit(g_tabListScrollThumbRect, x, y))
                {
                    g_draggingTabListScroll = true;
                    g_tabListDragOffset = y - g_tabListScrollThumbRect.top;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g_tabListRect, FALSE);
                    return 0;
                }
                if (Hit(g_tabListScrollTrackRect, x, y))
                {
                    int trackH = std::max<int>(1, g_tabListScrollTrackRect.bottom - g_tabListScrollTrackRect.top);
                    int thumbH = std::max<int>(1, g_tabListScrollThumbRect.bottom - g_tabListScrollThumbRect.top);
                    int visible = std::max<int>(1, g_tabListRect.bottom - g_tabListRect.top - S(12));
                    int maxScroll = std::max<int>(0, g_tabListContentHeight - visible);
                    int travel = std::max<int>(1, trackH - thumbH);
                    int thumbTop = std::clamp<int>(y - thumbH / 2, g_tabListScrollTrackRect.top, g_tabListScrollTrackRect.bottom - thumbH);
                    ScrollTabListTo(hwnd, MulDiv(thumbTop - g_tabListScrollTrackRect.top, maxScroll, travel));
                    return 0;
                }
                for (size_t i = 0; i < g_tabListItemRects.size() && i < g_tabListItemIds.size(); ++i)
                {
                    if (Hit(g_tabListItemRects[i], x, y))
                    {
                        ActivateTab(hwnd, g_tabListItemIds[i]);
                        return 0;
                    }
                }
                if (Hit(g_tabListRect, x, y))
                {
                    return 0;
                }
                if (!Hit(g_tabCountRect, x, y))
                {
                    g_tabListOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            if (Hit(g_sidebarPanelToggleRect, x, y))
            {
                g_pressedButtonRect = g_sidebarPanelToggleRect;
                g_hasPressedButton = true;
                SetTimer(hwnd, 2, 90, nullptr);
                InvalidateRect(hwnd, &g_sidebarPanelToggleRect, FALSE);
                Command(hwnd, CmdSidebarPanelToggle);
                return 0;
            }
            if (!Active())
            {
                if (!g_homeSearchQuery.empty() && Hit(g_homeSearchClearRect, x, y))
                {
                    g_homeSearchQuery.clear();
                    g_homeSearchFocused = true;
                    g_homeSearchCaretVisible = true;
                    SetTimer(hwnd, 15, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (Hit(g_homeSearchRect, x, y))
                {
                    g_homeSearchFocused = true;
                    g_homeSearchCaretVisible = true;
                    SetTimer(hwnd, 15, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                g_homeSearchFocused = false;
            }
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
            if (!Active() && Hit(g_homeScrollThumbRect, x, y))
            {
                g_draggingHomeScroll = true;
                g_homeScrollDragOffset = y - g_homeScrollThumbRect.top;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_homeScrollTrackRect, FALSE);
                return 0;
            }
            if (!Active() && Hit(g_homeScrollTrackRect, x, y))
            {
                int trackH = std::max<int>(1, g_homeScrollTrackRect.bottom - g_homeScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_homeScrollThumbRect.bottom - g_homeScrollThumbRect.top);
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT viewport = HomeContentViewportRect(client);
                int visible = std::max<int>(1, viewport.bottom - viewport.top);
                int maxScroll = std::max<int>(0, g_homeContentHeight - visible);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(y - thumbH / 2, g_homeScrollTrackRect.top, g_homeScrollTrackRect.bottom - thumbH);
                ScrollHomeTo(hwnd, MulDiv(thumbTop - g_homeScrollTrackRect.top, maxScroll, travel));
                return 0;
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
                CancelStageMotion(hwnd);
                g_draggingMainScroll = true;
                g_mainScrollDragOffset = y - g_mainScrollThumbRect.top;
                g_mainScrollDragStartY = y;
                g_mainScrollDragStartPos = g_scrollY;
                g_dragScrollTarget = g_scrollY;
                g_dragScrollPosted = false;
                g_mainScrollThumbLive = true;
                g_mainScrollThumbLiveTop = g_mainScrollThumbRect.top;
                g_mainScrollThumbLiveHeight = std::max<int>(1, g_mainScrollThumbRect.bottom - g_mainScrollThumbRect.top);
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
                return 0;
            }
            if (Active() && Hit(g_mainScrollTrackRect, x, y))
            {
                CancelStageMotion(hwnd);
                int trackH = std::max<int>(1, g_mainScrollTrackRect.bottom - g_mainScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_mainScrollThumbRect.bottom - g_mainScrollThumbRect.top);
                int stageH = std::max<int>(1, g_stage.bottom - g_stage.top);
                int maxScroll = std::max<int>(0, g_contentHeight - stageH);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(y - thumbH / 2, g_mainScrollTrackRect.top, g_mainScrollTrackRect.bottom - thumbH);
                int newScroll = MulDiv(thumbTop - g_mainScrollTrackRect.top, maxScroll, travel);
                ScrollStageTo(hwnd, newScroll, true, true);
                return 0;
            }
            if (Active() && Hit(g_mainHScrollThumbRect, x, y))
            {
                g_draggingMainHScroll = true;
                g_mainHScrollDragOffset = x - g_mainHScrollThumbRect.left;
                g_mainHScrollDragStartX = x;
                g_mainHScrollDragStartPos = g_scrollX;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
                return 0;
            }
            if (Active() && Hit(g_mainHScrollTrackRect, x, y))
            {
                int trackW = std::max<int>(1, g_mainHScrollTrackRect.right - g_mainHScrollTrackRect.left);
                int thumbW = std::max<int>(1, g_mainHScrollThumbRect.right - g_mainHScrollThumbRect.left);
                int stageW = std::max<int>(1, g_stage.right - g_stage.left);
                int maxScroll = std::max<int>(0, g_contentWidth - stageW);
                int travel = std::max<int>(1, trackW - thumbW);
                int thumbLeft = std::clamp<int>(x - thumbW / 2, g_mainHScrollTrackRect.left, g_mainHScrollTrackRect.right - thumbW);
                int newScroll = MulDiv(thumbLeft - g_mainHScrollTrackRect.left, maxScroll, travel);
                ScrollStageXTo(hwnd, newScroll);
                return 0;
            }
            if (Active() && (g_sideMode == 0 || g_sideMode == 1))
            {
                UpdateSidebarScrollRects();
                if (Hit(g_sidebarScrollThumbRect, x, y))
                {
                    g_draggingSidebarScroll = true;
                    g_sidebarScrollDragOffset = y - g_sidebarScrollThumbRect.top;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g_sidebarScrollTrackRect, FALSE);
                    return 0;
                }
                if (Hit(g_sidebarScrollTrackRect, x, y))
                {
                    int trackH = std::max<int>(1, g_sidebarScrollTrackRect.bottom - g_sidebarScrollTrackRect.top);
                    int thumbH = std::max<int>(1, g_sidebarScrollThumbRect.bottom - g_sidebarScrollThumbRect.top);
                    RECT area = SidebarScrollRect();
                    int visible = std::max<int>(1, area.bottom - area.top);
                    int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
                    int travel = std::max<int>(1, trackH - thumbH);
                    int thumbTop = std::clamp<int>(y - thumbH / 2, g_sidebarScrollTrackRect.top, g_sidebarScrollTrackRect.bottom - thumbH);
                    ScrollSidebarTo(hwnd, MulDiv(thumbTop - g_sidebarScrollTrackRect.top, maxScroll, travel));
                    return 0;
                }
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
                int tabId = i < g_tabRectIds.size() ? g_tabRectIds[i] : static_cast<int>(i);
                RECT close{ g_tabRects[i].right - S(24), g_tabRects[i].top + S(2), g_tabRects[i].right - S(4), g_tabRects[i].bottom - S(2) };
                if (Hit(close, x, y)) { CloseTab(tabId); return 0; }
                if (Hit(g_tabRects[i], x, y))
                {
                    g_pressedButtonRect = g_tabRects[i];
                    g_hasPressedButton = true;
                    SetTimer(hwnd, 2, 90, nullptr);
                    InvalidateRect(hwnd, &g_tabRects[i], FALSE);
                    ActivateTab(hwnd, tabId);
                    g_draggingTab = true;
                    g_tabDetachArmed = false;
                    g_tabDragFloating = false;
                    g_draggingTabIndex = tabId;
                    g_tabDragStart = POINT{ x, y };
                    g_tabDragOffset = POINT{ x - g_tabRects[i].left, y - g_tabRects[i].top };
                    g_tabDragSourceRect = g_tabRects[i];
                    g_tabDragCurrentRect = g_tabRects[i];
                    g_tabDragInsertIndex = tabId;
                    SetCapture(hwnd);
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
            if (!Active())
            {
                for (size_t i = 0; i < g_homeHits.size(); ++i)
                {
                    if (Hit(g_homeHits[i].rect, x, y))
                    {
                        g_pressedButtonRect = g_homeHits[i].rect;
                        g_hasPressedButton = true;
                        SetTimer(hwnd, 2, 90, nullptr);
                        InvalidateRect(hwnd, &g_homeHits[i].rect, FALSE);
                        if (HandleHomeAction(hwnd, g_homeHits[i].action, g_homeHits[i].recentIndex)) return 0;
                    }
                }
            }
            if (!Active()) for (size_t i = 0; i < g_recentRects.size(); ++i)
            {
                if (!Hit(g_recentRects[i], x, y)) continue;
                if (i == 0) OpenDialog(hwnd);
                else if (i - 1 < g_homeRecentIds.size())
                {
                    int recentIndex = g_homeRecentIds[i - 1];
                    if (recentIndex >= 0 && recentIndex < static_cast<int>(g_recent.size()) &&
                        std::filesystem::exists(g_recent[static_cast<size_t>(recentIndex)].path))
                    {
                        OpenPdf(hwnd, g_recent[static_cast<size_t>(recentIndex)].path);
                    }
                }
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
                    g_selectionIsImage = false;
                    g_selectionPage = page;
                    g_selectionStart = POINT{ x, y };
                    g_selectionEnd = POINT{ x, y };
                    g_selectionPageRect = pageRect;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &g_stage, FALSE);
                    return 0;
                }
            }
            if (SidebarVisible() && Hit(g_sidebar, x, y))
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
        case WM_RBUTTONDOWN:
        {
            if (g_autoScrollActive)
            {
                StopAutoScroll(hwnd);
                return 0;
            }
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            for (size_t i = 0; i < g_tabRects.size(); ++i)
            {
                if (Hit(g_tabRects[i], x, y))
                {
                    POINT pt{ x, y };
                    ClientToScreen(hwnd, &pt);
                    g_contextTab = i < g_tabRectIds.size() ? g_tabRectIds[i] : static_cast<int>(i);
                    ShowTabMenu(hwnd, pt.x, pt.y);
                    return 0;
                }
            }
            if (g_hasContentSelection && Hit(g_stage, x, y))
            {
                POINT pt{ x, y };
                ClientToScreen(hwnd, &pt);
                ShowSelectionMenuWithShortcuts(hwnd, pt.x, pt.y, HasImageSelection());
                return 0;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            g_mousePos = POINT{ mx, my };
            if (g_customZoomOpen)
            {
                g_hasHoverTip = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_confirmClearRecent)
            {
                g_hasHoverTip = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_draggingTab)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                int dx = mx - g_tabDragStart.x;
                int dy = my - g_tabDragStart.y;
                bool moved = (dx * dx + dy * dy) > S(8) * S(8);
                int tabTop = g_tabRects.empty() ? S(38) : g_tabRects.front().top;
                int tabBottom = g_tabRects.empty() ? S(76) : g_tabRects.front().bottom;
                g_tabDetachArmed = moved && (mx < 0 || mx > client.right || my < tabTop - S(12) || my > tabBottom + S(36));
                if (moved)
                {
                    g_tabDragFloating = true;
                    int w = g_tabDragSourceRect.right - g_tabDragSourceRect.left;
                    int h = g_tabDragSourceRect.bottom - g_tabDragSourceRect.top;
                    g_tabDragCurrentRect = RECT{
                        mx - g_tabDragOffset.x,
                        my - g_tabDragOffset.y,
                        mx - g_tabDragOffset.x + w,
                        my - g_tabDragOffset.y + h
                    };
                    if (!g_tabDetachArmed)
                        g_tabDragInsertIndex = ComputeTabInsertIndexFromPoint(mx);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, g_tabDetachArmed ? IDC_SIZEALL : IDC_ARROW));
                return 0;
            }
            bool oldMainScrollHover = g_mainScrollHover;
            bool oldSidebarPanelToggleHover = g_sidebarPanelToggleHover;
            bool oldMainHScrollHover = g_mainHScrollHover;
            bool oldHomeScrollHover = g_homeScrollHover;
            bool oldSidebarScrollHover = g_sidebarScrollHover;
            g_mainScrollHover = Hit(g_mainScrollTrackRect, mx, my);
            g_mainHScrollHover = Hit(g_mainHScrollTrackRect, mx, my);
            g_homeScrollHover = Hit(g_homeScrollTrackRect, mx, my);
            g_sidebarScrollHover = SidebarVisible() && (g_sideMode == 0 || g_sideMode == 1) && Hit(SidebarScrollRect(), mx, my);
            g_sidebarPanelToggleHover = Hit(g_sidebarPanelToggleRect, mx, my);
            if (oldSidebarPanelToggleHover != g_sidebarPanelToggleHover)
            {
                InvalidateRect(hwnd, &g_sidebarPanelToggleRect, FALSE);
            }
            if (oldMainScrollHover != g_mainScrollHover)
            {
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
            }
            if (oldMainHScrollHover != g_mainHScrollHover)
            {
                InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
            }
            if (oldHomeScrollHover != g_homeScrollHover)
            {
                InvalidateRect(hwnd, &g_homeScrollTrackRect, FALSE);
            }
            if (oldSidebarScrollHover != g_sidebarScrollHover)
            {
                RECT area = SidebarScrollRect();
                InvalidateRect(hwnd, &area, FALSE);
            }
            if (g_draggingMainScroll)
            {
                UpdateMainScrollDragStateFromPointer(hwnd, my, true);
                if (!g_dragScrollPosted)
                {
                    g_dragScrollPosted = true;
                    PostMessageW(hwnd, WM_QUIETPDF_DRAG_SCROLL, 0, 0);
                }
                return 0;
            }
            if (g_draggingMainHScroll)
            {
                int trackW = std::max<int>(1, g_mainHScrollTrackRect.right - g_mainHScrollTrackRect.left);
                int thumbW = std::max<int>(1, g_mainHScrollThumbRect.right - g_mainHScrollThumbRect.left);
                int stageW = std::max<int>(1, g_stage.right - g_stage.left);
                int maxScroll = std::max<int>(0, g_contentWidth - stageW);
                int travel = std::max<int>(1, trackW - thumbW);
                int dragDelta = mx - g_mainHScrollDragStartX;
                int newScroll = g_mainHScrollDragStartPos;
                if (travel > 0 && maxScroll > 0)
                    newScroll = g_mainHScrollDragStartPos + MulDiv(dragDelta, maxScroll, travel);
                ScrollStageXTo(hwnd, newScroll);
                return 0;
            }
            if (g_draggingHomeScroll)
            {
                int trackH = std::max<int>(1, g_homeScrollTrackRect.bottom - g_homeScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_homeScrollThumbRect.bottom - g_homeScrollThumbRect.top);
                RECT client{};
                GetClientRect(hwnd, &client);
                RECT viewport = HomeContentViewportRect(client);
                int visible = std::max<int>(1, viewport.bottom - viewport.top);
                int maxScroll = std::max<int>(0, g_homeContentHeight - visible);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(my - g_homeScrollDragOffset, g_homeScrollTrackRect.top, g_homeScrollTrackRect.bottom - thumbH);
                ScrollHomeTo(hwnd, MulDiv(thumbTop - g_homeScrollTrackRect.top, maxScroll, travel));
                return 0;
            }
            if (g_draggingSidebarScroll)
            {
                UpdateSidebarScrollRects();
                int trackH = std::max<int>(1, g_sidebarScrollTrackRect.bottom - g_sidebarScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_sidebarScrollThumbRect.bottom - g_sidebarScrollThumbRect.top);
                RECT area = SidebarScrollRect();
                int visible = std::max<int>(1, area.bottom - area.top);
                int maxScroll = std::max<int>(0, g_sidebarContentHeight - visible);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(my - g_sidebarScrollDragOffset, g_sidebarScrollTrackRect.top, g_sidebarScrollTrackRect.bottom - thumbH);
                ScrollSidebarTo(hwnd, MulDiv(thumbTop - g_sidebarScrollTrackRect.top, maxScroll, travel));
                return 0;
            }
            if (g_draggingTabListScroll)
            {
                int trackH = std::max<int>(1, g_tabListScrollTrackRect.bottom - g_tabListScrollTrackRect.top);
                int thumbH = std::max<int>(1, g_tabListScrollThumbRect.bottom - g_tabListScrollThumbRect.top);
                int visible = std::max<int>(1, g_tabListRect.bottom - g_tabListRect.top - S(12));
                int maxScroll = std::max<int>(0, g_tabListContentHeight - visible);
                int travel = std::max<int>(1, trackH - thumbH);
                int thumbTop = std::clamp<int>(my - g_tabListDragOffset, g_tabListScrollTrackRect.top, g_tabListScrollTrackRect.bottom - thumbH);
                ScrollTabListTo(hwnd, MulDiv(thumbTop - g_tabListScrollTrackRect.top, maxScroll, travel));
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
            int oldHoverHomeHit = g_hoverHomeHit;
            int oldHoverRecentRect = g_hoverRecentRect;
            int oldHoverTabListItem = g_hoverTabListItem;
            bool oldHasHoverButton = g_hasHoverButton;
            RECT oldHoverButtonRect = g_hoverButtonRect;
            g_hoverTab = -1;
            g_hoverTabClose = -1;
            g_hoverOutlineRow = -1;
            g_hoverOutlineToggle = -1;
            g_hoverSearchResult = -1;
            g_hoverHomeHit = -1;
            g_hoverRecentRect = -1;
            g_hoverTabListItem = -1;
            g_hasHoverButton = false;
            g_hoverButtonRect = RECT{};
            for (size_t i = 0; i < g_tabRects.size(); ++i)
            {
                int tabId = i < g_tabRectIds.size() ? g_tabRectIds[i] : static_cast<int>(i);
                if (Hit(g_tabRects[i], mx, my)) g_hoverTab = tabId;
                RECT close{ g_tabRects[i].right - S(24), g_tabRects[i].top + S(2), g_tabRects[i].right - S(4), g_tabRects[i].bottom - S(2) };
                if (Hit(close, mx, my))
                {
                    g_hoverTabClose = tabId;
                    break;
                }
            }
            if (g_tabListOpen)
            {
                for (size_t i = 0; i < g_tabListItemRects.size() && i < g_tabListItemIds.size(); ++i)
                {
                    if (Hit(g_tabListItemRects[i], mx, my))
                    {
                        g_hoverTabListItem = g_tabListItemIds[i];
                        break;
                    }
                }
            }
            if (SidebarVisible() && g_sideMode == 0)
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
            if (SidebarVisible() && g_sideMode == 2)
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
            if (g_hoverTab >= 0)
            {
                for (size_t i = 0; i < g_tabRects.size() && i < g_tabRectIds.size() && i < g_tabTitleTruncated.size(); ++i)
                {
                    if (g_tabRectIds[i] == g_hoverTab && g_tabTitleTruncated[i])
                    {
                        g_hasHoverTip = true;
                        g_hoverTip = g_tabs[static_cast<size_t>(g_hoverTab)]->name;
                        g_hoverTipRect = g_tabRects[i];
                        break;
                    }
                }
            }
            if (!Active())
            {
                for (size_t i = 0; i < g_recentRects.size(); ++i)
                {
                    if (Hit(g_recentRects[i], mx, my))
                    {
                        g_hoverRecentRect = static_cast<int>(i);
                        break;
                    }
                }
                for (size_t i = 0; i < g_homeHits.size(); ++i)
                {
                    if (Hit(g_homeHits[i].rect, mx, my))
                    {
                        g_hoverHomeHit = static_cast<int>(i);
                        if (!g_homeHits[i].tip.empty())
                        {
                            g_hasHoverTip = true;
                            g_hoverTip = g_homeHits[i].tip;
                            g_hoverTipRect = g_homeHits[i].rect;
                        }
                        break;
                    }
                }
            }
            for (auto const& b : g_buttons)
            {
                if (Hit(b.rect, mx, my))
                {
                    g_hasHoverButton = true;
                    g_hoverButtonRect = b.rect;
                }
                if (!g_hasHoverTip && !b.tip.empty() && Hit(b.rect, mx, my))
                {
                    g_hasHoverTip = true;
                    g_hoverTip = b.tip;
                    g_hoverTipRect = b.rect;
                    break;
                }
            }
            if (hadTip != g_hasHoverTip || oldTip != g_hoverTip || oldHoverTabClose != g_hoverTabClose || oldHoverTab != g_hoverTab || oldHoverHomeHit != g_hoverHomeHit || oldHoverRecentRect != g_hoverRecentRect || oldHoverTabListItem != g_hoverTabListItem)
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
            if (g_draggingTab)
            {
                int index = g_draggingTabIndex;
                int insertIndex = g_tabDragInsertIndex;
                bool detach = g_tabDetachArmed;
                bool floating = g_tabDragFloating;
                g_draggingTab = false;
                g_tabDetachArmed = false;
                g_tabDragFloating = false;
                g_draggingTabIndex = -1;
                g_tabDragInsertIndex = -1;
                ReleaseCapture();
                if (detach)
                    DetachTabToNewWindow(hwnd, index);
                else if (floating)
                    MoveTabToInsertIndex(hwnd, index, insertIndex);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_selectingContent)
            {
                RECT selection = CurrentSelectionRect();
                bool largeEnough =
                    (selection.right - selection.left) >= S(8) &&
                    (selection.bottom - selection.top) >= S(6);
                bool imageHit = false;
                if (PdfTab* tab = Active())
                {
                    RefreshTextSelection(*tab);
                    if (!largeEnough && g_selectionQuads.empty())
                        imageHit = TrySelectImageAtPoint(*tab, g_selectionPage, g_selectionPageRect, g_selectionEnd);
                }
                g_selectingContent = false;
                ReleaseCapture();
                g_selectionIsImage = imageHit || (largeEnough && g_selectionQuads.empty());
                g_hasContentSelection = imageHit || (largeEnough && (!g_selectionQuads.empty() || g_selectionIsImage));
                if (!g_hasContentSelection)
                {
                    g_selectionQuads.clear();
                    g_selectionText.clear();
                    g_selectionIsImage = false;
                }
                InvalidateRect(hwnd, &g_stage, FALSE);
                return 0;
            }
            if (g_draggingMainScroll)
            {
                UpdateMainScrollDragStateFromPointer(hwnd, GET_Y_LPARAM(lParam), true);
                g_draggingMainScroll = false;
                g_mainScrollThumbLive = false;
                g_dragScrollPosted = false;
                ScrollStageTo(hwnd, g_dragScrollTarget, true, false);
                if (PdfTab* tab = Active())
                {
                    SaveActiveTabProgress();
                    if (g_continuousRead) SyncSidebarProgressFromScroll(hwnd, *tab);
                }
                ReleaseCapture();
                InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
                return 0;
            }
            if (g_draggingMainHScroll)
            {
                g_draggingMainHScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
                return 0;
            }
            if (g_draggingHomeScroll)
            {
                g_draggingHomeScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, &g_homeScrollTrackRect, FALSE);
                return 0;
            }
            if (g_draggingSidebarScroll)
            {
                g_draggingSidebarScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, &g_sidebar, FALSE);
                return 0;
            }
            if (g_draggingTabListScroll)
            {
                g_draggingTabListScroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, &g_tabListRect, FALSE);
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
                if (StageLightPaintActive())
                {
                    SetTimer(hwnd, 3, 80, nullptr);
                    return 0;
                }
                g_pagePrefetchPending = false;
                PrefetchOnePage();
                return 0;
            }
            if (wParam == 4)
            {
                if (!g_smoothStageScroll)
                {
                    KillTimer(hwnd, 4);
                    return 0;
                }
                int current = g_scrollY;
                int target = ClampStageScrollValue(g_stageScrollTarget);
                int delta = target - current;
                if (delta == 0)
                {
                    g_stageScrollVelocity = 0.0;
                    g_smoothStageScroll = false;
                    KillTimer(hwnd, 4);
                    return 0;
                }
                double step = static_cast<double>(delta) * 0.30;
                int dy = step < 0.0 ? static_cast<int>(std::floor(step)) : static_cast<int>(std::ceil(step));
                int nextScroll = ClampStageScrollValue(current + dy);
                ScrollStageTo(hwnd, nextScroll, true);
                g_stageScrollPos = static_cast<double>(nextScroll);
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
                ULONGLONG now = GetTickCount64();
                if (now < g_stageLightPaintUntil)
                {
                    SetTimer(hwnd, 8, static_cast<UINT>(std::max<ULONGLONG>(10, g_stageLightPaintUntil - now)), nullptr);
                    return 0;
                }
                KillTimer(hwnd, 8);
                g_stageLightPaintUntil = 0;
                if (PdfTab* tab = Active())
                {
                    TouchRecentProgress(*tab, false);
                    if (g_sidebarProgressRefreshPending)
                    {
                        SyncSidebarProgressFromScroll(hwnd, *tab);
                        g_sidebarProgressRefreshPending = false;
                    }
                }
                if (g_stageIdleRefreshPending)
                {
                    g_stageIdleRefreshPending = false;
                    InvalidateRect(hwnd, &g_stage, FALSE);
                    if (!IsRectEmpty(&g_mainScrollTrackRect))
                        InvalidateRect(hwnd, &g_mainScrollTrackRect, FALSE);
                    if (!IsRectEmpty(&g_mainHScrollTrackRect))
                        InvalidateRect(hwnd, &g_mainHScrollTrackRect, FALSE);
                }
                SchedulePagePrefetch(hwnd);
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
            if (wParam == 15)
            {
                if (!g_homeSearchFocused || Active())
                {
                    g_homeSearchCaretVisible = true;
                    KillTimer(hwnd, 15);
                    return 0;
                }
                g_homeSearchCaretVisible = !g_homeSearchCaretVisible;
                InvalidateRect(hwnd, &g_homeSearchRect, FALSE);
                return 0;
            }
            if (wParam == 16)
            {
                FlushTouchPan(hwnd);
                return 0;
            }
            if (wParam == 17)
            {
                if (!g_customZoomOpen || !g_customZoomFocused)
                {
                    g_customZoomCaretVisible = true;
                    KillTimer(hwnd, 17);
                    return 0;
                }
                g_customZoomCaretVisible = !g_customZoomCaretVisible;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == 18)
            {
                StepTouchMomentum(hwnd);
                return 0;
            }
            if (wParam == 19)
            {
                StepAutoScroll(hwnd);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
        {
            StopAutoScroll(hwnd);
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            ScrollByWheelDelta(hwnd, pt, delta, (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
            return 0;
        }
        case WM_MOUSEHWHEEL:
        {
            StopAutoScroll(hwnd);
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int pixels = WheelPixels(delta, S(64), g_stageHWheelRemainder);
            if (pixels != 0) ScrollStageXTo(hwnd, g_scrollX + pixels);
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
                StopTouchMomentum(hwnd);
                g_gesturePanning = false;
                g_pageWheelDeltaAccum = 0;
                g_gestureLastX = pt.x;
                g_gestureLastY = pt.y;
            }
            else if (info.dwID == GID_END)
            {
                FlushTouchPan(hwnd);
                g_gesturePanning = false;
                g_gestureSidebar = false;
                g_pageWheelDeltaAccum = 0;
            }
            else if (info.dwID == GID_PAN)
            {
                if ((info.dwFlags & GF_BEGIN) != 0 || !g_gesturePanning)
                {
                    g_gesturePanning = true;
                    g_gestureSidebar = SidebarVisible() && Hit(g_sidebar, pt.x, pt.y);
                    g_gestureLastX = pt.x;
                    g_gestureLastY = pt.y;
                }
                else
                {
                    int deltaX = pt.x - g_gestureLastX;
                    int deltaY = pt.y - g_gestureLastY;
                    g_gestureLastX = pt.x;
                    g_gestureLastY = pt.y;
                    POINT targetPt = pt;
                    if (g_gestureSidebar)
                    {
                        targetPt.x = std::clamp(targetPt.x, g_sidebar.left, std::max(g_sidebar.left, g_sidebar.right - 1));
                        targetPt.y = std::clamp(targetPt.y, g_sidebar.top, std::max(g_sidebar.top, g_sidebar.bottom - 1));
                    }
                    QueueTouchPanPixels(hwnd, targetPt, deltaY, deltaX);
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
            ScrollStageTo(
                hwnd,
                newScroll,
                code == SB_THUMBTRACK || code == SB_THUMBPOSITION,
                code == SB_THUMBTRACK || code == SB_THUMBPOSITION);
            return 0;
        }
        case WM_HSCROLL:
        {
            int code = LOWORD(wParam);
            int newScroll = g_scrollX;
            if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION)
            {
                SCROLLINFO si{};
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_HORZ, &si);
                newScroll = si.nTrackPos;
            }
            else if (code == SB_LINELEFT) newScroll -= S(40);
            else if (code == SB_LINERIGHT) newScroll += S(40);
            else if (code == SB_PAGELEFT) newScroll -= g_stage.right - g_stage.left;
            else if (code == SB_PAGERIGHT) newScroll += g_stage.right - g_stage.left;
            else if (code == SB_LEFT) newScroll = 0;
            else if (code == SB_RIGHT) newScroll = g_contentWidth;
            ScrollStageXTo(hwnd, newScroll);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_F11) { ApplyImmersiveWindow(hwnd, !g_immersive); return 0; }
            if (wParam == VK_ESCAPE && g_autoScrollActive) { StopAutoScroll(hwnd); return 0; }
            if (g_customZoomOpen)
            {
                if (wParam == VK_RETURN)
                {
                    CommitCustomZoomDialog(hwnd);
                    return 0;
                }
                if (wParam == VK_ESCAPE)
                {
                    CloseCustomZoomDialog(hwnd);
                    return 0;
                }
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && g_hasContentSelection)
            {
                if (wParam == 'C')
                {
                    CopyCurrentSelectionToClipboard(hwnd);
                    return 0;
                }
                if (!g_selectionQuads.empty())
                {
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
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'F')
            {
                if (!Active())
                {
                    g_homeSearchFocused = true;
                    g_homeSearchCaretVisible = true;
                    SetTimer(hwnd, 15, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                g_sidebarOpen = true;
                g_sideMode = 2;
                g_searchInputFocused = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'O') { OpenDialog(hwnd); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'P')
            {
                PrintActiveDocument(hwnd);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'W') { CloseTab(g_active); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'S')
            {
                if (PdfTab* tab = Active()) SaveAnnotationsToPdf(hwnd, *tab);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_pageInputFocused) { CancelPageInput(hwnd); return 0; }
            if (wParam == VK_ESCAPE && g_homeSearchFocused)
            {
                g_homeSearchFocused = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_hasContentSelection) { ClearTextSelection(); InvalidateRect(hwnd, &g_stage, FALSE); return 0; }
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
            if (g_customZoomOpen && g_customZoomFocused)
            {
                if (wParam == VK_BACK)
                {
                    if (!g_customZoomText.empty()) g_customZoomText.pop_back();
                }
                else if (wParam == VK_RETURN)
                {
                    CommitCustomZoomDialog(hwnd);
                    return 0;
                }
                else if ((wParam >= L'0' && wParam <= L'9') && g_customZoomText.size() < 7)
                {
                    if (g_customZoomText == L"0") g_customZoomText.clear();
                    g_customZoomText.push_back(static_cast<wchar_t>(wParam));
                }
                else if (wParam == L'.' && g_customZoomText.find(L'.') == std::wstring::npos && !g_customZoomText.empty() && g_customZoomText.size() < 7)
                {
                    g_customZoomText.push_back(L'.');
                }
                g_customZoomCaretVisible = true;
                SetTimer(hwnd, 17, 500, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
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
            if (g_homeSearchFocused && !Active())
            {
                bool changed = false;
                if (wParam == VK_BACK)
                {
                    if (!g_homeSearchQuery.empty())
                    {
                        g_homeSearchQuery.pop_back();
                        changed = true;
                    }
                }
                else if (wParam == VK_RETURN)
                {
                    return 0;
                }
                else if (wParam >= 32 && g_homeSearchQuery.size() < 120)
                {
                    g_homeSearchQuery.push_back(static_cast<wchar_t>(wParam));
                    changed = true;
                }
                if (changed)
                {
                    g_homeSearchCaretVisible = true;
                    SetTimer(hwnd, 15, 500, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
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
        {
            if (g_immersive) ApplyImmersiveWindow(hwnd, false);
            StopFileWatcher();
            StopPageRenderWorker();
            bool saveLastSession = !HasOtherQingYueWindows(hwnd);
            for (auto& tab : g_tabs) TouchRecentProgress(*tab, false);
            if (saveLastSession) CaptureLastSessionFromOpenTabs();
            for (auto& tab : g_tabs) DropTab(*tab);
            ReleasePaintBuffer();
            if (g_glassBackdrop) DeleteObject(g_glassBackdrop);
            SaveState();
            PostQuitMessage(0);
            return 0;
        }
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
        std::vector<std::wstring> launchFiles;
        bool forceNewWindow = false;
        int cliArgc = 0;
        PWSTR* cliArgv = CommandLineToArgvW(GetCommandLineW(), &cliArgc);
        if (cliArgv)
        {
            for (int i = 1; i < cliArgc; ++i)
            {
                if (_wcsicmp(cliArgv[i], L"--set-default-pdf") == 0)
                {
                    bool ok = TrySetDefaultPdfAssociationDirect();
                    int status = g_defaultAssocStatus;
                    LocalFree(cliArgv);
                    return ok ? 0 : status;
                }
                if (_wcsicmp(cliArgv[i], L"--set-default-pdf-shell") == 0)
                {
                    bool ok = RegisterPdfAssociationForCurrentUser() && TrySetDefaultPdfAssociationViaShell();
                    int status = g_defaultAssocStatus;
                    LocalFree(cliArgv);
                    return ok ? 0 : status;
                }
                if (_wcsicmp(cliArgv[i], L"--new-window") == 0)
                {
                    forceNewWindow = true;
                    continue;
                }
                if (std::filesystem::exists(cliArgv[i]))
                {
                    try
                    {
                        if (_wcsicmp(std::filesystem::path(cliArgv[i]).extension().c_str(), L".pdf") == 0)
                            launchFiles.push_back(cliArgv[i]);
                    }
                    catch (...) {}
                }
            }
            LocalFree(cliArgv);
        }
        if (!forceNewWindow && !launchFiles.empty())
        {
            HWND existing = FindWindowW(L"XiaoYuePDFWindow", nullptr);
            if (existing && SendOpenFilesToExistingWindow(existing, launchFiles))
                return 0;
        }

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Gdiplus::GdiplusStartupInput gdiplusInput{};
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
        LoadState();
        g_statusOffset = g_statusPinned ? 0 : S(34);
        g_statusTargetOffset = g_statusOffset;
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"XiaoYuePDFWindow";
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, AppName, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 840, nullptr, nullptr, instance, nullptr);
        if (!g_hwnd) return 1;
        ApplyWindowChrome();
        RestoreLastSessionAndOpenLaunchFiles(g_hwnd, launchFiles);
        ShowWindow(g_hwnd, show == SW_SHOWMINIMIZED ? show : SW_SHOWMAXIMIZED);
        UpdateWindow(g_hwnd);
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



