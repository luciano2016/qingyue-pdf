# GitHub 发布准备

## 仓库基本信息

建议 GitHub About 信息：

- Description: `轻量、安静的 Windows PDF 阅读器，让用户没有打扰，专注阅读。`
- Website: 暂留空，后续可填写官网或 Releases 页面
- Topics: `pdf`, `pdf-reader`, `windows`, `win32`, `cpp`, `mupdf`, `annotation`, `agplv3`

建议默认分支使用 `main`。当前本地分支如仍为 `master`，可在首次推送前统一改名，或保留现状后在 GitHub 设置中选择默认分支。

## 首次公开前

1. 检查完整 Git 历史中是否包含密钥、个人路径、测试文档或用户数据。删除当前文件不会清除历史中的敏感内容。
2. 确认有权公开项目图标、安装器插图和其他美术资源。
3. 确认发布的 MuPDF 二进制与仓库记录的 1.28.0 源码和当前适配层一致。
4. 开启 GitHub Security Advisories、Dependabot alerts 和私密漏洞报告。
5. 为默认分支启用 Pull Request 审查和禁止强推；个人项目可保留管理员绕过。
6. 创建 `bug`、`enhancement`、`performance`、`ui`、`documentation` 标签。
7. 在 About 中勾选 Releases；是否启用 Discussions 可根据维护精力决定。

## 历史体积

当前既有 Git 历史包含早期构建产物和开发机绝对路径，打包体积约 105 MB。当前版本已停止跟踪可再生成的 native DLL/LIB，且扫描未发现常见密钥或敏感文档扩展名。若希望首次公开仓库更小、更整洁，可以在备份后使用 git filter-repo 清理历史，或从当前提交建立新的公开仓库历史；这会改变提交哈希，必须在公开推送前单独决定。

## 首次推送

在 GitHub 创建空仓库后，将下面的地址替换为实际仓库地址：

```bash
git remote add origin https://github.com/<owner>/<repository>.git
git push -u origin master
```

如果决定把默认分支改为 `main`：

```bash
git branch -M main
git push -u origin main
```

## 发布 0.8.6

1. 完成 [发布检查清单](../RELEASE_CHECKLIST.md)。
2. 创建注释 tag：`git tag -a v0.8.6 -m "轻阅PDF 0.8.6"`。
3. 推送分支和 tag。
4. 在 GitHub Releases 中使用 [0.8.6 发布说明](RELEASE_NOTES_0.8.6.md)。
5. 上传 `轻阅PDF-Setup-x64.exe`、`QingYuePDF-Setup-x64.exe` 和对应 SHA256 文件。
6. 明确安装包是否有数字签名。未签名安装包可能触发 Windows SmartScreen，这是正常的信誉机制，不应通过关闭安全功能规避。

## AGPL 发布责任

这不是法律意见。分发安装包时，应确保接收者能获得该版本完整、可构建的对应源码，包括轻阅PDF 的修改、native 适配层、构建脚本、许可证和精确的 MuPDF 来源。商业销售本身并不被 AGPL 禁止，但闭源分发通常与当前依赖许可不兼容；如需闭源商业发行，应向 Artifex 咨询 MuPDF 商业许可，并重新评估 SumatraPDF GPLv3 改编代码。
