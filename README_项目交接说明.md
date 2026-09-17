# ESP32-C3 蓝牙开关 — 项目交接说明

> 迁移日期：2026-09-17
> 原路径：`C:\Users\mike\Desktop\cheji\BLEDD\123\helloword`
> 现路径：`C:\Users\mike\Desktop\cheji\BLEDD\esp32_c3_bledd`
> 目的：开启新工作区，保留全部对话记录与工程成果

---

## 一、项目是什么

基于 **ESP32-C3**（RISC-V，仅支持 BLE）的 **手机蓝牙开关**：手机通过 BLE 发送指令，芯片翻转 GPIO 电平，进而控制继电器 / LED。

- 芯片：ESP32-C3（chip revision v0.4）
- 框架：ESP-IDF v6.1-beta1
- 协议栈：NimBLE（Peripheral / GATT Server）
- **状态：✅ 已完整跑通并实测验证**

---

## 二、当前配置快照

| 项 | 值 | 位置 |
|---|---|---|
| 蓝牙设备名 | `C3_BLE_01` | `main/main.c:25` |
| 服务 UUID | `0000F000-0000-1000-8000-00805F9B34FB` | `main/main.c` → `gatt_svcs[]` |
| 控制特征 UUID | `0000F001-0000-1000-8000-00805F9B34FB` | 同上 |
| 特征属性 | READ \| WRITE \| NOTIFY | 同上 |
| 控制引脚 | **GPIO3** | `main/main.c:28` |
| 触发方式 | **高电平触发** | `SWITCH_ON_LEVEL=1` / `SWITCH_OFF_LEVEL=0` |
| ON 输出 | **3.3 V** | — |
| OFF 输出 | **0 V** | — |

**控制指令**

| 动作 | 文本 | HEX |
|---|---|---|
| 开 | `1` 或 `on` | `31` 或 `6F6E` |
| 关 | `0` 或 `off` | `30` 或 `6F6666` |

> ⚠️ 若后续改接**低电平触发**继电器模块，必须把 `SWITCH_ON_LEVEL` 改回 `0`、`SWITCH_OFF_LEVEL` 改回 `1`，否则开关逻辑会完全反转。

---

## 三、踩过的坑（务必不要再犯）

这些是本项目调试过程中**真正卡住过**的问题，每条都曾消耗大量时间：

### 1. 广播时长参数必须传 `BLE_HS_FOREVER`
```c
ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                  ble_gap_event_handler, NULL);
```
- 传 `0` 会被 NimBLE 当成"广播 0 毫秒后停止"，芯片打印 `advertising...` 但 RF 空中**几乎不存在**，手机永远搜不到。
- `BLE_HS_FOREVER = INT32_MAX`（定义在 `host/ble_hs.h`）。
- **这是"串口说在广播、手机却搜不到"的真正根因**（曾误诊为地址未注册）。

### 2. GATT 服务必须在 `nimble_port_freertos_init` 之前注册
```c
// 正确位置：ble_init() 中，主机任务启动之前
ble_svc_gap_init();
ble_svc_gatt_init();
ble_gatts_count_cfg(gatt_svcs);
ble_gatts_add_svcs(gatt_svcs);
nimble_port_freertos_init(ble_host_task);   // ← 之后不能再注册
```
- 放在 `ble_on_sync`（sync 回调）里注册，`add_svcs` 会返回 0 但**服务不激活**，`gatts_register_cb` 不触发 → 手机能连上但 Discover 不到任何服务 → 连接后立刻断开（表现为反复 `connected → disconnected`）。
- 官方 `bleprph` 示例在 `main.c` 的 `gatt_svr_init()` 中、于 `nimble_port_freertos_init` 之前完成注册。

### 3. NimBLE 主机初始化顺序（v6.1）
- `nimble_port_init()` **内部已包含** `esp_bt_controller_init` + `enable`。
- 手动再调一次会导致 `ESP_ERR_INVALID_STATE (0x103)` 并 abort。
- 正确顺序：`nvs_flash_init()` → `nimble_port_init()` → 设置 `ble_hs_cfg` 回调 → `nimble_port_freertos_init()`。

### 4. `ble_hs_util_ensure_addr` 的头文件
- 声明在 **`host/util/util.h`**，不在 `host/ble_hs.h`。
- 需要显式 `#include "host/util/util.h"`。
- `ble_hs_id_infer_auto` 则在 `host/ble_hs_id.h`，已被 `ble_hs.h` 包含，无需额外 include。

### 5. 地址类型：Random Static，不要依赖 MAC
- 日志中控制器打印的 `Bluetooth MAC: b4:3a:45:57:a1:9e` 是 **Public 地址**。
- 实际广播用的是 **Random Static 地址**，`own_addr_type=1`，手机看到的 MAC 与前者不同，且**重烧固件后可能变化**。
- **App 必须按设备名 `C3_BLE_01` 或 服务 UUID 识别设备，绝不能用 MAC。**

