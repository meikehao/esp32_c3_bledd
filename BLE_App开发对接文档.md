# ESP32-C3 蓝牙开关 — App 开发对接文档

> 适用固件：`helloword` 工程（ESP-IDF v6.1-beta1 + NimBLE）
> 文档版本：v1.0 ｜ 整理日期：2026-09-07
> 状态：手机端 BLE 调试助手已实测通过（HEX `31`/`30` 可正常控制 GPIO3）

---

## 0. 三十秒速览（给赶时间的开发者）

| 项目 | 值 |
|---|---|
| 设备广播名 | `洁洁的哈士奇` |
| 服务 UUID | `0000F000-0000-1000-8000-00805F9B34FB`（16-bit 简写 `0xF000`） |
| 控制特征 UUID | `0000F001-0000-1000-8000-00805F9B34FB`（16-bit 简写 `0xF001`） |
| 特征属性 | **READ** + **WRITE** + **NOTIFY** |
| 开指令 | 文本 `1` 或 `on`；HEX `31` 或 `6F6E` |
| 关指令 | 文本 `0` 或 `off`；HEX `30` 或 `6F6666` |
| 写入类型 | **必须 Write Request（带响应）**，不支持 Write Without Response |
| 配对/密码 | **无**，安全等级 0，直连即可读写 |
| 控制引脚 | GPIO3（ON 时输出 **3.3V**；OFF 时输出 **0V**） |
| 触发方式 | **高电平触发**（`SWITCH_ON_LEVEL = 1`），适用于高电平触发模块 / 经限流电阻驱动 LED |

**最小可用流程**：扫描 `洁洁的哈士奇` → 连接 → 发现服务 → 对 `F001` 写 `31` → GPIO3 输出 3.3V，负载导通。

---

## 1. 设备标识

### 1.1 广播信息

| 字段 | 值 | 说明 |
|---|---|---|
| 设备名 | `洁洁的哈士奇` | 已写入广播包，扫描阶段即可直接读取，**推荐按名称过滤** |
| 广播类型 | 可连接非定向广播（ADV_IND） | 手机可直接发起连接 |
| 广播通道 | 37 / 38 / 39 全开（chan_map = 0x07） | 标准配置 |
| 广播间隔 | 30 ~ 60 ms（NimBLE 默认 FAST_INTERVAL1） | 扫描响应快，一般 1 秒内可见 |
| 广播超时 | 永久（`BLE_HS_FOREVER`） | 不会自行停止广播 |
| 发射功率 | 默认 | 空旷环境约 10 m 可用距离 |

### 1.2 蓝牙地址（重要）

- 固件使用 **Random Static Address**（`own_addr_type = 1`）进行广播。
- 因此手机看到的 MAC 地址 **不等于** 芯片控制器固化的 Public MAC。
  - 例如：控制器 Public MAC 为 `b4:3a:45:57:a1:9e`，但手机侧实际显示的是类似 `EF:7D:DC:50:A5:58` 的随机静态地址。
- **该随机地址在固件重新烧录或 NVS 清空后可能变化。**

> ⚠️ **App 开发强制要求：不要持久化或依赖 MAC 地址做设备识别。**
> 请一律使用 **设备名 `洁洁的哈士奇`** 或 **服务 UUID `0xF000`** 进行扫描过滤与重连匹配。

### 1.3 安全与配对

| 项 | 配置 | 影响 |
|---|---|---|
| 安全等级 | `CONFIG_BT_NIMBLE_SM_LVL=0` | **不要求认证、不要求加密** |
| 配对方式 | 无（No Bonding 强制） | App **无需** 处理配对弹窗、PIN 输入 |
| 最大连接数 | 3 | 同时最多 3 个中心设备连接 |
| 最大绑定数 | 3 | 当前流程未启用绑定 |

> 安全提示：当前为**开放设备**，任何手机都能连上并控制。量产前应提高安全等级（见第 9 节）。

---

## 2. GATT 服务结构

连接并完成服务发现后，设备暴露 3 个服务：

