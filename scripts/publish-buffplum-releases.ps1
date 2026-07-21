<#
.SYNOPSIS
Builds and publishes local Windows releases for the BuffPlum Sunshine and Moonlight forks.

.DESCRIPTION
The default command builds both projects in parallel, creates versioned packages and
checksums under release-local, temporarily disables release-triggered workflows,
publishes prereleases, verifies every uploaded SHA-256 digest, and restores workflows.

The script is resumable. Use BuildOnly to prepare artifacts, then PublishOnly to upload
them later. GitHub commands always name the BuffPlum repository explicitly so a checkout
with both origin and upstream remotes cannot accidentally publish to upstream.

.EXAMPLE
pwsh -File .\scripts\publish-buffplum-releases.ps1

.EXAMPLE
pwsh -File .\scripts\publish-buffplum-releases.ps1 -Mode Plan

.EXAMPLE
pwsh -File .\scripts\publish-buffplum-releases.ps1 -Target Moonlight -Mode PublishOnly `
  -MoonlightVersion v6.2.92-buffplum.5
#>

[CmdletBinding()]
param(
    [ValidateSet('All', 'Sunshine', 'Moonlight')]
    [string]$Target = 'All',

    [ValidateSet('Plan', 'BuildOnly', 'PublishOnly', 'BuildAndPublish')]
    [string]$Mode = 'BuildAndPublish',

    [string]$SunshineVersion,
    [string]$MoonlightVersion,
    [string]$SunshineNotesPath,
    [string]$MoonlightNotesPath,

    [ValidateRange(1, 64)]
    [int]$BuildParallel = 4,

    [string]$QtBin = 'D:\dev\qt\6.11.1\msvc2022_64\bin',
    [string]$MsysUcrtBin = 'D:\dev\msys64\ucrt64\bin',

    [switch]$SkipTests,
    [switch]$NoParallelBuilds
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$SunshineRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$WorkspaceRoot = [IO.Path]::GetFullPath((Split-Path -Parent $SunshineRoot))
$MoonlightRoot = Join-Path $WorkspaceRoot 'moonlight-qt-git'
$ReleaseRoot = Join-Path $WorkspaceRoot 'release-local'
$SunshineRepository = 'BuffPlum/foundation-sunshine'
$MoonlightRepository = 'BuffPlum/moonlight-qt'
$SunshineWorkflow = '.github/workflows/main.yml'
$MoonlightWorkflow = '.github/workflows/fork-release-windows.yml'

function Write-Step {
    param([string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Invoke-Native {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory
    )

    if ($WorkingDirectory) {
        Push-Location -LiteralPath $WorkingDirectory
    }

    try {
        Write-Host ('> {0} {1}' -f $FilePath, ($ArgumentList -join ' ')) -ForegroundColor DarkGray
        & $FilePath @ArgumentList
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
        }
    }
    finally {
        if ($WorkingDirectory) {
            Pop-Location
        }
    }
}

function Get-NativeOutput {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory
    )

    if ($WorkingDirectory) {
        Push-Location -LiteralPath $WorkingDirectory
    }

    try {
        $output = & $FilePath @ArgumentList
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
        }
        return $output
    }
    finally {
        if ($WorkingDirectory) {
            Pop-Location
        }
    }
}

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command was not found: $Name"
    }
}

function Assert-ChildPath {
    param(
        [string]$Path,
        [string]$Parent
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $resolvedParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $resolvedPath.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe path outside expected parent: $resolvedPath"
    }
    return $resolvedPath
}

function Reset-Directory {
    param(
        [string]$Path,
        [string]$Parent
    )

    $safePath = Assert-ChildPath -Path $Path -Parent $Parent
    if (Test-Path -LiteralPath $safePath) {
        Remove-Item -LiteralPath $safePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $safePath -Force | Out-Null
}

function Assert-RepositoryReady {
    param(
        [string]$Root,
        [string]$ExpectedRepository
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Root '.git'))) {
        throw "Git repository was not found: $Root"
    }

    $status = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'status', '--porcelain=v1'))
    if ($status.Count -gt 0) {
        throw "Working tree must be clean before release: $Root`n$($status -join "`n")"
    }

    $origin = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'remote', 'get-url', 'origin') | Select-Object -First 1).Trim()
    $expectedPattern = 'github\.com[:/]' + [regex]::Escape($ExpectedRepository) + '(?:\.git)?$'
    if ($origin -notmatch $expectedPattern) {
        throw "Unexpected origin for $Root. Expected $ExpectedRepository, got $origin"
    }

    $branch = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'branch', '--show-current') | Select-Object -First 1).Trim()
    $head = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'rev-parse', 'HEAD') | Select-Object -First 1).Trim()
    $remoteLine = Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'ls-remote', 'origin', "refs/heads/$branch") | Select-Object -First 1
    if (-not $remoteLine) {
        throw "Remote branch origin/$branch was not found. Push the branch before releasing."
    }
    $remoteHead = ($remoteLine -split "`t")[0]
    if ($remoteHead -ne $head) {
        throw "Local HEAD $head is not pushed to origin/$branch ($remoteHead)."
    }

    $submoduleState = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'submodule', 'status', '--recursive'))
    $badSubmodules = @($submoduleState | Where-Object { $_ -match '^[+-]' })
    if ($badSubmodules.Count -gt 0) {
        throw "Submodules are not at the recorded commits in $Root`n$($badSubmodules -join "`n")"
    }

    return [pscustomobject]@{
        Branch = $branch
        Head = $head
    }
}

function Get-RemoteTagNames {
    param([string]$Root)

    $lines = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'ls-remote', '--tags', 'origin'))
    return @(
        $lines |
            ForEach-Object { ($_ -split "`t")[1] } |
            Where-Object { $_ -and $_ -notlike '*^{}' } |
            ForEach-Object { $_ -replace '^refs/tags/', '' }
    )
}

