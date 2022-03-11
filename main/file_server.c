/* HTTP File Server Example



*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#include "sdmmc.h"
#include "uart_tcp_server.h"
#include "wifi_manager.h"
#include "file_server.h"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX 255 // (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define FOLDER_PATH 128
/* Max size of an individual file. Make sure this
 * value is same as that set in html file*/
#define MAX_FILE_SIZE (200 * 1024 * 1024) // 200 MB
#define MAX_FILE_SIZE_STR "200MB"

/* Scratch buffer size */
/* This is the buffer used to upload and download files from sd card. */
/* Larger buffer will mean, higher troughput. */
#define SCRATCH_BUFSIZE (16 * 1024)

/* Internal read and write buffers size. Used when reading and writing files on fat partition. */
/* By default this is 128 byte. This can increase troughput, especially the read buffer size is important for download speed */
#define READ_BUF (4 * 1024)
#define WRITE_BUF (4 * 1024)



static const char *TAG = "http_server";


/* FreeRTOS handle used for connection notifications between this task and the wi-fi manager */
static TaskHandle_t xTaskToNotify = NULL;

/* Status for firmware update*/
static int flash_status;

/* Error description for firmware update*/
static char flash_error[50] = {0};

struct file_server_data
{
    /* Base path of file storage */
    char base_path[16];
    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

// Receive message back from the wi-fi manager after connect/disconnect/scan.
void xTask_connection_give()
{
    if ( xTaskToNotify != NULL ) {
        /* Notify the task that job is done */
        xTaskNotifyGive( xTaskToNotify );
    }
}

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0); // Response body can be empty
    return ESP_OK;
}

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t file_get_handler(httpd_req_t *req)
{
    extern const unsigned char file_png_start[] asm("_binary_file_png_start");
    extern const unsigned char file_png_end[] asm("_binary_file_png_end");
    const size_t file_png_size = (file_png_end - file_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)file_png_start, file_png_size);
    return ESP_OK;
}

static esp_err_t folder_get_handler(httpd_req_t *req)
{
    extern const unsigned char folder_png_start[] asm("_binary_folder_png_start");
    extern const unsigned char folder_png_end[] asm("_binary_folder_png_end");
    const size_t folder_png_size = (folder_png_end - folder_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)folder_png_start, folder_png_size);
    return ESP_OK;
}

/* Handler to respond with an logo file embedded in flash.
 * Browsers expect to GET website icon at URI /logo.png.
 * This can be overridden by uploading file with same name */
static esp_err_t logo_get_handler(httpd_req_t *req)
{
    extern const unsigned char logo_png_start[] asm("_binary_logo_png_start");
    extern const unsigned char logo_png_end[] asm("_binary_logo_png_end");
    const size_t logo_png_size = (logo_png_end - logo_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)logo_png_start, logo_png_size);
    return ESP_OK;
}

static esp_err_t back_get_handler(httpd_req_t *req)
{
    extern const unsigned char back_png_start[] asm("_binary_back_png_start");
    extern const unsigned char back_png_end[] asm("_binary_back_png_end");
    const size_t back_png_size = (back_png_end - back_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)back_png_start, back_png_size);
    return ESP_OK;
}

