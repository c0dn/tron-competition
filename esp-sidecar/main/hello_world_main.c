/*
 * ESP32-C3 sidecar - FreeRTOS "Hello world" bring-up.
 *
 * Minimal firmware that prints chip info once and then spins a FreeRTOS task
 * printing a heartbeat, to verify the build/flash/monitor toolchain on the
 * SuperMini over native USB Serial/JTAG.
 *
 * TODO(decoder): the BLE wire contract already lives in the repo at
 *   <repo>/shared/schema.h  (single source of truth, shared with the
 *   micro:bit encoder). To decode wearable adverts:
 *     1. Add the include path in main/CMakeLists.txt:
 *          idf_component_register(... INCLUDE_DIRS "." "../../shared")
 *     2. #include "schema.h"
 *     3. After matching company_id == MIND_COMPANY_ID (0xFFFF) in the MSD,
 *        cast the payload to const mind_adv_payload_t * and read the fields
 *        (both sides little-endian -> byte-for-byte, no manual unpack).
 *     4. Resolve identity from AdvA: byte[4] == DEVICE_ID, byte[5] == 0xC0
 *        (see MIND_ADVA); map DEVICE_ID -> device label via a static table.
 *   The micro:bit side (microbit/app/wearable_app) already emits against this
 *   header. FREEZE the contract with the team before writing decode.
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

static void hello_task(void *arg)
{
    unsigned n = 0;
    while (1) {
        printf("Hello world from FreeRTOS task (%u)\n", n++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* Print chip information once at boot. */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("esp-sidecar bring-up: %s, %d CPU core(s), %s%s%s\n",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
               (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }
    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    xTaskCreate(hello_task, "hello_task", 2048, NULL, 5, NULL);
}
