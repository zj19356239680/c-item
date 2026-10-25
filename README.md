# ApiGate

ApiGate 是一个用于学习和逐步开发 API 网关的 C++20 项目。当前版本提供异步
HTTP/1.1 健康检查、配置校验、JSON 日志，以及可选的路径路由与多个静态 HTTP/HTTPS 上游。
代理支持 GET 及有界正文的 POST、PUT、PATCH、同一路由多实例轮询，以及可选主动健康检查与故障摘除。支持默认关闭的 HTTP/HTTPS 上游连接池。它没有流式上传、重试、
缓存、每客户端或分布式限流、用户账号/角色授权或 HTTP/2；支持可选下游 TLS 和路由级 API Key 认证。按路由限流不构成生产容量保证。
不要把它当作生产网关。本项目在 Linux 环境下开发，
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
64 KiB 返回 413；格式错误请求可能返回 400 或直接断开。一次请求的头部和正文共享
10 秒读取期限，持续发送片段不会刷新该期限。读取超时记录安全的
`http_request_rejected` / `reason=read_timeout` 并直接关闭连接，不尝试写 HTTP 错误响应，
也不承诺返回错误正文。当前行为尚未制定版本化的
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
上游响应正文上限为 1 MiB；默认每个请求都新建上游连接，可显式启用连接池。所有方法均不自动重试，尤其
不会重放带正文请求。
并发代理达到 `APIGATE_MAX_CONCURRENT_PROXIES` 时，新代理候选不会访问上游，而是
立即返回 503 和
`{"error":{"code":"gateway_overloaded","message":"proxy capacity is exhausted"}}`；
响应不设置 `Retry-After`，本地健康端点不占用代理名额。

`APIGATE_MAX_IN_FLIGHT_BYTES` 是进程内所有 session 和代理交换共享的**缓冲正文逻辑
预约预算**。已知 `Content-Length` 的下游请求按长度预约，chunked 或未知长度请求按
完整 64 KiB 预约；已知长度的上游响应按长度预约，chunked 或未知长度响应按完整
1 MiB 预约。正文读完后，保守预约会缩减到实际大小。预算不足时不继续读取对应正文：
下游请求返回 503 后强制关闭连接且不访问上游，上游响应则关闭该上游交换并返回 503；
两者正文均为
`{"error":{"code":"gateway_overloaded","message":"buffer capacity is exhausted"}}`，
且不设置 `Retry-After`。permit 会保持到正文不再由解析器、代理请求或下游响应写入持有，
正常完成、错误、断连、超时和停机取消都会自动归还。

代理会移除请求和响应中的标准 hop-by-hop 头以及 `Connection` 动态列出的头，覆盖
上游 `Host`，按缓冲后的实际正文重建 `Content-Length`，并将下游 `Server` 保持为
`ApiGate`。`Content-Type`、`Authorization`、`Cookie` 等
端到端请求头在公开路由和旧单上游模式下保持转发；受保护路由认证成功后移除 Authorization，
不把网关密钥传给上游。这些头不会写入 ApiGate 日志。客户端提供的
`X-Forwarded-*` 不会被信任或转发，本阶段也不生成这些头。启用代理不会改变本地
健康端点；`/readyz` 仍不主动探测上游。

活动下游连接达到 `APIGATE_MAX_CONNECTIONS` 时，应用暂停发起新的 accept；已进入
监听 backlog 的连接由操作系统排队，恢复容量后继续接收。这一背压策略不会主动向
backlog 中的连接返回 HTTP 503，也不修改系统 backlog。连接数、代理数与正文预约预算
彼此独立，不能互相替代。正文预算不计 HTTP 头、socket 内核缓冲、Beast 固定缓冲、
C++ 对象元数据、allocator 额外开销、本地小型 JSON 响应或日志缓冲，也不是 RSS、
allocator 使用量或生产容量硬上限；正文复制和分配容量还可能使实际内存高于预约值。
默认 64 MiB 未经生产负载测量，上线前仍需按真实流量测量并调整全部限制。

收到 `SIGTERM` 或 `SIGINT` 后，服务立即停止 accept，并关闭空闲 keep-alive、部分请求
及其他尚未分派的连接。已经分派的本地响应或上游代理交换可以在
`APIGATE_SHUTDOWN_GRACE_MS` 期限内完成，响应写完后连接关闭且不再读取下一请求；期限
到达后剩余操作会被取消。正常完成及受控超时都返回 0，非预期运行时故障仍返回非零。
排空期间再次收到 `SIGTERM` 或 `SIGINT` 会立即取消剩余会话和上游交换，不再等待
排空期限；这仍视为操作者请求并返回 0。期限到达和第二次信号是两个独立的强制清理
来源。当前没有预排空 HTTP API 或独立管理端口；这些停机行为也不构成生产就绪保证。
外部进程管理器的强制终止期限仍应为取消回调和进程清理保留余量。

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
构建脚本默认限制并发：分析构建为 3，Debug/Release 为 4，限制同时运行的重型
clang-tidy/编译进程数量。直接使用构建预设也采用相同默认值。
需要调整时可运行 `CMAKE_BUILD_PARALLEL_LEVEL=1 bash scripts/check.sh`，或
`CMAKE_BUILD_PARALLEL_LEVEL=2 bash scripts/build.sh linux-debug`；脚本只接受
正整数，空值和 0 会在配置前拒绝。直接调用 CMake 时使用 `--parallel N` 覆盖预设。
这是单次项目构建的并发限制，不是内存硬上限；不限制依赖安装或多个独立构建的
合计负载。低内存环境仍应避免同时启动多个构建；如出现明显内存压力或远程连接
卡顿，将 `CMAKE_BUILD_PARALLEL_LEVEL` 降为 1。这些默认值不保证所有机器都能稳定运行。
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

