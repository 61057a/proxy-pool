# 给 AI 的提交流程（AGENTS.md）

本文件写给接手这个仓库的 AI / 自动化代理看。人类读者请直接看 `README.md`。

仓库：<https://github.com/61057a/proxy-pool>　默认分支：`main`

---

## 1. 这个项目是什么

C++11 写的 Windows 免费代理池工具：抓取公开代理列表 → 并发校验 → 在本机开一个自动轮换的
HTTP 代理端口。零第三方依赖，只用 Windows 自带 API（WinHTTP / Winsock / GDI+）。

```
sources.txt        代理源列表（一行一个 URL）
src/common.*       日志、字符串工具、代理行解析
src/net.*          Winsock 初始化、带超时的 TCP 连接
src/fetcher.*      WinHTTP 抓取源文件
src/validator.*    代理校验（TCP 快筛 + HTTP 出口 IP 校验、延迟统计）
src/pool.*         线程安全代理池（轮询 / 固定节点 / 连续失败剔除）
src/server.*       本地 HTTP 代理服务（明文转发 + CONNECT 隧道）
src/app.*          参数解析 + 抓取校验流程 + 刷新调度（CLI 与 GUI 共用）
src/main.cpp       命令行入口
src/gui.cpp        GDI+ 图形界面（手绘、暗色、节点列表可点选固定）
test/parse_test.cpp  解析器单元测试
test/pool_test.cpp   代理池单元测试
test/selftest.py     端到端自测（本地假上游，无需外网）
test/e2e.ps1         端到端测试（真实抓取，需要外网）
build.bat            构建两个 exe
start.bat / start-gui.bat  便携启动入口
```

`dist/`、`build/`、`*.exe`、`*.obj`、`*.o`、`*.log` 都被 `.gitignore` 排除，**仓库里只放源码**。

---

## 2. 环境

- 编译器：MinGW-w64 g++ **4.9.2**（必须支持 C++11，但不能用 C++14/17 语法）
- 默认路径：`C:\Program Files (x86)\Dev-Cpp\MinGW64\bin`，`build.bat` 会自己加进 PATH
- 构建目标：Windows x64，静态链接（`-static -static-libgcc -static-libstdc++`）
- 无 CMake、无 vcpkg、无包管理器，不要引入任何第三方库

---

## 3. 改完代码必须做的三步

```bat
:: 1) 编译（会覆盖 dist\ 下的两个 exe）
build.bat

:: 2) 单元测试（无需外网）
g++ -std=c++11 -Isrc test\parse_test.cpp src\common.cpp -o build\parse_test.exe -lws2_32
build\parse_test.exe
g++ -std=c++11 -Isrc test\pool_test.cpp src\pool.cpp src\common.cpp -o build\pool_test.exe
build\pool_test.exe

:: 3) 端到端（无需外网，起两个假上游验证 GET / CONNECT / 全挂返回 5xx）
python test\selftest.py
```

成功判据：

- `build.bat` 输出 `Build OK:`，且 `dist\proxypool.exe`、`dist\proxypool-gui.exe` 都存在
- `parse_test` 17 项全过、`pool_test` 13 项全过、`selftest.py` 全部 PASS
- 若改动涉及真实网络路径，再跑一次 `powershell -ExecutionPolicy Bypass -File test\e2e.ps1`（1~2 分钟）

注意（Windows / PowerShell 环境特有的坑）：

- 控制台输出中文常是乱码，判断内容用文件读取工具，或 `Get-Content -Encoding UTF8`
- `build.bat` 本身成功，但 PowerShell 可能因为 stderr 把整条命令判成 exit 1；
  只看有没有 `Build OK:`，不要只看退出码
- 重新编译前先结束正在运行的 `proxypool.exe` / `proxypool-gui.exe`，否则
  `cannot open output file dist\proxypool.exe: Permission denied`

---

## 4. 代码约定

