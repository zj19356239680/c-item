# ApiGate

ApiGate 是一个用于学习和逐步开发 API 网关的 C++20 项目。当前版本只提供最小的异步
HTTP/1.1 服务：健康检查、配置校验和 JSON 日志。它**尚不能转发请求**，也没有缓存、
限流、认证、TLS 或 HTTP/2；不要把它当作生产网关。本项目在 Linux 环境下开发，
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

在运行服务的终端按 `Ctrl+C` 停止。`/readyz` 当前只表示进程正在响应请求，
**不检查上游服务或其他依赖**。

## 当前 HTTP 行为

| 请求 | 状态码 | JSON 正文或说明 |
| --- | ---: | --- |
| `GET /healthz` | 200 | `{"service":"api-gate","status":"ok"}` |
| `GET /readyz` | 200 | `{"service":"api-gate","status":"ready"}` |
| `GET /missing` | 404 | `{"error":{"code":"not_found","message":"route not found"}}` |
| `POST /healthz` | 405 | `{"error":{"code":"method_not_allowed","message":"only GET is supported"}}`；`Allow: GET` |

表中服务名使用默认配置。路由匹配时忽略查询字符串；任意非 `GET` 方法都返回
405，即使路径未知。正常响应设置 `Server: ApiGate`、
`Content-Type: application/json` 和 `Cache-Control: no-store`；
正文长度及连接头由 HTTP 库按请求生成。请求头超过 8 KiB 返回 431，正文超过
64 KiB 返回 413；格式错误请求可能返回 400 或直接断开。当前行为尚未制定版本化的
API 兼容承诺，具体设计边界见[设计文档](设计文档.md)。

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

例如，换一个本机端口运行：

```bash
APIGATE_LISTEN_PORT=9000 bash scripts/run.sh
```

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