function Get-NextSunshineVersion {
    param([string[]]$Tags)

    $base = 'v' + (Get-Date).ToString('yyyy.Mdd')
    $pattern = '^' + [regex]::Escape($base) + '-buffplum\.(\d+)$'
    $numbers = @($Tags | ForEach-Object { if ($_ -match $pattern) { [int]$Matches[1] } })
    $next = if ($numbers.Count -eq 0) { 1 } else { ($numbers | Measure-Object -Maximum).Maximum + 1 }
    return "$base-buffplum.$next"
}

function Get-NextMoonlightVersion {
    param([string[]]$Tags)

    $versions = @(
        $Tags | ForEach-Object {
            if ($_ -match '^v(\d+\.\d+\.\d+)-buffplum\.(\d+)$') {
                [pscustomobject]@{
                    Base = [version]$Matches[1]
                    BaseText = $Matches[1]
                    Revision = [int]$Matches[2]
                }
            }
        }
    )

    if ($versions.Count -eq 0) {
        $fallback = (Get-Content -Raw (Join-Path $MoonlightRoot 'app/version.txt')).Trim()
        return "v$fallback-buffplum.1"
    }

    $latestBase = ($versions | Sort-Object Base -Descending | Select-Object -First 1).Base
    $sameBase = @($versions | Where-Object { $_.Base -eq $latestBase })
    $latest = $sameBase | Sort-Object Revision -Descending | Select-Object -First 1
    return "v$($latest.BaseText)-buffplum.$($latest.Revision + 1)"
}

function Assert-VersionFormat {
    param(
        [string]$Version,
        [ValidateSet('Sunshine', 'Moonlight')] [string]$Kind
    )

    $pattern = if ($Kind -eq 'Sunshine') {
        '^v\d{4}\.\d{3,4}-buffplum\.\d+$'
    }
    else {
        '^v\d+\.\d+\.\d+-buffplum\.\d+$'
    }

    if ($Version -notmatch $pattern) {
        throw "Invalid $Kind version: $Version"
    }
}

function Get-PreviousBuffPlumTag {
    param(
        [string]$Root,
        [ValidateSet('Sunshine', 'Moonlight')] [string]$Kind,
        [string]$CurrentVersion
    )

    $tags = @(Get-RemoteTagNames -Root $Root)
    if ($Kind -eq 'Sunshine') {
        $candidates = @($tags | Where-Object { $_ -match '^v\d{4}\.\d{3,4}-buffplum\.\d+$' -and $_ -ne $CurrentVersion })
    }
    else {
        $candidates = @($tags | Where-Object { $_ -match '^v\d+\.\d+\.\d+-buffplum\.\d+$' -and $_ -ne $CurrentVersion })
    }

    foreach ($tag in ($candidates | Sort-Object -Descending)) {
        & git -C $Root merge-base --is-ancestor $tag HEAD 2>$null
        if ($LASTEXITCODE -eq 0) {
            return $tag
        }
    }
    return $null
}

