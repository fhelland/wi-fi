#pragma once
#ifndef SDMMC_H_INCLUDED
#define SDMMC_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT    "/sdcard"

/* Get the current total and free space on a mounted sd-card */
uint8_t get_freespace_sd(uint32_t* tot, uint32_t* free);

/* Get the card name and speed of a mounted sd-card */
uint8_t get_sdcard_info(char* name, uint16_t* freq_khz);

/*  All in one function to mount sd card 
*   If format_if_fail is set to true, the card will be formatted to FAT32, if the current partition is not readable. */
esp_err_t mount_sd_card(bool format_if_fail);

/* Format a sd-card that is already mounted */
esp_err_t format_sd_card(void);

#ifdef __cplusplus
}
#endif

#endif  /* SDMMC_H_INCLUDED */