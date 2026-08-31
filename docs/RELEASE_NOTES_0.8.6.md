# 轻阅PDF 0.8.6

轻阅PDF 是一款轻量、安静的 PDF 阅读器。让用户没有打扰，专注阅读。

这是项目首次按完整开源发布流程整理的版本，主要补齐许可证、第三方声明、可复现构建文档和 GitHub 协作规范。阅读器功能延续 0.8.5 的连续阅读翻页稳定性修复。

## 本版内容

- 按 AGPLv3 发布项目源码
- 记录 MuPDF 1.28.0 的固定来源与 SHA256
- 完整标注 SumatraPDF 字体加载器改编代码及 GPLv3 许可
- 提供 MuPDF 获取脚本和 Windows x64 构建指南
- 增加 Issue、Pull Request、贡献、安全和发布模板
- 移除构建脚本中的开发者本机绝对路径

## 下载与校验

下载 `轻阅PDF-Setup-x64.exe`。发布页同时提供 SHA256 文本，可在 PowerShell 中校验：

```powershell
certutil -hashfile .\轻阅PDF-Setup-x64.exe SHA256
```

当前安装包如未进行代码签名，Windows 可能显示 SmartScreen 提示。请只从本仓库 Releases 下载并核对 SHA256。

## 系统要求

- Windows 10/11 x64

## 开源许可

项目按 AGPLv3 发布，并包含不同许可证的第三方组件。详情见仓库中的 `LICENSE` 和 `THIRD_PARTY_NOTICES.md`。
