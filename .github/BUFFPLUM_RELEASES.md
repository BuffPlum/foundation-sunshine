# BuffPlum 本地构建与 GitHub Releases 发布流程

本文档用于在 Windows 上从本地源码构建 BuffPlum Foundation Sunshine 与 BuffPlum Moonlight，并将经过校验的安装包和便携包发布到 GitHub Releases。

发布原则：

- 二进制必须由本机源码构建，GitHub Actions 只保留为备用，不承担本流程的构建。
- 默认发布为 **prerelease**，不要在资产上传完成前创建可被自动更新发现的正式版。
- 构建和发布分开执行：先 `Plan`，再 `BuildOnly`，核验本地资产后才执行 `PublishOnly`。
- Release notes 只写“更新了什么”和“为什么改成这样”，不要加入构建日志、提交清单或泛泛宣传。
- 发布脚本会临时停用 release-triggered workflow，发布完成或异常退出时通过 `finally` 恢复原状态。
- 安装包目前没有商业代码签名，Windows 可能提示“未知发布者”。

## 1. 目录约定

本文档假设工作区结构如下：

```text
E:\code\C\sunshine-and-moonlight\
├─ foundation-sunshine-git\
├─ moonlight-qt-git\
├─ release-notes\
└─ release-local\
```

发布入口脚本：

```text
foundation-sunshine-git\scripts\publish-buffplum-releases.ps1
```

所有命令均在 PowerShell 7 中执行。先定义路径和本次版本号：

```powershell
$ReleaseWorkspace = 'E:\code\C\sunshine-and-moonlight'
$SunshineRoot = Join-Path $ReleaseWorkspace 'foundation-sunshine-git'
$MoonlightRoot = Join-Path $ReleaseWorkspace 'moonlight-qt-git'

$SunshineVersion = 'v2026.722-buffplum.2'
$MoonlightVersion = 'v6.2.92-buffplum.7'

$SunshineNotes = Join-Path $ReleaseWorkspace "release-notes\sunshine-$SunshineVersion.md"
$MoonlightNotes = Join-Path $ReleaseWorkspace "release-notes\moonlight-$MoonlightVersion.md"

Set-Location $SunshineRoot
```

版本号仅为示例。发布前必须根据远端已有 tag 选择未使用的新版本。

## 2. 环境要求

公共工具：

- PowerShell 7：`pwsh`
- Git：`git`
- GitHub CLI：`gh`
- 7-Zip：`7z.exe`
- 已登录有权访问两个 BuffPlum 仓库的 GitHub 账号

Sunshine 本地构建默认依赖：

- Node.js：`D:\DevTools\Scoop\apps\nvm\current\nodejs\nodejs`
- MSYS2 UCRT64：`D:\dev\msys64\ucrt64\bin`
- Inno Setup 6：`C:\Program Files (x86)\Inno Setup 6\ISCC.exe`
- CMake、Ninja、编译工具链及完整子模块

Moonlight 本地构建默认依赖：

- Qt MSVC x64：`D:\dev\qt\6.11.1\msvc2022_64\bin`
- Visual Studio 2022 C++ Build Tools
- WiX/MSI 构建环境及完整子模块

如果本机路径不同，可在执行脚本时传入：

```powershell
-NodeBin '实际 Node.js 目录' `
-MsysUcrtBin '实际 MSYS2 UCRT64 bin 目录' `
-QtBin '实际 Qt MSVC x64 bin 目录'
```

先检查基础命令和 GitHub 登录：

```powershell
git --version
gh --version
pwsh --version
7z.exe | Select-Object -First 2
gh auth status
```

`gh auth status` 应显示当前账号对 `BuffPlum/foundation-sunshine` 与 `BuffPlum/moonlight-qt` 具有 `repo` 和 `workflow` 权限。

## 3. 版本规则

Sunshine 使用按日期递增的独立版本：

```text
vYYYY.MDD-buffplum.N
```

例如 2026 年 7 月 22 日第一次发布为：

```text
v2026.722-buffplum.1
```

同一天再次发布时只增加最后的修订号：

```text
v2026.722-buffplum.2
```