function Prepare-ReleaseNotes {
    param(
        [string]$Root,
        [string]$Version,
        [string]$Destination,
        [string]$ProvidedPath,
        [ValidateSet('Sunshine', 'Moonlight')] [string]$Kind
    )

    if ($ProvidedPath) {
        $source = [IO.Path]::GetFullPath($ProvidedPath)
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Release notes file was not found: $source"
        }
        if ($source -ne [IO.Path]::GetFullPath($Destination)) {
            Copy-Item -LiteralPath $source -Destination $Destination -Force
        }
        return
    }

    $previous = Get-PreviousBuffPlumTag -Root $Root -Kind $Kind -CurrentVersion $Version
    $range = if ($previous) { "$previous..HEAD" } else { 'HEAD' }
    $subjects = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'log', '--format=%s', $range))
    if ($subjects.Count -eq 0) {
        $subjects = @('Local Windows release build')
    }
    $changeList = ($subjects | ForEach-Object { "- $_" }) -join "`n"
    $head = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Root, 'rev-parse', 'HEAD') | Select-Object -First 1).Trim()

    $content = @"
## 更新内容

$changeList

## 构建信息

- 版本：``$Version``
- 本地构建提交：``$head``
- Windows x64 安装包和便携包均为本地构建。
- 发布包未进行代码签名，Windows 可能显示“未知发布者”。

## 安全提示

全磁盘文件传输包含覆盖和删除能力，仅建议在可信本地网络以及已配对设备之间使用。
"@
    Set-Content -LiteralPath $Destination -Value $content -Encoding utf8NoBOM
}

function Write-Checksums {
    param(
        [string]$Directory,
        [string[]]$PackagePaths,
        [string]$ProductName
    )

    $records = @(
        $PackagePaths | Sort-Object | ForEach-Object {
            $item = Get-Item -LiteralPath $_
            $hash = Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256
            [pscustomobject]@{
                Hash = $hash.Hash
                File = $item.Name
                Size = '{0:N2} MB' -f ($item.Length / 1MB)
            }
        }
    )

    $lines = @(
        "# $ProductName Release Package Checksums"
        "# Generated: $([DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss')) UTC"
        '# Algorithm: SHA256'
        ''
        'Verify your downloads with:'
        '  Windows (PowerShell): Get-FileHash -Algorithm SHA256 <filename>'
        '  Linux/macOS:          shasum -a 256 <filename>'
        ''
        '================================================================================'
        ''
    )
    $lines += $records | ForEach-Object { "$($_.Hash.ToLowerInvariant())  $($_.File)" }
    Set-Content -LiteralPath (Join-Path $Directory 'SHA256SUMS.txt') -Value ($lines -join "`n") -Encoding utf8NoBOM -NoNewline
    $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Directory 'checksums.json') -Encoding utf8NoBOM
}

