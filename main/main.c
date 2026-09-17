#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"   /* ble_hs_util_ensure_addr() 的声明所在（官方 bleprph 示例同此） */
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "driver/gpio.h"   /* 控制物理开关（继电器/负载）的 GPIO 驱动 */

/* 蓝牙设备名：想改名字只改这一行 */
#define BLE_DEVICE_NAME "C3_BLE_01"

/* ---------- 蓝牙开关配置：手机发 on/off（或 1/0）控制此引脚 ---------- */
#define SWITCH_GPIO      GPIO_NUM_3    /* 接继电器/负载的引脚，按你的接线改 */
#define SWITCH_ON_LEVEL  1             /* 输出该电平=导通。低电平触发继电器(常见蓝板)用 0；
                                          高电平触发模块/驱动 LED 请改成 1 */
#define SWITCH_OFF_LEVEL 0             /* 与 ON 相反 */

/* 前向声明：供下方数组/事件回调在定义前引用 */
void ble_app_advertise(void);
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static void notify_state(uint16_t conn_handle);   /* 状态变化时主动回传 ON/OFF */

/* ---------- GATT：一个服务 + 一个可通知特性 ---------- */
static uint16_t gatt_chr_val_handle;
static char rx_buf[64];   /* 手机通过 Write 写进来的数据暂存（大小可自行调整） */
static uint8_t g_own_addr_type;   /* 广播用的本机地址类型（由 NimBLE 推断，对齐官方示例） */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xF000),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xF001),
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &gatt_chr_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* 特性被读取时回的内容 */
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int lvl = gpio_get_level(SWITCH_GPIO);
        const char *state = (lvl == SWITCH_ON_LEVEL) ? "ON" : "OFF";
        os_mbuf_append(ctxt->om, state, strlen(state));
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* 手机把数据 Write 进来：取出并打印，留出业务逻辑钩子 */
        uint16_t len = ctxt->om->om_len;
        if (len > sizeof(rx_buf) - 1)
            len = sizeof(rx_buf) - 1;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, len, NULL);
        if (rc == 0) {
            rx_buf[len] = '\0';
            printf("BLE recv %u bytes: %s\n", len, rx_buf);
            /* 解析为开关指令并控制 GPIO（手机发来的即“蓝牙开关”指令） */
            if (strncmp(rx_buf, "on", 2) == 0 || strncmp(rx_buf, "1", 1) == 0) {
                gpio_set_level(SWITCH_GPIO, SWITCH_ON_LEVEL);
                printf("BLE switch: ON\n");
                notify_state(conn_handle);
            } else if (strncmp(rx_buf, "off", 3) == 0 || strncmp(rx_buf, "0", 1) == 0) {
                gpio_set_level(SWITCH_GPIO, SWITCH_OFF_LEVEL);
                printf("BLE switch: OFF\n");
                notify_state(conn_handle);
            } else {
                printf("BLE switch: unknown cmd '%s' (use on/off or 1/0)\n", rx_buf);
                /* 未知指令也回传错误标识，便于 App 感知被拒 */
                struct os_mbuf *om = ble_hs_mbuf_from_flat("ERR", 3);
                if (om) ble_gatts_notify_custom(conn_handle, gatt_chr_val_handle, om);
            }
        } else {
            printf("BLE recv copy failed rc=%d\n", rc);
        }
        return 0;   /* 0 = 写入成功（手机端会收到写入确认） */
    }
    return 0;
}

/* 连上后“回发一个”：发一条通知（带数据），发完即止 */
static void gatt_send_one(uint16_t conn_handle)
{
    const char *msg = "connected";
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
    if (om == NULL) {
        printf("BLE: mbuf alloc fail\n");
        return;
    }
    /* notify_custom 会自行消费 om，无需再 free */
    int rc = ble_gatts_notify_custom(conn_handle, gatt_chr_val_handle, om);
    if (rc != 0)
        printf("BLE: notify fail rc=%d\n", rc);
    else
        printf("BLE: sent one notification\n");
}

/* 开关状态变化时，主动把当前 ON/OFF 通知给已订阅的手机 */
static void notify_state(uint16_t conn_handle)
{
    int lvl = gpio_get_level(SWITCH_GPIO);
    const char *state = (lvl == SWITCH_ON_LEVEL) ? "ON" : "OFF";
    struct os_mbuf *om = ble_hs_mbuf_from_flat(state, strlen(state));
    if (om == NULL) {
        printf("BLE: state mbuf alloc fail\n");
        return;
    }
    int rc = ble_gatts_notify_custom(conn_handle, gatt_chr_val_handle, om);
    if (rc != 0)
        printf("BLE: state notify fail rc=%d\n", rc);
    else
        printf("BLE: sent state notification %s\n", state);
}

