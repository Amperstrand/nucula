#include "commands.h"
#include "sdkconfig.h"
#include "board.h"
#include "console.h"
#include "wallet.hpp"
#include "wallet_store.hpp"
#include "unit.hpp"
#include "keyset.hpp"
#include "cashu_json.hpp"
#include "crypto.h"
#include "crypto_test.h"
#include "selftest.hpp"
#include "nfc.hpp"
#include "keypad.h"
#include "display.h"
#include "ui.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/i2c_master.h"
#include "i2c_bus.h"

#define TAG "nucula"

// System commands: NFC control, keypad probing, and the reboot/heap/
// tasks/log/bench/selftest diagnostics.

static void cmd_nfc(const char *arg)
{
    if (!arg || strlen(arg) == 0) {
        console_printf("nfc: %s\r\n", nfc_status_str());
        return;
    }
    if (strncmp(arg, "request ", 8) == 0) {
        int amount = atoi(arg + 8);
        if (amount <= 0) {
            console_print("usage: nfc request <amount> [u=<unit>]\r\n");
            return;
        }
        if (nfc_state() == NfcState::off) {
            console_print("error: NFC not available\r\n");
            return;
        }
        CmdOpts opts;
        if (!parse_cmd_opts(strchr(arg + 8, ' '), opts))
            return;
        const std::string unit = opts.unit.empty()
            ? cashu::Wallet::default_unit() : opts.unit;
        char amt[48];
        cashu::format_amount(amt, sizeof(amt), amount, unit.c_str());
        console_printf("requesting %s via NFC...\r\n", amt);
        if (!nfc_request_start(amount, unit.c_str(), nullptr))
            console_print("error: failed to start\r\n");
        return;
    }
    if (strcmp(arg, "stop") == 0) {
        nfc_request_stop();
        console_print("nfc stopped\r\n");
        ui_refresh();
        return;
    }
    console_print("usage: nfc [request <amount> [u=<unit>]|stop]\r\n");
}