| # | 服务 UUID | 名称 | 说明 |
|---|---|---|---|
| 1 | `00001800-0000-1000-8000-00805F9B34FB` | Generic Access | BLE 标准服务（含 Device Name 等） |
| 2 | `00001801-0000-1000-8000-00805F9B34FB` | Generic Attribute | BLE 标准服务 |
| 3 | **`0000F000-0000-1000-8000-00805F9B34FB`** | **Unknown Service** | ⭐ **自定义开关服务（App 只用这个）** |

### 2.1 自定义服务 0xF000 详情

| 项 | 值 |
|---|---|
| 服务 UUID | `0000F000-0000-1000-8000-00805F9B34FB` |
| 特征 UUID | `0000F001-0000-1000-8000-00805F9B34FB` |
| 特征属性 | `READ` \| `WRITE` \| `NOTIFY` |
| CCCD UUID | `00002902-0000-1000-8000-00805F9B34FB`（NimBLE 自动添加，用于开启 Notify） |
| Attribute Handle | 服务 = 14，特征声明 = 15，特征值 = 16（仅供调试参考，**App 请勿硬编码**） |

> **16-bit 与 128-bit UUID 的关系**
> `0xF000` / `0xF001` 是 Bluetooth SIG Base UUID 的简写形式：
> `0000xxxx-0000-1000-8000-00805F9B34FB`
> 即 `0xF001` ⇔ `0000F001-0000-1000-8000-00805F9B34FB`。
> 部分平台（Android）两种写法都接受，iOS 必须用**完整 128-bit** 字符串。

### 2.2 标准服务中的"陷阱特征"

`Generic Attribute` 服务下包含 `0x2B29`（Client Supported Features）等标准特征。它们**只支持 Read，不支持 Write**。

> ⚠️ 实测踩坑：BLE 调试助手的特征下拉列表中会列出这些标准特征，若误选 `0x2B29` 写入，App 侧会直接返回失败（计数"失败 +1"），且芯片端**完全无反应**。
> **App 必须确保写入目标是 `0xF001`。**

---

## 3. 完整通信时序

```
┌─────────────┐                                    ┌──────────────┐
│   手机 App   │                                    │  ESP32-C3    │
│  (Central)  │                                    │ (Peripheral) │
└──────┬──────┘                                    └──────┬───────┘
       │                                                  │
       │  ① 扫描（过滤 name=洁洁的哈士奇）                    │
       │ ──────────────────────────────────────────────►  │
       │                                                  │  持续广播（30-60ms）
       │  ◄────── ADV_IND (含设备名 洁洁的哈士奇) ───────────│
       │                                                  │
       │  ② connectGatt()                                 │
       │ ──────────────────────────────────────────────►  │
       │  ◄──────────── 连接完成 (conn_handle=1) ─────────│
       │                                                  │
       │  ③ discoverServices()                            │
       │ ──────────────────────────────────────────────►  │
       │  ◄──── 服务列表 (1800 / 1801 / F000) ────────────│
       │                                                  │
       │  ④ [可选] requestMtu(247)                        │
       │ ──────────────────────────────────────────────►  │
       │  ◄──── MTU 协商结果（芯片侧上限 256）────────────│
       │                                                  │
       │  ⑤ [可选] 开启 Notify（写 CCCD = 0x0001）        │
       │ ──────────────────────────────────────────────►  │
       │  ◄──── 通知一条 "connected" (9 字节) ────────────│
       │                                                  │
       │  ⑥ 写 F001 = 0x31（开）                          │
       │ ──────────────────────────────────────────────►  │
       │  ◄──── Write Response (status=0 成功) ───────────│   GPIO3 → 3.3V（继电器吸合）
       │                                                  │
       │  ⑦ 写 F001 = 0x30（关）                          │
       │ ──────────────────────────────────────────────►  │
       │  ◄──── Write Response (status=0 成功) ───────────│   GPIO3 → 0V（继电器释放）
       │                                                  │
       │  ⑧ 断开                                          │
       │ ──────────────────────────────────────────────►  │
       │                                                  │   自动重新开始广播
```