### 容量与慢客户端基线

构建 Debug 可执行文件后，可用仅依赖 Python 标准库的本地工具重复观察慢速正文占用、
预算拒绝、额度回收和 ApiGate RSS 采样：

```bash
python3 scripts/capacity-baseline.py build/linux-debug/bin/api-gate
python3 scripts/capacity-baseline.py build/linux-debug/bin/api-gate --smoke
```

普通模式默认运行 3 轮，每轮各启动 8 个竞争请求，已知长度预约为 32768 字节，单次
等待超时为 10 秒；`--smoke` 对未显式指定的参数改用 1 轮、2 个竞争请求、4096 字节和
5 秒。可用 `--rounds`、`--contenders`、`--reservation-bytes`（1–65536）及
`--timeout-seconds` 调整。工具为 ApiGate 和受控 HTTP 上游选择动态端口，自行清理进程、
线程与 socket；完整基线保持手动执行，CTest 只运行很小的 smoke 回归。
每次运行会为子进程单独设置正文预算，使其等于 `--reservation-bytes`，并把连接和代理
数量上限设为大于竞争请求数，以隔离正文预算拒绝；不会修改程序默认的 64 MiB 配置。
预约为 1 字节时，慢上传在 header 后暂停，暂不发送正文，避免提前完成唯一的正文字节。

成功时标准输出只有一份 JSON，包含 schema 版本、实际参数、两个场景的请求/成功/正文
预算 503/其他错误计数、耗时、日志核验，以及从 `/proc/<pid>/status` 周期采样的
`baseline_kib`、`approx_peak_kib` 和 `delta_kib`。诊断写入标准错误。RSS 是近似采样，
不是 allocator 精确统计或跨机器稳定阈值；工具也不设置延迟、吞吐或 RSS 合格线。
该结果只是测量基线，不是生产容量或性能承诺，应在目标机器和代表性流量下重新运行。
输出结果文件及诊断日志属于本地产物，不应提交仓库。

### VS Code Dev Container

仓库的 `.devcontainer/` 提供独立的单容器 Linux 开发环境，不复用宿主 vcpkg，也不
挂载 Docker socket。先安装 VS Code Dev Containers 扩展并确保 Docker 可用，再从
WSL 中打开仓库，执行 `Dev Containers: Rebuild and Reopen in Container`。首次创建
会以非 root `vscode` 用户运行 `scripts/install-deps.sh`，把固定版本的 vcpkg 准备到
`/home/vscode/.local/share/apigate/vcpkg`；第一版不持久化跨重建缓存。

进入容器后选择 CMake Configure Preset `linux-debug`，即可使用 CMake Tools 配置、
构建和调试。C/C++ 扩展由 CMake Tools 提供配置，生成的
`build/linux-debug/compile_commands.json` 可用于 IntelliSense。也可在容器终端执行：

```bash
cmake --preset linux-debug
bash scripts/test.sh linux-debug
bash scripts/check.sh
```

开发容器不会改变 `deploy/Dockerfile`、Compose 或生产运行镜像。退出开发容器后，宿主
仍可使用相同 Preset 和脚本；若同一工作区的 CMake 缓存记录了另一环境的 vcpkg 路径，
`scripts/build.sh` 会按现有逻辑刷新对应预设的缓存。容器只增加 GDB 所需的
`SYS_PTRACE` capability，不放开 Docker socket 或其他宿主资源。容器内 `build/`
使用可执行的临时文件系统，避免复用宿主 CMake 缓存；容器停止后需重新构建。

## 配置

程序只读取下列环境变量；`.env.example` 是示例，不会自动加载。