static void cmd_reboot(const char *arg)
{
    (void)arg;
    console_print("rebooting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// -------------------------------------------------------------------------
// Telemetry
// -------------------------------------------------------------------------

static void cmd_heap(const char *arg)
{
    (void)arg;
    console_printf("free:          %lu\r\n", (unsigned long)esp_get_free_heap_size());
    console_printf("largest block: %u\r\n",
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    console_printf("min ever free: %lu\r\n",
                   (unsigned long)esp_get_minimum_free_heap_size());
}

static void cmd_tasks(const char *arg)
{
    (void)arg;
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
    if (!st) {
        console_print("error: out of memory\r\n");
        return;
    }
    n = uxTaskGetSystemState(st, n, NULL);
    console_printf("%-16s %4s %10s\r\n", "name", "prio", "stack-min");
    for (UBaseType_t i = 0; i < n; i++)
        console_printf("%-16s %4u %10u\r\n", st[i].pcTaskName,
                       (unsigned)st[i].uxCurrentPriority,
                       (unsigned)st[i].usStackHighWaterMark);
    free(st);
}

static void cmd_log(const char *arg)
{
    esp_log_level_t level;
    if (arg && arg[0] && (arg[1] == '\0' || arg[1] == ' ')) {
        switch (arg[0]) {
            case 'e': level = ESP_LOG_ERROR; break;
            case 'w': level = ESP_LOG_WARN;  break;
            case 'i': level = ESP_LOG_INFO;  break;
            case 'd': level = ESP_LOG_DEBUG; break;
            default:  goto usage;
        }
        const char *tag = arg + 1;
        while (*tag == ' ') tag++;
        esp_log_level_set(*tag ? tag : "*", level);
        console_printf("log level '%c' set for %s\r\n", arg[0], *tag ? tag : "*");
        return;
    }
usage:
    console_print("usage: log <e|w|i|d> [tag]\r\n");
}

static void cmd_bench(const char *arg)
{
    (void)arg;
    console_print("benchmarking crypto primitives...\r\n");
    crypto_run_benchmark(wallet_store_ctx());
    console_print("done (results logged at info level)\r\n");
}

static void cmd_selftest(const char *arg)
{
    (void)arg;
    console_print("running self-tests (details logged at info level)...\r\n");
    bool ok = crypto_run_tests(wallet_store_ctx()) != 0;
    if (!cashu::keyset_run_tests())
        ok = false;
    if (!cashu::unit_run_tests())
        ok = false;
    if (!cashu::cashu_json_run_tests())
        ok = false;
    if (!nucula_pure_selftests())
        ok = false;
    if (!cashu::Wallet::run_tests())
        ok = false;
    console_printf("self-tests %s\r\n", ok ? "PASSED" : "FAILED");
}

// -------------------------------------------------------------------------
// Keypad
// -------------------------------------------------------------------------

static void cmd_keypad(const char *arg)
{
    if (!arg || strcmp(arg, "scan") != 0) {
        console_print("usage: keypad scan\r\n");
        console_print("  scan: probe each PCF8574 pin (P0-P6) and report which\r\n");
        console_print("        other pins go low. Press keys while scanning.\r\n");
        return;
    }

    console_print("keypad scan — press keys, each fires once per press (~30s)\r\n\r\n");

    int64_t deadline = esp_timer_get_time() + 30LL * 1000000;
    while (esp_timer_get_time() < deadline) {
        // Pull from the queue the background task fills — 200ms window per iteration
        char key = keypad_wait_event(200);
        if (key) {
            char line[32];
            snprintf(line, sizeof(line), "key: '%c'\r\n", key);
            console_print(line);
        }
    }
    console_print("scan done\r\n");
}

// Scan one bus handle for ACKing devices; prints hex addresses.
static int scan_devices(i2c_master_bus_handle_t bus, int sda, int scl)
{
    int found = 0;
    for (uint8_t addr = 1; addr < 0x78; addr++) {
        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = addr;
        dev.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dh;
        if (i2c_master_bus_add_device(bus, &dev, &dh) == ESP_OK) {
            uint8_t reg = 0x37; // VersionReg — read is safe on an MFRC522
            uint8_t val;
            if (i2c_master_transmit_receive(dh, &reg, 1, &val, 1, 20) == ESP_OK) {
                console_printf("  0x%02X (%02X) @ sda=%d scl=%d\r\n",
                               addr, val, sda, scl);
                found++;
            }
            i2c_master_bus_rm_device(dh);
        }
        if ((addr & 0x0F) == 0x0F)
            vTaskDelay(1); // feed the task watchdog between probe batches
    }
    return found;
}

#if CONFIG_NUCULA_BOARD_ATOM
// Second-controller helpers for probing arbitrary pin pairs: the main
// bus owns I2C_NUM_0, and classic ESP32 has a second controller (the
// C3 does not — these diagnostics are atom-only).
static int scan_bus(int sda, int scl)
{
    // Anonymous-union member (clk_source) makes designated initializers
    // unusable for this struct in C++.
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_1;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK || !bus)
        return -1;
    int found = scan_devices(bus, sda, scl);
    i2c_del_master_bus(bus);
    return found;
}

static void cmd_i2cdump(const char *arg)
{
    int sda = 33, scl = 32, addr = 0x5D;
    if (arg && strlen(arg) > 0 &&
        sscanf(arg, "%d %d %x", &sda, &scl, &addr) != 3) {
        console_print("usage: i2cdump <sda> <scl> <addr-hex> — dump regs 00-3F\r\n");
        return;
    }
    // Anonymous-union member (clk_source) makes designated initializers
    // unusable for this struct in C++.
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_1;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK || !bus) {
        console_print("bus create failed\r\n");
        return;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = (uint8_t)addr;
    dev.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dh;
    if (i2c_master_bus_add_device(bus, &dev, &dh) != ESP_OK) {
        console_print("device add failed\r\n");
        i2c_del_master_bus(bus);
        return;
    }
    for (int r = 0; r < 0x40; r += 8) {
        console_printf("%02X:", r);
        for (int i = 0; i < 8; i++) {
            uint8_t reg = (uint8_t)(r + i), val;
            if (i2c_master_transmit_receive(dh, &reg, 1, &val, 1, 20) == ESP_OK)
                console_printf(" %02X", val);
            else
                console_print(" --");
        }
        console_print("\r\n");
    }
    i2c_master_bus_rm_device(dh);
    i2c_del_master_bus(bus);
}
#endif

static void cmd_i2cscan(const char *arg)
{
    int total = 0;
    console_printf("scanning main bus sda=%d scl=%d...\r\n",
                   BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
    int n = scan_devices(i2c_bus_get(), BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
    if (n <= 0)
        console_print("  nothing found\r\n");
    total += n > 0 ? n : 0;

#if CONFIG_NUCULA_BOARD_ATOM
    if (arg && strlen(arg) > 0) {
        int sda, scl;
        if (sscanf(arg, "%d %d", &sda, &scl) != 2) {
            console_print("usage: i2cscan [sda scl] — scan one pair, or sweep common pins\r\n");
            return;
        }
        console_printf("scanning sda=%d scl=%d...\r\n", sda, scl);
        if (scan_bus(sda, scl) == 0)
            console_print("  nothing found\r\n");
        console_print("scan done\r\n");
        return;
    }

    static const int pairs[][2] = {
        {26, 32}, {32, 33}, {21, 22}, {25, 26}, {16, 17}, {33, 32},
    };
    for (auto &p : pairs) {
        if (p[0] == BOARD_I2C_SDA_PIN && p[1] == BOARD_I2C_SCL_PIN)
            continue;
        console_printf("scanning sda=%d scl=%d...\r\n", p[0], p[1]);
        n = scan_bus(p[0], p[1]);
        if (n <= 0)
            console_print("  nothing found\r\n");
        total += n > 0 ? n : 0;
    }
#endif
    console_printf("scan done, %d device(s)\r\n", total);
}

#if CONFIG_NUCULA_BOARD_ATOM
#include "rc522.h"
static void cmd_nfcdump(const char *arg)
{
    int pages = arg && strlen(arg) > 0 ? atoi(arg) : 16;
    if (pages <= 0 || pages > 64) pages = 16;
    rc522_tag_t tag;
    if (!rc522_poll(&tag)) {
        console_print("no tag\r\n");
        return;
    }
    console_printf("uid_len=%d uid=", (int)tag.uid_len);
    for (int i = 0; i < tag.uid_len; i++)
        console_printf("%02X", tag.uid[i]);
    console_printf(" sak=%02X\r\n", tag.sak);
    for (int p = 0; p < pages; p += 4) {
        uint8_t blk[16];
        if (!rc522_ul_read((uint8_t)p, blk)) {
            console_printf("%02X: read failed\r\n", p);
            return;
        }
        console_printf("%02X:", p);
        for (int i = 0; i < 16; i++)
            console_printf(" %02X", blk[i]);
        console_print("\r\n");
    }
}
#endif

static void cmd_i2crecover(const char *arg)
{
    int clocks = arg && strlen(arg) > 0 ? atoi(arg) : 32;
    if (clocks <= 0 || clocks > 4096) {
        console_print("usage: i2crecover <clocks> — SCL-clock a stuck bus\r\n");
        return;
    }
    bool ok = i2c_bus_recover(clocks);
    console_printf("recovery(%d clocks): %s\r\n", clocks,
                   ok ? "SDA high" : "SDA STILL LOW");
    console_print("reboot before using the bus again\r\n");
}

void commands_system_register(void)
{
    console_register_cmd("nfc",     cmd_nfc,      "nfc [request <amount>|stop]");
    console_register_cmd("keypad",  cmd_keypad,   "keypad scan — probe PCF8574 wiring");
    console_register_cmd("i2cscan", cmd_i2cscan,  "i2cscan [sda scl] — find I2C devices");
    console_register_cmd("i2crecover", cmd_i2crecover, "i2crecover <clocks> — clear stuck bus");
#if CONFIG_NUCULA_BOARD_ATOM
    console_register_cmd("i2cdump", cmd_i2cdump, "i2cdump <sda> <scl> <addr> — dump regs");
    console_register_cmd("nfcdump", cmd_nfcdump, "nfcdump [pages] — dump tag pages");
#endif
    console_register_cmd("reboot",  cmd_reboot,   "restart the device");
    console_register_cmd("heap",    cmd_heap,     "show heap usage");
    console_register_cmd("tasks",   cmd_tasks,    "show task stack high-water marks");
    console_register_cmd("log",     cmd_log,      "log <e|w|i|d> [tag] — set log level");
    console_register_cmd("bench",   cmd_bench,    "benchmark crypto primitives");
    console_register_cmd("selftest", cmd_selftest, "run crypto/keyset self-tests");
}