### 6. Brownout 阈值
- 曾误设为 3.27 V，RF 发射瞬间电流导致电压跌落 → 反复重启。
- 已改回默认的 **2.51 V**。位置：menuconfig → Brownout Detector。

### 7. `adv_channel_map=0` 不是 bug
- NimBLE 在 `ble_gap.c` 中会把 `chan_map==0` 自动替换为默认 `0x07`（37/38/39 全开）。日志显示 0 属于正常占位。

---

## 四、手机端调试要点

使用 **BLE 调试助手**（安卓）时：
- **「发送特征」下拉框必须选到 `0000f001-...`**。
- 下拉里会混有标准特征 `0x2B29`（Client Supported Features，位于 Generic Attribute 服务，**只支持读**），误选后写入一律失败（App 界面"失败"计数 +1），芯片端**完全无反应**。
- HEX 模式下推荐只发**单字节**：`31` = 开，`30` = 关（即字符 `'1'`/`'0'` 的 ASCII），不会触发格式校验。
- 判断链路是否正常：对 `F001` 执行 **Read**，固定返回 `hello`。读到即证明链路/特征/回调全部正常。

---

## 五、编译与烧录

### 当前下载方式：USB（USB Serial/JTAG）
- 占用 **GPIO18(D-) / GPIO19(D+)**，这两个脚不能再作普通 GPIO。
- sdkconfig 现状：主 console = **UART0**（GPIO20/21，115200）；secondary console = **USB Serial/JTAG**。
- 因此 `printf` 仍从 UART0 输出，串口监视器接法不变。

### 命令
```bash
# 1) 进入环境
idf_env.bat

# 2) 编译（迁移后首次必须完整重建，见下方注意事项）
idf.py build

# 3) 烧录 + 监视（COMx 替换为实际端口）
idf.py -p COMx flash monitor
```

### ⚠️ 迁移后首次编译注意
`build/` 目录**未随迁移复制**（内含硬编码的旧绝对路径，迁移后必然失效）。
在新目录直接 `idf.py build` 会**自动生成新的 build 目录**，属正常流程，无需手工干预。
切勿把旧 `build/` 拷回来，会导致 CMake 缓存路径错乱。

### 手动下载模式（若自动复位失效）
1. 按住 GPIO9 / BOOT
2. 点一下 EN
3. 松开 BOOT
4. 执行：
```bash
cd build
"C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe" -m esptool --chip esp32c3 -p COMx -b 115200 --before no-reset --after no-reset write-flash @flash_args
```

---

## 六、正常运行的串口日志（对照用）

```
=== Hello ESP32-C3 ===
chip revision : v0.4
reset reason  : POWERON
BLE step: nvs_flash_init
BLE step: nvs ok
BLE step: nimble_port_init
I (781) BLE_INIT: Bluetooth MAC: b4:3a:45:57:a1:9e
BLE step: controller + host ready
BLE step: nimble_port_freertos_init
registered service 0x1800 with handle=1
registered service 0x1801 with handle=6
registered service 0xf000 with handle=14
registered characteristic 0xf001 with def_handle=15 val_handle=16
BLE ready, name=C3_BLE_01, advertising...
BLE step: init done
BLE connected, conn_handle=1
BLE recv 1 bytes: 1
BLE switch: ON
BLE recv 1 bytes: 0
BLE switch: OFF
```

**关键判据**：必须出现 `registered service 0xf000` 与 `registered characteristic 0xf001`。缺这两行说明 GATT 未注册（回到第三节第 2 条）。

---

## 七、待办与改进建议

| # | 事项 | 说明 |
|---|---|---|
| 1 | **App 开发** | 完整对接文档见 **`BLE_App开发对接文档.md`**（含 Android Kotlin / iOS Swift / Web Bluetooth 代码） |
| 2 | Read 返回状态 | 当前 Read `F001` 固定返回 `hello`，**不返回开关状态** → App 无法知道当前是开是关。需改 READ 分支返回 `"ON"`/`"OFF"` |
| 3 | 状态实时回传 | 当前 Notify 仅在订阅当刻发一次 `connected`。需在 WRITE 分支电平变化后主动 `ble_gatts_notify_custom` |
| 4 | 安全性 | 当前 `SM_LVL=0`（开放，无配对），任何手机都能控制。量产前需提高安全等级 |
| 5 | 量产优化 | 广播间隔 30~60ms 功耗偏高；设备名可加入唯一后缀避免多设备同名 |

---

## 八、对话记录位置

历史对话与决策记录保存在：
```
.workbuddy/memory/
├── 2026-09-05.md
├── 2026-09-06.md
└── 2026-09-07.md
```
新工作区会自动读取这些文件，可直接追问历史细节。

---

*本文档由迁移流程自动生成，用于新工作区无缝接续。若配置发生变更，请同步更新本文档与 `BLE_App开发对接文档.md`。*