| 环境变量 | 程序默认值 | 校验 |
| --- | --- | --- |
| `APIGATE_SERVICE_NAME` | `api-gate` | 1–64 个字母、数字、点、下划线或连字符 |
| `APIGATE_ENVIRONMENT` | `development` | 同上 |
| `APIGATE_LOG_LEVEL` | `info` | `trace`、`debug`、`info`、`warn`、`error`、`critical` |
| `APIGATE_LISTEN_ADDRESS` | `127.0.0.1` | IPv4/IPv6 字面地址，不解析主机名 |
| `APIGATE_LISTEN_PORT` | `8080` | 0–65535；0 由内核分配临时端口 |
| `APIGATE_TLS_CERT_FILE` | 未设置 | PEM 服务端证书链；与 APIGATE_TLS_KEY_FILE 同时设置，启用单监听器 HTTPS |
| `APIGATE_TLS_KEY_FILE` | 未设置 | 匹配的未加密 PEM 私钥，不交互询问口令 |
| `APIGATE_METRICS_ENABLED` | `false` | 严格 true/false；在现有监听器暴露无认证的 GET /metrics |
| `APIGATE_MAX_CONNECTIONS` | `256` | 1–65535；活动下游连接数上限 |
| `APIGATE_MAX_CONCURRENT_PROXIES` | `32` | 1–65535；并发上游代理交换数上限 |
| `APIGATE_MAX_IN_FLIGHT_BYTES` | `67108864` | 1–1073741824；全局缓冲正文预约预算，单位字节 |
| `APIGATE_SHUTDOWN_GRACE_MS` | `5000` | 1–60000；信号排空期限，单位毫秒 |
| `APIGATE_UPSTREAM_KEEP_ALIVE` | `false` | 严格 true/false；可选 HTTP/HTTPS 连接复用，32 条全局空闲连接、30 秒空闲期限 |
| `APIGATE_UPSTREAM_HOST` | 未设置 | 纯 DNS 主机名、IPv4 或 IPv6 字面地址；必须与端口同时设置 |
| `APIGATE_UPSTREAM_PORT` | 未设置 | 1–65535；必须与主机同时设置 |
| `APIGATE_UPSTREAM_TIMEOUT_MS` | `3000`（代理启用时） | 1–60000；仅可与完整上游配置一起使用 |
| `APIGATE_UPSTREAM_CA_FILE` | 未设置 | 可加载的 PEM CA 文件；替代系统 HTTPS 信任库；空值或加载失败在监听前报错，不输出路径 |
| `APIGATE_ROUTES` | 未设置 | 非空 JSON 数组，最多 32 条、原始值最多 64 KiB；与任一旧上游变量互斥 |

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

按路径转发到不同后端（先取消当前 shell 中的旧上游变量）：

```bash
unset APIGATE_UPSTREAM_HOST APIGATE_UPSTREAM_PORT APIGATE_UPSTREAM_TIMEOUT_MS
APIGATE_ROUTES='[{"prefix":"/users","host":"127.0.0.1","port":9001,"strip_prefix":true,"rate_limit":{"requests_per_second":10,"burst":20}},{"prefix":"/orders","host":"127.0.0.1","port":9002,"timeout_ms":2000}]' \
bash scripts/run.sh
```

旧单后端形式每项必填 `prefix`、`host`、整数 `port`（1–65535）；可选整数 `timeout_ms`
（1–60000 毫秒，默认 3000）和 boolean `strip_prefix`（默认 false）。
`strip_prefix` 只接受 JSON true/false，不接受数字、字符串或 null。
host 使用上述 DNS/IP 规则。拒绝未知、重复或缺失字段、
重复 prefix、空值和非法类型；错误不会回显路由配置。所有配置在监听前校验，只在启动时读取。
prefix 必须以 `/` 开始；除 `/` 外不得以 `/` 结束。路径段只能含 ASCII 字母、数字及
`-._~`，不允许空段、`.`、`..`，也不能包含 query、fragment、空格、控制字符、反斜杠或 `%`。

上面的旧路由级 host/port/timeout_ms 形式继续兼容。也可以为同一路由配置多个实例：

```bash
unset APIGATE_UPSTREAM_HOST APIGATE_UPSTREAM_PORT APIGATE_UPSTREAM_TIMEOUT_MS
APIGATE_ROUTES='[{"prefix":"/api","strip_prefix":true,"upstreams":[{"host":"127.0.0.1","port":9001},{"host":"127.0.0.1","port":9002,"timeout_ms":2000}],"rate_limit":{"requests_per_second":10,"burst":20}}]' \
bash scripts/run.sh
```

每条路由必须选择一种后端形式。`upstreams` 与路由级 `host`、`port`、`timeout_ms`、`scheme`
任一字段互斥（即使该字段为空或 null）；数组非空且最多 16 项。每项仅接受必填
host、port 和可选 timeout_ms、scheme，沿用上述主机、整数端口及超时校验，超时默认 3000 毫秒。
未知、重复、缺失字段、非法类型和非预期嵌套均拒绝。重复地址不去重，每项是独立槽位。
原始配置仍最多 64 KiB、最多 32 条路由，与旧三项上游环境变量互斥。

### HTTPS 上游

两种路由后端形式都支持 `scheme`，只接受字符串 `http` 或 `https`，省略为 `http`。
不根据端口推断协议，port 始终必填，host 仍是 DNS/IP 而不是 URL。旧路由和旧
`APIGATE_UPSTREAM_HOST/PORT/TIMEOUT_MS` 模式保持 HTTP。同一路由可以混合两种实例：

```bash
unset APIGATE_UPSTREAM_HOST APIGATE_UPSTREAM_PORT APIGATE_UPSTREAM_TIMEOUT_MS
APIGATE_ROUTES='[{"prefix":"/api","strip_prefix":true,"upstreams":[{"host":"backend.example.com","port":443,"scheme":"https"},{"host":"127.0.0.1","port":9001}]}]' \
bash scripts/run.sh
```