- **C++11**：不要用 `auto` 返回类型推导、泛型 lambda、`std::make_unique`、`if constexpr`、`std::filesystem`
- 字符串统一 `std::wstring` 走 Win32 API，`std::string` 走网络与日志；UTF-8 源码
- 源文件 UTF-8 **无 BOM**、**LF** 换行（git 可能提示 LF→CRLF，属正常，不要改成 CRLF 提交）
- 缩进 4 空格，大括号独占一行（与现有文件保持一致）
- 注释用中文，解释「为什么」而不是复述代码
- 新增源文件必须同时加进 `build.bat` 的 `CORE` 变量（GUI 额外链接库在第二行 `g++` 里）
- GUI 相关：所有绘制走 GDI+，直角风格，不要加圆角；新增控件要同时处理高 DPI 缩放

---

## 5. 提交与推送

提交身份固定为仓库所有者，**不要**改成你自己的名字：

```bat
git -c user.name=61057a -c user.email=61057a@users.noreply.github.com commit -m "..."
```

或者先设一次：

```bat
git config user.name 61057a
git config user.email 61057a@users.noreply.github.com
```

远程走 **SSH**（HTTP 推送会要求交互式凭据，自动化环境里会卡住）：

```bat
git remote -v
:: origin  git@github.com:61057a/proxy-pool.git (fetch/push)

git add -A
git status --short          :: 确认没有 build/ dist/ *.exe 混进来
git commit -m "fix: ..."
git push origin main
```

- 验证 SSH 可用：`ssh -T git@github.com`（返回 `Hi 61057a!` 即正常）
- 不要 `git push --force`，除非明确要重写历史并且已确认远程没有别人的提交
- 提交信息用中文，格式 `feat: / fix: / docs: / refactor: / chore:` + 一句摘要，
  多行时正文用 `-` 列表说明改了什么

---

## 6. 绝对不能提交的东西

- `build/`、`dist/`、任何 `*.exe` / `*.obj` / `*.o` / `*.log`（`.gitignore` 已排除，别用 `git add -f`）
- 任何 token / PAT / API key / 密码。发布前跑一次自查：

```bat
findstr /S /I /N /C:"ghp_" /C:"api_key" /C:"apikey" /C:"secret" /C:"password" src\*.cpp src\*.h *.bat *.txt *.md test\*
```

（`src/gui.cpp` 里的 `GdiplusStartup` 有一个名为 `token` 的局部变量，那是 GDI+ 的句柄，
**不是**凭据，属于正常命中。）

- 用户目录、本机绝对路径、个人配置：代码里用 `GetModuleFileName` 取 exe 目录拼相对路径
- 本机凭据文件 `%USERPROFILE%\.config\github\gh_token.sec`（见 5.1）：不要复制进仓库，
  也不要 `cat` / `Get-Content` 打印内容到日志或聊天

### 5.1 GitHub 凭据（本机已保存，不在仓库里）

本机（Windows 用户 `kkksc04`）已经把 token 用 **DPAPI 加密**保存在仓库之外的目录，
只有当前 Windows 用户能解密：

```
%USERPROFILE%\.config\github\gh_token.sec
= C:\Users\kkksc04\.config\github\gh_token.sec
```

需要调 GitHub API（建库、发 Release、改仓库简介 / 主页 / topics）时，在**当前 PowerShell 进程**里载入：

```powershell
$sec  = Get-Content "$env:USERPROFILE\.config\github\gh_token.sec" | ConvertTo-SecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
$env:GH_TOKEN = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)

gh api user -q .login     # 应输出 61057a
```

载入后 `gh` 的建库、`gh release create`、`gh repo edit`、`gh api` 全部可用。

- 该 token 的 scope 只有 `repo`，够本仓库用；**缺 `read:org`**，所以
  `gh auth login --with-token` 会报 `missing required scope 'read:org'`，
  不要走 `gh auth login`，直接用 `GH_TOKEN` 环境变量
