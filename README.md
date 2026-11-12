# proxypool — 免费代理池启动工具（C++ / 零外部依赖）

从公开源抓取免费 HTTP 代理 → 并发校验 → 在本机开一个**自动轮换**的 HTTP 代理端口。
带两个前端：命令行版 `proxypool.exe`，和可以**手动挑节点**的图形界面 `proxypool-gui.exe`。

- 只依赖 Windows 自带 API：WinHTTP（抓源）+ Winsock（校验与转发）+ GDI+（界面），编译产物是单个 exe
- 本地就是一个标准 HTTP 代理：支持明文 `GET http://...` 和 HTTPS 的 `CONNECT` 隧道
- 每条请求按轮询挑一个上游，失败自动换下一个；连续失败 3 次的节点会被剔除
- 后台定时重新抓取/校验，池子见底时提前刷新
- GUI 里点一下某个节点即可固定使用它，再点一次恢复自动轮换

> ⚠️ 免费公开代理没有任何安全保障：对方的服务器能看到你的明文流量，可能记录、篡改、劫持。
> 只用来做爬虫、测地域、临时调试这类不敏感的活儿，**不要用来登录账号、支付、传私密数据**。
> 也请确认你所在地区对使用代理的规定。

实测参考（本机 2026-11）：3176 个去重候选 → 620 个 TCP 可连 → 40 个通过 HTTP 校验，全程约 49 秒；
随后经代理访问 `http://www.baidu.com`、`https://www.example.com`、`https://www.baidu.com` 均返回 200。
免费节点寿命很短，命中率随网络环境波动很大。

## 构建

需要 MinGW-w64（脚本里默认找 `C:\Program Files (x86)\Dev-Cpp\MinGW64\bin`，装了别的就把它加进 PATH）：

```bat
build.bat
```

输出两个静态链接、不依赖 libgcc/libstdc++/libwinpthread 的程序：

| 文件 | 说明 | 大小 |
| --- | --- | --- |
| `dist\proxypool.exe` | 命令行版（带控制台窗口，日志实时滚动） | ~1.9 MB |
| `dist\proxypool-gui.exe` | 图形界面版（无控制台，可手动选节点） | ~2.0 MB |

两个 exe 读同一份 `sources.txt`、支持同一套参数，日志和池子逻辑完全共用（`src/app.cpp`）。

## 使用

### 图形界面（推荐）

```bat
start-gui.bat
:: 或者直接双击 dist\proxypool-gui.exe
```

界面从上到下：

- **本地代理地址**：`http://127.0.0.1:8080`，右侧「复制」按钮直接进剪贴板
- **统计条**：可用节点数、已转发请求数、失败数；右边显示「下次刷新 09:32」或
  「HTTP 校验 812/1548」的实时进度
- **节点列表**：按延迟从低到高，`ip:port` + 延迟数值 + 延迟条 + 状态（就绪 / 失败 N）
- **底部**：「立即刷新」按钮 + 当前模式标签

**手动选择节点**：用鼠标点列表里的任意一行，该行变蓝并出现左侧竖条，底部标签变成
`已固定：1.2.3.4:8080（点击该行恢复自动）`，此时所有请求固定走这个出口。

- 再点同一行（或点底部标签）→ 恢复自动轮换
- 固定节点连续失败 3 次（挂掉）→ 自动临时回退到轮换，等它恢复又自动切回去
- 下一轮刷新后该节点不在新列表里 → 自动回到轮换模式，不会卡死

界面用 GDI+ 手绘，暗色主题，支持高 DPI 缩放、鼠标滚轮滚动列表、窗口缩放。
所有卡片、按钮、标签、提示条都是直角风格（无圆角），Win11 下窗口本身也用
`DWMWA_WINDOW_CORNER_PREFERENCE` 关掉了系统圆角。
命令行的参数在 GUI 上同样有效，例如 `start-gui.bat --port 8090 --max 50`。

### 命令行

```bat
:: 启动，默认监听 127.0.0.1:8080
dist\proxypool.exe

:: 只刷新一次，打印可用代理清单
dist\proxypool.exe --once --max 30

:: 局域网共享（手机/其他机器可用）
dist\proxypool.exe --host 0.0.0.0 --port 8080
```

启动后正常会看到：