static esp_err_t home_get_handler(httpd_req_t *req)
{
    extern const unsigned char home_png_start[] asm("_binary_home_png_start");
    extern const unsigned char home_png_end[] asm("_binary_home_png_end");
    const size_t home_png_size = (home_png_end - home_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
    httpd_resp_send(req, (const char *)home_png_start, home_png_size);
    return ESP_OK;
}

static esp_err_t wifi_resp_html(httpd_req_t *req) 
{
    char port[6];
    char hostname[33];

    wifi_config_t ap_wifi_conf = {};

    esp_wifi_get_config(ESP_IF_WIFI_AP, &ap_wifi_conf);

    esp_netif_ip_info_t ip_info = {};
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    esp_netif_t* esp_netif_ap = wifi_manager_get_esp_netif_ap();

	esp_netif_get_ip_info(esp_netif_ap, &ip_info);
    
	char ip[IP4ADDR_STRLEN_MAX]; /* note: IP4ADDR_STRLEN_MAX is defined in lwip */

	esp_ip4addr_ntoa(&ip_info.ip, ip, IP4ADDR_STRLEN_MAX);    
    
    if (!read_from_nvs(hostname)) {
        strcpy(hostname, DEFAULT_HOSTNAME);
    }
    /* Get handle to embedded html file */
    extern const unsigned char wifi_start[] asm("_binary_wifi_html_start");
    extern const unsigned char wifi_end[] asm("_binary_wifi_html_end");
    const size_t wifi_size = (wifi_end - wifi_start);
    httpd_resp_send_chunk(req, (const char *)wifi_start, wifi_size);

    httpd_resp_sendstr_chunk(req,   "<div class=\"row\">"
                                    "<div class=\"column\">"
                                    "<h2>Access Point Info</h2>"
                                    "<table>"
                                    "<tbody>"
                                    "<tr><td><strong>Name:</strong></td><td>");
    
    httpd_resp_sendstr_chunk(req,   (char *)ap_wifi_conf.ap.ssid);
    httpd_resp_sendstr_chunk(req,   "</td></tr><tr><td><strong>IP adress:</strong></td><td>");
    httpd_resp_sendstr_chunk(req,   ip);       
 /*   httpd_resp_sendstr_chunk(req,   "</td></tr><tr><td><strong>Gateway:</strong></td><td>");   
    httpd_resp_sendstr_chunk(req,   gw);       
    httpd_resp_sendstr_chunk(req,   "</td></tr><tr><td><strong>Netmask:</strong></td><td>");   
    httpd_resp_sendstr_chunk(req,   netmask);
 */   httpd_resp_sendstr_chunk(req,   "</td></tr><tr><td><strong>TCP Server port:</strong></td><td>");     
    sprintf(port, "%i", PORT);
    httpd_resp_sendstr_chunk(req,   port);
    httpd_resp_sendstr_chunk(req,   "</td></tr><tr><td><strong>Hostname:</strong></td><td><a href=\"http://");   
    httpd_resp_sendstr_chunk(req,   hostname);
    httpd_resp_sendstr_chunk(req,   ".local\">");  
    httpd_resp_sendstr_chunk(req,   hostname);
    httpd_resp_sendstr_chunk(req,   "</a></td>"); 
    httpd_resp_sendstr_chunk(req,   "</tr>"
                                    "</tbody>"
                                    "</table>"
                                    "</div>");
    httpd_resp_sendstr_chunk(req,   "<div id=\"ap-info\" class=\"column\"></div></div>");

    httpd_resp_sendstr_chunk(req,   "<hr><div style=\"margin: 10px;\"><h2>Wi-Fi Networks</h2><div id=\"ap-list\"></div></div>");
    httpd_resp_sendstr_chunk(req,    "</body></html>");
    httpd_resp_sendstr_chunk(req,   NULL);

    return ESP_OK;
}

static esp_err_t ap_list_handler(httpd_req_t *req)
{
    if(wifi_manager_lock_json_buffer(( TickType_t ) 10)) {
        char* ap_buf = wifi_manager_get_ap_list_json();

        if (strlen(ap_buf) <= 5 )
        {   
            /* Store the handle of the calling task. */
            xTaskToNotify = xTaskGetCurrentTaskHandle();
            
            wifi_manager_unlock_json_buffer();
            
            xTaskNotifyStateClear(xTaskToNotify);
            
            wifi_manager_scan_async();
            
            uint32_t ulNotificationValue;
        
            const TickType_t xMaxBlockTime = 10000 / portTICK_RATE_MS;// pdMS_TO_TICKS ( 100000 ) ;

            //Block until scan is done or until timeout
            ulNotificationValue = ulTaskNotifyTake( pdTRUE, xMaxBlockTime );

            if (ulNotificationValue == 1) printf("Wi-FI scan done event\n");
            
            else printf("Scan event timeout\n");

            wifi_manager_lock_json_buffer(( TickType_t ) 10);

            xTaskToNotify = NULL;    

        }
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
		httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_send(req, ap_buf, strlen(ap_buf));
        ESP_LOGI(TAG, "Access points sent to html");
        wifi_manager_unlock_json_buffer();
        wifi_manager_scan_async();
        return ESP_OK;
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_send(req, NULL, 0);
		ESP_LOGE(TAG, "No access points found");
        wifi_manager_scan_async();
		return ESP_FAIL;	
    }
}


static esp_err_t connect_handler(httpd_req_t *req)
{    
    uint32_t ulNotificationValue = 0;
    TickType_t xMaxBlockTime = 0;

    xTaskToNotify = xTaskGetCurrentTaskHandle();
    
    xTaskNotifyStateClear(xTaskToNotify);


    /* We need to check if we have an IP adress to test if we are already connected to a network. If we are, we need to trigger a disconnect first */
    /* If we are not connected, IP adress will be 0.0.0.0 */

    ////////////// the following code will get the current ip adress //////
    esp_netif_ip_info_t ip_info = {};
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    esp_netif_t* esp_netif_sta = wifi_manager_get_esp_netif_sta();
	esp_netif_get_ip_info(esp_netif_sta, &ip_info);
    char ip[IP4ADDR_STRLEN_MAX];
    sprintf(ip, IPSTR,IP2STR(&ip_info.ip) );
    /////////////////////////////////////////////////////////////////////

    if (strcmp(ip, "0.0.0.0") != 0) {
        printf("Already connected to another network, triggering disconnect!!!\n");

        xTaskNotifyStateClear(xTaskToNotify);

        wifi_manager_disconnect_async(); 
        
        // Block until disconnection is done or until timeout. Disconnect should be quick.
        xMaxBlockTime = 10000 / portTICK_RATE_MS;
        ulNotificationValue = ulTaskNotifyTake(  pdTRUE, xMaxBlockTime );

    }
    
    /* buffers for the headers */
    size_t ssid_len = 0, password_len = 0, username_len = 0;
    char *ssid = NULL, *password = NULL, *username; 
    /* length of values provided */
	ssid_len = httpd_req_get_hdr_value_len(req, "X-Custom-ssid");
    username_len = httpd_req_get_hdr_value_len(req, "X-Custom-user");
	password_len = httpd_req_get_hdr_value_len(req, "X-Custom-pwd");

    wifi_config_t* config = wifi_manager_get_wifi_sta_config();
    wpa_enterprise_settings_t* wpa_ent = wifi_manager_get_wpa_enterprise_login();
    
    memset(wpa_ent, 0x00, sizeof(wpa_enterprise_settings_t));
	memset(config, 0x00, sizeof(wifi_config_t));
    
    if (username_len > 0 && username_len <= USERNAME_LEN_MAX) 
    {
        ssid = malloc(sizeof(char) * (ssid_len + 1));
        username = malloc(sizeof(char) * (username_len + 1));
		password = malloc(sizeof(char) * (password_len + 1));
        httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, ssid_len+1);
		httpd_req_get_hdr_value_str(req, "X-Custom-user", username, username_len+1);
		httpd_req_get_hdr_value_str(req, "X-Custom-pwd", password, password_len+1);


        memcpy(config->sta.ssid, ssid, ssid_len);  
        memcpy(wpa_ent->user, username, username_len);
        memcpy(wpa_ent->password, password, password_len);

        wpa_ent->user_len = username_len;
        wpa_ent->password_len = password_len;

        wifi_manager_connect_async();

        ESP_LOGI(TAG, "Waiting for task notification");
        
        uint32_t ulNotificationValue;

        // Block until connection is done or until timeout. 
        // Sometimes a connection prosess can take long time, ex if a wrong password is supplied for a wpa enterprise network    
        const TickType_t xMaxBlockTime = (30000 / portTICK_RATE_MS); 
        ulNotificationValue = ulTaskNotifyTake( pdTRUE, xMaxBlockTime );



        if( ulNotificationValue == 1 ) {     
            ESP_LOGI(TAG,"Got connection status");

        } else {
            ESP_LOGI(TAG,"Connection status timed out");
        }
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_send(req, NULL, 0 );
    
        free(ssid);
        free(username);
        free(password);
        xTaskToNotify = NULL;
        return ESP_OK;

	}       
    else if(ssid_len && ssid_len <= MAX_SSID_SIZE && password_len <= MAX_PASSWORD_SIZE ){

		/* get the actual value of the headers */
		ssid = malloc(sizeof(char) * (ssid_len + 1));
		password = malloc(sizeof(char) * (password_len + 1));
		httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, ssid_len+1);
		httpd_req_get_hdr_value_str(req, "X-Custom-pwd", password, password_len+1);

		memcpy(config->sta.ssid, ssid, ssid_len);
		memcpy(config->sta.password, password, password_len);

		ESP_LOGI(TAG, "ssid: %s, password: %s", ssid, password);
		ESP_LOGD(TAG, "http_server_post_handler: wifi_manager_connect_async() call");
		
        wifi_manager_connect_async();

        ESP_LOGI(TAG, "Waiting for task notification");    
           
        xMaxBlockTime = (30000 / portTICK_RATE_MS);

        //Block until connection is done or until timeout
        ulNotificationValue = ulTaskNotifyTake( pdTRUE, xMaxBlockTime );
        
        if( ulNotificationValue == 1 ) {
            ESP_LOGI(TAG,"Got connection status");

        } else {
            ESP_LOGI(TAG,"Connection status timed out");
        }
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_send(req, NULL, 0 );
        
        /* free memory */
        free(ssid);
        free(password);
        xTaskToNotify = NULL;
        return ESP_OK;
    } else
    {
		/* bad request, the authentification header is not complete/not the correct format */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error, bad request!");
        xTaskToNotify = NULL;
        return ESP_FAIL;
	}
}

