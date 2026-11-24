@echo off
rem release.bat --- 一键发布：检查工作树 -> 打 tag -> 推送，触发 GitHub Actions 构建并发布 Release。
rem
rem 用法（在仓库根目录）：
rem     tools\release.bat 1.0.0
rem     tools\release.bat 1.1.0-beta    （带后缀会发布成 pre-release，需在页面上手动标记）
rem
rem 上线的是 tag，不是当前分支：工作树必须干净，否则打出的 tag 里缺少你刚改的东西。
rem 构建、校验、上传全部在 CI 里做（见 .github/workflows/release.yml），本机不编译。
setlocal

set "VER=%~1"
if "%VER%"=="" goto usage
rem 去掉可能带上的 v 前缀，统一成 vX.Y.Z
if /i "%VER:~0,1%"=="v" set "VER=%VER:~1%"
rem 只做宽松校验（数字与点），严格格式（1.2 / 1.2.3）由 CI 判：不合法会在构建前中止，不会发出脏 Release。
echo %VER%| findstr /r "^[0-9][0-9.]*$" >nul
if errorlevel 1 (
    echo [x] 版本号格式不对：只能含数字和点，如 1.0.0，收到 "%VER%"
    exit /b 1
)

set "TAG=v%VER%"

git rev-parse --git-dir >nul 2>nul
if errorlevel 1 (
    echo [x] 当前目录不是 git 仓库
    exit /b 1
)

rem 工作树必须干净：有未提交改动时打 tag，CI 会从 origin 上拉那个提交，编出来的不是你本地的代码。
for /f "usebackq delims=" %%i in (`git status --porcelain`) do (
    echo [x] 工作树不干净，先提交或 stash：
    echo     %%i
    exit /b 1
)

rem 本地和远程都不能已有同名 tag，否则 push 会被拒或覆盖掉已发布的版本。
git rev-parse -q --verify "refs/tags/%TAG%" >nul
if not errorlevel 1 (
    echo [x] 标签 %TAG% 已存在。要重发同一版本，先删除：
    echo     git tag -d %TAG% ^&^& git push origin :refs/tags/%TAG%
    exit /b 1
)
git ls-remote --exit-code --tags origin "refs/tags/%TAG%" >nul 2>nul
if not errorlevel 1 (
    echo [x] 远程已有标签 %TAG%，换一个版本号
    exit /b 1
)

echo 即将发布 %TAG%（提交 %TAG% -> HEAD）
git --no-pager log --oneline -1
git tag -a "%TAG%" -m "WinVClip %TAG%"
if errorlevel 1 (
    echo [x] 打 tag 失败
    exit /b 1
)
git push origin "%TAG%"
if errorlevel 1 (
    echo [x] 推送失败。本地 tag 已创建，修好后可重试：git push origin %TAG%
    exit /b 1
)

echo.
echo [ok] %TAG% 已推送，GitHub Actions 正在构建。
echo      进度：https://github.com/nihaozyj7/WinVClip/actions
echo      发布页：https://github.com/nihaozyj7/WinVClip/releases
exit /b 0

:usage
echo 用法: tools\release.bat ^<版本号^>
echo   tools\release.bat 1.0.0
exit /b 1