Moonlight 保留上游基础版本并增加 BuffPlum 修订号：

```text
v6.2.92-buffplum.7
```

不要复用已经存在的 tag。可以先查看最近版本：

```powershell
gh release list -R BuffPlum/foundation-sunshine --limit 10
gh release list -R BuffPlum/moonlight-qt --limit 10
```

## 4. 确认提交并推送

发布脚本要求：

- 工作区没有未提交文件；
- 当前分支已经推送到 `origin`；
- 本地 HEAD 与远端当前分支一致；
- 子模块位于仓库记录的提交，没有 `+` 或 `-` 状态；
- `origin` 必须分别指向 BuffPlum 的两个 fork。

检查两个仓库：

```powershell
$SunshineBranch = git -C $SunshineRoot branch --show-current
$MoonlightBranch = git -C $MoonlightRoot branch --show-current

git -C $SunshineRoot status --short
git -C $SunshineRoot remote get-url origin
git -C $SunshineRoot submodule status --recursive
git -C $SunshineRoot rev-parse HEAD
git -C $SunshineRoot rev-parse "origin/$SunshineBranch"

git -C $MoonlightRoot status --short
git -C $MoonlightRoot remote get-url origin
git -C $MoonlightRoot submodule status --recursive
git -C $MoonlightRoot rev-parse HEAD
git -C $MoonlightRoot rev-parse "origin/$MoonlightBranch"
```

如果存在本次发布需要的改动，应只暂存明确文件，再提交并推送：

```powershell
git -C $SunshineRoot add -- path/to/changed-file
git -C $SunshineRoot commit -m 'Describe the Sunshine change'
git -C $SunshineRoot push origin $SunshineBranch

git -C $MoonlightRoot add -- path/to/changed-file
git -C $MoonlightRoot commit -m 'Describe the Moonlight change'
git -C $MoonlightRoot push origin $MoonlightBranch
```

不要使用 `git add -A` 把无关文件混入 release 提交。如果本次只修改 Sunshine，则不需要为了凑版本而发布 Moonlight。

## 5. 编写 Release notes

每个准备发布的仓库单独建立一个 Markdown 文件。正文只能保留下面两个章节：

```markdown
## 更新了什么

- 用用户能理解的语言说明实际变化。
- 只写本次版本可观察到的功能、行为或修复。

## 为什么改成这样

- 说明问题背景、取舍和兼容性原因。
- 涉及全盘读写时，明确仅用于个人设备与可信局域网。
```

建议文件名：

```text
release-notes\sunshine-v2026.722-buffplum.2.md
release-notes\moonlight-v6.2.92-buffplum.7.md
```

正式发布时始终显式传入 `-SunshineNotesPath` 或 `-MoonlightNotesPath`。如果不传，脚本会根据提交生成通用说明，不符合本项目“只解释更新内容和原因”的发布规范。

## 6. 生成发布计划

先让脚本验证仓库、远端、版本格式和目标提交，但不构建、不创建 tag、不修改 GitHub：

```powershell
pwsh -NoProfile -File .\scripts\publish-buffplum-releases.ps1 `
  -Mode Plan `
  -Target All `
  -SunshineVersion $SunshineVersion `
  -MoonlightVersion $MoonlightVersion `
  -SunshineNotesPath $SunshineNotes `
  -MoonlightNotesPath $MoonlightNotes
```

输出中的版本号和 commit SHA 必须与准备发布的提交一致。

如果只发布 Sunshine：

```powershell
pwsh -NoProfile -File .\scripts\publish-buffplum-releases.ps1 `
  -Mode Plan `
  -Target Sunshine `
  -SunshineVersion $SunshineVersion `
  -SunshineNotesPath $SunshineNotes
```

如果只发布 Moonlight，将 `Target` 改为 `Moonlight`，并只传 Moonlight 的版本与 notes 参数。

## 7. 本地正式构建

推荐先单独执行 `BuildOnly`。构建失败时不会创建远端 tag 或 release，也不会改动 GitHub workflow 状态。

同时构建两端：

