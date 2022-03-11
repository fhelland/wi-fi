/*  TCP Server

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/unistd.h>


#include "esp_log.h"
#include "driver/uart.h"
#include <sys/param.h>
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "driver/gpio.h"
#include "soc/uart_reg.h"


#include "uart_tcp_server.h"

// Socket file stream
static int sock = - 1;

static const char *TAG = "TCP_server";

void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };


    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(EX_UART_NUM, UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    uart_set_sw_flow_ctrl(EX_UART_NUM, true, 1, 120);       /* enable xon/xoff flow control. FIFO buffer is 128 Bytes. */
}

void rx_task(void *arg)
{
    static const char *RX_TASK_TAG = "UART_RX_TASK";
    if (!uart_is_driver_installed(EX_UART_NUM)) {
        uart_init();
    }
    
    char* data = (char*) malloc(UART_RX_BUF_SIZE);
    int written = 1;
    while (1)
    {
        memset(data, 0xdd, UART_RX_BUF_SIZE);
        const int rxBytes = uart_read_bytes(EX_UART_NUM, data, UART_RX_BUF_SIZE, 100 / portTICK_PERIOD_MS);
        if (rxBytes > 0 ) {

            // send() command can return less bytes than supplied length.
            // Walk-around for robust implementation.
            int to_write = rxBytes; 
            while (to_write > 0) {
                // send will attempt to send data received on uart2 to a socket. If socket is not connected, send will return -1.
                // Otherwise it will return the number of bytes actually sent.
                written = send(sock, data + (rxBytes - to_write), to_write, 0);
                
                ESP_LOGI(RX_TASK_TAG, "Received %i bytes from UART. Sent to SOCKET: %d bytes", rxBytes, written);

                if (written < 0) {
                    ESP_LOGW(RX_TASK_TAG, "Error occurred during sending to socket: Error no: %d", errno);
                    break;
                }
                to_write -= written;
           }
        }
    } 

    free(data);
    uart_driver_delete(EX_UART_NUM);
    ESP_LOGI(RX_TASK_TAG, "Uart driver uninstalled");
    vTaskDelete(NULL);
}



/* Function to receive data on open socket and retransmit to uart */
static void do_retransmit(const int sock)       
{
    int len;
    char* rx_buffer = (char*) malloc(RX_BUF_SIZE);
    static const char *TX_TASK_TAG = "SOCKET_RX_TASK";
    esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);
    //size_t uart_buf_len = -1;
    do {
        //memset(rx_buffer, 0xee, RX_BUF_SIZE);
        len = recv(sock, rx_buffer, RX_BUF_SIZE, 0);

        if (len < 0) {
            ESP_LOGE(TX_TASK_TAG, "Error occurred during receiving: (%d)", errno);
        } else if (len == 0) {
            ESP_LOGW(TX_TASK_TAG, "Connection closed");
        } else {
            //ESP_LOGI(TX_TASK_TAG, "Received %d bytes from socket", len);
            const int txBytes = uart_write_bytes(EX_UART_NUM, rx_buffer, len);
            ESP_LOGI(TX_TASK_TAG, "Received %d bytes from socket. Sent %i bytes to UART", len, txBytes);
        }
        //printf("Free memmory: %i KB\n", esp_get_free_heap_size() / 1024);
        //printf("Lowest free memmory since boot: %i KB\n", esp_get_minimum_free_heap_size() / 1024);
    } while (len > 0);
    free(rx_buffer);
    ESP_LOGI(TX_TASK_TAG, "Exiting socket transmit function");

}

void tcp_server_task(void *pvParameters)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }
#ifdef CONFIG_TCP_SERVER_IPV6
    else if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_TCP_SERVER_IPV4) && defined(CONFIG_TCP_SERVER_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#ifdef CONFIG_TCP_SERVER_IPV6
        else if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        do_retransmit(sock); // receive data on open socket

        shutdown(sock, 0);
        close(sock);
        sock = -1;
    }

CLEAN_UP:
    ESP_LOGE(TAG, "Closing TCP-server Task, reboot device to start again");
    close(listen_sock);
    vTaskDelete(NULL);
    //start_tcp_server_task();        //restart the task.

}

void   start_tcp_server_task(void) 
{

#ifdef CONFIG_TCP_SERVER_IPV4
    xTaskCreate(tcp_server_task, "tcp_server", 1024*4, (void*)AF_INET, 12, NULL);
#endif
#ifdef CONFIG_TCP_SERVER_IPV6
    xTaskCreate(tcp_server_task, "tcp_server", 1024*4, (void*)AF_INET6, 12, NULL);
#endif
}

uint8_t read_from_nvs(char *out_string)
{
	nvs_handle my_handle;
	uint8_t status = 0;
    esp_err_t esp_err;
	size_t sz;
    const char *NVS_READ_TAG = "NVS READ";
    //ESP_LOGI(NVS_READ_TAG,"Opening Non-Volatile Storage (NVS) handle for reading... ");
    esp_err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (esp_err != ESP_OK) {
        ESP_LOGI(NVS_READ_TAG,"Error opening NVS handle!  (%s|)  \n", esp_err_to_name(esp_err));
        status = 0;
        return status;
    }

    //ESP_LOGI(NVS_READ_TAG, "Reading name from NVS!!");

    esp_err = nvs_get_str(my_handle, "ssid_name", NULL, &sz);
    if (sz > 32) sz = 32;         // SSID name is maximum 32 characters long
    esp_err = nvs_get_str(my_handle, "ssid_name", out_string, &sz);

    if(esp_err == ESP_OK) {
        status = 1;
    }
    else {
        status = 0;
        // ESP_LOGI(NVS_READ_TAG,"Error getting ssid_name: (%s)", esp_err_to_name(esp_err));
    }
    nvs_close(my_handle);
    return status;
}