### 各步骤要点

**① 扫描**
- 按设备名 `洁洁的哈士奇` 过滤最省事；也可按服务 UUID `0xF000` 过滤（更严谨）。
- Android 11 及以下**必须**开启定位权限（见第 5.1 节）。

**② 连接**
- 连接成功后芯片串口会打印 `BLE connected, conn_handle=1`（可用于调试确认）。

**③ 服务发现**
- 必须等服务发现完成（`onServicesDiscovered`）后才能读写，提前操作会失败。

**④ MTU 协商（可选但推荐）**
- 芯片侧 `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU = 256`。
- 本应用指令只有 1~3 字节，MTU 不影响功能；但若 App 有后续扩展需求，建议主动协商到 247。

**⑤ 开启 Notify（可选）**
- 使能 Notify 后，芯片会在**订阅当刻**回发一条 `connected`（9 字节 ASCII）。
- Android 必须**手动写 CCCD 描述符**才能收到通知（见第 5.2 节，这是最常见的坑）。

**⑥⑦ 写入控制指令**
- 必须使用 **Write Request（带响应）**。特征未声明 `WRITE_NO_RSP`，用 Write Without Response 会失败。

**⑧ 断开**
- 芯片检测到断开后会**自动重新开始广播**，App 可直接再次扫描连接，无需用户干预硬件。

---

## 4. 指令集

### 4.1 控制指令（写入 0xF001）

| 功能 | 文本模式 | HEX 模式 | 字节数 | GPIO3 输出 | 适用负载 |
|---|---|---|---|---|---|
| **开** | `1` | `31` | 1 | **3.3 V**（高电平） | 高电平触发模块吸合 / LED 点亮 |
| **开** | `on` | `6F 6E`（可写作 `6F6E`） | 2 | **3.3 V** | 同上 |
| **关** | `0` | `30` | 1 | **0 V**（低电平） | 模块释放 / LED 熄灭 |
| **关** | `off` | `6F 66 66`（可写作 `6F6666`） | 3 | **0 V** | 同上 |

> 当前固件为**高电平触发**（`SWITCH_ON_LEVEL = 1`）。若改回低电平触发模块，上表电平需对调，详见第 6.1 节。

**匹配规则（固件实现）**

```c
if (strncmp(rx_buf, "on", 2) == 0 || strncmp(rx_buf, "1", 1) == 0)      → ON
else if (strncmp(rx_buf, "off", 3) == 0 || strncmp(rx_buf, "0", 1) == 0) → OFF
else                                                                     → 未知指令
```

**行为细节**

1. **区分大小写**：仅小写 `on` / `off` 有效，`ON` / `Off` 被判为未知指令。
2. **前缀匹配**：只要以 `on` 或 `1` 开头即判为 ON（如 `online` 也会触发 ON）。
3. **未知指令不改变电平**：GPIO 保持原状态，仅串口打印 `unknown cmd`。
4. **长度上限**：接收缓冲 64 字节，超出部分被截断。
5. **响应**：写入成功返回 GATT status = 0（`GATT_SUCCESS`）。App 应在 `onCharacteristicWrite` 中检查该状态。

### 4.2 读取指令（读取 0xF001）

| 操作 | 返回值 | 说明 |
|---|---|---|
| Read `0xF001` | ASCII `ON` 或 `OFF`（大小写敏感） | 当前 GPIO3 实际电平对应的开关状态（断电重启后依然准确） |

> **推荐用法**：把 Read `0xF001` 当作 App 的**连通性自检 / 心跳**，同时获取真实开关状态。
> 能读出 `ON`/`OFF` 即证明链路、特征、回调全部正常。

### 4.3 通知（Notify 0xF001）

| 触发时机 | 内容 | 长度 |
|---|---|---|
| App 使能 Notify 的当刻 | ASCII `connected`（`63 6F 6E 6E 65 63 74 65 64`） | 9 字节 |
| 开关电平变化后 | ASCII `ON` 或 `OFF` | 2~3 字节 |
| 收到未知指令 | ASCII `ERR` | 3 字节 |