```powershell
pwsh -NoProfile -File .\scripts\publish-buffplum-releases.ps1 `
  -Mode BuildOnly `
  -Target All `
  -SunshineVersion $SunshineVersion `
  -MoonlightVersion $MoonlightVersion `
  -SunshineNotesPath $SunshineNotes `
  -MoonlightNotesPath $MoonlightNotes `
  -BuildParallel 4
```

默认情况下，`Target All` 会并行构建 Sunshine 和 Moonlight。调试构建问题时可增加 `-NoParallelBuilds`，让日志按顺序输出。

不要在正式发布中使用 `-SkipTests`。它只适合定位环境问题。

Sunshine 构建包含：

1. `npm ci`
2. WebUI 测试
3. WebUI 生产构建
4. CMake + Ninja Release 构建
5. CTest
6. Inno Setup 安装包
7. CPack 便携 ZIP
8. `7z t` 完整性检查
9. SHA-256 文件生成

Moonlight 构建包含：

1. `scripts\build-arch.bat release x64`
2. MSVC + Qt Release 构建
3. MSI 打包
4. 便携 ZIP 打包
5. 二进制版本字符串检查
6. `7z t` 完整性检查
7. SHA-256 文件生成

## 8. 检查本地发布资产

构建完成后，资产位于：

```text
release-local\foundation-sunshine-<SunshineVersion>\
release-local\moonlight-qt-<MoonlightVersion>\
```

Sunshine 应包含：

```text
Sunshine.<version>.WindowsInstaller.exe
Sunshine.<version>.WindowsPortable.zip
RELEASE_NOTES.md
SHA256SUMS.txt
checksums.json
```

Moonlight 应包含：

```text
MoonlightSetup-x64-<version-without-leading-v>.msi
MoonlightPortable-x64-<version-without-leading-v>.zip
RELEASE_NOTES.md
SHA256SUMS.txt
checksums.json
```

检查目录和校验文件：

```powershell
$SunshineReleaseDir = Join-Path $ReleaseWorkspace "release-local\foundation-sunshine-$SunshineVersion"
$MoonlightReleaseDir = Join-Path $ReleaseWorkspace "release-local\moonlight-qt-$MoonlightVersion"

Get-ChildItem $SunshineReleaseDir
Get-Content (Join-Path $SunshineReleaseDir 'SHA256SUMS.txt')

Get-ChildItem $MoonlightReleaseDir
Get-Content (Join-Path $MoonlightReleaseDir 'SHA256SUMS.txt')
```

至少手动重算一次安装包和便携包的摘要：

```powershell
Get-ChildItem $SunshineReleaseDir -File |
  Where-Object Extension -In '.exe', '.zip' |
  Get-FileHash -Algorithm SHA256

Get-ChildItem $MoonlightReleaseDir -File |
  Where-Object Extension -In '.msi', '.zip' |
  Get-FileHash -Algorithm SHA256
```

重算结果必须与 `SHA256SUMS.txt` 一致。

## 9. 发布 prerelease

本地资产确认无误后执行 `PublishOnly`：

```powershell
pwsh -NoProfile -File .\scripts\publish-buffplum-releases.ps1 `
  -Mode PublishOnly `
  -Target All `
  -SunshineVersion $SunshineVersion `
  -MoonlightVersion $MoonlightVersion
```

`PublishOnly` 会按顺序执行：

1. 再次确认工作区干净且 HEAD 已推送；
2. 临时停用两个仓库的 release-triggered workflow；
3. 在当前 HEAD 创建 annotated tag 并推送；
4. 创建或更新 GitHub prerelease；
5. 上传安装包、便携包、`SHA256SUMS.txt` 和 `checksums.json`；
6. 使用 GitHub 返回的 asset digest 与本地 SHA-256 逐项比较；
7. 在 `finally` 中恢复发布前处于 active 的 workflow。

`RELEASE_NOTES.md` 用作 release 正文，不作为下载资产上传。

只发布 Sunshine：

```powershell
pwsh -NoProfile -File .\scripts\publish-buffplum-releases.ps1 `
  -Mode PublishOnly `
  -Target Sunshine `
  -SunshineVersion $SunshineVersion
```

## 10. 发布后独立核验