HTTPS 验证证书链及**配置 host** 的服务器身份，DNS 主机发送该名称作为 SNI，IP 按
IP SAN 验证且不发送 DNS SNI；不采用下游 Host，也没有跳过验证开关。最低 TLS 1.2，
ALPN 只提供 HTTP/1.1。未设置 CA 文件时使用运行机器的系统 CA bundle，依次查找
`/etc/ssl/certs/ca-certificates.crt`、`/etc/pki/tls/certs/ca-bundle.crt`、`/etc/ssl/cert.pem`，
加载首个存在的文件；不是 vcpkg 的安装路径。私有 CA 可通过
`APIGATE_UPSTREAM_CA_FILE=/path/to/ca.pem` 指定，它**替代而非叠加**系统信任。
显式文件即使没有 HTTPS 路由也要校验；纯 HTTP 且未指定文件不加载系统信任。
`--check-config` 同样加载本地信任配置，但不执行 DNS 或连接后端，也不输出路径或证书。

TLS 握手单独使用所选后端的 timeout_ms；超时返回安全 504，证书、身份及其他握手
失败返回安全 502，诊断阶段为 `handshake`（期限触发仍为 `timeout`）。完整、有界定的
HTTP 响应收到后直接回收底层连接，不等待 TLS close_notify；未完整响应的 TLS 截断
仍作为读取失败，不能当作成功。取消和第二信号强停会关闭握手及读写的底层 socket。
轮询、限流、Host、前缀剥离、query、全局容量和正文预算语义不变，不自动重试。

OpenSSL 由现有 vcpkg/CMake 管理，当前 x64-linux 静态链接 SSL/crypto；运行环境仍需
系统 CA（Ubuntu 的 ca-certificates）或可读的自定义 PEM。TLS 进程测试还需本机 openssl
命令生成临时测试 CA/密钥，不使用个人证书。下游默认 HTTP，可独立启用单监听器 HTTPS；不支持 mTLS、HTTP/2、
自动重试或全面生产容量保证；主动摘除需显式配置 health_check。

### 轮询与路由限流

每条路由的轮询位置在单进程内由所有客户端共享，同一 keep-alive 连接逐请求参与。
初始选择第一个槽位，按数组顺序循环；不同路由互不影响，重启后从第一个重新开始。
处理顺序为完整读正文并校验、匹配一次路由、限流、申请全局代理名额、选择一次后端、
启动一次交换。429 和代理容量 503 不推进位置；选择后即使启动失败、上游失败或取消
也不回退，不自动重试其他实例。业务失败本身不摘除实例；主动摘除需显式启用健康检查。Host 按实际后端重建，使用该后端自身
的逐阶段超时；strip_prefix、query、方法和正文保持原语义。限流额度属于整条路由，
不是各后端独立额度；连接、并发代理和正文预算也仍全局共享。健康端点不参与轮询。
轮询只分配请求，不保证耗时、负载或资源完全均衡，不保证客户端公平性或生产容量。
这不是高可用或故障转移机制，也没有跨进程协调或权重；连接复用需显式启用。

按原始目标 `?` 前的路径、区分大小写并在路径段边界选择最长前缀，与配置顺序无关。
`/users` 匹配 `/users` 和 `/users/42`，不匹配 `/users-extra`；`/` 可兜底。
默认保留原始路径。启用 `strip_prefix` 让后端无需感知外部前缀：先选路，再仅从 path
原始字节中移除匹配前缀，不重新选路。例如 `/users/42` → `/42`，`/users`、`/users/`
→ `/`，`/users?x=1` → `/?x=1`。根路由 `/` 即使启用也保持原样；旧单上游模式不剥离。
query 逐字节保留，包括重复参数、百分号大小写、空值和单独尾随 `?`。
这只是可选匹配前缀剥离，不是路径规范化或安全过滤：不解码、不折叠重复斜杠，
不解释 `.`、`..`；例如 `/users//42` → `//42`。不支持任意 rewrite 模板或 query、
Location、Cookie Path 重写；后端对路径的解释可能不同。
本地 `/healthz`、`/readyz` 优先于任何路由；剥离后变为这两个路径的请求仍发往已选上游。
合法且支持的方法未命中时返回
本地 404，不回退到其他上游。同一 keep-alive 连接的每个请求重新选路，Host 按所选
上游重建，使用该路由的超时。连接、代理并发和正文预算仍由所有路由全局共享。
静态轮询不提供故障转移、服务发现或动态更新。

每条路由还可选 `rate_limit` 对象；省略时不限流。配置后必须同时提供整数
`requests_per_second` 和 `burst`，两者范围均为 1–1000000；拒绝布尔、浮点、字符串、
null、未知或重复字段。前者是每秒补充的令牌数，后者是最多可积累的令牌数和初始
突发额度，每次限流准入消耗一个令牌。按单调时间惰性补充，不排队、不自动重试。
同一进程所有客户端共享所选路由的桶，即使 keep-alive 也逐请求检查；不同路由独立，
即使指向同一后端。多进程之间不共享，重启会恢复初始突发额度。前缀剥离不改变桶归属。
本地 `/healthz`、`/readyz` 豁免；未命中和原本非法的请求保留原有响应，不消耗令牌。

