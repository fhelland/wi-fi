/*  Main file. Start all other tasks.
*/

#include <string.h>
#include <time.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "driver/uart.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "soc/rtc_periph.h"

// custom include files:
#include "uart_tcp_server.h"
#include "sdmmc.h"
#include "spi.h"
#include "file_server.h"
#include "wifi_manager.h"

static const char *TAG = "MAIN";

/* These pins will wake the module up from sleep */

#define WAKEUP1                 (34)           /* Wake up and turn on Wi-Fi                      */
#define WAKEUP2                 (35)           /* Wake up and only run Receive SPI messages      */

// Deep sleep pin :
#define SLEEP_PIN               (0)         // Pin that will trigger sleep. Use pin 0 for testing with boot button on dev board.

// settings for interrupt
#define ESP_INTR_FLAG_DEFAULT   (0)
TaskHandle_t ISR = NULL;

// interrupt service routine for sleep pin
void IRAM_ATTR sleep_isr_handler(void* arg) {
    BaseType_t xYieldRequired;
    // This will resume the sleep task.
    xYieldRequired = xTaskResumeFromISR( ISR );
    // We should switch context so the ISR returns to a different task.
    portYIELD_FROM_ISR( xYieldRequired  );
}

// task that will send device to sleep on pin interrupt ////////////
void sleep_task(void *arg)
{
    gpio_pad_select_gpio(SLEEP_PIN);
    // set the correct direction
    gpio_set_direction(SLEEP_PIN, GPIO_MODE_INPUT);
    // enable interrupt on falling (1->0) edge for button pin. For rising use PIO_INTR_POSEGDE
    gpio_set_intr_type(SLEEP_PIN, GPIO_INTR_NEGEDGE);
    // install ISR service with default configuration
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // attach the interrupt service routine
    gpio_isr_handler_add(SLEEP_PIN, sleep_isr_handler, (void*) SLEEP_PIN);

    
    // The task suspends itself.
    vTaskSuspend( NULL );
    // The task is now suspended, so will not reach here until the ISR resumes it.

    printf("Detected interrup, going to sleep now !\n");

    //Check if the wi-fi manager has been started. Wi-fi needs to be stopped before going to sleep.
    if (wifi_manager_get_esp_netif_ap() != NULL) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }
      
    rtc_gpio_isolate(GPIO_NUM_12);
    esp_deep_sleep_start();
    vTaskDelete(NULL);
}



// Start mdns service. Wi-fi module can then be reached on "mdns-name.local" instead of ip adress
// mDns is using port 5353, this port needs to open in local  firewall.
// more info here: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mdns.html

void start_mdns_service()
{
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    char name[33];
    
    //set hostname
    if (read_from_nvs(name)) {
        mdns_hostname_set(name);
    } else {
        mdns_hostname_set(DEFAULT_HOSTNAME);
    }

    
    //set default instance
    mdns_instance_name_set("wifi_mdns");
    mdns_txt_item_t serviceTxtData[2] = {
        {"board", "esp32"},
        {"location", "/"}
    };
    //initialize service
    ESP_ERROR_CHECK( mdns_service_add("HTTP-WebServer", "_http", "_tcp", 80, serviceTxtData, 2) );

}

void app_main(void)
{
    // Set global log level. Change to ESP_LOG_WARN or ESP_LOG_ERROR for less log output.
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_err_t ret;
    bool format = false;
    // all-in-one function to mount sd card on base path "/sdcard"
    ret = mount_sd_card(format);

    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL){
            ESP_LOGE(TAG, "Failed to mount filesystem. Make sure card is formated ");
        }     
        else        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Make sure SD card lines have pull-up resistors in place.",
                          esp_err_to_name(ret));
        }
    }


     
    // start a freeRtos task to wait for SPI messages. Priority should be high for this task.
    xTaskCreate(SPI_task, "SPI_receiver", 1024*5, NULL, 20, NULL);    
   
    // update time. this will send a "do_get clock" command over uart 2.
    get_clock(3);

   //Initialize NVS partition. If full, then erease it and re-init.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // if true, enable http file server, uart-tcp server and wi-fi manager
    bool wifi_wakeup = false;
    
    //get wake-up pin
    switch (esp_sleep_get_wakeup_cause()) {

        case ESP_SLEEP_WAKEUP_EXT1: {
            uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

            if (wakeup_pin_mask != 0) {
                int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
                printf("Wake up from GPIO %d\n", pin);
                if (pin == WAKEUP1) {
                    wifi_wakeup = true;
                }
            } else {
                printf("Wake up from GPIO\n");

            }
            break;
        }
        /*case ESP_SLEEP_WAKEUP_TOUCHPAD: {
            printf("Wake up from touch on pad %d\n", esp_sleep_get_touchpad_wakeup_status());
            break;
        }*/
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            printf("Not a deep sleep reset\n");
            //if (gpio_get_level(WAKEUP1) == 1) {
                wifi_wakeup = true;
            //}
    }

    const int ext_wakeup_pin_1 = WAKEUP1;
    const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
    const int ext_wakeup_pin_2 = WAKEUP2;
    const uint64_t ext_wakeup_pin_2_mask = 1ULL << ext_wakeup_pin_2;

    printf("Enabling EXT1 wakeup on pin GPIO%d and GPIO%d\n", ext_wakeup_pin_1, ext_wakeup_pin_2);
    // enable wakeup from sleep
    esp_sleep_enable_ext1_wakeup(ext_wakeup_pin_1_mask | ext_wakeup_pin_2_mask, ESP_EXT1_WAKEUP_ANY_HIGH);


    // Start sleep task. Will will react to interupt on a configured pin and put device to sleep
    xTaskCreate( sleep_task, "sleep_task", 1*1024, NULL , 4, &ISR );

    
    if (wifi_wakeup == true) {
        
        // Attempt to get node description by sending "get node description" command over uart2
        // Save the name to nvs partition. This name will be used for SSID name for access point and mDNS host-name.
        // Maximum allowed charachters in a SSID name is 32.
        // DNS names can contain only alphabetical characters (A-Z), numeric characters (0-9), the minus sign (-), and the period (.)
        char * nodeDescription = (char * )malloc(MAX_SSID_SIZE + 1); 

        if (get_node_description(nodeDescription, 1) == 1 ) {
            char ssid[MAX_SSID_SIZE + 1]; // 
            read_from_nvs(ssid);
            if (strcmp(nodeDescription, ssid) == 0) {
                ESP_LOGI(TAG, "Node description already saved to NVS\n");
            } else { 
                write_to_nvs(nodeDescription);                    
                ESP_LOGI(TAG, "Node description written to NVS: %s", ssid); 
            }
        } else{
            ESP_LOGI(TAG, "Timed out getting name from sensor");
        }
        free(nodeDescription);
        
        
        // ////////////////////////////////////////////////////////
        // Start all TASKS below                        //

        /* Start the Wi-Fi Manager */
        wifi_manager_start();
        
        /* Start the HTTP web server*/
        start_file_server(SD_MOUNT);
        
        /* Start the mDNS service */
        start_mdns_service();
        
        /* Start the TCP web server*/
        start_tcp_server_task();
        
        /* Start UART RX to TCP Socet task*/
        xTaskCreate(rx_task, "uart_rx_task", 1024*4, NULL, 10, NULL); 

    }

// If this is the first run on new image, mark it as valid. 
// Only valid if ONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is enabled in sdkconfig.
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE

    const esp_partition_t *running = esp_ota_get_running_partition();
    
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "First boot of new firmware, marking this as the new valid firmware");
        }
    } 

#endif

}
