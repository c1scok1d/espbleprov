#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "main.h"
#include "wifi_drv.h"
#include "nvs_drv.h"
#include "driver/adc.h"
#include "data.h"
#include "rest_methods.h"

#define SSID_CHR_UUID 0xFEF4
#define PASS_CHR_UUID 0xFEF5
#define NAME_CHR_UUID 0xFEF6  // Example UUID for sensorName
#define LOCATION_CHR_UUID 0xFEF7  // Example UUID for sensorLocation
#define PROV_STATUS_UUID 0xDEAD
//#define MANUFACTURER_NAME "Rodland Farms"



char *TAG = "PlantPulse";

static bool ssid_pswd_flag[2] = {
    false,
};
uint8_t ble_addr_type;
uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t prov_status_attr_handle = 0;

main_struct_t main_struct = {.credentials_recv = 0, .isProvisioned = false};

// Notification function for PROV_STATUS_UUID
void notify_prov_status(uint8_t status_data)
{
    char *TAG = "NOTIFY";
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&status_data, sizeof(status_data));
        if (!om)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for notification");
            return;
        }

        int rc = ble_gattc_notify_custom(g_conn_handle, prov_status_attr_handle, om);
        if (rc != 0)
        {
            ESP_LOGE(TAG, "Error sending notification; rc=%d", rc);
        }
        else
        {
            ESP_LOGI(TAG, "Notification sent: %d", status_data);
        }
    }
    else
    {
        ESP_LOGW(TAG, "No connected client to notify");
    }
}

// Write data to ESP32 defined as server
static int device_write(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char *TAG = "WRITE";
    switch (ble_uuid_u16(ctxt->chr->uuid))
    {
    case SSID_CHR_UUID:
        ssid_pswd_flag[0] = true;
        strncpy(main_struct.ssid, (char *)ctxt->om->om_data, ctxt->om->om_len);
        main_struct.ssid[ctxt->om->om_len] = '\0';
        ESP_LOGI(TAG, "Received SSID: %s", main_struct.ssid);
        break;

    case PASS_CHR_UUID:
        ssid_pswd_flag[1] = true;
        strncpy(main_struct.password, (char *)ctxt->om->om_data, ctxt->om->om_len);
        main_struct.password[ctxt->om->om_len] = '\0';
        ESP_LOGI(TAG, "Received Password: %s", main_struct.password);
        break;

    case NAME_CHR_UUID:
        ssid_pswd_flag[1] = true;
        strncpy(main_struct.name, (char *)ctxt->om->om_data, ctxt->om->om_len);
        main_struct.name[ctxt->om->om_len] = '\0';
        ESP_LOGI(TAG, "Received Device Name: %s", main_struct.name);
        break;
    
    case LOCATION_CHR_UUID:
        ssid_pswd_flag[1] = true;
        strncpy(main_struct.location, (char *)ctxt->om->om_data, ctxt->om->om_len);
        main_struct.location[ctxt->om->om_len] = '\0';
        ESP_LOGI(TAG, "Received Device Location: %s", main_struct.location);
        break;

    default:
        ESP_LOGW(TAG, "Unknown characteristic written.");
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ssid_pswd_flag[0] && ssid_pswd_flag[1]) // ssid and password received
    {

        main_struct.credentials_recv = true;

        save_to_nvs(main_struct.ssid, main_struct.password, main_struct.name, main_struct.location, main_struct.credentials_recv);
        printf("Password saved to %s\n", main_struct.password);
        printf("ssid to %s\n", main_struct.ssid);
        printf("wvalue %d\n", main_struct.credentials_recv);
    }

    return 0;
}

// Read data from ESP32 defined as server
static int device_read(uint16_t con_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // uint32_t status_data = main_struct.isProvisioned ? 1 : 0; // Example: 1 = provisioned, 0 = not provisioned
    // os_mbuf_append(ctxt->om, &status_data, sizeof(status_data));
    // ESP_LOGI(TAG, "BLE Read: Provisioning Status - %u", status_data);
    return 0; // Success
}