耗尽时不访问上游，返回 429：
`{"error":{"code":"rate_limited","message":"request rate limit is exceeded"}}`。
`Retry-After` 按下一令牌的等待时间向上取整为整数秒，至少为 1；它只是重试建议，
其他客户端可能先消费令牌，不保证届时获准。429 与连接、代理并发和正文预算容量
机制独立，不是既有的容量 503。限流在完整读取正文、校验和选路之后，代理名额申请
之前执行一次；通过后即使容量拒绝、上游失败、断连或取消也不返还令牌。
它保护上游请求速率，不是上传带宽限制、连接容量限制、公平性或生产容量保证。
正常 429 沿用低敏感度 `http_request_completed` 日志，不记录路径、query 或正文，
不误报上游错误；实际容量和性能仍需在目标负载下测量。

### 可选下游 TLS（HTTPS）

默认监听明文 HTTP。只有同时设置两个非空配置后，**同一个监听器改为 HTTPS**：

```bash
APIGATE_TLS_CERT_FILE=/path/to/server-chain.pem \
APIGATE_TLS_KEY_FILE=/path/to/server-key.pem \
bash scripts/run.sh
curl --cacert /path/to/trusted-ca.pem https://localhost:8080/healthz
```

服务端证书必须覆盖客户端访问时使用的主机名/IP。客户端应配置可信 CA 并验证证书链和
主机身份；示例没有关闭校验。提供 PEM 证书链和匹配的未加密 PEM 私钥，在监听前加载并
检查匹配；任一配置缺失、空值、文件不可读/无效、私钥不匹配或加密均安全失败，不提示输入
口令，也不回显路径、证书、私钥或 OpenSSL 错误栈。程序不会生成用户证书或修改文件权限。
`--check-config` 同样加载校验，只报告 `tls_enabled` 等安全状态，不创建监听器；
`--version` 保持不读取配置。

最低 TLS 1.2，仅 HTTP/1.1；ALPN 只协商 http/1.1，未提供 ALPN 仍可使用 HTTP/1.1，
提供 ALPN 但无兼容协议则拒绝握手。不开放第二个 HTTP 端口，不自动跳转或识别混合协议。
上游独立选择 HTTP/HTTPS，服务端与上游客户端 TLS context、证书和验证策略分离。

accept 后立即占用连接名额，再异步握手。握手共享一次固定 **5 秒总期限**，零碎数据不刷新；
完成后才开始原 HTTP 读取期限。握手失败、超时、断连和取消不产生 HTTP 响应、不计 HTTP
累计指标，不取得代理 permit 或正文预算，只作低敏感度结构化诊断并释放连接名额。
HTTP keep-alive、请求/响应边界、路由、原始 query、限流、轮询、健康状态和指标语义不变。

正常 HTTP 连接关闭时尝试 TLS close_notify，关闭阶段最多 **2 秒**；对端不响应也会关闭。
排空立即取消未完成握手、空闲或部分请求，已分派请求仍按原期限完成并不再读取下一请求。
已有排空总 deadline 不会因 TLS shutdown 延长，deadline 和第二信号能强制终止握手及关闭。
协议错误、超时、RST 和强停可直接关闭底层 socket；未完成请求缺失 close_notify 仍失败，
不全局忽略 stream_truncated。TLS 有额外缓冲和握手开销，正文预算不是 RSS 上限。

TLS 是传输加密，**不是认证**；未要求客户端证书。启用后 `/metrics` 仍无访问认证，任何
能访问该监听器的客户端仍可读取，不代表仅可信客户端可访问。首版仅单份证书，不提供
mTLS、SNI 多证书、0-RTT、HTTP/2、ACME、签发或证书热更新，也不构成完整生产就绪保证。

### 可选路由级 API Key 认证

静态路由可选 `auth`，省略即公开。两种后端形式都支持；旧 `APIGATE_UPSTREAM_*` 模式不增加
认证配置。`auth` 必须是只含字符串 `api_key_env` 的非空对象，拒绝 null、类型错误、重复、
未知字段及非法嵌套。示例只含环境变量引用，**不要将密钥写入路由 JSON**：

```json
[{"prefix":"/private","host":"127.0.0.1","port":9001,"strip_prefix":true,"auth":{"api_key_env":"APIGATE_AUTH_KEY_PRIVATE"}}]
```

引用名必须是 `APIGATE_AUTH_KEY_` 加 1–64 个大写字母、数字或下划线。引用的密钥必须为
32–256 字节 ASCII 字母、数字、下划线或连字符，不修剪或自动纠正。启动时读取一次；缺失、
空值、非法引用/密钥会在监听前安全失败，不回显密钥或环境变量值。受保护路由必须同时配置
下游 TLS 证书和私钥，没有允许明文密钥的绕过开关。`--check-config` 校验并报告安全的
`authenticated_route_count`，不输出引用值或摘要；`--version` 不读取配置。

