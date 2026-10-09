# ApiGate

ApiGate 是一个用于学习和逐步开发 API 网关的 C++20 项目。当前版本提供异步
HTTP/1.1 健康检查、配置校验、JSON 日志，以及可选的单一静态 HTTP 上游代理。
代理支持 GET 及有界正文的 POST、PUT、PATCH。它没有多上游、流式上传、重试、连接池、
缓存、限流、认证、TLS 或 HTTP/2；不要把它当作生产网关。本项目在 Linux 环境下开发，
仅支持 Linux 构建与运行。

## 快速启动与验证

首次准备系统构建工具时，在 Linux 终端以普通用户执行以下命令；
`sudo` 会交互询问你的密码，不要将密码写入脚本或环境变量：

```bash
sudo apt-get -o APT::Update::Error-Mode=any update
sudo apt-get install -y --no-install-recommends \
  build-essential ca-certificates clang clang-format clang-tidy cmake coreutils curl git \
  ninja-build pkg-config python3 shellcheck tar unzip zip
```

然后在已取得**当前代码**的仓库根目录中，以普通用户运行：

```bash
bash scripts/install-deps.sh
bash scripts/run.sh
```

安装脚本会检查系统工具并准备固定版本的 vcpkg，不会自行调用 `sudo`；
首次下载和构建可能较久。
`run.sh` 会构建 Debug 版本并在前台启动服务，默认监听 `127.0.0.1:8080`。
在另一个终端执行：

```bash
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/readyz
```

默认配置下，两次响应的正文分别是：

```json
{"service":"api-gate","status":"ok"}
```

```json
{"service":"api-gate","status":"ready"}
```

在运行服务的终端按 `Ctrl+C` 会启动有截止时间的优雅排空。`/readyz` 在正常运行时
表示进程正在响应请求，**不检查上游服务或其他依赖**；排空开始后监听器立即关闭，
新的探针通常得到连接失败，而不是固定的 HTTP 503。

## 当前 HTTP 行为

| 请求 | 状态码 | JSON 正文或说明 |
| --- | ---: | --- |
| `GET /healthz` | 200 | `{"service":"api-gate","status":"ok"}` |
| `GET /readyz` | 200 | `{"service":"api-gate","status":"ready"}` |
| `GET /missing` | 404 | `{"error":{"code":"not_found","message":"route not found"}}` |
| `POST /healthz` | 405 | `{"error":{"code":"method_not_allowed","message":"only GET is supported"}}`；`Allow: GET` |

表中服务名使用默认配置。路由匹配时忽略查询字符串；本地端点只接受 `GET`，代理关闭
时 POST、PUT、PATCH 也继续返回 405。正常响应设置 `Server: ApiGate`、
`Content-Type: application/json` 和 `Cache-Control: no-store`；
正文长度及连接头由 HTTP 库按请求生成。请求头超过 8 KiB 返回 431，正文超过
64 KiB 返回 413；格式错误请求可能返回 400 或直接断开。当前行为尚未制定版本化的
API 兼容承诺，具体设计边界见[设计文档](设计文档.md)。

同时设置 `APIGATE_UPSTREAM_HOST` 和 `APIGATE_UPSTREAM_PORT` 后，除
`/healthz`、`/readyz` 外的 origin-form GET、POST、PUT、PATCH 会代理到该 HTTP 上游，
原始方法、路径和查询保持不变。GET 只允许空正文；POST、PUT、PATCH 允许空正文或最多
64 KiB 的正文。absolute-form、Upgrade 或带正文 GET 返回安全的 400 JSON，HEAD、
DELETE、OPTIONS、CONNECT 及其他方法返回 405。上游解析、连接、写入、读取、协议错误
或响应超限返回 502，任一上游阶段超时返回 504；上游 1xx 信息响应（包括 101）也会被
拒绝为 502，不继续读取后续响应或切换协议。错误正文不会包含上游地址、请求目标或
系统错误文本。下游 Content-Length 与 chunked 正文都会先完整缓冲并按解析后的有效
载荷限制为 64 KiB；超限返回 413 且不访问上游。当前不流式转发请求体。非空 `Expect`
在读取正文前返回 417 `expectation_failed` 并关闭下游连接，不获取代理名额或访问上游。
上游响应正文上限为 1 MiB，且每个请求都新建上游连接；所有方法均不自动重试，尤其
不会重放带正文请求。
并发代理达到 `APIGATE_MAX_CONCURRENT_PROXIES` 时，新代理候选不会访问上游，而是
立即返回 503 和
`{"error":{"code":"gateway_overloaded","message":"proxy capacity is exhausted"}}`；
响应不设置 `Retry-After`，本地健康端点不占用代理名额。

