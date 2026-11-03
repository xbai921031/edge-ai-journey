# ESP32-ClawBot Software Architecture

## 1. 项目概述

ESP32-ClawBot 是一个运行在 ESP32-S3 上的 AI 聊天机器人固件，支持接入微信（iLink）、QQ、飞书、Telegram 等即时通讯平台，通过 OpenAI 兼容 API 调用大语言模型（如 DeepSeek、GPT、Qwen 等），具备工具调用（Function Calling）、事件路由、长短期记忆等能力。

- **目标硬件**: ESP32-S3 + 16MB Flash + PSRAM
- **开发框架**: ESP-IDF v6.0.1
- **语言**: C（核心）、TypeScript/TSX（Web 前端）

---

## 2. 目录结构

```
esp32-ClawBot/
├── CMakeLists.txt                    # 根项目定义
├── partitions_16MB.csv               # 16MB Flash 分区表
├── sdkconfig.defaults                # 默认 Kconfig 配置
│
├── main/                             # 应用入口
│   ├── main.c                        # app_main() 入口
│   ├── app_fs.c / app_fs.h           # 文件系统初始化（FATFS + RAMFS）
│   ├── http_server_adapter.c / .h    # HTTP 服务适配层
│   ├── edge_agent_version.c / .h     # 版本信息
│   ├── lv_conf.h                     # LVGL 配置
│   ├── Kconfig.projbuild             # 项目级 Kconfig 选项
│   ├── lcd/                          # LCD 驱动（预留）
│   └── os/                           # OS 抽象层（预留）
│
├── components/
│   ├── app_config/                   # 持久化配置管理（NVS）
│   ├── app_claw/                     # 应用编排层
│   ├── claw_core/                    # 核心 Agent 引擎
│   ├── claw_cap/                     # 能力注册与执行框架
│   ├── claw_event_router/            # 事件路由引擎
│   ├── claw_manager/                 # Agent 生命周期与会话管理
│   ├── claw_memory/                  # 长期记忆子系统
│   ├── claw_skill/                   # 技能系统（Lua）
│   ├── settings/                     # NVS 键值存储封装
│   ├── http_reuse/                   # HTTP 连接复用池
│   ├── official_http_server/         # Web 管理后台（前端 + REST API）
│   ├── cap_im_platform/              # IM 平台集成（微信/QQ/飞书/TG）
│   ├── cap_im_local/                 # 本地 IM 能力存根
│   ├── cap_llm_config/               # LLM 配置能力
│   ├── claw_ramfs/                   # 内存文件系统
│   ├── claw_utils/                   # 通用工具函数
│   └── lvgl/                         # LVGL 图形库（第三方）
│
└── tester/                           # 测试工具
    └── index.html
```

---

## 3. Flash 分区布局

```
partitions_16MB.csv:
┌──────────┬──────┬──────────┬──────────┬──────┐
│ Name     │ Type │ SubType  │ Offset   │ Size │
├──────────┼──────┼──────────┼──────────┼──────┤
│ nvs      │ data │ nvs      │ 0xA000   │ 24KB │  ← 配置存储 (WiFi/LLM/微信)
│ otadata  │ data │ ota      │ 0x10000  │ 8KB  │  ← OTA 数据
│ phy_init │ data │ phy      │ 0x12000  │ 4KB  │  ← PHY 初始化
│ ota_0    │ app  │ ota_0    │ auto     │ 4MB  │  ← 固件分区
│ system   │ data │ fat      │ auto     │ 1MB  │  ← 系统 FATFS
│ storage  │ data │ fat      │ auto     │ 3MB  │  ← 存储 FATFS（会话/记忆）
└──────────┴──────┴──────────┴──────────┴──────┘
```

---

## 4. 核心组件详解

### 4.1 main/ — 应用入口

**`main.c`** — `app_main()` 执行流程：

1. `nvs_flash_init()` — 初始化 NVS
2. `app_config_init()` — 初始化配置存储
3. `init_wifi()` — 启动 Wi-Fi（STA + SoftAP 双模）
4. `app_fs_init()` — 挂载文件系统（system FATFS + storage FATFS + RAMFS）
5. `http_server_init()` + `http_server_start()` — 启动 Web 管理后台
6. `app_claw_start()` — 启动 AI Agent

Wi-Fi 策略：
- 有已保存凭据 → STA 连接路由器 + SoftAP 备份
- 无凭据 → 纯 SoftAP 模式（热点名 `ESP-ClawBot`），浏览器访问 `192.168.4.1` 配置