```
 本地代理：  http://127.0.0.1:8080
 可用节点：  42 个（每 600 秒自动刷新）
```

### 参数

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| `--port N` | 本地监听端口 | `8080` |
| `--ctl-port N` | 本机控制接口端口，`0` 关闭（只绑 `127.0.0.1`） | 端口 + 1 |
| `--host IP` | 绑定地址，`0.0.0.0` 表示局域网可访问 | `127.0.0.1` |
| `--sources FILE` | 代理源列表文件 | exe 同目录 `sources.txt` |
| `--threads N` | HTTP 校验并发（TCP 快筛自动用 4 倍，上限 512） | `64` |
| `--timeout MS` | 单代理连接/校验超时 | `3000` |
| `--refresh SEC` | 刷新间隔 | `600` |
| `--max N` | 池内最多保留节点数（按延迟排序取前 N） | `100` |
| `--min N` | 可用节点少于该值时提前刷新 | `10` |
| `--check URL` | 校验用的 IP 回显地址 | `http://icanhazip.com/` |
| `--no-validate` | 只做 TCP 连通性测试（快，但可用率低很多） | 关 |
| `--once` | 刷新一次后打印结果并退出 | 关 |
| `-v, --verbose` | 打印调试日志（含每个失效原因） | 关 |

### 本机控制接口

代理运行时会额外在 `127.0.0.1:<端口+1>` 开一个控制接口（只绑本机，不对外），
给启动器、脚本这类程序用：

| 请求 | 作用 |
| --- | --- |
| `GET /status` | `mode` / `pinned` / 池大小 / 转发计数 / 冷却节点数 |
| `GET /fixed` | 固定到一个健康节点；已固定在健康节点上时保持不变 |
| `GET /rotate?dead=IP:端口&ttl=秒` | 换下一个健康节点，并把 `dead` 在 `ttl` 秒内记为额度用尽 |
| `GET /auto` | 恢复自动轮换 |

`--ctl-port 0` 或控制端口被占用时不启用（日志会提示），代理本身不受影响。
工具箱启动器就是靠它实现「启动 OpenCode 时固定出口，免费额度用尽后自动换出口」。

## 怎么用起来

**curl**

```bat
curl -x http://127.0.0.1:8080 https://api.ipify.org
curl -x http://127.0.0.1:8080 http://httpbin.org/ip
```

**Python requests**

```python
import requests
r = requests.get("http://httpbin.org/ip",
                 proxies={"http": "http://127.0.0.1:8080",
                          "https": "http://127.0.0.1:8080"},
                 timeout=15)
print(r.text)
```

**Windows 系统代理**：设置 → 网络和 Internet → 代理 → 手动设置 → HTTP 代理 `127.0.0.1:8080`
（HTTPS 流量浏览器会自动走 `CONNECT`，不用单独填）

**Scrapy**：`settings.py` 里 `HTTPPROXY_ENABLED = True`、`HTTPPROXY = "127.0.0.1:8080"`，
由本工具负责轮换，比在爬虫里自己维护 IP 池省事。

## 原理

```
sources.txt ──► 并发抓取(WinHTTP) ──► 去重打散 ──► 一轮 TCP 快筛(1.5s)
                                                      │
                                            存活节点（通常 5%~15%）
                                                      ▼
                              二轮 HTTP 校验：经代理解析出 http://icanhazip.com/
                                                      │
                                             按延迟排序、截断
                                                      ▼
                                      ProxyPool（轮询分发 + 失败计数）
                                                      ▼
                        本地 127.0.0.1:8080  ──►  上游免费代理  ──►  目标站点
```

- **两段式校验**：免费列表里九成以上是死节点，所以先用短超时的 TCP 连接快速筛一遍（同一时间
  能扫两万个候选），再让活下来的节点真的去 `GET http://icanhazip.com/`，响应体必须是纯 IP 才算数。
  这样能同时过滤「端口开着但不是 HTTP 代理」和「能连但出不了网」两类垃圾。
- **回显地址兜底**：若首选回显地址命中太少（可能是出口机到不了），会自动换 `http://ip.3322.net/`
  再校验一遍，取结果多的那份。
- **假代理识别**：很多公开列表里混着 CDN 边缘 IP（尤其 Cloudflare）。这些 IP 恰好能直接应答
  托管在 CDN 上的回显站，看起来"可用"，实际转发不了任何请求。工具会先取本机公网 IP，
  凡是通过代理后返回的仍是本机 IP 的，一律剔除。