function Build-SunshineRelease {
    param(
        [string]$Version,
        [string]$NotesPath
    )

    Write-Step "Building Sunshine $Version"
    if (-not (Test-Path -LiteralPath $MsysUcrtBin)) {
        throw "MSYS2 UCRT64 bin directory was not found: $MsysUcrtBin"
    }
    $innoCompiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    if (-not (Test-Path -LiteralPath $innoCompiler)) {
        throw "Inno Setup compiler was not found: $innoCompiler"
    }
    Require-Command npm.cmd
    Require-Command 7z.exe

    Invoke-Native -FilePath git -ArgumentList @('-C', $SunshineRoot, 'submodule', 'update', '--init', '--recursive')

    $buildRoot = Join-Path $SunshineRoot 'build-ucrt64'
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
    $oldPath = $env:PATH
    $oldBranch = $env:BRANCH
    $oldBuildVersion = $env:BUILD_VERSION
    $oldCommit = $env:COMMIT
    $oldDriverDeps = $env:DRIVER_DEPS_REQUIRED
    $oldSourceAssets = $env:SUNSHINE_SOURCE_ASSETS_DIR
    $oldAssets = $env:SUNSHINE_ASSETS_DIR

    try {
        $env:PATH = "$MsysUcrtBin;D:\dev\msys64\usr\bin;$oldPath"
        $env:BRANCH = 'master'
        $env:BUILD_VERSION = $Version
        $env:COMMIT = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $SunshineRoot, 'rev-parse', 'HEAD') | Select-Object -First 1).Trim()
        $env:DRIVER_DEPS_REQUIRED = 'OFF'
        $env:SUNSHINE_SOURCE_ASSETS_DIR = Join-Path $SunshineRoot 'src_assets'
        $env:SUNSHINE_ASSETS_DIR = $buildRoot

        Invoke-Native -FilePath npm.cmd -ArgumentList @('ci', '--registry=https://registry.npmjs.org', '--no-audit', '--no-fund') -WorkingDirectory $SunshineRoot
        if (-not $SkipTests) {
            Invoke-Native -FilePath npm.cmd -ArgumentList @('run', 'test:webui') -WorkingDirectory $SunshineRoot
        }
        Invoke-Native -FilePath npm.cmd -ArgumentList @('run', 'build') -WorkingDirectory $SunshineRoot

        $configureArgs = @(
            '-S', '.', '-B', 'build-ucrt64', '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DBUILD_DOCS=OFF',
            '-DBUILD_TESTS=OFF',
            '-DBUILD_TRAY_TESTS=ON',
            '-DBUILD_WEB_UI=OFF',
            '-DSUNSHINE_ASSETS_DIR=assets',
            '-DDRIVER_DEPS_REQUIRED=OFF',
            '-DSUNSHINE_PUBLISHER_NAME=BuffPlum',
            '-DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/BuffPlum/foundation-sunshine',
            '-DSUNSHINE_PUBLISHER_ISSUE_URL=https://github.com/BuffPlum/foundation-sunshine/issues'
        )
        Invoke-Native -FilePath (Join-Path $MsysUcrtBin 'cmake.exe') -ArgumentList $configureArgs -WorkingDirectory $SunshineRoot
        Invoke-Native -FilePath (Join-Path $MsysUcrtBin 'cmake.exe') -ArgumentList @('--build', 'build-ucrt64', '--parallel', $BuildParallel.ToString()) -WorkingDirectory $SunshineRoot
        if (-not $SkipTests) {
            Invoke-Native -FilePath (Join-Path $MsysUcrtBin 'ctest.exe') -ArgumentList @('--test-dir', 'build-ucrt64', '--output-on-failure') -WorkingDirectory $SunshineRoot
        }

        $staging = Join-Path $buildRoot 'inno_staging'
        $cpackOutput = Join-Path $buildRoot 'cpack_artifacts'
        Reset-Directory -Path $staging -Parent $buildRoot
        Reset-Directory -Path $cpackOutput -Parent $buildRoot
        Invoke-Native -FilePath (Join-Path $MsysUcrtBin 'cmake.exe') -ArgumentList @('--install', 'build-ucrt64', '--prefix', $staging) -WorkingDirectory $SunshineRoot
        Invoke-Native -FilePath $innoCompiler -ArgumentList @('build-ucrt64\sunshine_installer.iss') -WorkingDirectory $SunshineRoot
        Invoke-Native -FilePath (Join-Path $MsysUcrtBin 'cpack.exe') -ArgumentList @('-G', 'ZIP', '--config', '.\CPackConfig.cmake', '--verbose') -WorkingDirectory $buildRoot
    }
    finally {
        $env:PATH = $oldPath
        $env:BRANCH = $oldBranch
        $env:BUILD_VERSION = $oldBuildVersion
        $env:COMMIT = $oldCommit
        $env:DRIVER_DEPS_REQUIRED = $oldDriverDeps
        $env:SUNSHINE_SOURCE_ASSETS_DIR = $oldSourceAssets
        $env:SUNSHINE_ASSETS_DIR = $oldAssets
    }

    $releaseDirectory = Join-Path $ReleaseRoot "foundation-sunshine-$Version"
    Reset-Directory -Path $releaseDirectory -Parent $ReleaseRoot
    $installer = Join-Path $releaseDirectory "Sunshine.$Version.WindowsInstaller.exe"
    $portable = Join-Path $releaseDirectory "Sunshine.$Version.WindowsPortable.zip"
    Copy-Item -LiteralPath (Join-Path $SunshineRoot 'build-ucrt64/cpack_artifacts/Sunshine.exe') -Destination $installer
    Copy-Item -LiteralPath (Join-Path $SunshineRoot 'build-ucrt64/cpack_artifacts/Sunshine.zip') -Destination $portable
    Invoke-Native -FilePath 7z.exe -ArgumentList @('t', $portable)

    Prepare-ReleaseNotes -Root $SunshineRoot -Version $Version -Destination (Join-Path $releaseDirectory 'RELEASE_NOTES.md') -ProvidedPath $NotesPath -Kind Sunshine
    Write-Checksums -Directory $releaseDirectory -PackagePaths @($installer, $portable) -ProductName Sunshine
    Write-Host "Sunshine artifacts: $releaseDirectory" -ForegroundColor Green
}

