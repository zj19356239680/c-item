# ApiGate 0.3.0 本地 Demo

本目录是可独立运行的演示工具，只使用 Python 3 标准库，不引用 `tests/`。
生产服务仍为 C++；演示不代表性能基线或生产容量保证。

## 准备与启动

仅支持 Linux（包括 WSL2）。先按[根 README 的依赖准备步骤](../../README.md#快速启动与验证)
准备 C++20、CMake、Ninja、vcpkg 和相关系统工具，再以普通用户执行：

```bash
bash scripts/install-deps.sh
bash scripts/demo.sh
```

入口用现有 `scripts/build.sh linux-debug` 构建，显式设置构建并发 1、依赖并发 1，
不改变仓库默认值。首次构建需要下载依赖且可能较久；构建总等待上限为 3600 秒。
需要 Bash、Python 3 标准库；构建依赖缺失会由现有入口明确报错。不使用 sudo、
Docker、个人 `.env` 或公网业务后端。准备依赖可能需要网络，已经构建后的演示只访问本地。

自动验收完成后，服务保持运行并输出实际地址、日志位置和 curl 示例，按 Ctrl+C 退出。
可从任意工作目录通过入口的绝对路径启动。`--binary` 相对路径按调用者的当前目录解析：

```bash
bash /path/to/api-gate/scripts/demo.sh --binary /path/to/api-gate/build/linux-analysis/bin/api-gate
bash scripts/demo.sh --smoke --binary build/linux-analysis/bin/api-gate
bash scripts/demo.sh --help
```

`--binary` 完全跳过构建。`--smoke` 执行同样的核心演示，成功后自动退出并返回 0，
失败返回非零。不要同时启动多个构建。自动演示对每次网络操作、启动和事件等待设置
5 秒上限；限流步骤还要求两次请求在一秒内完成，主机过载时会明确失败而非假报通过。

## 自动验证及手动体验

两个后端均绑定 `127.0.0.1:0`，分别返回标记 A、B；ApiGate 也绑定 `127.0.0.1:0`。
后端端口来自绑定结果，网关端口来自 `http_listener_started` JSON 日志，没有先占端口再释放的窗口。
所有继承的 `APIGATE_*` 变量在子进程环境中删除后，再注入演示专用配置，不改变父环境。
不读取个人 `.env`。演示配置使用本地明文 HTTP，勿用真实凭据访问。

| 请求或步骤 | 验证内容 |
| --- | --- |
| `/healthz`、`/readyz` | 正常运行返回 200；readyz 不代表后端健康 |
| `/a/echo?demo=1&raw=%2F`、`/b/echo` | 选定 A/B；strip_prefix 后保留原始 query |
| 连续四次 `/round/echo` | 实际后端标记依次 A、B、A、B |
| `POST /a/body?original=yes` | 方法、目标和正文 `demo-body` 保留 |
| `HEAD /a/head` | 保留非零长度元数据，实际无正文 |
| 连续两次 `/limited` | 1 请求/秒、突发额度 1，返回 200、429；期间不足以补充一个令牌 |
| `/stream` | 后端先发 `first-`，客户端读到正文后才通过事件释放 `last!`；不是只验证响应头 |
| 请求 ID | 响应唯一 ID、后端收到的 ID、完成日志的 request_id 三者一致；覆盖客户端和后端自带 ID |
| `/metrics` | 返回 Prometheus 0.0.4 格式且包含 apigate_* 指标 |

每步输出预期、实际结果及 PASS/FAIL；遇到失败停止，打印有限的网关诊断后清理。
网关原始日志写在临时目录，不混入正常的步骤输出。示例（以入口打印的端口替换 PORT）：

```bash
curl -i 'http://127.0.0.1:PORT/a/hello?demo=1'
curl -s 'http://127.0.0.1:PORT/round/hello'
curl -I 'http://127.0.0.1:PORT/a/head'
curl -s 'http://127.0.0.1:PORT/metrics'
grep -F '<响应中的 X-Request-ID>' '<入口打印的 gateway.jsonl 路径>'
```

请求 ID 用于关联单次请求，不是认证或完整分布式追踪。手动访问 `/stream` 时，自动演示
使用的尾段事件已释放，因此会立即完整返回；提前交付的证明来自自动步骤的受控事件。

## 退出与边界

Ctrl+C、SIGTERM、启动失败和网络/事件超时均进入资源清理；先向本轮网关进程发送
SIGTERM，最多等待 5 秒，再对该 PID 强停并等待回收。构建中断时只终止本轮构建的独立
进程组。释放后端尾段事件、停止监听线程并关闭 socket、日志和管道；删除本轮临时目录。
不按进程名批量终止。日志仅供运行期间查看，退出后删除，请先完成手动查询。

这是本地功能演示，不覆盖所有协议、负载或异常组合，不构成生产就绪或长期兼容承诺。
响应仍受 1 MiB 总上限和原有总期限约束，不支持无限流、SSE 或流式上传。
`/metrics` 无认证；演示仅绑定回环地址，生产监听器的访问范围由用户配置决定。

专项配置和既有验证方式见根 README：

- [下游 TLS](../../README.md#可选下游-tlshttps)及[HTTPS 上游](../../README.md#https-上游)。
- [路由 API Key 与分阶段轮换](../../README.md#可选路由级-api-key-认证)：要求下游 TLS，启动时加载，轮换需重启，无零停机承诺。
- [主动健康检查与摘除恢复](../../README.md#主动健康检查与故障摘除)。
- [流式响应](../../README.md#可选流式响应)、[连接池](../../README.md#可选上游连接池)、[指标](../../README.md#请求指标与运行状态)、[设计边界](../../设计文档.md)。

## 演示自身的回归

CTest `integration.demo_smoke` 使用 CMake 传入的已构建可执行文件，不再构建。
包含 smoke、手动保留服务、SIGINT/SIGTERM、启动失败及启动超时清理、仓库外工作目录、继承环境隔离、
非法参数和缺少 Python 的诊断。总超时 60 秒；失败为非零退出，不改变 sanitizer 设置。

```bash
ctest --test-dir build/linux-analysis -R '^integration.demo_smoke$' --output-on-failure
```
