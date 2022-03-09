#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/rtc_io.h"

//#include "esp_crc.h"
#include "esp_spi_flash.h"
#include "esp_log.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"

#include "uart_tcp_server.h"
#include "sdmmc.h"
#include "file_server.h"
#include "spi.h"




static const int maxMessages = MAX_SPI_MESSAGES;       //SPI message buffer size. Buffer size: maxMessages*SPI_BLOCK_SIZE

static uint8_t spiQueueBit;          // We set these bits high when we queue a message and low when we receive a message


static const char *TAG="SPI_receiver";

//Called after a transaction is queued and ready for pickup by master. We use this to set the handshake line high.
void IRAM_ATTR my_post_setup_cb(spi_slave_transaction_t *trans) {
    WRITE_PERI_REG(GPIO_OUT_W1TS_REG, (1<<GPIO_HANDSHAKE));
}

//Called after transaction is received. We use this to set the handshake line low.
void IRAM_ATTR my_post_trans_cb(spi_slave_transaction_t *trans) {
    WRITE_PERI_REG(GPIO_OUT_W1TC_REG, (1<<GPIO_HANDSHAKE));
    // set bit low to indicate buffer is received
    CLEAR_BIT(spiQueueBit, ((int)trans->user));
}

void init_esp32_spi_slave()
{   
    esp_err_t ret;

    //Configuration for the SPI bus
    spi_bus_config_t buscfg={
        .mosi_io_num=GPIO_MOSI,
        .miso_io_num=GPIO_MISO,
        .sclk_io_num=GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX(4092, SPI_PKT_SIZE),
        .flags = 0,
    };

    //Configuration for the SPI slave interface
    spi_slave_interface_config_t slvcfg={
        .mode=1,                      /**< SPI mode, representing a pair of (CPOL, CPHA) configuration:
                                         - 0: (0, 0)
                                         - 1: (0, 1)
                                         - 2: (1, 0)
                                         - 3: (1, 1)
                                     */
        .spics_io_num=GPIO_CS,
        .queue_size=6,
        .flags = ESP_INTR_FLAG_IRAM, //SPI_SLAVE_RXBIT_LSBFIRST,
        .post_setup_cb=my_post_setup_cb,
        .post_trans_cb=my_post_trans_cb
    };

    //Configuration for the handshake line
    gpio_config_t io_conf={
        .intr_type=GPIO_INTR_DISABLE,
        .mode=GPIO_MODE_OUTPUT,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE)
    };

    //Configure handshake line as output
    gpio_config(&io_conf);

   //Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

    //Initialize SPI slave interface
    ret=spi_slave_initialize(RCV_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    assert(ret==ESP_OK);
}