// Array of pointers to other service definitions
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {// service
     .type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180), // Define UUID for device type
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(SSID_CHR_UUID), // SSID characteristic
          .flags = BLE_GATT_CHR_F_WRITE,
          .access_cb = device_write},
         {.uuid = BLE_UUID16_DECLARE(PASS_CHR_UUID), // Password characteristic
          .flags = BLE_GATT_CHR_F_WRITE,
          .access_cb = device_write},
         {.uuid = BLE_UUID16_DECLARE(NAME_CHR_UUID), // ensor Name characteristic
          .flags = BLE_GATT_CHR_F_WRITE,
          .access_cb = device_write},  // Write callback function for sensorName
         {.uuid = BLE_UUID16_DECLARE(LOCATION_CHR_UUID), // Sensor Location characteristic
          .flags = BLE_GATT_CHR_F_WRITE,
          .access_cb = device_write},  // Write callback function for sensorLocation
         {0}}},
    {0} // No more service
};

// BLE event handling
#define BLE_CONN_MAX_RETRIES 3

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    static int ble_conn_retries = 0;
    char *TAG = "BLE GAP EVENT";

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE GAP EVENT CONNECT %s", event->connect.status == 0 ? "OK!" : "FAILED!");

        if (event->connect.status == 0) {
            // Connection successful
            g_conn_handle = event->connect.conn_handle;
            ble_conn_retries = 0;  // Reset retries after a successful connection
        } else if (event->connect.status != 0) {
            // Connection failed
            if (ble_conn_retries < BLE_CONN_MAX_RETRIES) {
                ESP_LOGW(TAG, "Connection failed, retrying (%d/%d)", ble_conn_retries + 1, BLE_CONN_MAX_RETRIES);
                ble_conn_retries++;
                ble_app_advertise();  // Retry advertising
            } else {
                ESP_LOGE(TAG, "BLE connection failed after %d retries.", BLE_CONN_MAX_RETRIES);
                // Optionally disable BLE or handle further steps
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE GAP EVENT DISCONNECT");
        // Handle disconnection (could include cleanup or retry advertising)
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE GAP EVENT ADV COMPLETE");
        ble_app_advertise();  // Advertise again if advertising is complete
        break;

    default:
        break;
    }

    return 0;
}


//#define MANUFACTURER_NAME "Rodland Farms"

// Define the BLE connection
void ble_app_advertise(void)
{
    char *TAG = "BLE_ADVERTISE";
    // GAP - device name definition
    struct ble_hs_adv_fields fields;
    const char *device_name;
    memset(&fields, 0, sizeof(fields));

    device_name = ble_svc_gap_device_name(); // Read the BLE device name
    // Set manufacturer data to the manufacturer name
    //uint8_t manuf_data[32];  // Make sure the size is sufficient for the manufacturer name
    //snprintf((char *)manuf_data, sizeof(manuf_data), "%s", MANUFACTURER_NAME);  // Copy the manufacturer name into manuf_data
    

    //fields.mfg_data = manuf_data;  // Pointer to the manufacturer data
    //fields.mfg_data_len = strlen((char *)manuf_data);  // Length of manufacturer name

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    //ESP_LOGI(TAG, "device name: %s, manufacterer: %s", device_name, manuf_data);

    // GAP - device connectivity definition
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // connectable or non-connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // discoverable or non-discoverable
    ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

static void nimble_host_task(void *param)
{
    char *TAG = "NimBLE";
    // Task entry log 
    ESP_LOGI(TAG, "NimBLE host task has been started!");

    // This function won't return until nimble_port_stop() is executed
    nimble_port_run();

    // Clean up at exit 
    vTaskDelete(NULL);
}

void disable_ble(void)
{
    char *TAG = "DISABLE_BLE";
    // Stop BLE advertising if running
    int rc = ble_gap_adv_stop();
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE advertising stopped.");
    } else {
        ESP_LOGE(TAG, "Failed to stop BLE advertising: %d", rc);
    }

    // Optionally, stop the NimBLE stack completely (if desired)
    nimble_port_stop(); // Uncomment if you want to disable all BLE functionality
}

