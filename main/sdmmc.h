#pragma once
#ifndef SDMMC_H_INCLUDED
#define SDMMC_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT    "/sdcard"

uint8_t get_freespace_sd(uint32_t* tot, uint32_t* free);

uint8_t get_sdcard_info(char* name, uint16_t* freq_khz);

esp_err_t init_sdmmc(void);

esp_err_t mount_sd_card(bool format_if_fail);

esp_err_t format_sd_card(void);

#ifdef __cplusplus
}
#endif

#endif  /* SDMMC_H_INCLUDED */