uint8_t write_to_nvs(char *in_string)
{
    nvs_handle my_handle;
	esp_err_t esp_err;
	uint8_t status = 0;
    
    
    esp_err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (esp_err != ESP_OK) {
        
        status = 0;
        return status;
    }
    
    esp_err = nvs_set_str(my_handle, "ssid_name", in_string);
    if (esp_err != ESP_OK) {
        
        status = 0;
    } else {
        status = 1;
    }
    
    nvs_commit(my_handle);
    nvs_close(my_handle);
    return status;
}

// Function to replace orig character with rep character in string str
void replacechar(char *str, char orig, char rep) {
    char *ix = str;
    while((ix = strchr(ix, orig)) != NULL) {
        *ix++ = rep;
    }
}

// Attempt to update clock from sending do_get clock command overt uart
// input: size_t n_times: Retry attempts if it fails to receive clock.
uint8_t get_clock(size_t n_times)
{

    if (!uart_is_driver_installed(EX_UART_NUM)) {
        uart_init();
    }

    char* buf = (char*) malloc(129);
    char* cmd = "do_get clock\n";
    uint8_t status = 0;
    int rxBytes, error = 0;
    struct tm tm;
    char * ptr = NULL;
    //int ch = '\n';
    
    do 
    {
        uart_write_bytes(EX_UART_NUM, cmd, strlen(cmd));
        rxBytes = uart_read_bytes(EX_UART_NUM, buf, 128, 20 / portTICK_PERIOD_MS);
        if (rxBytes > 0) {
            buf[rxBytes] = '\0'; //Null-terminate
            
            // check that the first character is a '#'
            if (strncmp(buf, "#", 1) == 0) {
                // clock received
                status = 1;
            } else {
                // in case we received something unexpected, wait until uart is free.
                error++;
                printf("UART busy, waiting...\n");
                while (uart_read_bytes(EX_UART_NUM, buf, 128, 20 / portTICK_PERIOD_MS) > 0) {
                    
                }
            }

        } else {
            ESP_LOGI(TAG, "ERROR  no reply from sensor");
            //uart_flush_input(EX_UART_NUM);
            error++;
            vTaskDelay (500 / portTICK_PERIOD_MS );
        }

        if (error > n_times) {
            ESP_LOGW(TAG, "Too many errors, aborting update clock");
            status = 0;
            return status;
        }

    } while (status == 0);
    
    //strip out date from received message:
    
    ptr = strrchr(buf, '#'); //Find last occurence of # and replace it with a NULL byte
    if (ptr != NULL) {
        buf[(int)(ptr - buf - 1)] = '\0';
    }
    
    ptr = strchr(buf, '#'); //Find first occurence of #
    if (ptr != NULL) {
        
        if (strptime(ptr+2, "%Y.%m.%d %H:%M:%S", &tm) != NULL) {    // Need to add 2 to bypass # and end-of-line characters
            time_t t = mktime(&tm);
            ESP_LOGI(TAG, "Setting time: %s\n", asctime(&tm));
            struct timeval now = { .tv_sec = t};
            // This will update system time on wi-fi module.
            settimeofday(&now, NULL);
        }
    } else {
        ESP_LOGW(TAG, "Failed getting date from uart");
        status = 0;
    }
    
    free(buf);

    uart_driver_delete(EX_UART_NUM);

    return status;
}

uint8_t get_node_description(char * out_string, size_t max_tries)
{
    if (!uart_is_driver_installed(EX_UART_NUM)) {
        uart_init();
    }
    char* buf = (char*) malloc(129);
    char* cmd = "get_node description\n";
    int iCharsConsumed, end, len, rxBytes, error_counter=0;
    uint8_t status = 0;
    char *pch;

    do {
        uart_write_bytes(EX_UART_NUM, cmd, strlen(cmd));
        rxBytes = uart_read_bytes(EX_UART_NUM, buf, 128, 20 / portTICK_PERIOD_MS);
        
        if (rxBytes > 0) {
            buf[rxBytes] = 0; // Null terminate.
            //ESP_LOGI("GET SSID", "Received the following %i characters: %s\n",rxBytes, buf );
            pch = strcasestr( buf, "Node Description" );
            
            if (pch == NULL) 
            {
                error_counter++;
                printf("UART busy, waiting...\n");
                while (uart_read_bytes(EX_UART_NUM, buf, 128, 20 / portTICK_PERIOD_MS) > 0) {
                    
                }
                
            }    
        }
        else {
            uart_flush_input(EX_UART_NUM); 
            //vTaskDelay (1000 / portTICK_PERIOD_MS );
            error_counter++;
        } 
        if (error_counter > max_tries) {
            ESP_LOGI("GET SSID", "ERROR getting node description");
            status = 0;
            return status;
        }
    } while ((pch = strcasestr( buf, "Node Description")) == NULL);
  
    int product, serial;

    /* extract product no, serial no and name from received string. */
    sscanf(pch, "%*s %*s %d %d %n", &product, &serial, &iCharsConsumed); //%32[0-9a-zA-Z-.]
    end = strcspn (pch,"\n");
    len = end - iCharsConsumed;

    strlcpy(out_string, pch + iCharsConsumed, len - 1);

    // Replace som characters to comply with dns name standard (for mDNS hostname)
    replacechar(out_string, ' ', '-');
    replacechar(out_string, '#', '0');

    ESP_LOGI(TAG, "Got the following name: %s", out_string);
    free(buf);
    uart_flush_input(EX_UART_NUM);
    uart_driver_delete(EX_UART_NUM);
    status = 1;
    return status;
}