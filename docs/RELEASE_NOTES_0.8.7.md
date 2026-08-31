# 轻阅PDF 0.8.7

## 本次变化

- 护眼米主题改用绘制阶段的轻量页面遮罩，与浅草绿主题采用相同机制。
- 移除护眼米主题对 MuPDF 输出像素的独立颜色转换，保持快速滚动时的绘制路径一致。

## 升级说明

可以直接覆盖安装。阅读进度、最近文档和主题设置会继续保留。

## 校验安装包

发布页同时提供 SHA256 文件，可在 PowerShell 中验证：

```powershell
Get-FileHash -Algorithm SHA256 .\轻阅PDF-Setup-x64.exe
```

当前安装包未提供商业代码签名，Windows SmartScreen 可能在首次运行时显示信誉提示。