**`app_fs.c`** — 三层文件系统：
| 挂载点 | 类型 | 大小 | 用途 |
|---|---|---|---|
| `/system` | FATFS (SPI Flash) | 1MB | 系统文件 |
| `/fatfs` | FATFS (SPI Flash) | 3MB | 会话历史、长期记忆、路由规则 |
| `/ramfs` | RAMFS | 32KB | 临时数据，重启丢失 |

**`http_server_adapter.c`** — 将 `app_config` 和 `cap_im_wechat` 的接口适配到官方 HTTP 服务器的服务回调结构，包括：
- 配置读写（WiFi / LLM / 微信）
- Wi-Fi 状态查询
- 微信扫码登录控制
- 设备重启

---

### 4.2 app_config — 持久化配置

**存储方式**：基于 `settings_store`（封装 ESP-IDF NVS），Namespace 为 `"app"`。

**配置字段**（通过 HTTP 管理后台或 CLI 修改）：

| 分类 | 字段 | NVS Key |
|---|---|---|
| Wi-Fi | SSID、密码、AP 行为 | `wifi_ssid`, `wifi_password`, `ap_*` |
| LLM | API Key、Backend、Model、Base URL、Auth Type | `llm_api_key`, `llm_backend`, `llm_model`, `llm_base_url` |
| LLM | Timeout、Max Tokens、Supports Tools/Vision | `llm_timeout_ms`, `llm_max_tokens`, `llm_sup_tools` |
| 微信 | Token、Base URL、CDN URL、Account ID | `wechat_token`, `wechat_base_url`, … |
| QQ | App ID、Secret | `qq_app_id`, `qq_app_secret` |
| 飞书 | App ID、Secret | `feishu_app_id`, `feishu_secret` |
| Telegram | Bot Token | `tg_bot_token` |
| 搜索 | Brave Key、Tavily Key | `brave_key`, `tavily_key` |
| 能力 | 启用/可见的能力组 | `en_cap_groups`, `vis_cap_groups` |

**关键行为**：
- 首次启动所有值为空，需通过 Web 界面配置
- 配置保存在 NVS 分区，重新烧录固件（不擦除 Flash）不会丢失
- 支持旧版 LLM 预设（openai / deepseek / qwen / anthropic）自动迁移到新的 `backend_type` 格式

---

### 4.3 app_claw — 应用编排层

**职责**：作为配置（`app_config`）与核心引擎（`claw_core`）之间的桥梁。

**核心流程** `app_claw_start()`：

```
app_claw_config_t ──→ claw_core_config_t
                         │
           ┌─────────────┼─────────────┐
           ▼             ▼             ▼
    claw_memory    claw_event_router  claw_agent_mgr
    (长期记忆)      (事件路由)        (Agent 管理)
           │             │             │
           └─────────────┼─────────────┘
                         ▼
                  claw_core_create()
                         │
              注册 Context Providers:
              · 系统 Prompt
              · 会话历史 (claw_memory)
              · 长期记忆 (claw_memory)
              · 工具列表 (claw_cap)
                         │
              注册 Completion Observer:
              · 记忆提取
                         │
                  claw_core_start()
```

**优化参数**：
- `max_tool_iterations = 10`（每轮请求最多 10 次工具调用）
- `task_stack_size = 10KB`（Agent 循环任务栈）

---

### 4.4 claw_core — 核心 Agent 引擎

系统最核心的组件，管理 LLM 交互的完整生命周期。

#### 4.4.1 关键数据结构

```
claw_core_config_t:
├── LLM 连接参数 (api_key, model, base_url, auth_type, timeout_ms, max_tokens)
├── 功能开关 (supports_tools, supports_vision)
├── 钩子函数 (persist_context, request_gate, call_cap, completion_observer)
├── 系统 Prompt
├── 任务参数 (stack_size, priority, core_affinity)
└── 运行时参数 (max_tool_iterations, queue_lengths)

claw_core_request_t:
├── request_id, flags, session_id
├── user_text (用户输入)
└── 来源信息 (channel, chat_id, sender_id, message_id)
```

#### 4.4.2 Agent 循环 (Agent Loop)