function Build-MoonlightRelease {
    param(
        [string]$Version,
        [string]$NotesPath
    )

    Write-Step "Building Moonlight $Version"
    if (-not (Test-Path -LiteralPath (Join-Path $QtBin 'qmake.exe'))) {
        throw "Qt qmake was not found under: $QtBin"
    }
    Require-Command 7z.exe
    Require-Command cmd.exe

    Invoke-Native -FilePath git -ArgumentList @('-C', $MoonlightRoot, 'submodule', 'update', '--init', '--recursive')
    $artifactVersion = $Version.TrimStart('v')
    $oldJomMaxCpus = $env:JOM_MAX_CPUS
    try {
        $env:JOM_MAX_CPUS = $BuildParallel.ToString()
        $buildCommand = "set `"CI_VERSION=$artifactVersion`"&& set `"PATH=$QtBin;%PATH%`"&& call scripts\build-arch.bat release x64"
        Invoke-Native -FilePath cmd.exe -ArgumentList @('/d', '/c', $buildCommand) -WorkingDirectory $MoonlightRoot
    }
    finally {
        $env:JOM_MAX_CPUS = $oldJomMaxCpus
    }

    $builtMsi = Join-Path $MoonlightRoot 'build/build-x64-release/Moonlight.msi'
    $builtPortable = Join-Path $MoonlightRoot "build/installer-x64-release/MoonlightPortable-x64-$artifactVersion.zip"
    $builtExe = Join-Path $MoonlightRoot 'build/deploy-x64-release/Moonlight.exe'
    foreach ($path in @($builtMsi, $builtPortable, $builtExe)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Expected Moonlight build output was not found: $path"
        }
    }

    if (Get-Command rg.exe -ErrorAction SilentlyContinue) {
        Invoke-Native -FilePath rg.exe -ArgumentList @('-a', '-F', $artifactVersion, $builtExe)
    }
    Invoke-Native -FilePath 7z.exe -ArgumentList @('t', $builtPortable)

    $releaseDirectory = Join-Path $ReleaseRoot "moonlight-qt-$Version"
    Reset-Directory -Path $releaseDirectory -Parent $ReleaseRoot
    $installer = Join-Path $releaseDirectory "MoonlightSetup-x64-$artifactVersion.msi"
    $portable = Join-Path $releaseDirectory "MoonlightPortable-x64-$artifactVersion.zip"
    Copy-Item -LiteralPath $builtMsi -Destination $installer
    Copy-Item -LiteralPath $builtPortable -Destination $portable

    Prepare-ReleaseNotes -Root $MoonlightRoot -Version $Version -Destination (Join-Path $releaseDirectory 'RELEASE_NOTES.md') -ProvidedPath $NotesPath -Kind Moonlight
    Write-Checksums -Directory $releaseDirectory -PackagePaths @($installer, $portable) -ProductName Moonlight
    Write-Host "Moonlight artifacts: $releaseDirectory" -ForegroundColor Green
}