static esp_err_t connect_status_handler(httpd_req_t *req)
{
    //char * json_response = NULL; 
	if(wifi_manager_lock_json_buffer(( TickType_t ) 10)){
		char *buff = wifi_manager_get_ip_info_json();
		if(buff){
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_set_status(req, "200 OK");
            httpd_resp_set_type(req, "application/json");
			httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
			httpd_resp_set_hdr(req, "Pragma", "no-cache");
			httpd_resp_send(req, buff, strlen(buff));
			wifi_manager_unlock_json_buffer();
            return ESP_OK;
		}
		else{
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_send(req, NULL, 0);
            return ESP_FAIL;
		}
	}
	else{
        httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_send(req, NULL, 0);
		ESP_LOGE(TAG, "http_server_netconn_serve: GET /status.json failed to obtain mutex");
        return ESP_FAIL;
	}
}

/* Handler for disconnecting from a network */
static esp_err_t disconnect_handler(httpd_req_t *req)
{
        wifi_manager_disconnect_async();
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "200 OK");
    	httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
		httpd_resp_set_hdr(req, "Pragma", "no-cache");
        //httpd_resp_set_hdr(req, "Location", "/?wifi");
        httpd_resp_send(req, NULL, 0); // Response body can be empty
        
        return ESP_OK;
}