```
┌─────────────────────────────────────────────────────┐
│                  Agent Loop Task                     │
├─────────────────────────────────────────────────────┤
│  1. IDLE: 等待 request_queue 中的请求                 │
│  2. BEFORE_BUILD_ITERATION_CONTEXT: 发布阶段通知        │
│  3. BUILDING_ITERATION_CONTEXT:                      │
│     · 收集 system_prompt (来自 context_providers)      │
│     · 加载历史 messages (来自 claw_memory)             │
│     · 构建 tools_json (来自 claw_cap)                 │
│     · 合并 runtime_messages + user_input              │
│  4. BEFORE_LLM_HTTP: 准备 HTTP 请求                   │
│  5. IN_LLM_HTTP: 发送 HTTP POST 到 LLM API            │
│     · 后端适配 (openai_compatible / anthropic)        │
│     · SSE 流式响应解析                                │
│  6. AFTER_LLM_BEFORE_TOOL: 解析 LLM 响应              │
│     · 如果有 tool_calls → 进入 7                      │
│     · 如果 final_answer → 进入 8                      │
│  7. RUNNING_TOOL: 执行工具调用                         │
│     · 通过 call_cap 回调调用 claw_cap                  │
│     · 将工具结果追加到 messages                        │
│     · 检查迭代次数 → 回到 3 或进入 8                   │
│  8. FINALIZING:                                      │
│     · 持久化上下文 (persist_context)                   │
│     · 发布 out_message 事件 (含最终回复)               │
│     · 通知 completion_observer                        │
│     · 将响应放入 response_queue                        │
└─────────────────────────────────────────────────────┘
```

#### 4.4.3 LLM 后端

位于 `claw_core/src/llm/backends/`，支持三种后端：

| 后端 | 文件 | 说明 |
|---|---|---|
| `openai_compatible` | `claw_llm_backend_openai_compatible.c` | OpenAI API 兼容（GPT/DeepSeek/Qwen 等） |
| `anthropic_compatible` | `claw_llm_backend_anthropic.c` | Anthropic API 兼容（Claude） |
| `custom` | `claw_llm_backend_custom.c` | 自定义后端 |

每个后端实现：
- `build_chat_body()` — 构建请求 JSON
- `parse_chat_response()` — 解析响应（含 tool_calls、streaming）

#### 4.4.4 HTTP 传输

`claw_llm_http_transport.c` — HTTP 请求传输层：
- 使用 `http_reuse` 连接池（减少 TLS 握手）
- 响应缓冲区初始大小 2KB，按需翻倍增长
- SSE 流式解析支持

---

### 4.5 claw_cap — 能力注册与执行框架

**概念模型**：

```
Capability Group (能力组)
  └── Capability Descriptor (能力描述符) × N
        ├── id, name, family, description
        ├── kind: CALLABLE / EVENT_SOURCE / HYBRID
        ├── cap_flags: CALLABLE_BY_LLM / EMITS_EVENTS / ...
        ├── input_schema_json (JSON Schema)
        ├── lifecycle: init() / start() / stop()
        └── execute(input_json, ctx, output) → esp_err_t
```

**能力调用链**：
```
LLM 返回 tool_call → claw_core → call_cap 回调 → claw_cap_call()
    → 查找 descriptor → execute() → 结果返回 LLM
```

**当前注册的能力组**：
- `cap_im_wechat` — 微信收发消息
- `claw_memory` — 长期记忆管理
- `event_router` — 事件路由规则管理
- 其他可扩展能力

**LLM 工具列表生成**：`claw_cap_build_llm_tools_json()` 遍历所有标记为 `CALLABLE_BY_LLM` 的 descriptor，生成 OpenAI Function Calling 格式的 JSON。

---

### 4.6 claw_event_router — 事件路由引擎

基于规则的事件路由系统，处理系统内部事件的流转。

**事件流**：
```
Event Source → Event Router → Rule Match → Action(s)
    │              │              │            │
    │              │              │            ├── CALL_CAP: 调用能力
    │              │              │            ├── RUN_AGENT: 启动 Agent
    │              │              │            ├── SEND_MESSAGE: 发送 IM 消息
    │              │              │            ├── EMIT_EVENT: 发射新事件
    │              │              │            └── DROP: 丢弃
    │              │              │
    │              │              └── Match 条件:
    │              │                  event_type, event_key, source_cap,
    │              │                  channel, chat_id, text (exact/prefix)
    │              │
    │              └── 默认规则: out_message → send_message
    │                  (将 claw_core 输出转发到 IM 平台)
    │
    └── 来源: claw_core, cap_im_wechat, etc.
```

**默认路由规则**（`/fatfs/router_rules/router_rules.json`）：
```json
{
  "id": "default_agent_response",
  "match": { "event_type": "out_message", "source_cap": "claw_core" },
  "actions": [{ "type": "send_message", ... }]
}
```

**关键参数**：
- 规则文件持久化在 storage FATFS 分区
- 支持运行时动态增删改规则
- 规则匹配后可选 `consume_on_match`（停止继续匹配）

---

### 4.7 claw_manager — Agent 生命周期管理

