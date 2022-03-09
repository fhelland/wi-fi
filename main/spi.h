#pragma once

#ifndef SPI_H_INCLUDED
#define SPI_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif


#define BASE_PATH       (SD_MOUNT"/")

#define SET_BIT(value, pos)     (value |= (1U<< pos))
#define CLEAR_BIT(value, pos)   (value &= (~(1U<< pos)))
#define IS_BIT_SET(value, pos)  (value & (1U<< pos))


/* Pin Layout:  */
#define GPIO_HANDSHAKE  22
#define GPIO_MOSI       23
#define GPIO_MISO       19
#define GPIO_SCLK       18
#define GPIO_CS         5
#define RCV_HOST    VSPI_HOST


/* SPI_BLOCK_SIZE: Maximum number of bytes in a SPI message. This needs to set same on the Master   * 
 * This value needs to be a multiple of 4 and buffer needs to be Word alligned.                     *
 * This should be set up as a factor of 1024 for most efficent writes to SD-card.                   *
 *                
 */
#define SPI_HEADER_SIZE     4
#define SPI_BLOCK_SIZE     4096         
#define SPI_PKT_SIZE        (SPI_BLOCK_SIZE + SPI_HEADER_SIZE)

#define FILE_PATH_MAX       255
#define FOLDER_NAME_MAX     128


/* Size of SPI message buffer 
 * Allocated memmory: MAX_SPI_MESSAGES * SPI_BLOCK_SIZE
 */
#define MAX_SPI_MESSAGES    4     

/* PACKAGE FORMAT :

|    Byte 1     |    Byte 2     |    Byte 3     |     Byte 4    |     Byte 4 - BLOCKSIZE    | 
|  Command byte |  Length MSB   |  Length LSB   |      Not used |           Body            |   

*/
typedef enum eControl { 
    OPEN_FILE = 0x01, 
    CLOSE_FILE = 0x02, 
    WRITEFILE = 0x04, 
    SYNCFILE = 0x08, 
    SLEEP = 0x10, 
    WAKEUP = 0x20, 
    MAKEDIR = 0x40

}eControl;


/*  Init and install SPI driver                */
void init_esp32_spi_slave();

/*  Main task of SPI Slave receive */
void SPI_task (void *arg);


#ifdef __cplusplus
}
#endif

#endif  /* SPI_H_INCLUDED   */