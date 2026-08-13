#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <string>

#include "resource.h"
#include "..\version.h"

namespace
{
    constexpr wchar_t const* AppName = L"轻阅PDF";
    constexpr wchar_t const* ExeName = L"轻阅PDF.exe";
    constexpr wchar_t const* DllName = L"quietmupdf.dll";
    constexpr wchar_t const* UninstallName = L"卸载轻阅PDF.exe";
    constexpr wchar_t const* UninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QingYuePDF";

    constexpr int IdPath = 1001;
    constexpr int IdBrowse = 1002;
    constexpr int IdInstall = 1003;
    constexpr int IdCancel = 1004;
    constexpr int IdDesktop = 1005;
    constexpr int IdStatus = 1006;

    HINSTANCE g_instance{};
    HWND g_pathEdit{};
    HWND g_status{};
    HWND g_installButton{};
    HWND g_browseButton{};
    HWND g_cancelButton{};
    HWND g_desktopCheck{};
    HFONT g_titleFont{};
    HFONT g_uiFont{};

    std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR value = nullptr;
        if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value)))
        {
            return {};
        }
        std::filesystem::path result = value;
        CoTaskMemFree(value);
        return result;
    }

    std::filesystem::path DefaultInstallDir()
    {
        return KnownFolder(FOLDERID_LocalAppData) / L"Programs" / AppName;
    }

    std::filesystem::path DesktopShortcut()
    {
        return KnownFolder(FOLDERID_Desktop) / (std::wstring(AppName) + L".lnk");
    }

    std::filesystem::path StartMenuFolder()
    {
        return KnownFolder(FOLDERID_StartMenu) / L"Programs" / AppName;
    }

    std::filesystem::path StartMenuShortcut()
    {
        return StartMenuFolder() / (std::wstring(AppName) + L".lnk");
    }

    void SetStatus(std::wstring const& text)
    {
        SetWindowTextW(g_status, text.c_str());
        UpdateWindow(g_status);
    }

    std::wstring WindowText(HWND hwnd)
    {
        int len = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(len + 1), L'\0');
        GetWindowTextW(hwnd, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
        return text;
    }

    bool WriteResource(int id, wchar_t const* type, std::filesystem::path const& target)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), type);
        if (!resource)
        {
            return false;
        }
        HGLOBAL loaded = LoadResource(module, resource);
        void* data = LockResource(loaded);
        DWORD size = SizeofResource(module, resource);
        HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        BOOL ok = WriteFile(file, data, size, &written, nullptr);
        CloseHandle(file);
        return ok && written == size;
    }

    bool CopySelf(std::filesystem::path const& target)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return CopyFileW(path, target.c_str(), FALSE) != 0;
    }

    bool CreateShortcut(std::filesystem::path const& linkPath, std::filesystem::path const& target, std::filesystem::path const& workDir)
    {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link))))
        {
            return false;
        }
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetDescription(L"轻量 PDF 阅读器");
        link->SetIconLocation(target.c_str(), 0);

        IPersistFile* file = nullptr;
        HRESULT hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
        bool ok = false;
        if (SUCCEEDED(hr))
        {
            std::filesystem::create_directories(linkPath.parent_path());
            ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
            file->Release();
        }
        link->Release();
        return ok;
    }

    void WriteStringValue(HKEY key, wchar_t const* name, std::wstring const& value)
    {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    void WriteUninstallRegistry(std::filesystem::path const& dir)
    {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        auto exe = dir / ExeName;
        auto uninstaller = dir / UninstallName;
        std::wstring uninstall = L"\"" + uninstaller.wstring() + L"\" /uninstall";
        DWORD estimatedSize = 18 * 1024;
        WriteStringValue(key, L"DisplayName", AppName);
        WriteStringValue(key, L"DisplayVersion", QINGYUE_VERSION_STRING_W);
        WriteStringValue(key, L"Publisher", L"轻阅");
        WriteStringValue(key, L"DisplayIcon", exe.wstring());
        WriteStringValue(key, L"InstallLocation", dir.wstring());
        WriteStringValue(key, L"UninstallString", uninstall);
        RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&estimatedSize), sizeof(estimatedSize));
        RegCloseKey(key);
    }

    std::filesystem::path RegisteredInstallDir()
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return DefaultInstallDir();
        }
        wchar_t buffer[MAX_PATH * 4]{};
        DWORD bytes = sizeof(buffer);
        DWORD type = 0;
        std::filesystem::path result = DefaultInstallDir();
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes) == ERROR_SUCCESS && type == REG_SZ)
        {
            result = buffer;
        }
        RegCloseKey(key);
        return result;
    }

    bool BrowseForFolder(HWND owner, std::filesystem::path& selected)
    {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileDialog, reinterpret_cast<void**>(&dialog))))
        {
            return false;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"选择安装位置");

        std::wstring current = WindowText(g_pathEdit);
        if (!current.empty())
        {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(std::filesystem::path(current).parent_path().c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }

        bool ok = false;
        if (SUCCEEDED(dialog->Show(owner)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    selected = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                item->Release();
            }
        }
        dialog->Release();
        return ok;
    }

    bool InstallTo(HWND owner, std::filesystem::path const& dir, bool desktopShortcut)
    {
        try
        {
            if (dir.empty())
            {
                MessageBoxW(owner, L"请选择安装位置。", AppName, MB_OK | MB_ICONWARNING);
                return false;
            }
            SetStatus(L"正在创建安装目录...");
            std::filesystem::create_directories(dir);

            auto exe = dir / ExeName;
            auto dll = dir / DllName;
            auto uninstaller = dir / UninstallName;

            SetStatus(L"正在写入程序文件...");
            if (!WriteResource(IDR_APP_EXE, RT_RCDATA, exe) || !WriteResource(IDR_MUPDF_DLL, RT_RCDATA, dll) || !CopySelf(uninstaller))
            {
                MessageBoxW(owner, L"安装文件写入失败，请确认目录权限后重试。", AppName, MB_OK | MB_ICONERROR);
                return false;
            }

            SetStatus(L"正在创建快捷方式...");
            if (desktopShortcut)
            {
                CreateShortcut(DesktopShortcut(), exe, dir);
            }
            CreateShortcut(StartMenuShortcut(), exe, dir);

            SetStatus(L"正在注册卸载信息...");
            WriteUninstallRegistry(dir);
            SetStatus(L"安装完成。");

            int launch = MessageBoxW(owner, L"轻阅PDF 已安装完成。\n\n是否立即打开？", AppName, MB_YESNO | MB_ICONINFORMATION);
            if (launch == IDYES)
            {
                ShellExecuteW(owner, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
            }
            return true;
        }
        catch (std::exception const&)
        {
            MessageBoxW(owner, L"安装失败，请换一个有写入权限的目录后重试。", AppName, MB_OK | MB_ICONERROR);
            SetStatus(L"安装失败。");
            return false;
        }
    }

    void SetControlsEnabled(bool enabled)
    {
        EnableWindow(g_pathEdit, enabled);
        EnableWindow(g_browseButton, enabled);
        EnableWindow(g_installButton, enabled);
        EnableWindow(g_cancelButton, enabled);
        EnableWindow(g_desktopCheck, enabled);
    }

    void Layout(HWND hwnd)
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int margin = 28;
        int width = rc.right - rc.left;
        MoveWindow(GetDlgItem(hwnd, IdPath), margin, 150, width - margin * 2 - 98, 30, TRUE);
        MoveWindow(GetDlgItem(hwnd, IdBrowse), width - margin - 88, 150, 88, 30, TRUE);
        MoveWindow(GetDlgItem(hwnd, IdDesktop), margin, 194, 220, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IdStatus), margin, 242, width - margin * 2, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IdInstall), width - margin - 196, rc.bottom - 58, 88, 32, TRUE);
        MoveWindow(GetDlgItem(hwnd, IdCancel), width - margin - 96, rc.bottom - 58, 88, 32, TRUE);
    }

    LRESULT CALLBACK InstallProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g_uiFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_titleFont = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

            HWND title = CreateWindowW(L"STATIC", L"安装 轻阅PDF", WS_CHILD | WS_VISIBLE, 28, 26, 420, 34, hwnd, nullptr, g_instance, nullptr);
            HWND desc = CreateWindowW(L"STATIC", L"选择安装位置。安装完成后会创建开始菜单快捷方式，并写入系统卸载项。", WS_CHILD | WS_VISIBLE, 28, 68, 520, 44, hwnd, nullptr, g_instance, nullptr);
            HWND label = CreateWindowW(L"STATIC", L"安装位置", WS_CHILD | WS_VISIBLE, 28, 124, 140, 22, hwnd, nullptr, g_instance, nullptr);
            g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultInstallDir().c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 28, 150, 380, 30, hwnd, reinterpret_cast<HMENU>(IdPath), g_instance, nullptr);
            g_browseButton = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 420, 150, 88, 30, hwnd, reinterpret_cast<HMENU>(IdBrowse), g_instance, nullptr);
            g_desktopCheck = CreateWindowW(L"BUTTON", L"创建桌面快捷方式", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 28, 194, 220, 24, hwnd, reinterpret_cast<HMENU>(IdDesktop), g_instance, nullptr);
            g_status = CreateWindowW(L"STATIC", L"准备安装。", WS_CHILD | WS_VISIBLE, 28, 242, 520, 24, hwnd, reinterpret_cast<HMENU>(IdStatus), g_instance, nullptr);
            g_installButton = CreateWindowW(L"BUTTON", L"安装", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 356, 298, 88, 32, hwnd, reinterpret_cast<HMENU>(IdInstall), g_instance, nullptr);
            g_cancelButton = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 456, 298, 88, 32, hwnd, reinterpret_cast<HMENU>(IdCancel), g_instance, nullptr);

            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
            for (HWND child : { desc, label, g_pathEdit, g_browseButton, g_desktopCheck, g_status, g_installButton, g_cancelButton })
            {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            }
            SendMessageW(g_desktopCheck, BM_SETCHECK, BST_CHECKED, 0);
            Layout(hwnd);
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IdBrowse:
            {
                std::filesystem::path selected;
                if (BrowseForFolder(hwnd, selected))
                {
                    SetWindowTextW(g_pathEdit, selected.c_str());
                }
                return 0;
            }
            case IdInstall:
            {
                SetControlsEnabled(false);
                bool desktop = SendMessageW(g_desktopCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool ok = InstallTo(hwnd, WindowText(g_pathEdit), desktop);
                SetControlsEnabled(true);
                if (ok)
                {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            case IdCancel:
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            break;
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, RGB(246, 248, 250));
            SetTextColor(dc, RGB(25, 31, 40));
            static HBRUSH brush = CreateSolidBrush(RGB(246, 248, 250));
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_titleFont) DeleteObject(g_titleFont);
            if (g_uiFont) DeleteObject(g_uiFont);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    int ShowInstallWizard(HINSTANCE instance, int show)
    {
        g_instance = instance;
        WNDCLASSW wc{};
        wc.lpfnWndProc = InstallProc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(246, 248, 250));
        wc.lpszClassName = L"XiaoYuePDFSetupWindow";
        RegisterClassW(&wc);

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"轻阅PDF 安装向导", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 620, 390, nullptr, nullptr, instance, nullptr);
        if (!hwnd)
        {
            return 1;
        }
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    int Uninstall()
    {
        auto dir = RegisteredInstallDir();
        int confirm = MessageBoxW(nullptr, L"确定要卸载 轻阅PDF 吗？", AppName, MB_YESNO | MB_ICONQUESTION);
        if (confirm != IDYES)
        {
            return 0;
        }

        DeleteFileW(DesktopShortcut().c_str());
        DeleteFileW(StartMenuShortcut().c_str());
        std::filesystem::remove_all(StartMenuFolder());
        DeleteFileW((dir / ExeName).c_str());
        DeleteFileW((dir / DllName).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);

        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MessageBoxW(nullptr, L"轻阅PDF 已卸载。", AppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool uninstall = false;
    for (int i = 1; argv && i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"/uninstall") == 0)
        {
            uninstall = true;
        }
    }
    if (argv)
    {
        LocalFree(argv);
    }
    int result = uninstall ? Uninstall() : ShowInstallWizard(instance, show);
    CoUninitialize();
    return result;
}