通过安全的进程环境注入高熵密钥，不把真实值写入源码、环境示例、历史命令或日志。交互示例
只引用变量和明确占位符，不提供可用密钥：

```bash
IFS= read -r -s -p 'API Key: ' APIGATE_AUTH_KEY_PRIVATE
printf '\n'
export APIGATE_AUTH_KEY_PRIVATE
APIGATE_TLS_CERT_FILE='<PEM_CERT_CHAIN_PATH>' \
APIGATE_TLS_KEY_FILE='<PEM_PRIVATE_KEY_PATH>' \
APIGATE_ROUTES='[{"prefix":"/private","host":"127.0.0.1","port":9001,"strip_prefix":true,"auth":{"api_key_env":"APIGATE_AUTH_KEY_PRIVATE"}}]' \
bash scripts/run.sh
```

在客户端安全注入同一变量后，保留证书链和主机身份验证，并通过标准输入提供头，避免把
展开后的密钥放入 curl 参数：

```bash
printf 'Authorization: Bearer %s\n' "$APIGATE_AUTH_KEY_PRIVATE" | \
  curl --cacert '<TRUSTED_CA_PATH>' --header @- https://localhost:8080/private/item
```

认证要求恰好一个 Authorization 头；Bearer 方案大小写不敏感，凭据为单个合法密钥。
重复头、空值、逗号列表、多凭据、错误方案或错误密钥统一返回 401，安全 JSON 错误码
`authentication_failed`、`WWW-Authenticate: Bearer realm="ApiGate"` 和 `Cache-Control: no-store`。
不从 query、Cookie 或正文取密钥。公开路由及旧单上游的 Authorization 转发行为不变。

顺序是完整读取请求并校验协议 → 按原始路径最长前缀选路 → 认证 → 路由限流 → 代理名额 →
健康后端/轮询选择 → 借用连接池/访问上游。只选路一次，失败不回退；strip_prefix 不重新选路
或认证。更具体路由只使用自身规则，不继承父路由；每个 keep-alive 请求独立认证。
401 沿原响应计数、耗时和写出链处理，不计为上游失败、限流或容量拒绝；失败不消费令牌、
推进轮询或申请代理名额。**认证在正文读取之后，不能阻止未认证客户端上传正文**，仍受既有
大小、超时和全局正文预算限制，正文占用在响应完成/关闭后归还。

本地 `/healthz`、`/readyz` 和启用后的精确 `/metrics` 豁免；`/metrics` 仍无访问认证，不能把
路由认证描述为整个监听器访问控制。长期配置只保留 OpenSSL SHA-256 的固定长度摘要，使用
固定 32 字节 CRYPTO_memcmp 比较；不记录凭据、摘要、完整路径或 query。不宣称进程内存
秘密完全清除，原始环境和处理中的请求仍可能含有密钥。
这是单路由、单密钥、启动时配置的不透明字符串认证，不是 JWT，不解析身份或角色；不包含
轮换/热更新、OAuth、用户账号、外部密钥管理或生产就绪保证。长度合法不等于密钥强度足够。

### 可选上游连接池

`APIGATE_UPSTREAM_KEEP_ALIVE` 默认 `false`，严格接受 `true` 或 `false`，空值和其他值在
监听前安全拒绝；CLI 帮助和 `--check-config` 的 `upstream_keep_alive` 与之同步。例如在
已有上游配置下运行 `APIGATE_UPSTREAM_KEEP_ALIVE=true bash scripts/run.sh`。
旧单上游、静态路由及轮询均支持；HTTP 与 HTTPS 均可复用，HTTPS 继续校验证书链、主机身份、
SNI、HTTP/1.1 ALPN 和最低 TLS 版本，不提供跳过验证。关闭时仍逐请求建连并发送
`Connection: close`。开启时从下游剥离 hop-by-hop 头，按上游持久连接语义构造请求。

服务器在“路由限流 → 代理名额 → 健康后端选择”之后借用匹配连接。键包含主机、端口、
HTTP/HTTPS 及 TLS context 身份，不跨不兼容 TLS 策略复用；不同地址的槽位不串用，
相同后端可以跨路由、跨下游客户端顺序复用。一个连接同时只处理一个交换；没有等待队列、
流水线或多路复用，无空闲连接就按原流程解析、建连、握手。健康探测不使用业务池。

首版全进程最多缓存 **32 条空闲连接**，空闲期限 **30 秒**，单个 timer 主动清理到期项，
借出时也检查到期，并重新检查 socket 和 TLS 内部是否存在非预期数据；不能确认干净的候选
直接丢弃，在发送业务请求前可寻找其他候选或新建连接，不读取并清空额外响应后复用。
超过上限的归还连接直接关闭；在途业务仍受代理名额限制。
仅请求写完、原始上游响应完整且允许持久连接、无分帧或残留数据异常时归还。
`Connection: close`、EOF 分帧、截断、协议错误、超时、取消、升级或其他不确定状态均丢弃。
空闲池不持有代理 permit、正文预算或请求/响应正文；仍有额外有界的 socket、TLS 和对象开销，
逻辑正文预算不是 RSS 上限。这些初始工程边界没有新增调优项，也不是性能或生产容量保证。

