# ESP32-C3 蓝牙开关固件

基于 **ESP32-C3**（RISC-V，仅支持 BLE）的手机蓝牙开关：手机通过 BLE 发送指令，芯片翻转 **GPIO3** 电平，进而控制继电器 / LED。固件已实测跑通。

> 仓库地址：https://github.com/meikehao/esp32_c3_bledd

---

## 特性

- **协议栈**：NimBLE（ESP-IDF v6.1-beta1），GATT Server / Peripheral 角色
- **控制特征**：`0xF001`，支持 Read / Write / Notify
- **指令简洁**：写 `1`（开）或 `0`（关）即可翻转 GPIO3
- **状态可读**：Read `0xF001` 返回当前实际电平对应的 `ON` / `OFF`（断电重启后依然准确）
- **状态回传**：开关电平变化后主动 Notify 当前状态（`ON` / `OFF`）；未知指令回传 `ERR`
- **零配对**：安全等级 0，手机直连即可控制（量产前建议提高，见下文）

---

## 硬件与接线

| 项 | 值 |
|---|---|
| 芯片 | ESP32-C3（chip revision v0.4） |
| 控制引脚 | **GPIO3** |
| 触发方式 | **高电平触发**（`SWITCH_ON_LEVEL = 1`） |
| ON 输出 | **3.3 V** |
| OFF 输出 | **0 V** |
| 上电默认 | OFF（0 V），继电器不吸合 |
| IO 电平标准 | 3.3 V（**非 5 V tolerant**） |

**继电器模块接线**

- `IN` → GPIO3
- `VCC` / `GND` → 独立电源（≥ 100 mA 余量）
- **必须与 ESP32-C3 共地（GND 相连）**
- GPIO3 只能提供信号（单脚约 20 mA），**禁止直接驱动继电器线圈、电机、灯带**；控制 220 V 市电须做好强电隔离与绝缘。

> ⚠️ **低电平触发模块**：若你的继电器是蓝色低电平触发模块，需把 `main.c` 中的 `SWITCH_ON_LEVEL` 改为 `0`、`SWITCH_OFF_LEVEL` 改为 `1`，否则开/关逻辑完全反转。App 侧指令不变（`31` 始终=开，`30` 始终=关）。

---

## BLE 协议

### 设备标识

| 项目 | 值 |
|---|---|
| 广播名 | `C3_BLE_01` |
| 服务 UUID | `0000F000-0000-1000-8000-00805F9B34FB`（简写 `0xF000`） |
| 控制特征 UUID | `0000F001-0000-1000-8000-00805F9B34FB`（简写 `0xF001`） |
| 特征属性 | READ + WRITE + NOTIFY |
| 写入类型 | **Write Request（带响应）**，不支持 Write Without Response |
| 配对 | 无（SM_LVL = 0） |
| 最大连接数 | 3 |
| ATT MTU 上限 | 256 |

> **地址说明**：固件使用 Random Static 地址广播，手机看到的 MAC 与芯片 Public MAC 不同，且重烧固件后可能变化。**App 必须按设备名 `C3_BLE_01` 或 服务 UUID `0xF000` 识别设备，切勿依赖 MAC。**

### 指令集（写入 `0xF001`）

| 动作 | 文本 | HEX | GPIO3 输出 |
|---|---|---|---|
| 开 | `1` 或 `on` | `31` 或 `6F6E` | 3.3 V |
| 关 | `0` 或 `off` | `30` 或 `6F6666` | 0 V |

匹配规则（区分大小写，前缀匹配）：

```c
if      (strncmp(rx, "on", 2) == 0 || strncmp(rx, "1", 1) == 0) → ON
else if (strncmp(rx, "off", 3) == 0 || strncmp(rx, "0", 1) == 0) → OFF
else                                                          → 未知指令
```

### 读取（Read `0xF001`）

返回当前开关状态：**`ON`** 或 **`OFF`**（与 GPIO3 实际电平一致）。

> 可作为连通性自检：能读到 `ON`/`OFF` 即证明链路、特征、回调均正常。

### 通知（Notify `0xF001`）