**Agent 类型**：
- **Root Agent** (`"0"`): 主 Agent，处理用户直接输入
- **Sub Agent**: 由工具调用动态创建的子 Agent（背景任务）

**会话策略** (`claw_session_policy_t`)：

| 策略 | 说明 |
|---|---|
| `CHAT` | 标准聊天会话，持久化历史 |
| `TRIGGER` | 事件触发，短暂上下文 |
| `GLOBAL` | 全局会话 |
| `EPHEMERAL` | 临时会话，不保存 |
| `NOSAVE` | 不保存历史 |

**会话 ID 生成**：
```
格式: chat:{channel}:{chat_id}@{platform}
示例: chat:wechat:o9cq80xhXKUb@im.wechat
```

**主要接口**：
- `claw_agent_mgr_submit_root_text()` — 提交用户文本到 Root Agent
- `claw_agent_mgr_spawn_subagent()` — 创建子 Agent
- `claw_agent_mgr_receive_root_for()` — 等待 Agent 响应

---

### 4.8 claw_memory — 长期记忆子系统

**核心功能**：

1. **上下文持久化** (`persist_context_callback`)
   - 将每轮对话的 user/assistant/tool 消息写入 FATFS
   - 索引文件 `.idx` + 数据文件 `.log`

2. **历史加载** (`session_history_provider`)
   - 下一轮对话时加载历史消息到 LLM Context

3. **记忆提取** (Async Extract)
   - 对话完成后异步调用 LLM 提取关键记忆
   - 存储结构化记忆条目（含标签、关键词、摘要）

4. **记忆查询** (`claw_memory_recall`)
   - 根据标签/关键词检索相关记忆
   - 将记忆注入到 LLM Context 的 System Prompt

5. **请求门控** (`request_gate_callback`)
   - 在 Agent 处理请求前检查是否需要过滤/拦截

**存储结构**：
```
/fatfs/sessions/
  └── s_chat_wechat_{user_id}_{hash}/
      ├── .idx    # 索引文件
      └── .log    # 数据文件
/fatfs/memory/
  └── (结构化记忆条目)
```

---

### 4.9 其他组件

| 组件 | 功能 |
|---|---|
| `settings` | NVS 键值存储封装，支持 string 读写/删除/提交 |
| `http_reuse` | HTTP 连接复用池，空闲连接 5 秒自动回收，减少 TLS 握手开销 |
| `official_http_server` | 内嵌 Web 服务器（SPIFFS 前端 + REST API），提供 Wi-Fi/LLM/微信配置界面 |
| `claw_skill` | Lua 脚本技能系统，支持动态加载/执行 |
| `claw_ramfs` | 基于 `esp_vfs` 的纯内存文件系统（最大 4 文件、32KB） |
| `claw_utils` | 通用工具（JSON 解析、字符串处理、日志等） |
| `lvgl` | 轻量级图形库（LVGL v8/v9），用于 LCD 显示 |
| `cap_im_platform` | IM 平台实现（微信 iLink 轮询、QQ/飞书/TG API） |
| `cap_im_local` | 本地 IM 能力接口定义 |
| `cap_llm_config` | LLM 配置管理能力 |

---

## 5. 完整工作流

### 5.1 启动流程

```
上电
 │
 ├─ 1. nvs_flash_init()
 │      └─ 初始化 NVS 分区
 │
 ├─ 2. app_config_init()
 │      └─ 初始化 "app" namespace，执行旧配置迁移
 │
 ├─ 3. esp_netif_init() + esp_event_loop_create_default()
 │
 ├─ 4. init_wifi()
 │      ├─ 有凭据: STA 连接路由器 + SoftAP 备份
 │      └─ 无凭据: 仅 SoftAP (SSID: ESP-ClawBot)
 │
 ├─ 5. vTaskDelay(6000)  — 等待 Wi-Fi 连接
 │
 ├─ 6. app_fs_init()
 │      ├─ 挂载 /system (FATFS, 1MB)
 │      ├─ 挂载 /fatfs  (FATFS, 3MB)
 │      └─ 挂载 /ramfs  (RAMFS, 32KB)
 │
 ├─ 7. http_server_init() + http_server_start()
 │      └─ 启动 Web 管理后台 (端口 80)
 │
 └─ 8. app_claw_start()
        ├─ 创建 claw_memory (加载历史/记忆)
        ├─ 创建 claw_event_router (加载路由规则)
        ├─ 创建 claw_agent_mgr (创建 Root Agent)
        ├─ 注册能力组 (微信、记忆、事件路由...)
        ├─ 注册 Context Providers (prompt、历史、工具)
        └─ claw_core_start() → Agent 进入 IDLE 等待
```