function Get-ReleaseSpec {
    param([ValidateSet('Sunshine', 'Moonlight')] [string]$Kind)

    if ($Kind -eq 'Sunshine') {
        $directory = Join-Path $ReleaseRoot "foundation-sunshine-$SunshineVersion"
        $artifactVersion = $SunshineVersion
        return [pscustomobject]@{
            Kind = $Kind
            Root = $SunshineRoot
            Repository = $SunshineRepository
            Workflow = $SunshineWorkflow
            Tag = $SunshineVersion
            Title = "BuffPlum Foundation Sunshine $SunshineVersion"
            Directory = $directory
            Notes = Join-Path $directory 'RELEASE_NOTES.md'
            Assets = @(
                (Join-Path $directory "Sunshine.$artifactVersion.WindowsInstaller.exe"),
                (Join-Path $directory "Sunshine.$artifactVersion.WindowsPortable.zip"),
                (Join-Path $directory 'SHA256SUMS.txt'),
                (Join-Path $directory 'checksums.json')
            )
        }
    }

    $directory = Join-Path $ReleaseRoot "moonlight-qt-$MoonlightVersion"
    $artifactVersion = $MoonlightVersion.TrimStart('v')
    return [pscustomobject]@{
        Kind = $Kind
        Root = $MoonlightRoot
        Repository = $MoonlightRepository
        Workflow = $MoonlightWorkflow
        Tag = $MoonlightVersion
        Title = "BuffPlum Moonlight $MoonlightVersion"
        Directory = $directory
        Notes = Join-Path $directory 'RELEASE_NOTES.md'
        Assets = @(
            (Join-Path $directory "MoonlightSetup-x64-$artifactVersion.msi"),
            (Join-Path $directory "MoonlightPortable-x64-$artifactVersion.zip"),
            (Join-Path $directory 'SHA256SUMS.txt'),
            (Join-Path $directory 'checksums.json')
        )
    }
}

function Assert-ReleaseFiles {
    param($Spec)

    foreach ($path in @($Spec.Notes) + $Spec.Assets) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Release file was not found: $path"
        }
    }
}

function Ensure-ReleaseTag {
    param($Spec)

    $head = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Spec.Root, 'rev-parse', 'HEAD') | Select-Object -First 1).Trim()
    & git -C $Spec.Root rev-parse -q --verify "refs/tags/$($Spec.Tag)" *> $null
    if ($LASTEXITCODE -eq 0) {
        $tagCommit = (Get-NativeOutput -FilePath git -ArgumentList @('-C', $Spec.Root, 'rev-list', '-n', '1', $Spec.Tag) | Select-Object -First 1).Trim()
        if ($tagCommit -ne $head) {
            throw "Tag $($Spec.Tag) points to $tagCommit instead of HEAD $head"
        }
    }
    else {
        Invoke-Native -FilePath git -ArgumentList @('-C', $Spec.Root, 'tag', '-a', $Spec.Tag, $head, '-m', $Spec.Title)
    }

    $remoteLines = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Spec.Root, 'ls-remote', '--tags', 'origin', "refs/tags/$($Spec.Tag)", "refs/tags/$($Spec.Tag)^{}"))
    if ($remoteLines.Count -eq 0) {
        Invoke-Native -FilePath git -ArgumentList @('-C', $Spec.Root, 'push', 'origin', "refs/tags/$($Spec.Tag)")
        $remoteLines = @(Get-NativeOutput -FilePath git -ArgumentList @('-C', $Spec.Root, 'ls-remote', '--tags', 'origin', "refs/tags/$($Spec.Tag)", "refs/tags/$($Spec.Tag)^{}"))
    }
    $peeled = $remoteLines | Where-Object { $_ -like '*^{}' } | Select-Object -First 1
    $remoteCommit = if ($peeled) { ($peeled -split "`t")[0] } else { $head }
    if ($remoteCommit -ne $head) {
        throw "Remote tag $($Spec.Tag) does not resolve to HEAD $head"
    }
}