不要只依赖脚本最后的成功提示。至少再独立检查一次 release 元数据、tag、资产摘要和 workflow 状态。

### 10.1 Release 状态与正文

```powershell
gh release view $SunshineVersion `
  -R BuffPlum/foundation-sunshine `
  --json tagName,name,isDraft,isPrerelease,publishedAt,url,body,assets

gh release view $MoonlightVersion `
  -R BuffPlum/moonlight-qt `
  --json tagName,name,isDraft,isPrerelease,publishedAt,url,body,assets
```

确认：

- `isDraft` 为 `false`；
- `isPrerelease` 为 `true`；
- 正文只有 `## 更新了什么` 和 `## 为什么改成这样`；
- 每个 release 都有 4 个下载资产。

### 10.2 Tag 指向

```powershell
git -C $SunshineRoot rev-list -n 1 $SunshineVersion
git -C $SunshineRoot ls-remote origin "refs/tags/$SunshineVersion^{}"

git -C $MoonlightRoot rev-list -n 1 $MoonlightVersion
git -C $MoonlightRoot ls-remote origin "refs/tags/$MoonlightVersion^{}"
```

本地 tag、远端 peeled tag 与准备发布的 HEAD 必须一致。

### 10.3 Workflow 已恢复

```powershell
gh workflow list -R BuffPlum/foundation-sunshine --all --json id,name,state,path
gh workflow list -R BuffPlum/moonlight-qt --all --json id,name,state,path
```

确认下列 workflow 恢复为 `active`：

```text
BuffPlum/foundation-sunshine  .github/workflows/main.yml
BuffPlum/moonlight-qt         .github/workflows/fork-release-windows.yml
```

### 10.4 没有误触发云端构建

```powershell
gh run list -R BuffPlum/foundation-sunshine --workflow main.yml --limit 5
gh run list -R BuffPlum/moonlight-qt --workflow fork-release-windows.yml --limit 5
```

本次发布时间之后不应出现由 `release` 事件触发的新构建。

## 11. 失败恢复

### BuildOnly 失败

- 不会创建 tag 或 release；
- 修复依赖或源码后重新运行 `BuildOnly`；
- `release-local\<对应版本>` 会在重新构建时安全重建；
- 不要使用 `-SkipTests` 掩盖真实失败后直接发布。

### PublishOnly 失败

- 脚本会在 `finally` 中尝试恢复之前启用的 release workflow；
- 仍应手动执行 `gh workflow list ... --all` 确认状态；
- 检查 tag 是否已经创建、release 是否存在、已经上传了哪些资产；
- tag 正确指向当前 HEAD 时，可以修复问题后用同一版本重新执行 `PublishOnly`，脚本会覆盖同名资产；
- tag 指向错误提交时立即停止，不要盲目删除或强推 tag，先核对本地与远端提交。

### 出现意外 Actions run

先查看具体事件、时间和 job：

```powershell
gh run view <run-id> -R <owner/repo>
```

只有确认它是本次手工 release 误触发、没有产生需要保留的结果时，才考虑清理该 run。不要删除无关历史记录。

## 12. 推荐的日常执行顺序

```text
1. 完成功能与文档
2. 运行项目测试
3. 只提交本次相关文件
4. 推送当前分支
5. 编写两个章节的 release notes
6. Mode Plan
7. Mode BuildOnly
8. 人工检查安装包、便携包与 SHA-256
9. Mode PublishOnly
10. 独立核验 release、tag、asset digest、workflow 和 Actions run
```

不要为了省一步直接依赖默认的 `BuildAndPublish`。拆分构建和发布，才能在任何远端状态发生变化之前检查最终交付物。

## 13. 安全边界

- BuffPlum 全盘读写只适用于个人设备与可信局域网。
- 不要把 Sunshine 管理端口、串流端口或文件数据面直接暴露到公网。
- 发布前确认默认仍为 `file_mapping_mode = read_only`，高级全盘模式必须由用户显式确认开启。
- 发布包未签名时，下载页必须同时提供 `SHA256SUMS.txt` 和 `checksums.json`。
- 可选驱动依赖下载失败但构建仍成功时，应确认该驱动确实是可选项，再继续发布。