借出检查不能消除检查之后对端关闭或发送额外数据的所有竞态。陈旧连接的写入/读取失败沿既有 502/504 及失败阶段分类返回，
直接丢弃，**不自动重试或切换后端**，POST 等请求不会因此重放；客户端不应假定失败表示
上游完全未收到请求。后续独立请求可以重新建连，DNS 只在需要新连接时解析，不实现 TTL 管理。

排空开始立即停止借出、关闭所有空闲连接并取消清理任务。已准入业务按原期限完成，完成后
关闭而不再归池；排空 deadline 和第二信号仍取消在途交换。池关闭和租约释放均幂等，
原有 Host、原始 query、strip_prefix、限流、轮询、健康摘除、指标和预算回收语义保持不变。
不包含重试、故障转移、被动熔断、连接池性能基线或新的指标体系。

### 请求指标与运行状态

`APIGATE_METRICS_ENABLED` 默认 `false`，严格接受 `true` 或 `false`；空值及其他值在监听前
拒绝，`--check-config` 报告 `metrics_enabled`。启用后使用原监听器，无新端口：

```bash
APIGATE_METRICS_ENABLED=true bash scripts/run.sh
curl -sS http://127.0.0.1:8080/metrics
```

端点没有认证，可被能够访问现有监听器的客户端读取；监听地址由用户配置，不能假定仅本机
可访问。启用时精确 `/metrics`（忽略 query）优先于代理路由；GET 返回指标，其他方法返回
405 / `Allow: GET`，非空 GET 正文及协议问题安全拒绝。`/metrics-other` 不匹配；已选代理路由
剥离后形成 `/metrics` 仍发往上游。关闭时不保留该路径，完全沿用原路由行为。
`/healthz`、`/readyz` 语义不变。指标请求不访问上游，不消费路由令牌或代理名额。