function Disable-ReleaseWorkflow {
    param($Spec)

    $workflowData = (Get-NativeOutput -FilePath gh -ArgumentList @('api', "repos/$($Spec.Repository)/actions/workflows")) -join "`n" | ConvertFrom-Json
    $workflow = $workflowData.workflows | Where-Object { $_.path -eq $Spec.Workflow } | Select-Object -First 1
    if (-not $workflow) {
        throw "Release workflow was not found in $($Spec.Repository): $($Spec.Workflow)"
    }

    if ($workflow.state -eq 'active') {
        Write-Step "Temporarily disabling $($Spec.Repository) workflow $($workflow.name)"
        Invoke-Native -FilePath gh -ArgumentList @('workflow', 'disable', $workflow.id.ToString(), '--repo', $Spec.Repository)
        return [pscustomobject]@{ Repository = $Spec.Repository; Id = $workflow.id; Restore = $true }
    }

    return [pscustomobject]@{ Repository = $Spec.Repository; Id = $workflow.id; Restore = $false }
}

function Restore-ReleaseWorkflow {
    param($State)

    if ($State.Restore) {
        Write-Step "Restoring release workflow in $($State.Repository)"
        Invoke-Native -FilePath gh -ArgumentList @('workflow', 'enable', $State.Id.ToString(), '--repo', $State.Repository)
    }
}

function Publish-Release {
    param($Spec)

    Assert-ReleaseFiles -Spec $Spec
    Ensure-ReleaseTag -Spec $Spec
    Write-Step "Publishing $($Spec.Repository) $($Spec.Tag)"

    & gh release view $Spec.Tag --repo $Spec.Repository *> $null
    if ($LASTEXITCODE -eq 0) {
        $uploadArgs = @('release', 'upload', $Spec.Tag, '--repo', $Spec.Repository, '--clobber') + $Spec.Assets
        Invoke-Native -FilePath gh -ArgumentList $uploadArgs
        Invoke-Native -FilePath gh -ArgumentList @(
            'release', 'edit', $Spec.Tag,
            '--repo', $Spec.Repository,
            '--title', $Spec.Title,
            '--notes-file', $Spec.Notes,
            '--draft=false',
            '--prerelease'
        )
    }
    else {
        $createArgs = @(
            'release', 'create', $Spec.Tag,
            '--repo', $Spec.Repository,
            '--verify-tag',
            '--title', $Spec.Title,
            '--notes-file', $Spec.Notes,
            '--prerelease'
        ) + $Spec.Assets
        Invoke-Native -FilePath gh -ArgumentList $createArgs
    }
}

function Verify-Release {
    param($Spec)

    Write-Step "Verifying $($Spec.Repository) $($Spec.Tag)"
    $release = $null
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        $json = (Get-NativeOutput -FilePath gh -ArgumentList @(
            'release', 'view', $Spec.Tag,
            '--repo', $Spec.Repository,
            '--json', 'name,tagName,isDraft,isPrerelease,publishedAt,url,assets'
        )) -join "`n"
        $release = $json | ConvertFrom-Json
        $missingDigest = @($release.assets | Where-Object { -not $_.digest })
        if ($missingDigest.Count -eq 0) { break }
        Start-Sleep -Seconds 2
    }

    if ($release.isDraft -or -not $release.isPrerelease -or $release.tagName -ne $Spec.Tag) {
        throw "Release metadata is incorrect for $($Spec.Repository) $($Spec.Tag)"
    }

    foreach ($path in $Spec.Assets) {
        $local = Get-Item -LiteralPath $path
        $remote = $release.assets | Where-Object { $_.name -eq $local.Name } | Select-Object -First 1
        if (-not $remote) {
            throw "Uploaded asset is missing: $($local.Name)"
        }
        $expectedDigest = 'sha256:' + (Get-FileHash -LiteralPath $local.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($remote.digest -ne $expectedDigest) {
            throw "Digest mismatch for $($local.Name). Local $expectedDigest, remote $($remote.digest)"
        }
    }

    Write-Host "Verified release: $($release.url)" -ForegroundColor Green
    return $release.url
}

Require-Command git
Require-Command gh
New-Item -ItemType Directory -Path $ReleaseRoot -Force | Out-Null
Invoke-Native -FilePath gh -ArgumentList @('auth', 'status')