- **轮换**：每个进来的请求取下一个节点，所以连发 N 次就是 N 个不同出口。
- **固定节点**：GUI 里点选某行后调用 `ProxyPool::pin()`，服务端每次请求都优先用这个节点
  （`next()` 里先查固定项）。固定项连续失败 3 次才临时回退轮换，恢复后自动切回；
  刷新后该节点不在新列表里则自动取消固定。
- **自愈**：某个节点连续 3 次失败会被标记剔除，等下一轮刷新再重新评估。

## 代理源

`sources.txt` 一行一个 URL，抓回来的文本按 `ip:port` 逐行解析，兼容：

```
1.2.3.4:8080
http://1.2.3.4:8080
94.43.164.242:8080:Georgia          # 带国家后缀
socks5://user:pass@1.2.3.4:1080     # 只取 host:port，本工具只当 HTTP 代理用
```

默认用 **jsDelivr CDN** 镜像 GitHub 上的列表（`raw.githubusercontent.com` 在国内多数网络下直连不通）：

```
https://cdn.jsdelivr.net/gh/用户/仓库@分支/路径
```

如果 CDN 也不通，可以换成 `https://gh-proxy.com/https://raw.githubusercontent.com/...` 这类镜像，
或者自己起个代理把源文件下到本地，再用 `file://` 之外的方式——最简单是写个小 HTTP 服务放到内网。

## 自测

不需要外网、不需要真实代理：

```bat
:: 解析器单元测试
g++ -std=c++11 -Isrc test\parse_test.cpp src\common.cpp -o build\parse_test.exe -lws2_32
build\parse_test.exe

:: 代理池单元测试（固定节点 / 失败回退 / 刷新清理）
g++ -std=c++11 -Isrc test\pool_test.cpp src\pool.cpp src\common.cpp -o build\pool_test.exe
build\pool_test.exe

:: 端到端：起一个假上游，验证明文 GET 转发 / CONNECT 隧道 / 上游全挂返回 5xx
python test\selftest.py

:: 端到端：真实抓取 + 真实请求（需要外网，约 1~2 分钟）
powershell -ExecutionPolicy Bypass -File test\e2e.ps1
```

- `parse_test`：17 项解析用例（`ip:port`、`ip:port:国家`、BOM、JSON/HTML 拒绝等）
- `pool_test`：13 项核心用例（轮询、固定、连续失败 3 次回退、节点恢复、刷新后清理固定）
- `selftest.py`：明文 `GET` 转发、`CONNECT` 隧道、上游全挂时返回 5xx
- `e2e.ps1`：真实抓取 + 经代理访问真实站点，校验监听进程确实是本次启动的实例

## 已知限制

- **只支持 HTTP/HTTPS 代理**（`CONNECT`），不支持 SOCKS4/5 上游；`socks5://` 源会被当成 HTTP 代理试，通常校验不过
- 校验只走明文 HTTP，不验证目标的 HTTPS 能力（绝大多数 HTTP 代理的 `CONNECT` 都能用）
- 仅 Windows（WinHTTP），换 Linux 需要把 `fetcher.cpp` 换成 libcurl
- 免费节点寿命很短，几分钟到几小时；`--refresh` 别设太长

## 目录

```
src/common.*     日志、字符串/代理行解析
src/net.*        Winsock 初始化、带超时的 TCP 连接
src/fetcher.*    WinHTTP 抓取源文件
src/validator.*  代理可用性校验（延迟 + 出口 IP）
src/pool.*       线程安全代理池（轮询 + 固定节点 + 失败剔除）
src/server.*     本地 HTTP 代理服务器（CONNECT 隧道 / 明文转发）
src/app.*        参数解析、抓取/校验流程、刷新进度（CLI 与 GUI 共用）
src/main.cpp     命令行入口、刷新调度、信号处理
src/gui.cpp      GDI+ 图形界面（绘制、命中测试、节点选择）
sources.txt      代理源列表
build.bat        一键构建两个 exe
test/selftest.py 本地自测（假上游）
test/e2e.ps1     真实网络端到端测试
test/pool_test.cpp 代理池单元测试（固定节点逻辑）
```