static esp_err_t upgrade_resp_html(httpd_req_t *req) //const char *dirpath
{
    struct tm * nowtm;
    time_t t;
    time (&t);
    nowtm = localtime (&t);
    char cardname[8], temp[24];
    uint16_t card_freq = 0;
    uint32_t tot=0, free=0;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    esp_ota_get_partition_description(running, &running_app_info);
    char slot[10];
    char address[10];
    sprintf(address, "0x%X", running->address);
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)
    {
        strcpy(slot, "factory");
    }
    else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
    {
        strcpy(slot, "OTA 0");
    }
    else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
    {
        strcpy(slot, "OTA 1");
    }
    else sprintf(slot, "slot %x", running->subtype);

    /* Send HTML file header */
    /* Get handle to embedded html file */
    extern const unsigned char upgrade_start[] asm("_binary_upgrade_html_start");
    extern const unsigned char upgrade_end[] asm("_binary_upgrade_html_end");
    const size_t upgrade_size = (upgrade_end - upgrade_start);

   /* Send the rest of the html code*/

    httpd_resp_send_chunk(req, (const char *)upgrade_start, upgrade_size);

    httpd_resp_sendstr_chunk(req, "<div class=\"firmwareID\"><h3>Current running image</h3>"
                                  "</br><p><strong>Version: </strong>");
    httpd_resp_sendstr_chunk(req, running_app_info.version);
    httpd_resp_sendstr_chunk(req, "</p><p><strong>Compile date: </strong>");
    httpd_resp_sendstr_chunk(req, running_app_info.date);
    httpd_resp_sendstr_chunk(req, " ");
    httpd_resp_sendstr_chunk(req, running_app_info.time);
    httpd_resp_sendstr_chunk(req, "</p><p><strong>ESP-IDF version: </strong>");
    httpd_resp_sendstr_chunk(req, running_app_info.idf_ver);
    httpd_resp_sendstr_chunk(req, "</p><p><strong>Current OTA slot: </strong>");
    httpd_resp_sendstr_chunk(req, slot);
    httpd_resp_sendstr_chunk(req, "</p><p><strong>Slot adress: </strong>");
    httpd_resp_sendstr_chunk(req, address);
    httpd_resp_sendstr_chunk(req, "</p><hr><h3>System time</h3><p>");
    httpd_resp_sendstr_chunk(req, asctime(nowtm));
    httpd_resp_sendstr_chunk(req, "</p><hr><h3>Memory</h3><p><strong>Free memmory:  </strong>");
    sprintf(temp, "%i kB", esp_get_free_heap_size() / 1024 );
    httpd_resp_sendstr_chunk(req,  temp);
    httpd_resp_sendstr_chunk(req, "</p><p><strong>Memory low mark  </strong>");
    sprintf(temp, "%i kB", esp_get_minimum_free_heap_size() / 1024 );
    httpd_resp_sendstr_chunk(req,  temp);
    httpd_resp_sendstr_chunk(req, "</p><hr><h3>SD Card</h3>");
    
    
    if (get_sdcard_info(cardname, &card_freq) == 1) {
        
        httpd_resp_sendstr_chunk(req, "<p><strong>Name: </strong>");  
        httpd_resp_sendstr_chunk(req,  cardname );
        if (card_freq == 20000) httpd_resp_sendstr_chunk(req, "</p><p><strong>Card speed: </strong>Default speed");
        else if (card_freq == 40000) httpd_resp_sendstr_chunk(req, "</p><p><strong>Card speed: </strong>High speed");
        else {
            if (card_freq < 1000) {
                sprintf(temp, "%i kHz\n", card_freq);
            } else {
                sprintf(temp, "%i MHz\n", card_freq / 1000);
            }
            httpd_resp_sendstr_chunk(req, "</p><p><strong>Card speed: </strong>");
            httpd_resp_sendstr_chunk(req, temp);
            httpd_resp_sendstr_chunk(req, "</p>");
        }
    } else {
        httpd_resp_sendstr_chunk(req, "<p><strong>No SD card detected</strong></p><br>"); 
    }
    if (get_freespace_sd(&tot, &free) == 1) {
        httpd_resp_sendstr_chunk(req, "<p><strong>Size: </strong>");  
        sprintf(temp, "%u MB\n", tot / 1024 );
        httpd_resp_sendstr_chunk(req,   temp);
        httpd_resp_sendstr_chunk(req, "</p><p><strong>Free space: </strong>");  
        sprintf(temp, "%u MB\n", free / 1024 );
        httpd_resp_sendstr_chunk(req,   temp);
        httpd_resp_sendstr_chunk(req, "</p>");
    }

    httpd_resp_sendstr_chunk(req,  "<button type=\"button\" onclick=\"partitionSDcard()\">Format</button>");
    
    httpd_resp_sendstr_chunk(req,  "<span class=\"loader-1\"></span><h4 id=\"format-status\"></h4>");
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Send HTTP response with a run-time generated html consisting of
 * a list of all files and folders under the requested path. */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
    char entrypath[FILE_PATH_MAX];
    char entrysize[16];
    const char *entrytype;
    struct dirent *entry;
    
    const size_t dirpath_len = strlen(dirpath);
    //dirpath[dirpath_len] = '\0';
    char dirpath_mod[dirpath_len + 1];
    strlcpy(dirpath_mod, dirpath, dirpath_len); //remove trailing '/' from dirpath.

    uint32_t free_kb = 0, tot_kb = 0;

    DIR *dir;
    dir = opendir(dirpath_mod);


    /* Retrieve the base path of file storage to construct the full path */
    strlcpy(entrypath, dirpath, sizeof(entrypath));


    /* Get handle to embedded file upload script */
    extern const unsigned char file_manager_start[] asm("_binary_file_manager_html_start");
    extern const unsigned char file_manager_end[] asm("_binary_file_manager_html_end");
    const size_t file_manager_size = (file_manager_end - file_manager_start);

    /* Add file upload form and script which on execution sends a POST request to /upload */

    httpd_resp_send_chunk(req, (const char *)file_manager_start, file_manager_size);

    if (!dir)
    {   
        httpd_resp_sendstr_chunk(req, "<h2 style=\"margin: 10px;\">Directory does not exist</h2>");
        httpd_resp_sendstr_chunk(req, "</body></html>");
    
        /* Send empty chunk to signal HTTP response completion */
        httpd_resp_sendstr_chunk(req, NULL);
        ESP_LOGE(TAG, "Failed to stat dir : %s", dirpath); 
        return ESP_FAIL;
    }

    

    httpd_resp_sendstr_chunk(req, "<div id=\"top\">"
                                  "<a style=\"padding-right: 10px;\" href=\"");
    httpd_resp_sendstr_chunk(req, req->uri);

    //httpd_resp_sendstr_chunk(req, "../\"><img src=\"/back.png\" width=\"16\" height=\"16\"> Back</a>"
      //                            "<a href=\"/\"/><img src=\"/home.png\" width=\"16\" height=\"16\"> Home</a>");
    httpd_resp_sendstr_chunk(req, "../\">&#x2190 Back</a>"
                                  "<a href=\"/\"/>&#x2302 Home</a>");


    /* This will create a clickable current folder link*/
    char buf[dirpath_len];
    char buf2[FOLDER_PATH] = {0};
    const size_t base_len = strlen(((struct file_server_data *)req->user_ctx)->base_path);
    const char *PATTERN = "/";
    const char *uri_path = dirpath + base_len;
    char *start = strstr(uri_path, PATTERN);
    char *end;
    int len1, len2;
    end = strstr(start + 1, PATTERN);
    while (end != NULL)
    {
        len1 = end - uri_path;
        len2 = end - start;
        strlcpy(buf2, start + 1, len2);
        strncpy(buf, uri_path, len1);
        buf[len1] = '/';
        buf[len1 + 1] = '\0';
        start = end;
        end = strstr(start + 1, PATTERN);
        if (end != NULL)
        {
            httpd_resp_sendstr_chunk(req, " &#10095 <a href= \"");
            httpd_resp_sendstr_chunk(req, buf);
            httpd_resp_sendstr_chunk(req, "\">");
            httpd_resp_sendstr_chunk(req, buf2);
            httpd_resp_sendstr_chunk(req, "</a>");
        }
        else
        {
            httpd_resp_sendstr_chunk(req, " &#10095 <strong>");
            httpd_resp_sendstr_chunk(req, buf2);
            httpd_resp_sendstr_chunk(req, "</strong>");
        }
    }
    httpd_resp_sendstr_chunk(req, "</div>");


    /* Send file-list table definition and column labels */
    httpd_resp_sendstr_chunk(req,
                             "<table id=\"files\" border=\"0\">"
                             "<col width=\"450px\" /><col width=\"100px\" /><col width=\"100px\" /><col width=\"200px\" /><col width=\"50px\" />"
                             "<thead><tr><th onclick=\"onColumnHeaderClicked(event)\">Name</th><th>Type</th><th onclick=\"onColumnHeaderClicked(event)\">Size</th>"
                             "<th onclick=\"onColumnHeaderClicked(event)\">Date</th><th><label><input type=\"checkbox\" onClick=\"toggle(this)\"></label></th>");

    httpd_resp_sendstr_chunk(req, "</tr>"
                                  "</thead>"
                                  "<tbody>");

    size_t folders = 0, files = 0;
    while ((entry = readdir(dir)) != NULL)
    {

        if (entry->d_type == DT_DIR)
        {
            folders++;
            entrytype = "directory";

            /* Send chunk of HTML file containing table entries with file name and size */
            httpd_resp_sendstr_chunk(req, "<tr><td><a href=\"");
            httpd_resp_sendstr_chunk(req, req->uri);
            httpd_resp_sendstr_chunk(req, entry->d_name);
            if (entry->d_type == DT_DIR)
            {
                httpd_resp_sendstr_chunk(req, "/");
            }
            httpd_resp_sendstr_chunk(req, "\"> "  
                                          "<img src=\"/folder.png\" width=\"16\" height=\"16\">");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "</a></td><td>");
            httpd_resp_sendstr_chunk(req, entrytype);
            httpd_resp_sendstr_chunk(req, "</td><td>"
                                          "</td><td>");
            httpd_resp_sendstr_chunk(req,  "</td><td><label><input type=\"checkbox\" value=\""); //class=\"delete\"></td></tr>");
            httpd_resp_sendstr_chunk(req, req->uri);
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "/\" name=\"");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "\"></label></td></tr>");
        }
    }

    
    float f_size = 0;

    rewinddir(dir); /* reset position */

    struct stat entry_stat;

    while ((entry = readdir(dir)) != NULL)
    {
       

        if (entry->d_type != DT_DIR)
        {
            files++;
            entrytype = "file";

            strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
            if (stat(entrypath, &entry_stat) == -1)
            {
                ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
                continue;
            }
            char date[32];
            sprintf(entrysize, "%ld", entry_stat.st_size);

            f_size += entry_stat.st_size;
            //ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);

            /* Send chunk of HTML file containing table entries with file name and size */
            httpd_resp_sendstr_chunk(req, "<tr><td><a href=\"");
            httpd_resp_sendstr_chunk(req, req->uri);
            httpd_resp_sendstr_chunk(req, entry->d_name);
    //        httpd_resp_sendstr_chunk(req, "/");
            httpd_resp_sendstr_chunk(req, "\">"  //UTF character for file icon &#128462 
                                            "<img src=\"/file.png\" width=\"16\" height=\"16\">");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "</a></td><td>");
            httpd_resp_sendstr_chunk(req, entrytype);
            httpd_resp_sendstr_chunk(req, "</td><td data-sort=\"");
            httpd_resp_sendstr_chunk(req, entrysize);
            httpd_resp_sendstr_chunk(req, "\">");

            if (entry_stat.st_size > 1024 && entry_stat.st_size < 1024 * 1024)
            {
                sprintf(entrysize, "%1.1lf kB", (float)entry_stat.st_size / 1024);
            }
            else if (entry_stat.st_size > 1024 * 1024 && (float)entry_stat.st_size < 1024 * 1024 * 1024)
            {
                sprintf(entrysize, "%1.1f MB", (float)entry_stat.st_size / 1024 / 1024);
            }
            else if (entry_stat.st_size > 1024 * 1024 * 1024)
            {
                sprintf(entrysize, "%1.1f GB", (float)entry_stat.st_size / 1024 / 1024 / 1024);
            }
            else
                sprintf(entrysize, "%li B", entry_stat.st_size);

            httpd_resp_sendstr_chunk(req, entrysize);
            httpd_resp_sendstr_chunk(req, "</td><td data-sort=\"");

            sprintf(date, "%ld", entry_stat.st_mtime);

            httpd_resp_sendstr_chunk(req, date);

            httpd_resp_sendstr_chunk(req, "\">");

            sprintf(date, "%s", ctime(&entry_stat.st_mtime));

            httpd_resp_sendstr_chunk(req, date);
            httpd_resp_sendstr_chunk(req,  "</td><td><label><input type=\"checkbox\" value=\""); //class=\"delete\"></td></tr>");
            httpd_resp_sendstr_chunk(req, req->uri);
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "\" name=\"");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "\"></label></td></tr>");
        }
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req,   "</tbody></table>");

    char elements[16];

    httpd_resp_sendstr_chunk(req,   "<div class=\"footer\">"
                                    "<span style=\"float: left\"><p>Folders: ");

    sprintf(elements, "%i", folders);
    httpd_resp_sendstr_chunk(req, elements);
    httpd_resp_sendstr_chunk(req, "<br>Files: ");
    sprintf(elements, "%i", files);
    httpd_resp_sendstr_chunk(req, elements);

    //sprintf(elements, "%1.1f kB", f_size / 1024);

    if (f_size > 1024 && f_size < 1024 * 1024)
    {
        sprintf(elements, "%1.1f kB", f_size / 1024);
    }
    else if (f_size > 1024 * 1024 && f_size < 1024 * 1024 * 1024)
    {
        sprintf(elements, "%1.1f MB", f_size / 1024 / 1024);
    }
    else if (f_size > 1024 * 1024 * 1024)
    {
        sprintf(elements, "%1.1f GB", f_size / 1024 / 1024 / 1024);
    }
    else
    {
        sprintf(elements, "%1.0f B", f_size);
    }

    httpd_resp_sendstr_chunk(req, "<br>Size: ");
    httpd_resp_sendstr_chunk(req, elements);

    httpd_resp_sendstr_chunk(req, "</p></span><span style=\"float: right; text-align: right;\"><p>");
    
    get_freespace_sd(&tot_kb, &free_kb);

    sprintf(elements, "%1.2f", (float)free_kb / 1024 / 1024);
    httpd_resp_sendstr_chunk(req, elements);
    httpd_resp_sendstr_chunk(req, " GB free of ");

    sprintf(elements, "%1.2f", (float)tot_kb / 1024 / 1024);
    httpd_resp_sendstr_chunk(req, elements);
    httpd_resp_sendstr_chunk(req, " GB<br>");

    sprintf(elements, "%1.1f", 100 * (float)free_kb / tot_kb);
    httpd_resp_sendstr_chunk(req, elements);
    httpd_resp_sendstr_chunk(req, "% free space");
    httpd_resp_sendstr_chunk(req, "</p></span></div></body></html>");
    
    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;

}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf"))
    {
        return httpd_resp_set_type(req, "application/pdf");
    }
    else if (IS_FILE_EXT(filename, ".html"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if (IS_FILE_EXT(filename, ".jpeg"))
    {
        return httpd_resp_set_type(req, "image/jpeg");
    }
    else if (IS_FILE_EXT(filename, ".ico"))
    {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    else if (IS_FILE_EXT(filename, ".png"))
    {
        return httpd_resp_set_type(req, "image/png");
    }

    /* This is a limited set only */
    /* For any other type always set as binary data */
    return httpd_resp_set_type(req, "application/octet-stream");
}

void str_replace(char *target, const char *needle, const char *replacement)
{
    size_t buf_len = strlen(target) + 1;
    buf_len = MIN(buf_len, 1024); //max lenght 1024
    char buffer[buf_len];
    char *insert_point = &buffer[0];
    const char *tmp = target;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);

    while (1)
    {
        const char *p = strstr(tmp, needle);

        // walked past last occurrence of needle; copy remaining part
        if (p == NULL)
        {
            strcpy(insert_point, tmp);
            break;
        }

        // copy part before needle
        memcpy(insert_point, tmp, p - tmp);
        insert_point += p - tmp;

        // copy replacement string
        memcpy(insert_point, replacement, repl_len);
        insert_point += repl_len;

        // adjust pointers, move on
        tmp = p + needle_len;
    }

    // write altered string back to target
    strcpy(target, buffer);
}

/* Extract base path from path */
void get_base_path(char *dest, const char *path, size_t destsize)
{
    size_t pathlen = strlen(path);
    if (pathlen + 1 > destsize)
    {
        /* Full path string won't fit into destination buffer */
        return;
    }

    const char *quest = strrchr(path, '/');

    if (quest)
    {
        pathlen = MIN(pathlen, quest - path + 1);
    }
    /* Construct base path */
    strlcpy(dest, path, pathlen + 1);
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{

    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    if (base_pathlen + pathlen + 1 > destsize)
    {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    if (strstr(dest, "%20") != NULL)
    {
        //ESP_LOGW(TAG, "file has spaces in path.......");
        str_replace(dest, "%20", " "); //work around for spaces in paths
    }
    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[255];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri, sizeof(filepath));
    if (!filename)
    {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    if (filename[strlen(filename) - 1] == '/')
    {
        //filepath[strlen(filepath) - 1] = '\0';          // Remove last '/' as it is not compatible with dir function
        return http_resp_dir_html(req, filepath);
    }

    if (stat(filepath, &file_stat) == -1)
    {
        /* If file not present on sdcard check if URI
         * corresponds to one of the hardcoded paths */
        if (strcmp(filename, "/index.html") == 0)
        {
            return index_html_get_handler(req);
        }
        else if (strcmp(filename, "/?upgrade") == 0)
        {
            return upgrade_resp_html(req);
        }
        else if (strcmp(filename, "/?wifi") == 0)
        {
            return wifi_resp_html(req);
        }        
        else if (strcmp(filename, "/favicon.ico") == 0)
        {
            return favicon_get_handler(req);
        }
        else if (strcmp(filename, "/logo.png") == 0)
        {
            return logo_get_handler(req);
        }
        else if (strcmp(filename, "/file.png") == 0)
        {
            return file_get_handler(req);
        }
        else if (strcmp(filename, "/folder.png") == 0)
        {
            return folder_get_handler(req);
        }
        else if (strcmp(filename, "/home.png") == 0)
        {
            return home_get_handler(req);
        }
        else if (strcmp(filename, "/back.png") == 0)
        {
            return back_get_handler(req);
        }
        else if (strcmp(filename, "/?ap_list") == 0)
        {
            return ap_list_handler(req);
        } 
        else if (strcmp(filename, "/?connect_status") == 0)
        {
            return connect_status_handler(req);
        }   
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "rb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }
    struct timeval t_start_wr, t_stop_wr;
    gettimeofday(&t_start_wr, NULL);
    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
    
    

    char len[21];
    char m_date[32];
    struct tm tm = *gmtime(&file_stat.st_mtime);                //get file modified date
    strftime(m_date, sizeof m_date, "%a, %d %b %Y %H:%M:%S GMT", &tm); //convert to http date (in english)
    
    sprintf(len, "%li", file_stat.st_size);             //file size

    /* Set response headers */
    set_content_type_from_file(req, filename);
    httpd_resp_set_hdr(req, "Content-Length", len);
    //httpd_resp_set_hdr(req, "Date", m_date);
    httpd_resp_set_hdr(req, "Last-Modified", m_date);
    setvbuf(fd, NULL, _IOFBF, READ_BUF); //increase internal read buffer size, this will increase download speed slightly.
    
    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do
    {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);
        if (chunksize > 0)
        {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
            {
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);

    
    /* Close file after sending complete */
    fclose(fd);
    
    ESP_LOGI(TAG, "File sending complete");
    
    gettimeofday(&t_stop_wr, NULL);
    float time_wr = 1e3f * (t_stop_wr.tv_sec - t_start_wr.tv_sec) + 1e-3f * (t_stop_wr.tv_usec - t_start_wr.tv_usec);
    
    printf("Download done: %5.3f s, %5.2f Mb/s\n", time_wr / 1000, (float)file_stat.st_size / (time_wr / 1000) / (1024 * 1024));
    //printf("Free memmory: %i KB\n", esp_get_free_heap_size() / 1024);
    //printf("Minimum free memmory: %i KB\n", esp_get_minimum_free_heap_size() / 1024);
    
    /* Respond with an empty chunk to signal HTTP response completion */
#ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char basepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    /* Skip leading "/upload" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/')
    {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == 0)
    {
        ESP_LOGE(TAG, "File already exists : %s", filepath);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists");
        return ESP_FAIL;
    }

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE)
    {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "File size must be less than " MAX_FILE_SIZE_STR "!");
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
        return ESP_FAIL;
    }

    fd = fopen(filepath, "wb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving file : %s...", filename);
    //setvbuf(fd, NULL, _IOFBF, WRITE_BUF);
    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    
    size_t received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    size_t remaining = req->content_len;
    
    struct timeval t_start_wr, t_stop_wr;
    
    gettimeofday(&t_start_wr, NULL);
    char progress[50];
    
    size_t status;
    int progr = 10;
    
    while (remaining > 0)
    {

        //ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(buf, 1, received, fd)))
        {
            /* Couldn't write everything to file!
             * Storage may be full? */
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            return ESP_FAIL;
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;

        status = 100 - 100*remaining / req->content_len;
        //status = (100 - status*100) ;


        if (status >= progr)
        {
            progr += 10;
            sprintf(progress, "Progress: %2.0i %%", status);
            //httpd_resp_send(req, progress, strlen(progress));
            ESP_LOGI(TAG, "%s", progress);
        }
    }

    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(TAG, "File reception complete");
    gettimeofday(&t_stop_wr, NULL);
    float time_wr = 1e3f * (t_stop_wr.tv_sec - t_start_wr.tv_sec) + 1e-3f * (t_stop_wr.tv_usec - t_start_wr.tv_usec);
    printf("Upload done in: %5.3f s, %5.2f Mb/s\n", time_wr / 1000, (float)req->content_len / (time_wr / 1000) / (1024 * 1024));
    printf("Free memmory: %i KB\n", esp_get_free_heap_size() / 1024);
    printf("Minimum free memmory: %i KB\n", esp_get_minimum_free_heap_size() / 1024);

    /* Get the current uri path */
    get_base_path(basepath, req->uri + sizeof("/upload") - 1, sizeof(basepath));

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", basepath);
#ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

static int remove_directory(const char *path)
{
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = -1;

    if (d)
    {
        struct dirent *p;

        r = 0;
        while (!r && (p = readdir(d)))
        {
            int r2 = -1;
            char *buf;
            size_t len;

            /* Skip the names "." and ".." as we don't want to recurse on them. Not valid for esp32 vfs, but we'll leave it in */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
                continue;

            len = path_len + strlen(p->d_name) + 2;
            buf = malloc(len);

            if (buf)
            {
                //struct stat statbuf;

                snprintf(buf, len, "%s/%s", path, p->d_name);
                if (p->d_type == DT_DIR)
                {
                    //if (!stat(buf, &statbuf)) {
                    //if (S_ISDIR(statbuf.st_mode))
                    r2 = remove_directory(buf);
                }
                else
                    r2 = unlink(buf);

                free(buf);
            }
            r = r2;
        }
        closedir(d);
    }

    if (!r)
        r = rmdir(path);

    return r;
}


static esp_err_t delete_files_handler(httpd_req_t *req)
{
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    memset(buf, '\0', SCRATCH_BUFSIZE);
    int received=0;


    /* Content length of the request gives
    * the size of the file being uploaded */
    size_t data_len = req->content_len;

    ESP_LOGI(TAG, "Received %i bytes", data_len);
    if ((received = httpd_req_recv(req, buf, MIN(data_len, SCRATCH_BUFSIZE))) > 0)
    {
        if (strstr(buf, "files") != NULL)
        {
            //printf("%s\n\n", buf);
            char filepath[FILE_PATH_MAX];
            cJSON *root = cJSON_Parse(buf);
            cJSON *files = cJSON_GetObjectItem(root, "files");
            cJSON *element;
            struct stat file_stat;
            int i;

            //const char *sys_info = cJSON_Print(root);
            //printf("JSON contains: %s\n", sys_info);


            int n = cJSON_GetArraySize(files);
            for (i = 0; i < n; i++)
            {
                element = cJSON_GetArrayItem(files, i);
                //name = cJSON_GetObjectItem(elem, "name");
                get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                            element->valuestring, sizeof(filepath));
                
                //printf("%s\n%s\n", filename, filepath);

                if (filepath[strlen(filepath) - 1] == '/')
                    {
                        // Remove trailing '/'
                        filepath[strlen(filepath) - 1] = '\0';
                        
                    }

                if (stat(filepath, &file_stat) == -1)
                {
                    cJSON_Delete(root);
                    ESP_LOGE(TAG, "File does not exist : %s", filepath);
                    /* Respond with 400 Bad Request */
                    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist:");
                    return ESP_FAIL;
                }
                if (S_ISDIR(file_stat.st_mode))
                {
                    if (rmdir(filepath) == 0)
                    {
                        ESP_LOGI(TAG, "Deleted directory : %s", filepath);
                        /* Redirect onto root to see the updated file list */
                    
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Directory not empty, triggering remove_dir function : %s", filepath);
                        // This is a custom function wich will loop trough all folder and sub-folders and delete files and folders one by one.
                        // This can potentyally take a long time if the folder you want to delete contains 1000s of files and sub-folder.
                        if (remove_directory(filepath) == 0)
                        {
                            ESP_LOGI(TAG, "Directory deleted: %s", filepath);
                        }
                        else
                        {
                            cJSON_Delete(root);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error! Directory could not be deleted!");
                            return ESP_FAIL;
                        }
                    }
                    //return ESP_OK;
                }
                else
                {
                    ESP_LOGI(TAG, "Deleting file : %s", filepath);
                    /* Delete file */
                    unlink(filepath);
                }
            }
           
            char basepath[FILE_PATH_MAX];
            get_base_path(basepath, req->uri + sizeof("/delete") - 1, sizeof(basepath));
            cJSON_Delete(root);

            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", basepath);
            //httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr(req, "Selected items deleted!");
            return ESP_OK;
        }
        else
        {
            ESP_LOGI(TAG, "No JSON array received");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error receiving json data");
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGI(TAG, "HTTP POST request body empty");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP POST request body empty");
        return ESP_FAIL;
    }
}



/* Handler to create a folder on the server */
static esp_err_t dir_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char basepath[FILE_PATH_MAX];
    struct stat file_stat;

    /* Skip leading "/dir" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/dir") - 1, sizeof(filepath));

    if (!filename)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Name too long");
        return ESP_FAIL;
    }
    /* Filename cannot illegal characters  */
    if (strpbrk(filename, "<>:\"\\|?*") != NULL)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Illegal character in name");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/')
    {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Foldername cannot end with /");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == 0)
    {
        ESP_LOGE(TAG, "Folder already exist : %s", filename);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Folder already exist");
        return ESP_FAIL;
    }
    // create direcotry
    int val = mkdir(filepath, S_IRWXU);
    if (val == -1)
    {
        printf("Failed to create directory %s\n", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create folder");
        return ESP_FAIL;
    }

    /* Get the current uri path */
    get_base_path(basepath, req->uri + sizeof("/dir") - 1, sizeof(basepath));

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", basepath);
#ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "Folder created successfully");

    return ESP_OK;
}

/* Return Firmware update status to http webpage */
esp_err_t OTA_update_status_handler(httpd_req_t *req)
{
    char ledJSON[150];

    ESP_LOGI("OTA", "Status Requested");
    if (strlen(flash_error) > 0)
    {
        sprintf(ledJSON, "{\"status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\",\"error\":\"%s\"}", flash_status, __TIME__, __DATE__, flash_error);
    }
    else
        sprintf(ledJSON, "{\"status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\"}", flash_status, __TIME__, __DATE__);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ledJSON, strlen(ledJSON));

    if (flash_status == 1) {
        ESP_LOGI(TAG, "Successful flashing, restarting");
        vTaskDelay(2000/portTICK_PERIOD_MS);
        esp_restart();
    }

    return ESP_OK;
}

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info, esp_app_desc_t *running_app_info)
{
    if (new_app_info == NULL) {
        strcpy(flash_error, "Invalid image header");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Running firmware version: %s", running_app_info->version);

    // the bin file to be flashed should contain a magic word in the file header. This should be 0xABCD5432 and is contained in ESP_APP_DESC_MAGIC_WORD
    if (new_app_info->magic_word != ESP_APP_DESC_MAGIC_WORD)
    {
        ESP_LOGW(TAG, "Invalid magic word (expected [ 0x%x ], received [0x%x ]", ESP_APP_DESC_MAGIC_WORD, new_app_info->magic_word);
        strcpy(flash_error, "Invalid magic word (expected: 0xABCD5432)");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Function to update firmware */
esp_err_t OTA_update_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle;
    esp_err_t err;
    char *ota_buff = ((struct file_server_data *)req->user_ctx)->scratch;
    //char ota_buff[BUFFSIZE];
    size_t content_length = req->content_len;
    //int content_received = 0;
    int remaining = req->content_len;
    int recv_len = 0;
    bool is_req_body_started = false;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    flash_error[0] = '\0';

    if (configured != running)
    {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    // Unsucessful Flashing
    flash_status = -1;
    size_t status, progress = 10;
    do
    {
        /* Read the data from the request */
        if ((recv_len = httpd_req_recv(req, ota_buff, MIN(remaining, SCRATCH_BUFSIZE))) <= 0)
        {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT)
            {
                ESP_LOGI("OTA", "Socket Timeout");
                /* Retry receiving if timeout occurred */
                continue;
            }
            ESP_LOGI("OTA", "OTA Other Error %d", recv_len);
            return ESP_FAIL;
        }

        // Is this the first data we are receiving
        // If so, it will have the information in the header we need.
        if (!is_req_body_started)
        {
            is_req_body_started = true;


            esp_app_desc_t new_app_info;
            esp_app_desc_t running_app_info;

            if (recv_len > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
            {
                // check current version vs downloading
                memcpy(&new_app_info, &ota_buff[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                if (new_app_info.date[strlen(new_app_info.date)] == '\0') {
                    printf("Compile date: %s - %s\n", new_app_info.date, new_app_info.time);
                    printf("IDF version: %s - Magic Byte: [ 0x%x ]\n", new_app_info.idf_ver, new_app_info.magic_word);
                }

                if (new_app_info.version[strlen(new_app_info.version)] == '\0') {
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
                }
                esp_ota_get_partition_description(running, &running_app_info);
                
                err = validate_image_header(&new_app_info, &running_app_info);

                if (err != ESP_OK)
                {
                #ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
                    httpd_resp_set_hdr(req, "Connection", "close");
                #endif
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not a valid image file");
                    return ESP_FAIL;
                }

                err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                    esp_ota_abort(ota_handle);
                    strcpy(flash_error, "esp_ota_begin failed");
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error flashing new firmware");
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "esp_ota_begin succeeded");
                // Lets write this first part of data out
                err = esp_ota_write(ota_handle, ota_buff, recv_len);
                if (err != ESP_OK)
                {
                    esp_ota_abort(ota_handle);
                    ESP_LOGE(TAG, "Error, OTA image is invalid");
                    strcpy(flash_error, "Image is invalid");
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error flashing new firmware");
                    return ESP_FAIL;
                }                
            }
            else
            {
                ESP_LOGE(TAG, "received package is too short, increase buffer size!");
                strcpy(flash_error, "Invalid image file");
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid image file");
                return ESP_FAIL;
            }
        }
        else
        {
            // Write OTA data
            err = esp_ota_write(ota_handle, ota_buff, recv_len);
        }

        if (err != ESP_OK)
        {
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "Error, OTA image is invalid");
            strcpy(flash_error, "OTA image is invalid");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error flashing new firmware");
            return ESP_FAIL;
        }
        //content_received += recv_len;
        remaining -= recv_len;
        
        status =  100 * remaining / content_length ;
        
        status = 100 - status ;

        if (status >= progress)
        {
            progress += 10;
            printf("Progress: %2.0i %%\n", status);
            //httpd_resp_send(req, progress, strlen(progress));
            //ESP_LOGI(TAG,"%s", progress);
        }

    } while (remaining > 0);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(TAG, "Image validation failed");
            strcpy(flash_error, "Image validation failed");
        }
        else
        {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
            sprintf(flash_error, "%s", esp_err_to_name(err));
        }
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    #ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
        httpd_resp_set_hdr(req, "Connection", "close");
    #endif
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        sprintf(flash_error, "%s", esp_err_to_name(err));
    #ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
        httpd_resp_set_hdr(req, "Connection", "close");
    #endif
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed");
        return ESP_FAIL;
    }
    flash_status = 1;
    #ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
        httpd_resp_set_hdr(req, "Connection", "close");
    #endif
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    httpd_resp_sendstr(req, "Transfer complete");
    ESP_LOGI(TAG, "Flash sucess, rebooting!!");

    

    return ESP_OK;
}

static esp_err_t format_handler(httpd_req_t *req)
{
    esp_err_t err;
    ESP_LOGI(TAG, "Received FORMAT SD Request from HTTP");
    err = format_sd_card();
    if (err == ESP_OK) ESP_LOGI(TAG, "Format success");
    else { 
        ESP_LOGW(TAG, "Format failed, trying to remount and then format...");
        bool format = true;
        err = mount_sd_card(format);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Re-mounted and formatted");
        } else {
            ESP_LOGW(TAG, "Mount and formate fail");
        }
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    #ifdef HTTP_SERVER_HTTPD_CONN_CLOSE_HEADER
        httpd_resp_set_hdr(req, "Connection", "close");
    #endif
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"Format complete\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"Format failed\"}");
    }
    
    
    return ESP_OK;
}

/* General handler for post requests from web page */
/* More requests can be added */
static esp_err_t http_server_post_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "POST request: %s", req->uri);
    
    const char *needle = "/";
    char *start, *end;
    int uri_len = strlen(req->uri);
    
    if (uri_len > CONFIG_HTTPD_MAX_URI_LEN) {

        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Uri lenght is too long !");
        return ESP_FAIL;
    }

    char uri[uri_len];
    int len = 0;

    start = strstr(req->uri, needle);
    end = strstr(start + 1, needle);
    if (end == NULL) {
        len = strlen(start);
    } else {
        len = end - start;
    }

    strlcpy(uri, req->uri, len + 1);

	if(strcmp(uri, "/upload") == 0) // Handler for uploading files to sd-card. Match all URIs of type /upload/path/to/file
    {
        return upload_post_handler(req);
    }  
    if(strcmp(uri, "/dir") == 0)    // Handler for creating directory. Match all URIs of type /dir/new_path_name
    {
        return dir_post_handler(req);
    }  
    else if(strcmp(uri, "/delete") == 0) // handler for deleting files
    {
        return delete_files_handler(req);
    } 
    else if(strcmp(uri, "/update") == 0) // Handler for updating firmware
    {
        return OTA_update_post_handler(req);
    }
    else if(strcmp(uri, "/status") == 0)  // Handler for getting status on a firmware update
    {
        return OTA_update_status_handler(req); 
    } 
    else if(strcmp(uri, "/connect") == 0) // Handler for conncting to a network
    {
        return connect_handler(req);
    }
    else if(strcmp(uri, "/partition") == 0) // Handler for formating the sd card
    {
        return format_handler(req);
    }
    /*else if(strcmp(uri, "/custom_request") == 0)
    {
        return custom_handler(req);
    } */
     
    printf("Post command: %s\n", uri);

    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "POST request not supported !");
    
    return ESP_FAIL;
}