$selectedKinds = if ($Target -eq 'All') { @('Sunshine', 'Moonlight') } else { @($Target) }
$sunshineState = $null
$moonlightState = $null
if ($selectedKinds -contains 'Sunshine') {
    $sunshineState = Assert-RepositoryReady -Root $SunshineRoot -ExpectedRepository $SunshineRepository
    if (-not $SunshineVersion) {
        $SunshineVersion = Get-NextSunshineVersion -Tags (Get-RemoteTagNames -Root $SunshineRoot)
    }
    Assert-VersionFormat -Version $SunshineVersion -Kind Sunshine
}
if ($selectedKinds -contains 'Moonlight') {
    $moonlightState = Assert-RepositoryReady -Root $MoonlightRoot -ExpectedRepository $MoonlightRepository
    if (-not $MoonlightVersion) {
        $MoonlightVersion = Get-NextMoonlightVersion -Tags (Get-RemoteTagNames -Root $MoonlightRoot)
    }
    Assert-VersionFormat -Version $MoonlightVersion -Kind Moonlight
}

Write-Step 'Release plan'
if ($sunshineState) {
    Write-Host "Sunshine: $SunshineVersion @ $($sunshineState.Head)"
}
if ($moonlightState) {
    Write-Host "Moonlight: $MoonlightVersion @ $($moonlightState.Head)"
}
Write-Host "Mode: $Mode"
Write-Host "Artifacts: $ReleaseRoot"
if ($Mode -eq 'Plan') {
    return
}

$buildRequested = $Mode -in @('BuildOnly', 'BuildAndPublish')
if ($buildRequested -and $Target -eq 'All' -and -not $NoParallelBuilds) {
    Write-Step 'Starting Sunshine and Moonlight builds in parallel'
    $childParallel = [Math]::Max(1, [int][Math]::Floor($BuildParallel / 2))
    $sunshineParameters = @{
        Target = 'Sunshine'
        Mode = 'BuildOnly'
        SunshineVersion = $SunshineVersion
        SunshineNotesPath = $SunshineNotesPath
        BuildParallel = $childParallel
        QtBin = $QtBin
        MsysUcrtBin = $MsysUcrtBin
        NoParallelBuilds = $true
    }
    $moonlightParameters = @{
        Target = 'Moonlight'
        Mode = 'BuildOnly'
        MoonlightVersion = $MoonlightVersion
        MoonlightNotesPath = $MoonlightNotesPath
        BuildParallel = $childParallel
        QtBin = $QtBin
        MsysUcrtBin = $MsysUcrtBin
        NoParallelBuilds = $true
    }
    if ($SkipTests) {
        $sunshineParameters.SkipTests = $true
        $moonlightParameters.SkipTests = $true
    }

    $jobs = @(
        Start-Job -Name 'Sunshine release build' -ScriptBlock { param($Script, $Parameters) & $Script @Parameters } -ArgumentList $PSCommandPath, $sunshineParameters
        Start-Job -Name 'Moonlight release build' -ScriptBlock { param($Script, $Parameters) & $Script @Parameters } -ArgumentList $PSCommandPath, $moonlightParameters
    )
    $jobs | Wait-Job | Out-Null
    foreach ($job in $jobs) {
        Receive-Job -Job $job
        if ($job.State -ne 'Completed') {
            throw "Parallel build failed: $($job.Name) ($($job.State))"
        }
    }
    $jobs | Remove-Job -Force
}
elseif ($buildRequested) {
    if ($selectedKinds -contains 'Sunshine') {
        Build-SunshineRelease -Version $SunshineVersion -NotesPath $SunshineNotesPath
    }
    if ($selectedKinds -contains 'Moonlight') {
        Build-MoonlightRelease -Version $MoonlightVersion -NotesPath $MoonlightNotesPath
    }
}

if ($Mode -eq 'BuildOnly') {
    Write-Step 'Build-only run completed'
    return
}

$specs = @($selectedKinds | ForEach-Object { Get-ReleaseSpec -Kind $_ })
$workflowStates = @()
$releaseUrls = @()
try {
    foreach ($spec in $specs) {
        $workflowStates += Disable-ReleaseWorkflow -Spec $spec
    }
    foreach ($spec in $specs) {
        Publish-Release -Spec $spec
        $releaseUrls += Verify-Release -Spec $spec
    }
}
finally {
    foreach ($state in $workflowStates) {
        Restore-ReleaseWorkflow -State $state
    }
}

Write-Step 'Release completed'
$releaseUrls | ForEach-Object { Write-Host $_ -ForegroundColor Green }
