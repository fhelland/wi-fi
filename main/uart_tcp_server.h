#pragma once
#ifndef UART_TCP_SERVER_H_INCLUDED
#define UART_TCP_SERVER_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define PORT                        CONFIG_TCP_SERVER_PORT                  /*  Port to be used in TCP server           */
#define KEEPALIVE_IDLE              CONFIG_TCP_SERVER_KEEPALIVE_IDLE         /*  Keep-alive idle time. In idle time without receiving any data from peer, will send keep-alive probe packet */
#define KEEPALIVE_INTERVAL          CONFIG_TCP_SERVER_KEEPALIVE_INTERVAL     /*  Keep-alive probe packet interval time.    */
#define KEEPALIVE_COUNT             CONFIG_TCP_SERVER_KEEPALIVE_COUNT        /*  Keep-alive probe packet retry count.    */

static const int RX_BUF_SIZE = 6*1024;
static const int UART_RX_BUF_SIZE = 6*1024;


                        //  RS232 adapter:       Colors   |   Pin
#define TXD_PIN         (GPIO_NUM_27)        // yellow = RX = PIN 4   
#define RXD_PIN         (GPIO_NUM_26)        // orange = TX = PIN 3  
#define EX_UART_NUM     UART_NUM_2



/*/////////////////////////////////////////////////////////
 *
 *          TASK for receiving data on UART 
 *          and transmitting incomming data to a TCP socket
 *
 */
void rx_task(void *arg);


/*/////////////////////////////////////////////////////////          
 *          Init uart with xon/xoff flowcontrol
 *          .baud_rate = 115200,
 *          .data_bits = UART_DATA_8_BITS,
 *          .parity    = UART_PARITY_DISABLE,
 *          .stop_bits = UART_STOP_BITS_1,
 */
void uart_init(void);

/*//////////////////////////////////////////////////////////
 *
 *                  Main tcp server task.
 *                  received data on a connected socket will
 *                  be directly retransmittet on UART
 *
 */
void start_tcp_server_task(void);


/*//////////////////////////////////////////////////////////
 *
 *           Read node description from NVS
 */ 
uint8_t read_from_nvs(char *out_string);

/*//////////////////////////////////////////////////////////
 *
 *           Save node description to NVS
 */ 
uint8_t write_to_nvs(char *in_string);

/*//////////////////////////////////////////////////////////
 *
 *           Send get_node desciprtion command to UART
 */ 
uint8_t get_node_description(char * out_string, size_t max_tries);

/*//////////////////////////////////////////////////////////
 *
 *           Send do_get clock command to UART
 */ 
uint8_t get_clock(size_t n_times);

/*//////////////////////////////////////////////////////////
 *
 *           Replace character in input string
 *           
 */ 
void replacechar(char *str, char orig, char rep);

#ifdef __cplusplus
}
#endif

#endif  /* UART_TCP_SERVER_H_INCLUDED */