### 5.2 微信消息处理流程

```
微信服务器 ──polling──→ cap_im_wechat (iLink)
                            │
                            │ 收到新消息
                            ▼
                    创建 claw_event_t
                     event_type = "in_message"
                     source_cap = "cap_im_wechat"
                     text = 用户消息内容
                            │
                            ▼
                    claw_event_router_handle_event()
                            │
                            │ 规则匹配: default_agent_route
                            │
                            ▼
                      RUN_AGENT 动作
                            │
                            ▼
                    claw_agent_mgr_submit_root_text()
                            │
                            ▼
                    claw_core_submit() → 放入 request_queue
                            │
                            ▼
                ┌── Agent Loop (见 4.4.2) ──┐
                │                            │
                │  1. 加载会话历史             │
                │  2. 构建 Context            │
                │     · system_prompt        │
                │     · messages (历史+用户)   │
                │     · tools (LLM 可调用)    │
                │  3. HTTP POST → LLM API    │
                │  4. 解析响应                │
                │     ├─ tool_call → 执行 → 返回结果 → 回到 3
                │     └─ final_answer → 进入 5
                │  5. 持久化对话历史           │
                │  6. 异步提取记忆             │
                └────────────────────────────┘
                            │
                            ▼
                    发布 out_message 事件
                     target_channel = "wechat"
                     text = LLM 回复内容
                            │
                            ▼
                    claw_event_router_handle_event()
                            │
                            │ 规则匹配: default_agent_response
                            │
                            ▼
                     SEND_MESSAGE 动作
                            │
                            ▼
                    cap_im_wechat_send_text()
                            │
                            ▼
                    微信用户收到回复
```

### 5.3 Web 配置流程

```
用户浏览器 ──HTTP──→ official_http_server
                       │
                       ├─ GET /          → Web 前端页面 (SPA)
                       ├─ GET /api/config → adapter_load_config() → app_config_load()
                       ├─ POST /api/config → adapter_save_config() → app_config_save()
                       │                    └─ 写入 NVS → 持久化
                       ├─ GET /api/wifi-status → adapter_get_wifi_status()
                       ├─ POST /api/wechat/login → adapter_wechat_login_start()
                       │                          └─ cap_im_wechat_qr_login_start()
                       │                              └─ 返回二维码 URL
                       └─ GET /api/wechat/login-status → 轮询登录状态
```

---

## 6. 关键优化记录

| 优化项 | 修改前 | 修改后 | 效果 |
|---|---|---|---|
| FreeRTOS Tick | 1000Hz | 100Hz | 减少 90% tick 中断 |
| FreeRTOS Trace | 开启 | 关闭 | 去除调试统计开销 |
| Core Stage Verbosity | Verbose | 关闭 | 消除每轮 stage 文本发布 |
| max_tool_iterations | 32 | 10 | 减少内存累积 |
| task_stack_size | 16KB | 10KB | 释放 6KB 栈内存 |
| HTTP 响应缓冲区 | 4KB | 2KB | 每次 LLM 请求节省 2KB |
| LLM max_tokens | 8192 | 4096 | 微信聊天场景足够 |

---

## 7. 依赖关系

```
app_main
  ├── nvs_flash          (ESP-IDF)
  ├── esp_netif           (ESP-IDF)
  ├── esp_wifi            (ESP-IDF)
  ├── fatfs + wear_levelling (ESP-IDF)
  ├── cJSON               (内置)
  ├── settings
  ├── app_config
  │     └── settings
  ├── app_fs
  │     ├── fatfs + wear_levelling
  │     └── claw_ramfs
  ├── http_server_adapter
  │     ├── official_http_server
  │     ├── app_config
  │     └── cap_im_wechat
  └── app_claw
        ├── app_config
        ├── claw_core
        │     ├── claw_cap
        │     ├── http_reuse
        │     └── cJSON
        ├── claw_cap
        ├── claw_event_router
        │     └── claw_cap
        ├── claw_manager
        │     ├── claw_core
        │     └── claw_session_mgr
        ├── claw_memory
        │     ├── claw_core
        │     └── claw_ramfs
        └── cap_im_platform
```

---

## 8. 构建与烧录

```bash
# 清理并构建
cd esp32-ClawBot
idf.py fullclean
idf.py build

# 烧录并查看日志
idf.py -p /dev/ttyUSB0 flash monitor

# 注意: 正常 flash 不会覆盖 NVS 分区，配置不会丢失
# 如需清除配置: idf.py erase_flash
```
