# ApiGate

ApiGate 是一个用于学习和逐步开发 API 网关的 C++20 项目。当前版本提供异步
HTTP/1.1 健康检查、配置校验、JSON 日志，以及可选的路径路由与多个静态 HTTP 上游。
代理支持 GET 及有界正文的 POST、PUT、PATCH，以及同一路由多实例轮询。它没有流式上传、重试、连接池、
缓存、每客户端或分布式限流、认证、TLS 或 HTTP/2；可选按路由限流不构成生产容量保证。
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
上游响应正文上限为 1 MiB，且每个请求都新建上游连接；所有方法均不自动重试，尤其
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
端到端请求头会转发给所配置上游，但不会写入 ApiGate 日志。客户端提供的
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
| `APIGATE_MAX_CONNECTIONS` | `256` | 1–65535；活动下游连接数上限 |
| `APIGATE_MAX_CONCURRENT_PROXIES` | `32` | 1–65535；并发上游代理交换数上限 |
| `APIGATE_MAX_IN_FLIGHT_BYTES` | `67108864` | 1–1073741824；全局缓冲正文预约预算，单位字节 |
| `APIGATE_SHUTDOWN_GRACE_MS` | `5000` | 1–60000；信号排空期限，单位毫秒 |
| `APIGATE_UPSTREAM_HOST` | 未设置 | 纯 DNS 主机名、IPv4 或 IPv6 字面地址；必须与端口同时设置 |
| `APIGATE_UPSTREAM_PORT` | 未设置 | 1–65535；必须与主机同时设置 |
| `APIGATE_UPSTREAM_TIMEOUT_MS` | `3000`（代理启用时） | 1–60000；仅可与完整上游配置一起使用 |
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

每条路由必须选择一种后端形式。`upstreams` 与路由级 `host`、`port`、`timeout_ms`
任一字段互斥（即使该字段为空或 null）；数组非空且最多 16 项。每项仅接受必填
host、port 和可选 timeout_ms，沿用上述主机、整数端口及超时校验，超时默认 3000 毫秒。
未知、重复、缺失字段、非法类型和非预期嵌套均拒绝。重复地址不去重，每项是独立槽位。
原始配置仍最多 64 KiB、最多 32 条路由，与旧环境变量互斥，不新增环境变量。

每条路由的轮询位置在单进程内由所有客户端共享，同一 keep-alive 连接逐请求参与。
初始选择第一个槽位，按数组顺序循环；不同路由互不影响，重启后从第一个重新开始。
处理顺序为完整读正文并校验、匹配一次路由、限流、申请全局代理名额、选择一次后端、
启动一次交换。429 和代理容量 503 不推进位置；选择后即使启动失败、上游失败或取消
也不回退，不自动重试其他实例、不摘除故障实例。Host 按实际后端重建，使用该后端自身
的逐阶段超时；strip_prefix、query、方法和正文保持原语义。限流额度属于整条路由，
不是各后端独立额度；连接、并发代理和正文预算也仍全局共享。健康端点不参与轮询。
轮询只分配请求，不保证耗时、负载或资源完全均衡，不保证客户端公平性或生产容量。
这不是高可用或故障转移机制，也没有跨进程协调、健康检查、权重或连接池。

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

新旧上游变量均未设置时代理关闭，原有未知 GET 的 404 及其他方法的 405 行为保持不变。
三项容量配置始终校验；代理关闭时代理并发上限不被使用。配置检查结果会报告
`max_connections`、`max_concurrent_proxies`、`max_in_flight_bytes`、
`shutdown_grace_ms`、`proxy_enabled`；旧单上游模式包含 `upstream_timeout_ms`，
路由模式只包含安全的 `route_count`，不会输出路由表、host 或 prefix，也不虚构统一超时。

当前没有每客户端/IP、全局或分布式限流；也不支持其他代理方法、
流式上传、连接池、动态路由、上游 TLS、HTTP/2、WebSocket 或预排空管理阶段。实际可承载数量
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