void SPI_task (void *arg)
{
    char path[FILE_PATH_MAX] = {0};         //file path
    FILE* file = NULL;                      //FILE pointer
    eControl msgCode = 0;                       //SPI Message code
    //struct timeval begin, end, w_begin, w_end;

    uint16_t spi_trans_len = 0;
    const size_t sd_mount_len = sizeof(BASE_PATH) - 1;         //Length of sd card base folder
    int spi_id = 0;
    
    esp_log_level_set(TAG, ESP_LOG_INFO);

    esp_err_t ret;
    
    //  SPI receive buffers. This needs to be word alligned and a multiple of 4, due to DMA restrictions in ESP32.
    WORD_ALIGNED_ATTR char * recvbuf[maxMessages]; //= (char *)malloc(maxMessages * sizeof(char*));
    // Create an array of SPI transactions.
    spi_slave_transaction_t spi_trans[maxMessages];
    // Pointer to the SPI transaction that currently has been received.
    spi_slave_transaction_t *ret_trans;

    char * currentBuffer = NULL;
    
    spiQueueBit = 0;          // We set these bits high when we queue a message and low when we receive a message

    //init SPI slave device
    init_esp32_spi_slave();


    // Prepare a set of SPI transactions 
    for (int k=0; k < maxMessages; k++) {
        recvbuf[k] = (char*)malloc(SPI_BLOCK_SIZE);
        spi_trans[k].user = malloc(sizeof(int));

        memset(recvbuf[k], 0xee, SPI_BLOCK_SIZE);
        memset(&spi_trans[k], 0, sizeof(spi_trans[k]));

        spi_trans[k].length=SPI_PKT_SIZE*8;             // Lenght of transaction in bits
        spi_trans[k].rx_buffer= recvbuf[k];              // Receive buffer
        spi_trans[k].tx_buffer=NULL;                    // No transmit phase
        spi_trans[k].user = ((int*)k);                  // Used to store transaction ID.

        ret = spi_slave_queue_trans(RCV_HOST, &spi_trans[k], 0);
       
        if (ret == ESP_OK) 
        {
            SET_BIT(spiQueueBit, k);            //Set SPI message queue bit
            //printf("SPI message %i queued   -   spiBIT: %X, Messages queued: %i    |   UserId = %i \n", k, spiQueueBit, get_spi_queue_size(RCV_HOST), ((int)spi_trans[k].user));
        } else 
        {
            printf("ERROR: SPI message %i not queued ! \n", k);
        }
    }

    ESP_LOGI(TAG, "Waiting for SPI messages");

    // This loop will wait for SPI messages forever
    do {          
        //Wait here until SPI message is received or ticks_to_wait is expired. If set to portMAX_DELAY, will wait forever.
        ret = spi_slave_get_trans_result(RCV_HOST, &ret_trans, 1000/portTICK_RATE_MS);  // 1000/portTICK_RATE_MS to wait 1000ms
        
        // Check if message was received or spi_slave_get_trans_result timed out.
        if (ret == ESP_OK) 
        {
            //Get id from returned SPI transaction
            spi_id = ((int)ret_trans->user);

            // Pointer to returned message buffer
            currentBuffer = ret_trans->rx_buffer;

            // Detected message length
            spi_trans_len = ret_trans->trans_len;
        
            //if transmission length is 0 it means we did not receive any data, probably noise or somehing on CS and clock line
            if(spi_trans_len > 0) 
            {   
                msgCode = currentBuffer[0];                                     // Command byte of SPI message
                uint16_t length = (currentBuffer[2] << 8) | (currentBuffer[1]); // Length of SPI message
                const char * spi_data = currentBuffer + SPI_HEADER_SIZE;        // Body of SPI message

                switch (msgCode) {
                
                case WRITEFILE:
                
                    // If the file has been closed, then try to reopen the file
                    if (file == NULL) {             
                        file = fopen(path, "ab");
                        if (file == NULL) {
                            printf("Cannot open file for writing %s\n", path);
                            
                            break;
                        }
                    }

                    size_t remaining = length;
                    
                    size_t bytes_written=0;

            
                    do {

                        /* Write received data to sd card */
                        int written = fwrite(spi_data, 1, MIN(SPI_BLOCK_SIZE, remaining), file);
                    
                        //crc = esp_crc32_be( crc, (const uint8_t *)spi_data,  MIN(SPI_BLOCK_SIZE, remaining));
                        
                        remaining -= written;

                        if (remaining > 0) 
                        {
                            if ( ret_trans == &spi_trans[spi_id] && !(IS_BIT_SET(spiQueueBit, spi_id)) ) 
                            {
                                    // clear buffer
                                    memset(recvbuf[spi_id], 0xee, SPI_BLOCK_SIZE);
                                    // Queue up the last received spi transaction
                                    ret = spi_slave_queue_trans(RCV_HOST, &spi_trans[spi_id], 0);
                                    if (ret != ESP_OK) {
                                        printf("SPI message %i could not be queued\n", spi_id);
                                    } 
                                    else 
                                    {             
                                        //set bit to indicate buffer has been queued successfully
                                        SET_BIT(spiQueueBit, spi_id);
                                        // clear buffer
                                        memset(recvbuf[spi_id], 0xee, SPI_BLOCK_SIZE);
                                    }

                            }
                    

                            // Wait here until SPI transmission is received or times out, ticks_to_wait. If SPI transmission is already received in the backgound, we will not wait.
                            ret = spi_slave_get_trans_result(RCV_HOST, &ret_trans, 1000/portTICK_RATE_MS);
                            
                            if (ret != ESP_OK) {
                                printf("Could not receive message! No messages queued or received since last time!\n");
                                goto ABORT;
                            } else {
                                spi_id = ((int)ret_trans->user);
                            }
                            spi_data = ret_trans->rx_buffer;            
                        }

                        bytes_written += written;
                    

                    } while (remaining != 0);

                    printf("WRITE COMMAND Received:   Received bytes vs written bytes: %i bytes vs %i bytes \n", length, bytes_written); 
                  
                    break;

                case OPEN_FILE:
                    
                    if (length > FILE_PATH_MAX) {
                        printf("Path length cannot be longer than %i characters,  Received: %i\n", FILE_PATH_MAX, length);
                        break;
                    }
                    
                    // replace all '\' with '/' in path string
                    replacechar(currentBuffer + SPI_HEADER_SIZE, '\\', '/');

                    // Check if file is already open and open file if need to
                    
                    if (strncmp(path + sd_mount_len, spi_data, length) != 0 )
                    {
                        //printf(" Old path: %s   |   New path: %s\n", path + sd_mount_len + 1, spi_data);
                        // Clear path variable and then build up received path
                        memset(path, 0, sizeof(path));
                        strcpy(path, BASE_PATH);
                        strncat(path, spi_data, length);

                        /* If another files open, close it */
                        if (file != NULL) {
                            fclose(file);
                            printf("Open file closed\n");
                        }
                        file = fopen(path, "ab");
                        //ab = Opens a file for appending in binary mode. If not exist, then create file.
                        if (file == NULL) {
                            printf("Cannot open file %s\n", path);
                        } else {
                            /* Increase internal write buffer from 128 bytes to whater blocksize we use. Needs to be run per file we open */
                            setvbuf(file, NULL, _IOFBF, SPI_BLOCK_SIZE);
                        }
                    } else if ((file == NULL) && (strlen(path) > 0)) {
                        file = fopen(path, "ab");
                        if (file == NULL) {
                            printf("Cannot open file %s\n", path);
                        } else {
                            /* Increase internal write buffer from 128 bytes to whater blocksize we use. Needs to be run per file we open */
                            setvbuf(file, NULL, _IOFBF, SPI_BLOCK_SIZE);
                        }
                    }

                    printf("\nOPEN FILE COMMAND Received: Length: %i bytes  |  filename: %s\n", length, path); 

                    break;

                
                case MAKEDIR:

                    if (length > FILE_PATH_MAX) {
                        printf("Path length cannot be longer than 255 characters");
                        break;
                    }
                    currentBuffer[length + SPI_HEADER_SIZE] = '\0';         // Null terminate path
                    //printf("Received path: %s\n", spi_data);

                    char * folder_path = (char*)malloc(length + sd_mount_len + 1);         // Allocate memmory for path + extra byte for '/' and a terminating 0 byte.
                    strcpy(folder_path, BASE_PATH);
                    strncat(folder_path, spi_data, length);

                    // We are done with this buffer, so we can return it to the queue:
                    if ( (ret_trans == &spi_trans[spi_id]) && !(IS_BIT_SET(spiQueueBit, spi_id))) 
                    {
                        // reset memory
                        memset(recvbuf[spi_id], 0xee, SPI_BLOCK_SIZE);
                        // Queue up SPI message as we are done with this buffer
                        ret = spi_slave_queue_trans(RCV_HOST, &spi_trans[spi_id], 0);

                        if (ret != ESP_OK) {
                            printf("SPI message %i could not be queued\n", spi_id);
                        } else {                               
                            //set bit to indicate buffer has been queued successfully
                            SET_BIT(spiQueueBit, spi_id);
                        }
                    }


                    if (mkdir(folder_path, S_IRWXU ) == 0) {    // S_IRWXU = chmod 777 
                        printf("MAKEDIR COMMAND: Directory created: %s\n", folder_path);
                    } else {
                        printf("MAKEDIR COMMAND: Directory already exists or could not be created: %s\n", folder_path);
                    }                   
                    free(folder_path);
                    break;

                case  CLOSE_FILE:
                    if (file != NULL) 
                    {
                        fclose(file);
                        file = NULL;
                        
                        printf("FILE CLOSE COMMAND: %s !\n", path);
                    }
                    break;

                case  SYNCFILE:
                    if (file != NULL) 
                    {                       
                        fflush(file);
                        fsync(fileno(file));
                        printf("FILE SYNC COMMAND: %s !\n", path);
                    }
                    break;
                
                case SLEEP:

                        ESP_LOGI(TAG, "Enter deep sleep");
                        // Need to stop wifi before going to sleep
                        esp_wifi_stop();
                        esp_wifi_deinit();
                        

                        #if CONFIG_IDF_TARGET_ESP32
                            // Isolate GPIO12 pin from external circuits. This is needed for modules
                            // which have an external pull-up resistor on GPIO12 (such as ESP32-WROVER)
                            // to minimize current consumption.

                            rtc_gpio_isolate(GPIO_NUM_12);

                        #endif
                        //esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_OFF);
                        esp_deep_sleep_start();

                    break;

                default:
                    
                    printf("UNKNOWN COMMAND byte: 0x%X     -  transmission length: %i\n", currentBuffer[0], spi_trans_len/8 );
                    break;
                }
                
            }              

            ABORT:
            // check if any buffers needs to be queued.
            for (int k=0; k < maxMessages; k++) {            
                if ( !(IS_BIT_SET(spiQueueBit, k))) 
                {
                    // reset buffer
                    memset(recvbuf[k], 0xee, SPI_BLOCK_SIZE);
                    // Queue up message
                    ret = spi_slave_queue_trans(RCV_HOST, &spi_trans[k], 0);
                            
                    if (ret != ESP_OK) {
                        printf("SPI message %i could not be queued\n", spi_id);
                    } 
                    else 
                    {                               
                        //set bit to indicate buffer has been queued successfully
                        SET_BIT(spiQueueBit, k);
                    }
                }
            }

        } else {
            if (file != NULL) {
                fclose(file);
                file = NULL;
                printf("file closed\n");
            }
        }   
    } while (1);
    
    /* Never reached */
    for (int k=0; k < maxMessages; k++) 
    {
        free(recvbuf[k]);
        free(spi_trans[k].user);
        printf("Buffer freed: %i\n", k);
    }
    free(recvbuf);

    vTaskDelete(NULL);
}