- 订阅当刻回发一次 `connected`（连接确认）。
- 之后 GPIO3 电平每次变化都会**主动回发** `ON`/`OFF`，实现状态实时回传；未知指令回发 `ERR` 告知指令被拒。
- 仅当对端已订阅 CCCD 时 Notify 才生效；未订阅时芯片端返回非 0，不影响开关动作本身。

---

## 5. 平台开发要点

### 5.1 Android

**权限（Manifest）**

```xml
<!-- Android 12 (API 31) 及以上 -->
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />

<!-- Android 11 (API 30) 及以下 -->
<uses-permission android:name="android.permission.BLUETOOTH"
    android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN"
    android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"
    android:maxSdkVersion="30" />

<uses-feature android:name="android.hardware.bluetooth_le" android:required="true" />
```

**运行时权限要求（必做）**

| Android 版本 | 扫描所需 | 连接所需 |
|---|---|---|
| 12+ (API 31+) | `BLUETOOTH_SCAN`（动态申请） | `BLUETOOTH_CONNECT`（动态申请） |
| 11 及以下 | `ACCESS_FINE_LOCATION`（动态申请）**+ 系统定位开关必须打开** | `BLUETOOTH` |

> ⚠️ **Android 6–11 上，仅授予权限还不够，系统「位置信息」总开关必须处于开启状态**，否则 BLE 扫描返回空列表——这是最常见的"搜不到设备"原因。

**核心代码（Kotlin）**

```kotlin
companion object {
    val SERVICE_UUID: UUID = UUID.fromString("0000f000-0000-1000-8000-00805f9b34fb")
    val CHAR_UUID:    UUID = UUID.fromString("0000f001-0000-1000-8000-00805f9b34fb")
    val CCCD_UUID:    UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    const val DEVICE_NAME = "洁洁的哈士奇"
}

// 1) 扫描（按设备名过滤）
val filter = ScanFilter.Builder().setDeviceName(DEVICE_NAME).build()
val settings = ScanSettings.Builder()
    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
    .build()
scanner.startScan(listOf(filter), settings, scanCallback)

// 2) 连接
val device = scanCallback.result.device
device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)

// 3) 服务发现完成后
override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
    val svc  = gatt.getService(SERVICE_UUID)
    val chr  = svc?.getCharacteristic(CHAR_UUID)

    // 3a) 请求 MTU（可选，芯片上限 256）
    gatt.requestMtu(247)

    // 3b) 开启 Notify —— Android 必须手动写 CCCD！
    gatt.setCharacteristicNotification(chr, true)
    val cccd = chr?.getDescriptor(CCCD_UUID)
    cccd?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
    gatt.writeDescriptor(cccd)   // 漏掉这步收不到通知
}

// 4) 发送控制指令
fun sendSwitch(on: Boolean) {
    val chr = gatt.getService(SERVICE_UUID).getCharacteristic(CHAR_UUID)
    chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT  // 必须用带响应写入
    chr.value = byteArrayOf(if (on) 0x31 else 0x30)   // '1' / '0'
    gatt.writeCharacteristic(chr)
}

// 5) 写入结果回调
override fun onCharacteristicWrite(gatt: BluetoothGatt,
                                   characteristic: BluetoothGattCharacteristic,
                                   status: Int) {
    if (status == BluetoothGatt.GATT_SUCCESS) { /* 成功 */ }
    else { /* 失败，通常为 0x03 (WRITE_NOT_PERMITTED) → 检查是否写错特征 */ }
}

// 6) 收到通知
override fun onCharacteristicChanged(gatt: BluetoothGatt,
                                     characteristic: BluetoothGattCharacteristic) {
    val text = characteristic.getStringValue(0)   // 期望 "connected"
}
```

**Android 常见坑**