输出采用 [Prometheus Text Format 0.0.4](https://prometheus.io/docs/instrumenting/exposition_formats/)，
UTF-8、LF，末行有 LF；`Content-Type: text/plain; version=0.0.4; charset=utf-8`，
`Cache-Control: no-store`。指标如下：

| 指标 | 含义与排除项 |
| --- | --- |
| `apigate_http_responses_total{status="200"}` | 为下游生成的最终响应数，不保证成功送达；本地、代理及早期协议拒绝均统计，每请求最多一次，写失败不重计，直接关闭无状态。status 固定为 100–599 或 other。 |
| `apigate_rejections_total{reason="rate_limit"}` | 实际本地准入拒绝，固定 reason 为 rate_limit、proxy_capacity、buffer_capacity、no_healthy_upstream；上游自己返回 429/503 不算。 |
| `apigate_upstream_failures_total{stage="timeout"}` | 业务代理失败，固定 stage 为 resolve、connect、handshake、write、read、timeout、response_too_large、invalid_response；排除健康探测、正常取消和普通上游 4xx/5xx。 |
| `apigate_request_processing_seconds_sum`、`_count` | 无 quantile 的 summary：单调时钟，从完整请求进入 process_request 到最终响应准备进入下游写出。包含业务上游处理，不含请求头等待、正文上传或下游写出。早期解析拒绝和未生成最终响应的取消不计样本，因此统计集合与响应计数不同。 |
| `apigate_active_connections`、`apigate_max_connections` | 真实当前下游连接数（包含抓取连接）及上限；连接容量通过暂停 accept 背压，不生成虚构的拒绝指标。 |
| `apigate_active_proxies`、`apigate_max_concurrent_proxies` | 现有业务代理 permit 占用及上限。 |
| `apigate_in_flight_body_bytes`、`apigate_max_in_flight_body_bytes` | 现有逻辑正文预算占用及上限，单位字节。 |
| `apigate_upstream_available{route_index="0",upstream_index="0"}` | 静态路由槽位允许轮询选择为 1，健康检查摘除为 0；初始及未启用检查为 1，不表示已检测健康或公网可达。同地址槽位不合并，旧环境变量单上游模式不输出此样本。 |

本地指标请求（包括其方法/协议拒绝）不进入累计响应、拒绝、失败及耗时统计；重复抓取不重置
或污染累计值，资源 gauge 始终反映真实占用。单进程统计，重启清零。整数计数达到 uint64
上限后饱和，耗时总和有限且饱和，不回绕、不输出 NaN/Inf。

标签不含路径、前缀、地址、query、正文、客户端或凭据。累计状态固定大小，最多 1033 条时间
序列（含 32×16 个槽位），每个响应正文硬上限 128 KiB。指标正文沿用本地控制响应边界，
不计入业务代理正文预算，也不扩大该预算；每个抓取仍有额外有界的渲染、正文与写出开销，
逻辑正文预算不是 RSS 上限。没有持久化、告警、百分位数、监控平台或生产容量保证。

### 主动健康检查与故障摘除

路由可选 health_check，省略即关闭；单后端和 upstreams 数组形式均支持。旧
APIGATE_UPSTREAM_* 单上游模式不增加健康检查。例如：

```bash
unset APIGATE_UPSTREAM_HOST APIGATE_UPSTREAM_PORT APIGATE_UPSTREAM_TIMEOUT_MS
APIGATE_ROUTES='[{"prefix":"/api","upstreams":[{"host":"127.0.0.1","port":9001},{"host":"127.0.0.1","port":9002}],"health_check":{"path":"/healthz","interval_ms":5000,"timeout_ms":1000,"unhealthy_threshold":3,"healthy_threshold":2}}]' \
bash scripts/run.sh
```

path 必填，必须以 `/` 开始，不含 query、fragment、空格或控制字符，不接受完整 URL。
其他字段可省略，且只接受 JSON 整数：interval_ms 默认 5000、范围 100–60000；
timeout_ms 默认 1000、范围 50–60000，不能大于 interval_ms；unhealthy_threshold 默认 3，
healthy_threshold 默认 2，两者范围 1–10。未知、重复字段和非法类型在监听前安全拒绝。
配置加载和 `--check-config` 不启动探测，不执行 DNS 或连接后端。

每个后端槽位初始可用，启动后尽快探测，重复地址的槽位仍独立。连续失败达到阈值摘除，
任意成功清零失败数；摘除后继续探测，连续成功达到恢复阈值重新加入，任意失败清零恢复数。
只在摘除/恢复时记录 `http_upstream_marked_unhealthy` / `http_upstream_marked_healthy`，
字段为 route_index、upstream_index 和固定 reason，不记录地址、健康路径或正文。
状态属于单进程，重启重置；初次结果前仍准入，摘除不会取消既有业务交换。

探测直接向槽位发送 GET，使用其 Host 和 HTTP/HTTPS；不应用 strip_prefix，不携带下游
认证头、Cookie 或正文，也不跟随重定向。HTTPS 复用相同 CA、身份、SNI 和 TLS 策略。
只检查完整合法的最终响应头及 200–299 状态；1xx、其他状态、协议/网络/TLS 错误或超时
均为失败，**不读取或校验健康响应正文**。响应头上限 16 KiB，输入缓冲上限 32 KiB。
timeout_ms 是 DNS、TCP、TLS、请求和响应头共享的总期限，不是逐阶段重新计时。

同槽位最多一个探测，全进程最多 4 个，按轮转顺序公平调度到期槽位，不积累任务队列。
同槽位相邻探测的启动时间至少间隔 interval_ms；饱和时延后，不将未执行任务计作失败，
也不承诺高负载下精确按时。探测不消耗业务令牌、代理名额或正文预算；仍有独立且有界的
socket、TLS、解析和 timer 开销。开始排空立即取消所有探测，不等自然超时、不计停机失败。

准入仍先限流、再申请全局代理名额，再从当前位置最多扫描所有槽位一次，跳过摘除槽位；
选中后位置移到其后继。全部不可用不推进位置，立即归还代理名额，返回安全 503：
`{"error":{"code":"no_healthy_upstream","message":"no healthy upstream is available"}}`。
不设置 Retry-After，不返还已消费的令牌；正文预算沿既有写完/关闭链释放。该响应不同于
容量不足的 503 和限流 429，不触发业务重试。`/healthz`、`/readyz` 仍只表示本地服务状态，
不是后端健康聚合。此能力不是被动熔断、故障转移、自动重试或高可用/生产容量保证。

新旧上游变量均未设置时代理关闭，原有未知 GET 的 404 及其他方法的 405 行为保持不变。
三项容量配置始终校验；代理关闭时代理并发上限不被使用。配置检查结果会报告
`max_connections`、`max_concurrent_proxies`、`max_in_flight_bytes`、
`shutdown_grace_ms`、`proxy_enabled`、`tls_enabled`；旧单上游模式包含 `upstream_timeout_ms`，
路由模式只包含安全的 `route_count`，不会输出路由表、host 或 prefix，也不虚构统一超时。

当前没有每客户端/IP、全局或分布式限流；也不支持其他代理方法、
流式上传、动态路由、mTLS、HTTP/2、WebSocket 或预排空管理阶段。实际可承载数量
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
  和 `VCPKG_DEFAULT_BINARY_CACHE` 覆盖。按预设隔离的已安装依赖默认为
  `${XDG_CACHE_HOME:-$HOME/.cache}/apigate/vcpkg/installed/<preset>`；脚本和直接使用
  CMake Preset 时均适用。显式 `-DVCPKG_INSTALLED_DIR=...` 或已有 CMake 缓存值优先；
  修改 XDG_CACHE_HOME 后，如需切换已有构建的默认位置，应使用 `cmake --fresh --preset ...`
  重新配置，不会自动迁移或删除旧目录。这些缓存不是完整离线镜像；清理后，后续构建
  可能需要重新联网下载或编译依赖。
- Docker 权限不足：确认守护进程可用，以及当前用户是否有权访问 Docker socket。
  如仅 Docker 构建网络下载超时，可在可信的 Linux 开发环境中使用
  `APIGATE_DOCKER_BUILD_NETWORK=host bash scripts/container-smoke.sh`。

架构、取舍、停止行为和测试覆盖见[设计文档](设计文档.md)。