/* 关闭时钟毛刺检测复位（沿用你原来的做法） */
static void disable_glitch_reset(void)
{
    REG_CLR_BIT(RTC_CNTL_FIB_SEL_REG, RTC_CNTL_FIB_GLITCH_RST);
    REG_CLR_BIT(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_GLITCH_RST_EN);
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_WDT:        return "WDT";
        default:                 return "OTHER";
    }
}

/* ---------- GAP 事件（连接/断开/订阅） ---------- */
/* GATT 服务/特征注册成功时由 NimBLE 回调，用于确认服务确实注册进栈 */
static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        printf("registered service %s with handle=%d\n",
               ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
               ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        printf("registered characteristic %s with def_handle=%d val_handle=%d\n",
               ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
               ctxt->chr.def_handle,
               ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                printf("BLE connected, conn_handle=%d\n", event->connect.conn_handle);
            } else {
                printf("BLE connect fail, status=%d, restart adv\n", event->connect.status);
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            printf("BLE disconnected, restart adv\n");
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            /* 对方（手机）订阅了通知 → 连上后回发一个 */
            if (event->subscribe.cur_notify == 1 &&
                event->subscribe.attr_handle == gatt_chr_val_handle) {
                printf("BLE client subscribed\n");
                gatt_send_one(event->subscribe.conn_handle);
            }
            break;
        default:
            break;
    }
    return 0;
}

void ble_app_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 通用可发现 */

    uint8_t adv_data[31];
    int len = 0;
    adv_data[len++] = 0x02;
    adv_data[len++] = BLE_HS_ADV_TYPE_FLAGS;
    adv_data[len++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    int nlen = strlen(BLE_DEVICE_NAME);
    adv_data[len++] = nlen + 1;
    adv_data[len++] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(adv_data + len, BLE_DEVICE_NAME, nlen);
    len += nlen;

    ble_gap_adv_set_data(adv_data, len);
    /* 时长必须是 BLE_HS_FOREVER(=INT32_MAX) 表示永久广播。
       传 0 会被 NimBLE 当成“广播 0 毫秒后立刻停止”，导致手机永远搜不到本设备。 */
    ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                      ble_gap_event_handler, NULL);
}

static void ble_on_reset(int reason)
{
    printf("BLE host reset, reason=%d\n", reason);
}

static void ble_on_sync(void)
{
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    /* 注册 NimBLE 主机身份地址（random + public 都要确保），
       再推断广播用的地址类型。完全对齐官方 bleprph 示例。 */
    ble_hs_util_ensure_addr(1);   /* 确保 random 身份地址 */
    ble_hs_util_ensure_addr(0);   /* 确保 public 身份地址 */
    if (ble_hs_id_infer_auto(0, &g_own_addr_type) != 0)
        g_own_addr_type = BLE_OWN_ADDR_PUBLIC;   /* 推断失败时的兜底 */

    ble_app_advertise();
    printf("BLE ready, name=%s, advertising...\n", BLE_DEVICE_NAME);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
}

void ble_init(void)
{
    printf("BLE step: nvs_flash_init\n");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    printf("BLE step: nvs ok\n");

    /* nimble_port_init() 内部会完成 BT 控制器 init + enable(BLE)。
       切勿再单独调用 esp_bt_controller_init/enable，否则控制器已使能、
       会返回 ESP_ERR_INVALID_STATE(0x103) 导致 abort 重启。 */
    printf("BLE step: nimble_port_init\n");
    ESP_ERROR_CHECK(nimble_port_init());
    printf("BLE step: controller + host ready\n");

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* GATT 服务必须在 NimBLE 主机任务启动之前注册，host sync 时才会真正
       激活 GATT 数据库并触发 gatts_register_cb。若放到 sync_cb(ble_on_sync)
       里再 add_svcs，函数虽返回 0，服务却不激活，手机 discover 失败、连接即断。
       对齐官方 bleprph 示例：gatt_svr_init() 在 nimble_port_freertos_init 之前调用。 */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        printf("ble_gatts_count_cfg failed, rc=%d\n", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        printf("ble_gatts_add_svcs failed, rc=%d\n", rc);
        return;
    }

    printf("BLE step: nimble_port_freertos_init\n");
    nimble_port_freertos_init(ble_host_task);
    printf("BLE step: init done\n");
}

/* ---------- 主函数 ---------- */
void app_main(void)
{
    disable_glitch_reset();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("\n=== Hello ESP32-C3 ===\n");
    printf("chip revision : v%d.%d\n", chip.revision / 100, chip.revision % 100);
    printf("reset reason  : %s\n", reset_reason_name(esp_reset_reason()));

    /* 初始化蓝牙开关引脚：上电默认关，避免继电器上电误吸合 */
    gpio_reset_pin(SWITCH_GPIO);
    gpio_set_direction(SWITCH_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SWITCH_GPIO, SWITCH_OFF_LEVEL);

    ble_init();                 /* 初始化并启动 BLE；BLE 主机在独立任务运行，
                                   这里直接返回即可，不需要 while 循环占用串口 */
}