代理会移除请求和响应中的标准 hop-by-hop 头以及 `Connection` 动态列出的头，覆盖
上游 `Host`，按缓冲后的实际正文重建 `Content-Length`，并将下游 `Server` 保持为
`ApiGate`。`Content-Type`、`Authorization`、`Cookie` 等
端到端请求头会转发给所配置上游，但不会写入 ApiGate 日志。客户端提供的
`X-Forwarded-*` 不会被信任或转发，本阶段也不生成这些头。启用代理不会改变本地
健康端点；`/readyz` 仍不主动探测上游。

活动下游连接达到 `APIGATE_MAX_CONNECTIONS` 时，应用暂停发起新的 accept；已进入
监听 backlog 的连接由操作系统排队，恢复容量后继续接收。这一背压策略不会主动向
backlog 中的连接返回 HTTP 503，也不修改系统 backlog。两项限制约束对象数量，不是
精确内存字节预算；默认值只是当前 MVP 的保守边界，不是生产容量或性能保证。

收到 `SIGTERM` 或 `SIGINT` 后，服务立即停止 accept，并关闭空闲 keep-alive、部分请求
及其他尚未分派的连接。已经分派的本地响应或上游代理交换可以在
`APIGATE_SHUTDOWN_GRACE_MS` 期限内完成，响应写完后连接关闭且不再读取下一请求；期限
到达后剩余操作会被取消。正常完成及受控超时都返回 0，非预期运行时故障仍返回非零。
当前没有预排空 HTTP API 或独立管理端口，也不支持用第二个信号立即强退。外部进程
管理器的强制终止期限必须大于应用排空期限，并为取消回调和进程清理保留余量。

## 构建、测试与运行

在仓库根目录执行：

```bash
bash scripts/build.sh linux-debug
bash scripts/test.sh linux-debug
bash scripts/check.sh
bash scripts/test.sh linux-release
bash scripts/run.sh --check-config
bash scripts/run.sh --version
```

`check.sh` 包含格式检查、clang-tidy、警告即错误、ASan/UBSan 构建和测试。
系统工具装好后，也可运行 `bash scripts/bootstrap.sh`，它依次准备 vcpkg 依赖并执行
`check.sh`。`run.sh --check-config` 仍会按所选预设构建程序，但程序只校验
配置并退出，不创建监听 socket；因此它不能发现端口占用。配置合法时，程序在标准
输出恰好写入一条 `configuration_valid` 单行 JSON 并返回 0；该结果不受
`APIGATE_LOG_LEVEL` 阈值影响。配置非法时，程序在标准错误输出
`bootstrap_failed` JSON 并返回非零状态。

`run.sh --version` 构建程序后输出版本。程序的直接输出格式为恰好一行
`api-gate <version>`，返回 0 且标准错误为空；版本来自 CMake 的
`project(VERSION ...)`。该命令不读取 `APIGATE_*` 配置、不创建日志器或监听 socket，
也不输出服务运行日志。版本输出不包含 Git SHA、构建时间或构建主机信息。

## 配置

程序只读取下列环境变量；`.env.example` 是示例，不会自动加载。

| 环境变量 | 程序默认值 | 校验 |
| --- | --- | --- |
| `APIGATE_SERVICE_NAME` | `api-gate` | 1–64 个字母、数字、点、下划线或连字符 |
| `APIGATE_ENVIRONMENT` | `development` | 同上 |
| `APIGATE_LOG_LEVEL` | `info` | `trace`、`debug`、`info`、`warn`、`error`、`critical` |
| `APIGATE_LISTEN_ADDRESS` | `127.0.0.1` | IPv4/IPv6 字面地址，不解析主机名 |
| `APIGATE_LISTEN_PORT` | `8080` | 0–65535；0 由内核分配临时端口 |
| `APIGATE_MAX_CONNECTIONS` | `256` | 1–65535；活动下游连接数上限 |
| `APIGATE_MAX_CONCURRENT_PROXIES` | `32` | 1–65535；并发上游代理交换数上限 |
| `APIGATE_SHUTDOWN_GRACE_MS` | `5000` | 1–60000；信号排空期限，单位毫秒 |
| `APIGATE_UPSTREAM_HOST` | 未设置 | 纯 DNS 主机名、IPv4 或 IPv6 字面地址；必须与端口同时设置 |
| `APIGATE_UPSTREAM_PORT` | 未设置 | 1–65535；必须与主机同时设置 |
| `APIGATE_UPSTREAM_TIMEOUT_MS` | `3000`（代理启用时） | 1–60000；仅可与完整上游配置一起使用 |