| 现象 | 原因 | 解决 |
|---|---|---|
| 扫描不到任何设备 | 定位未开 / 权限未授予 | 检查系统定位开关 + 运行时权限 |
| `onCharacteristicWrite` status = 3 | 写入了不支持写的特征（如 `0x2B29`） | 确认目标是 `0xF001` |
| 写入无报错但芯片无反应 | 同上 / 指令大小写错误 | 抓包或看芯片串口打印 |
| 收不到 Notify | 漏写 CCCD 描述符 | 补 `writeDescriptor(cccd)` |
| 反复断连 | 多个 App 同时连接 / 超出 3 连接上限 | 断开其他连接 |

### 5.2 iOS

**Info.plist**

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>需要蓝牙权限以控制开关</string>
```

**核心代码（Swift）**

```swift
let serviceUUID = CBUUID(string: "0000F000-0000-1000-8000-00805F9B34FB")
let charUUID    = CBUUID(string: "0000F001-0000-1000-8000-00805F9B34FB")

// 1) 按服务 UUID 扫描（iOS 后台扫描推荐用 service filter）
centralManager.scanForPeripherals(withServices: [serviceUUID], options: nil)

// 2) 连接
centralManager.connect(peripheral, options: nil)

// 3) 发现服务
func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
    guard let svc = p.services?.first(where: { $0.uuid == serviceUUID }) else { return }
    p.discoverCharacteristics([charUUID], for: svc)
}

// 4) 开启 Notify（iOS 自动处理 CCCD，无需手动写）
func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
    guard let chr = svc.characteristics?.first(where: { $0.uuid == charUUID }) else { return }
    p.setNotifyValue(true, for: chr)       // iOS 自动写 CCCD
    self.characteristic = chr
}

// 5) 发送控制指令
func sendSwitch(on: Bool) {
    let value: UInt8 = on ? 0x31 : 0x30    // '1' / '0'
    peripheral.writeValue(Data([value]), for: characteristic, type: .withResponse)
}

// 6) 写入结果
func peripheral(_ p: CBPeripheral, didWriteValueFor chr: CBCharacteristic, error: Error?) {
    if let e = error { print("写入失败: \(e)") }
}

// 7) 收到通知
func peripheral(_ p: CBPeripheral, didUpdateValueFor chr: CBCharacteristic, error: Error?) {
    if let data = chr.value { print(String(data: data, encoding: .utf8) ?? "") }  // "connected"
}
```

**iOS 注意事项**

- iOS 必须使用**完整 128-bit UUID 字符串**，不支持 `0xF001` 这种简写。
- iOS 的 `peripheral.identifier`（UUID）是系统生成的，**与 MAC 无关且可能变化**，不要作为设备唯一标识持久化。
- 本设备无配对要求，iOS 不会弹出配对框。

### 5.3 Web Bluetooth（安卓 Chrome，快速验证用）

```javascript
const SERVICE = '0000f000-0000-1000-8000-00805f9b34fb';
const CHAR    = '0000f001-0000-1000-8000-00805f9b34fb';

const device = await navigator.bluetooth.requestDevice({
  filters: [{ name: '洁洁的哈士奇' }],
  optionalServices: [SERVICE]
});
const server = await device.gatt.connect();
const service = await server.getPrimaryService(SERVICE);
const chr = await service.getCharacteristic(CHAR);

// 发送指令（字符串模式，内部按 UTF-8 编码）
await chr.writeValue(new TextEncoder().encode('1'));   // 开
await chr.writeValue(new TextEncoder().encode('0'));   // 关