| 触发时机 | 内容 |
|---|---|
| App 使能 Notify 当刻 | `connected`（9 字节，连接确认） |
| 开关电平变化后 | `ON` / `OFF`（状态实时回传） |
| 收到未知指令 | `ERR`（告知指令被拒） |

---

## 构建与烧录

### 环境

- ESP-IDF v6.1-beta1（本仓库含 `idf_env.bat` 一键进入环境）
- 下载方式：USB Serial/JTAG（占用 GPIO18 / GPIO19，不可作普通 GPIO）
- 调试串口：UART0（GPIO20/21，115200）

### 命令

```bash
# 1) 进入 IDF 环境
idf_env.bat

# 2) 编译
idf.py build

# 3) 烧录 + 监视（COMx 替换为实际端口）
idf.py -p COMx flash monitor
```

> **迁移后首次编译**：`build/` 未随工程复制，直接 `idf.py build` 会自动生成新的 `build/` 目录，无需手动干预；切勿把旧 `build/` 拷回（含硬编码旧路径，会导致缓存错乱）。

### 手动下载（自动复位失效时）

1. 按住 GPIO9 / BOOT → 点一下 EN → 松开 BOOT
2. 执行：

```bash
cd build
"C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe" -m esptool \
  --chip esp32c3 -p COMx -b 115200 --before no-reset --after no-reset write_flash @flash_args
```

---

## 验证（用 BLE 调试助手，如 nRF Connect / LightBlue）

- **Read `0xF001`** → 返回 `ON` 或 `OFF`
- **写 `31`** → 串口 `BLE switch: ON`，收到 `ON` 通知，GPIO3 = 3.3 V
- **写 `30`** → 串口 `BLE switch: OFF`，收到 `OFF` 通知，GPIO3 = 0 V
- **写非法值**（如 `x`）→ 串口 `unknown cmd`，收到 `ERR` 通知

### 正常串口日志（关键判据）

```
BLE ready, name=C3_BLE_01, advertising...
registered service 0xf000 with handle=14
registered characteristic 0xf001 with def_handle=15 val_handle=16
BLE connected, conn_handle=1
BLE recv 1 bytes: 1
BLE switch: ON
BLE recv 1 bytes: 0
BLE switch: OFF
```

必须出现 `registered service 0xf000` 与 `registered characteristic 0xf001`；缺这两行说明 GATT 未注册。

---

## 目录结构

```
.
├── main/
│   ├── main.c             # 固件主程序（BLE + GPIO 控制）
│   └── CMakeLists.txt
├── CMakeLists.txt
├── sdkconfig              # ESP-IDF 工程配置
├── idf_env.bat            # 一键进入 IDF 环境（Windows）
├── .devcontainer/         # VS Code + ESP-IDF 容器配置
├── use/                   # 参考固件与工具配置（二进制已 gitignore）
├── README.md              # 本文档
├── README_项目交接说明.md  # 工程总览、踩坑记录、日志判据
└── BLE_App开发对接文档.md  # App（Android/iOS/Web）对接规范
```

---

## 已知限制与改进方向

| # | 限制 | 建议 |
|---|---|---|
| 1 | 安全等级 0（开放，无配对） | 量产前提高 `CONFIG_BT_NIMBLE_SM_LVL` 到 2~3，启用 Passkey / Numeric Comparison |
| 2 | 设备名固定 `C3_BLE_01` | 多设备场景加入 MAC 后几位做唯一化后缀 |
| 3 | 广播间隔 30~60 ms（功耗偏高） | 电池供电改为 100 ms ~ 1 s |
| 4 | 单引脚单路开关 | 扩展特征（如 `0xF002`）或改用 `pin,level` 指令格式 |
| 5 | Random Static 地址 | 如需固定标识，改用 Public 地址或在广播包携带自定义 ID |

---

## 相关文档

- **`README_项目交接说明.md`** — 工程迁移说明、配置快照、调试踩坑、完整日志判据
- **`BLE_App开发对接文档.md`** — 给 App 开发者的对接规范（UUID / 指令集 / Android Kotlin、iOS Swift、Web Bluetooth 代码 / 时序 / 排错）

---

*固件基于 ESP-IDF v6.1-beta1 + NimBLE，已实测通过。如修改 `main.c` 中的 UUID、引脚或指令集，请同步更新本文档与相关对接文档。*