- `$env:GH_TOKEN` 只对当前进程有效，命令结束即失效，不会残留在系统里
- **绝对不要**把 token 明文写进 `AGENTS.md`、源码、提交信息或 Release 说明：
  本仓库是公开的，GitHub 会扫描并自动吊销泄露的 token
- 换机器 / 换用户时自行生成：GitHub → Settings → Developer settings → Personal access tokens，
  勾 `repo`，然后写入加密文件：

```powershell
'<新token>' | ConvertTo-SecureString -AsPlainText -Force |
    ConvertFrom-SecureString | Set-Content "$env:USERPROFILE\.config\github\gh_token.sec" -NoNewline
```

- 当前 token 没有设置过期时间，建议每 3~6 个月轮换一次；怀疑泄露立刻在 GitHub 撤销

---

## 7. 发 Release（源码在仓库，二进制在 Release）

仓库只放源码；编译产物通过 GitHub Release 分发。

### 7.1 打包

1. 先 `build.bat`，确认两个 exe 是新编译的
2. 版本号记在 tag 与文件名里，例如 `v1.0.0` → `proxypool-v1.0.0-win64.zip`
3. zip 内是**平铺**的 6 个文件（不要套一层目录）：

```
proxypool.exe
proxypool-gui.exe
sources.txt
start.bat
start-gui.bat
README.md
```

打包命令（PowerShell）：

```powershell
$stage = "$env:TEMP\proxypool-release"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item dist\proxypool.exe,dist\proxypool-gui.exe,dist\sources.txt,start.bat,start-gui.bat,README.md $stage
$zip = "$env:TEMP\proxypool-v1.0.0-win64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-Item $zip | Select-Object Name,Length
(Get-FileHash $zip -Algorithm SHA256).Hash
```

4. 校验压缩包内容，确认只有上面 6 个文件、且没夹带 `build/` 或源码：

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::OpenRead($zip)
$z.Entries | Select-Object FullName,Length | Format-Table -AutoSize
$z.Dispose()
```

### 7.2 创建 Release

先按 [5.1](#51-github-凭据本机已保存不在仓库里) 载入 `GH_TOKEN`，然后：

```powershell
gh release create v1.0.0 "$env:TEMP\proxypool-v1.0.0-win64.zip" `
  --repo 61057a/proxy-pool `
  --title "v1.0.0" `
  --notes "首个版本：命令行 + 图形界面两个前端，静态链接单 exe。"
```

- tag 名与 zip 文件名保持一致（`v1.0.0` ↔ `proxypool-v1.0.0-win64.zip`）
- 如果 `api.github.com` 连不上，先重试；仍失败就让人类在网页上手动上传资产
- 发完确认：

```powershell
gh release view v1.0.0 --repo 61057a/proxy-pool
```

- 发布后执行 `Remove-Item Env:\GH_TOKEN` 清理当前进程即可；token 本身保存在 5.1 的加密文件里，**不要撤销**
- 补发资产用 `gh release upload v1.0.0 <file> --clobber`

### 7.3 版本号同步

升版本时同步改这些地方：

- `README.md` 里的体积说明（`~1.9 MB` / `~2.0 MB`，按实际 `dist\*.exe` 大小改）
- 本次 Release 的 tag / zip 文件名 / Release notes

---

## 8. 快速检查清单（每次提交前）

- [ ] `build.bat` 输出 `Build OK:`
- [ ] `parse_test` / `pool_test` / `selftest.py` 全过
- [ ] `git status --short` 里没有 `build/`、`dist/`、`*.exe`
- [ ] 没有 token / 密钥 / 本机绝对路径
- [ ] 提交身份是 `61057a <61057a@users.noreply.github.com>`
- [ ] `git push origin main` 成功（走 SSH）
- [ ] 若本次要发版：zip 内容 6 个文件、tag 与文件名一致、Release 资产已上传