例如，换一个本机端口运行：

```bash
APIGATE_LISTEN_PORT=9000 bash scripts/run.sh
```

例如，把支持的方法转发到本机测试上游：

```bash
APIGATE_UPSTREAM_HOST=127.0.0.1 \
APIGATE_UPSTREAM_PORT=9001 \
bash scripts/run.sh
```

三项上游变量均未设置时代理关闭，原有未知 GET 的 404 及其他方法的 405 行为保持不变。两项容量配置
始终校验；代理关闭时代理并发上限不被使用。配置检查结果会报告两个数值上限、
`shutdown_grace_ms`、`proxy_enabled` 和启用时的 `upstream_timeout_ms`，不会输出上游主机名。

当前没有全局字节预算、每客户端/IP 限制或请求速率限制；也不支持其他代理方法、
流式上传、连接池、多上游、上游 TLS、HTTP/2、WebSocket 或预排空管理阶段。实际可承载数量
仍受文件描述符、内存、CPU、请求/响应缓冲和操作系统 backlog 等因素约束。现有测试
覆盖不构成生产容量或协议兼容性承诺。

无效配置会在监听前失败并以非零状态退出。不要将密码或令牌写入
`.env.example`、命令行或仓库；若自行创建 `.env`，该文件已被 Git 忽略。

## Docker

需要可用的 Docker Engine、Docker CLI 和 Compose 插件。Compose 默认将容器
8080 端口映射到宿主机的 `127.0.0.1:8080`：

```bash
docker compose -f deploy/compose.yaml up --build
```

随后可用上面的 `curl` 命令验证。Compose 配置使用非 root 用户、只读根文件系统、
移除 Linux capabilities、`no-new-privileges` 和 10 秒停止宽限期；镜像自带访问
`/healthz` 的健康检查。以下烟雾测试会检查健康端点、运行身份、只读文件系统、
capabilities 和信号退出，运行时也指定 `no-new-privileges`：

```bash
bash scripts/container-smoke.sh
```

该脚本只清理自己创建的临时镜像和容器。Docker 守护进程权限由本机管理员配置；
不要仅为省去 `sudo` 就盲目加入 `docker` 组。

## WSL 与常见问题

在 WSL 2 中建议使用普通 Linux 用户，并把仓库放在 Linux 文件系统内，例如
`~/src/api-gate`，而非长期在 `/mnt/c` 或 `/mnt/d` 下构建。迁移有未提交修改的仓库时，
需同时保留工作树、未跟踪文件和 `.git`，核对 Git 状态后再切换；仅重新 `clone`
不会带上未提交修改。

- 端口已占用：设置 `APIGATE_LISTEN_PORT` 为其他端口。Compose 可设置
  `APIGATE_HOST_PORT` 来改变宿主机映射端口。
- 首次依赖下载失败：保留缓存后重试。下载缓存默认为
  `${XDG_CACHE_HOME:-$HOME/.cache}/vcpkg/downloads`，二进制包缓存默认为
  `${XDG_CACHE_HOME:-$HOME/.cache}/vcpkg/archives`；可分别通过 `VCPKG_DOWNLOADS`
  和 `VCPKG_DEFAULT_BINARY_CACHE` 覆盖。按预设隔离的已安装依赖仍位于
  `~/.cache/apigate/vcpkg/installed`。这些缓存不是完整离线镜像；清理后，后续构建
  可能需要重新联网下载或编译依赖。
- Docker 权限不足：确认守护进程可用，以及当前用户是否有权访问 Docker socket。
  如仅 Docker 构建网络下载超时，可在可信的 Linux 开发环境中使用
  `APIGATE_DOCKER_BUILD_NETWORK=host bash scripts/container-smoke.sh`。

架构、取舍、停止行为和测试覆盖见[设计文档](设计文档.md)。