/* Function to start the file server */
esp_err_t start_file_server(const char *base_path)
{
   
    static struct file_server_data *server_data = NULL;

    /* Validate file storage base path */
    if (!base_path || strcmp(base_path, "/sdcard") != 0)
    {
        ESP_LOGE(TAG, "File server presently supports only '/sdcard' as base path");
    }

    if (server_data)
    {
        ESP_LOGE(TAG, "File server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(server_data->base_path, base_path,
            sizeof(server_data->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 6;
    config.backlog_conn = 4;
    
    
    // Lets bump up the stack size (default is 4096)
    config.stack_size = 1024*6;

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    /* URI handler for all GET commands */
    httpd_uri_t http_server_get_request = {
        .uri = "/*", // Match all URIs of type /path/to/file
        .method = HTTP_GET,
        .handler = download_get_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &http_server_get_request);

    /* General URI handler for Post requests */
    httpd_uri_t http_server_post_request = {
        .uri = "/*", // Match all URIs of type post
        .method = HTTP_POST,
        .handler = http_server_post_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &http_server_post_request);

/* General URI handler for Delete requests */
    httpd_uri_t http_server_delete_request = {
        .uri = "/*", 
        .method = HTTP_DELETE,
        .handler = disconnect_handler,
        .user_ctx = server_data // Pass server data as context
    };
    httpd_register_uri_handler(server, &http_server_delete_request);

    return ESP_OK;
}