void check_credentials(void *arg)
{
    char *TAG = "CHECK_CREDS";
    while (1)
    {
        //if (main_struct.credentials_recv)
        //{
            ESP_LOGI(TAG, "Checking WiFi Credentials");
            wifi_init();
            //disable_ble();
            vTaskDelete(NULL);
        //}

        vTaskDelay(1000);
    }
}

void nvs_init(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

void notify_status(void *arg)
{
    while (1)
    {
        if (main_struct.isProvisioned)
        {
            notify_prov_status(main_struct.isProvisioned); // Send notification
            main_struct.isProvisioned = 0;

            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // Check every second
    }
}
// Helper function to convert the MAC address to a string
void mac_to_string(uint8_t *mac, char *mac_str) {
    // Format only the last 4 bytes of the MAC address into the string
    sprintf(mac_str, "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
}


// BLE sync callback function
void ble_app_on_sync(void)
{
    // The BLE stack is now synchronized and ready to run BLE operations
    ESP_LOGI(TAG, "BLE sync completed.");

    // Retrieve the MAC address
    uint8_t mac[6];   // Array to hold the MAC address (6 bytes)
    char device_name[32];  // Buffer to hold the MAC address as a string

    // Automatically infer the address type and MAC address
    ble_hs_id_infer_auto(0, &ble_addr_type);  // Infers the address type
    ble_hs_id_infer_auto(0, mac);  // Retrieves the MAC address

    // Convert only the last 4 bytes of the MAC address to string (without colons)
    mac_to_string(mac + 2, device_name);  // Only use last 4 bytes of MAC address

    // Create final device name with "PlantPulse-" prefix and the last 4 bytes of the MAC address
    char final_device_name[50];  // Ensure enough space for "PlantPulse-" + MAC address
    snprintf(final_device_name, sizeof(final_device_name), "PlantPulse %s", device_name);

    // Set the BLE device name
    ble_svc_gap_device_name_set(final_device_name);  // Set the device name
    //ble_svc_gap_device_name_set("pLANTpULSE");  // Set the device name
    
    ESP_LOGI(TAG, "Device BLE name set to: %s", final_device_name);

    // Initialize services
    ble_svc_gap_init();                        // Initialize NimBLE GAP service
    ble_svc_gatt_init();                       // Initialize NimBLE GATT service
    ble_gatts_count_cfg(gatt_svcs);            // Initialize NimBLE config GATT services
    ble_gatts_add_svcs(gatt_svcs);             // Add GATT services
}

// Main application entry point
void app_main() {
    char *TAG = "MAIN";
    // Configure ADC1 channel 5 (GPIO5) for 12-bit width and 11dB attenuation
    adc1_config_width(ADC_WIDTH_BIT_12);  // Set ADC width to 12 bits (0-4095 range)
    adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);  // Set ADC attenuation to 11dB (0-3.6V range)

    nvs_init();
    read_from_nvs(main_struct.ssid, main_struct.password, main_struct.name, main_struct.location, &main_struct.credentials_recv);

    // Only initialize BLE if credentials are NOT set
    if (!main_struct.credentials_recv) {
        // Initialize NimBLE host stack
        nimble_port_init();  // Step 1: Initialize the host stack

        // Set up the sync callback (this will be used later in sync events)
        ble_hs_cfg.sync_cb = ble_app_on_sync;  // Step 2: Set the sync callback function


        // Start NimBLE host task thread and return 
        xTaskCreate(nimble_host_task, "PlantPulse", 4 * 1024, NULL, 5, NULL);

    } else {
        ESP_LOGI(TAG, "Wi-Fi credentials already set. Skipping BLE provisioning.");
    } 

    // Start moisture reading in a separate FreeRTOS task
    //xTaskCreate(monitor, "monitor", 4 * 1024, NULL, 5, NULL);
    // Check Wifi Credentials 
    xTaskCreate(check_credentials, "check_credentials", 4 * 1024, NULL, 5, NULL);

    //xTaskCreate(notify_status, "notify_status", 2 * 1024, NULL, 5, NULL);
}