// 接收通知
await chr.startNotifications();
chr.addEventListener('characteristicvaluechanged', e => {
  console.log(new TextDecoder().decode(e.target.value));  // "connected"
});
```

**限制**：仅安卓 Chrome / Edge 支持；**iOS Safari 不支持 Web Bluetooth**；页面必须运行在 **HTTPS 或 localhost** 下。

---

## 6. 硬件行为说明

| 项 | 值 |
|---|---|
| 控制引脚 | **GPIO3** |
| ON 输出 | **3.3 V**（高电平，`SWITCH_ON_LEVEL = 1`） |
| OFF 输出 | **0 V**（低电平，`SWITCH_OFF_LEVEL = 0`） |
| 上电默认 | OFF（0 V），高电平触发模块不吸合 |
| IO 电平标准 | 3.3 V（**非 5 V  tolerant**） |

### 6.1 触发方式：当前为「高电平触发」

固件当前配置为**高电平触发**——发"开"指令时 GPIO3 输出 3.3 V：

```c
#define SWITCH_ON_LEVEL  1   /* 当前值：高电平触发模块 / 驱动 LED */
#define SWITCH_OFF_LEVEL 0   /* 与 ON 相反 */
```

**两种触发方式对照**

| 负载类型 | `SWITCH_ON_LEVEL` | `SWITCH_OFF_LEVEL` | 发 `31`（开）时 GPIO3 | 发 `30`（关）时 GPIO3 |
|---|---|---|---|---|
| 高电平触发模块 / LED（**当前**） | `1` | `0` | **3.3 V → 吸合 / 点亮** | 0 V → 释放 / 熄灭 |
| 低电平触发继电器（蓝色模块） | `0` | `1` | 0 V → 吸合 | 3.3 V → 释放 |

> ⚠️ **重要**：若你的继电器是**低电平触发**模块，而固件保持当前的高电平配置，控制逻辑会**完全反转**——发"开"反而释放、发"关"反而吸合。请按上表核对你的模块类型后再烧录。

改宏后需重新编译烧录；**App 侧指令不变**（`31` 始终表示"开"、`30` 始终表示"关"），变的只是引脚实际输出电平。

**上电瞬态**：`gpio_reset_pin` 到 `gpio_set_level(OFF)` 之间存在极短窗口。
- 当前配置（OFF = 0 V，低电平）：上电瞬间即使引脚短暂浮空，低电平触发模块也**不会误吸合**，属于安全状态。
- 若改回低电平触发配置（OFF = 3.3 V），建议在 IN 脚加一个 10 kΩ 上拉电阻，避免上电抖动导致继电器误动作。

### 6.2 接线安全（务必遵守）

1. GPIO3 **只能提供信号**，单脚驱动能力约 20 mA，**禁止直接驱动继电器线圈、电机、灯带**。
2. 继电器模块：
   - `IN` → GPIO3
   - `VCC` / `GND` → 独立电源（≥ 100 mA 余量）
   - **必须与 ESP32-C3 共地（GND 相连）**
3. 控制 220 V 市电时务必注意强电隔离、绝缘与防护，继电器选型留足余量。

---

## 7. 排错清单

| 现象 | 排查顺序 |
|---|---|
| **扫描不到设备** | ① 芯片是否上电并打印 `BLE ready, name=洁洁的哈士奇, advertising...`；② 手机定位开关 + App 定位权限；③ 距离是否 < 1 m；④ 换 LightBlue / nRF Connect 交叉验证 |
| **连上但发现不了服务** | ① 是否等待 `onServicesDiscovered` 回调；② 芯片串口是否打印 `registered service 0xf000 with handle=14`（无此行说明 GATT 未注册，需重烧固件） |
| **写入失败（status ≠ 0）** | ① 确认目标是 `0xF001` 而非 `0x2B29` / 服务本身；② 确认使用 Write Request 而非 Write Without Response；③ status=3 表示属性不支持写 |
| **写入成功但芯片无反应** | ① 看芯片串口有无 `BLE recv N bytes:`；② 无 → 数据未达回调；③ 有但 `unknown cmd` → 指令内容/大小写错误 |
| **反复断连** | ① 是否有其他 App 同时连接（BLE 通常单连接）；② 供电不足（RF 发射瞬间电流较大，确保 3.3 V 电源 ≥ 500 mA） |
| **能读到 `ON`/`OFF` 但写不进去** | 特征选择正确但写入数据格式异常；尝试单字节 `31` / `30` |

**验证用串口日志（正常流程应出现）**

```
registered service 0xf000 with handle=14
registered characteristic 0xf001 with def_handle=15 val_handle=16
BLE ready, name=洁洁的哈士奇, advertising...
BLE connected, conn_handle=1
BLE recv 1 bytes: 1
BLE switch: ON
BLE recv 1 bytes: 0
BLE switch: OFF
```

---

## 8. 已知限制与改进建议

| # | 限制 | 影响 | 状态 / 建议改法（固件侧） |
|---|---|---|---|
| 1 | Read `F001` 固定返回 `hello`，**不返回当前开关状态** | App 启动/重连后无法知道灯是开是关 | ✅ **已实现**：Read 按 `gpio_get_level` 返回当前 `ON`/`OFF` |
| 2 | Notify 仅在订阅当刻发一次 `connected` | 无法实现状态实时回显 | ✅ **已实现**：电平变化时主动 `ble_gatts_notify_custom` 回发 `ON`/`OFF` |
| 3 | 地址随机、无配对 | 任何手机都能控制 | 提高 `CONFIG_BT_NIMBLE_SM_LVL`，启用 Passkey 配对 |
| 4 | 未知指令静默（仅串口打印） | App 无法获知指令是否被拒绝 | ✅ **已实现**：未知指令 Notify 回传 `ERR` |
| 5 | 单引脚单路开关 | 无法控制多路 | 扩展 GATT 特征（如 `0xF002`）或使用指令格式 `pin,level` |

> 第 1、2、4 项已实现，"App 显示真实开关状态"与"指令被拒提示"已具备，无需再改固件；剩余未实现项见上表。

---

## 9. 量产前建议

| 项目 | 当前状态 | 建议 |
|---|---|---|
| 安全等级 | SM_LVL = 0（开放） | 提高到 LVL 2~3，启用 Passkey 或 Numeric Comparison |
| 设备名 | 固定 `洁洁的哈士奇` | 加入 MAC 后几位做唯一化，避免多设备同名 |
| 地址类型 | Random Static | 若需固定标识，改用 Public 地址或在广播包中携带自定义 ID |
| 广播间隔 | 30~60 ms（功耗较高） | 电池供电场景改为 100 ms ~ 1 s |
| 状态回传 | 无 | 见第 8 节第 1、2 项 |

---

## 10. 关键参数速查表

| 参数 | 值 | 来源 |
|---|---|---|
| 设备名 | `洁洁的哈士奇` | `main.c` → `BLE_DEVICE_NAME` |
| 服务 UUID | `0000F000-0000-1000-8000-00805F9B34FB` | `main.c` → `gatt_svcs[]` |
| 特征 UUID | `0000F001-0000-1000-8000-00805F9B34FB` | 同上 |
| 特征属性 | READ \| WRITE \| NOTIFY | 同上 |
| 控制引脚 | GPIO3 | `main.c` → `SWITCH_GPIO` |
| ON 电平 | **1（3.3 V）** | `main.c` → `SWITCH_ON_LEVEL` |
| OFF 电平 | **0（0 V）** | `main.c` → `SWITCH_OFF_LEVEL` |
| 触发方式 | **高电平触发** | 由 `SWITCH_ON_LEVEL = 1` 决定 |
| ATT MTU 上限 | 256 | sdkconfig → `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` |
| 最大连接数 | 3 | sdkconfig → `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` |
| 安全等级 | 0（无配对） | sdkconfig → `CONFIG_BT_NIMBLE_SM_LVL` |
| 广播间隔 | 30 ~ 60 ms | NimBLE 默认 FAST_INTERVAL1 |
| 广播通道图 | 0x07（37/38/39） | `BLE_GAP_ADV_DFLT_CHANNEL_MAP` |
| BLE 协议栈 | NimBLE（ESP-IDF v6.1-beta1） | — |
| 芯片 | ESP32-C3（RISC-V，仅 BLE，不支持经典蓝牙） | — |

---

*本文档基于实测通过的固件整理。若后续修改 `main.c` 中的 UUID、引脚或指令集，请同步更新本文档对应章节。*
