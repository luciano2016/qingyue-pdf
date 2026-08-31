# 从源码构建

本文描述 Windows x64 Release 构建。命令均在仓库根目录运行。

## 环境要求

- Windows 10/11 x64
- Visual Studio Build Tools，包含 MSVC v145、Windows 10/11 SDK 和 MSBuild
- PowerShell 5.1 或更高版本
- Git

项目文件当前使用 `PlatformToolset v145` 和 Windows SDK `10.0.26100.0`。如使用其他已安装版本，请在 Visual Studio 中调整工程属性，或通过 MSBuild 属性覆盖。

## 1. 获取 MuPDF

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\fetch-mupdf.ps1
```

脚本下载 MuPDF 1.28.0 并验证归档：

```text
URL: https://mupdf.com/downloads/archive/mupdf-1.28.0-source.tar.gz
SHA256: 21C7F064903154F1C3A7458BEE81F130FC36F9B5147EA13328F9980E02D2DEA2
```

源码最终位于 `third_party\mupdf-1.28.0-source`。该目录被 Git 忽略。

## 2. 编译 MuPDF 静态库

使用 Visual Studio 打开：

```text
third_party\mupdf-1.28.0-source\platform\win32\mupdf.sln
```

选择 `Release | x64` 并生成解决方案。完成后应存在：

```text
third_party\mupdf-1.28.0-source\platform\win32\x64\Release\libmupdf.lib
```

## 3. 编译 native 适配层

```bat
native\quietmupdf\build.cmd
```

脚本通过 `vswhere.exe` 自动查找 Visual Studio。特殊环境可设置：

```bat
set VCVARS64=C:\path\to\vcvars64.bat
set MUPDF_ROOT=C:\path\to\mupdf-1.28.0-source
set MUPDF_LIBDIR=C:\path\to\MuPDF\x64\Release
native\quietmupdf\build.cmd
```

输出位于 `native\quietmupdf\out`。

## 4. 编译主程序

在 Developer Command Prompt 中运行：

```bat
msbuild QuietPDF.Win32.sln /m /p:Configuration=Release /p:Platform=x64
```

输出位于 `build\x64\Release`。

## 5. 生成安装包

```bat
installer\build_setup.cmd
```

该脚本会重新生成 native 适配层、主程序、轻量卸载器、CAB 载荷和自定义安装器，并在 `dist` 中生成安装包及 SHA256 文件。

如 native DLL 已由同一源码和工具链验证构建，可临时设置 `SKIP_NATIVE_BUILD=1` 跳过第 3 步；正式发布不建议跳过。

## 常见问题

### 找不到 Visual Studio

确认已安装 Visual Studio Installer，并安装“使用 C++ 的桌面开发”。也可以设置 `VSWHERE`、`VCVARS64` 或 `MSBUILD` 环境变量覆盖自动探测结果。

### 找不到 vcruntime140.dll

安装 MSVC Redistributable 组件。构建脚本会从 Visual Studio 设置的 `VCToolsRedistDir` 下自动查找运行库。

### MuPDF 链接失败

确认 MuPDF 使用 `Release | x64` 构建，并且版本为 1.28.0。不要混用 x86、Debug 或其他版本的静态库。
