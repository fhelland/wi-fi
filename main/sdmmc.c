 /* Mount SD card 
  * 
  *
  *  */

#include <sys/time.h>
#include <string.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "driver/sdmmc_defs.h"
#include "diskio_sdmmc.h"
#include "sdmmc.h"
#include "vfs_fat_internal.h"

#include "esp_vfs.h"
#include "diskio_impl.h"

static const char *TAG = "SD_card";

#define CHECK_EXECUTE_RESULT(err, str) do { \
    if ((err) !=ESP_OK) { \
        ESP_LOGE(TAG, str" (0x%x).", err); \
        goto cleanup; \
    } \
    } while(0)

sdmmc_card_t* card = NULL;
static char * base_path = NULL;
static FATFS* fs = NULL;

static esp_err_t partition_card(const esp_vfs_fat_mount_config_t *mount_config,
                                const char *drv, sdmmc_card_t *card, BYTE pdrv);


static void call_host_deinit(const sdmmc_host_t *host_config)
{
    if (host_config->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host_config->deinit_p(host_config->slot);
    } else {
        host_config->deinit();
    }
}

static esp_err_t init_sdmmc_host(int slot, const void *slot_config, int *out_slot)
{
    *out_slot = slot;
    return sdmmc_host_init_slot(slot, (const sdmmc_slot_config_t*) slot_config);
}

esp_err_t mount_sd_card(bool format_if_fail) {

    esp_err_t err;
    uint8_t pdrv = FF_DRV_NOT_USED;
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case mount fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .max_files = 10,
            .allocation_unit_size = 16 * 1024
    };

    if (format_if_fail == true) {
        mount_config.format_if_mount_failed = true;
        //printf("Format if mount failed is selected!\n");
    } else {
        mount_config.format_if_mount_failed = false;
        //printf("Format if mount failed is NOT selected!\n");
    }
    bool host_inited = false;
    
    int card_handle = -1;   //uninitialized

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
#ifdef CONFIG_HTTP_SERVER_SD_CARD_HIGHSPEED
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; //SDMMC_FREQ_HIGHSPEED or SDMMC_FREQ_DEFAULT for 40M hz or 20 Mhz
#endif
    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    // card detect on gpio pin 21. Comment for no card detect
    slot_config.gpio_cd = GPIO_NUM_21;
    // use internal pullup for sdcard lines, not needed if external pullup is used. Only needed for testing.
    // slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }

    // not using ff_memalloc here, as allocation in internal RAM is preferred
    card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGD(TAG, "could not locate new sdmmc_card_t");
        err = ESP_ERR_NO_MEM;
        return err;
    }

    base_path = (char*)malloc(sizeof(SD_MOUNT));
    strcpy(base_path, SD_MOUNT);
    if(base_path == NULL){
        ESP_LOGD(TAG, "could not copy base_path");
        free(card);
        card = NULL;
        err = ESP_ERR_NO_MEM;
        return err;
    }

    err = (host.init)();
    CHECK_EXECUTE_RESULT(err, "host init failed");
    //deinit() needs to be called to revert the init
    host_inited = true;
    //If this failed (indicated by card_handle != -1), slot deinit needs to called()
    //leave card_handle as is to indicate that (though slot deinit not implemented yet)
    err = init_sdmmc_host(host.slot, &slot_config, &card_handle);
    CHECK_EXECUTE_RESULT(err, "slot init failed");

    // probe and initialize card. This takes some time. 40-60mS.
    err = sdmmc_card_init(&host, card);
    CHECK_EXECUTE_RESULT(err, "sdmmc_card_init failed");

    ff_diskio_register_sdmmc(pdrv, card);
    //ESP_LOGI(TAG, "using pdrv=%i", pdrv);
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    
    // connect FATFS to VFS
    err = esp_vfs_fat_register(base_path, drv, mount_config.max_files, &fs);
    
    if (err == ESP_ERR_INVALID_STATE) {
        // it's okay, already registered with VFS
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_vfs_fat_register failed 0x(%x)", err);
        goto fail;
    }

    // Try to mount partition
    FRESULT res = f_mount(fs, drv, 1);

    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGW(TAG, "failed to mount card (%d)", res);
        if (!((res == FR_NO_FILESYSTEM || res == FR_INT_ERR)
              && mount_config.format_if_mount_failed)) {
            goto fail;
        }

        err = partition_card(&mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        }

        ESP_LOGW(TAG, "mounting again");
        res = f_mount(fs, drv, 0);
        if (res != FR_OK) {
            err = ESP_FAIL;
            ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
            goto fail;
        }
    }
    sdmmc_card_print_info(stdout, card);

    return ESP_OK;
cleanup:
    if (host_inited) {
        call_host_deinit(&host);
    }
    free(card);
    free(base_path);
    card = NULL;
    base_path = NULL;
    return err;

fail:
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(base_path);
    ff_diskio_unregister(pdrv);
    free(card);
    free(base_path);
    free(fs);
    fs = NULL;
    card = NULL;
    base_path = NULL;
    return err;
}


static esp_err_t partition_card(const esp_vfs_fat_mount_config_t *mount_config,
                                const char *drv, sdmmc_card_t *card, BYTE pdrv)
{
    FRESULT res = FR_OK;
    esp_err_t err;
    const size_t workbuf_size = 4096;
    void* workbuf = NULL;

    ESP_LOGW(TAG, "partitioning card");

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    DWORD plist[] = {100, 0, 0, 0};
    res = f_fdisk(pdrv, plist, workbuf);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_fdisk failed (%d)", res);
        goto fail;
    }
    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
                card->csd.sector_size,
                mount_config->allocation_unit_size);
    ESP_LOGW(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
    res = f_mkfs(drv, FM_ANY, alloc_unit_size, workbuf, workbuf_size);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_mkfs failed (%d)", res);
        goto fail;
    }

    free(workbuf);
    return ESP_OK;
fail:
    free(workbuf);
    return err;
}

esp_err_t format_sd_card(void) {
    
    esp_err_t err;
    const char mount_point[] = SD_MOUNT;
    
    // get the current drive number
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xff) {
        // exit if no sd-card is found
        return ESP_ERR_INVALID_ARG;
    }
    if (card == NULL) 
    {
        return ESP_FAIL;
    }

    // correct format for drive number.
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 10,
            .allocation_unit_size = 16 * 1024
    };
    ESP_LOGW(TAG, "Format SD card !!");
    
    err = partition_card(&mount_config, drv, card, pdrv);
        
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(TAG, "mounting after formatting");
    FRESULT res = f_mount(fs, drv, 0);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
        goto fail;
    }

    if (err == ESP_OK) return err; 

fail:
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(mount_point);
    ff_diskio_unregister(pdrv);
    return err;
}

/* Get total space and free space on mounted SD card */ 
uint8_t get_freespace_sd(uint32_t *tot, uint32_t *free) {

    FATFS *fs;
    DWORD fre_clust, fre_sect, tot_sect;
    uint8_t status = 0;
    
    if (!card) return status;

    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xff) {
        return status;
    }

    char drv[3] = {(char)('0' + pdrv), ':', 0};

    /* Get volume information and free clusters of drive drv  */
    if(f_getfree(drv, &fre_clust, &fs) == FR_OK) {
        /* Get total sectors and free sectors */
        tot_sect = (fs->n_fatent - 2) * fs->csize;
        fre_sect = fre_clust * fs->csize;

        /* free space (assuming 512 bytes/sector) */
	    *tot = tot_sect / 2;
	    *free = fre_sect / 2;
        status = 1;
        return status;

    } else{
        tot = 0;
        free = 0;
        return status;
    }

}

/* Get info from a mounted SD card. Will return Name and frequency*/
uint8_t get_sdcard_info(char* name, uint16_t* freq_khz) {
    
    uint8_t status = 0;
    if (card != NULL)
    {
        if (strlcpy(name, card->cid.name, sizeof(card->cid.name)) >= sizeof(card->cid.name)) {
            ESP_LOGW(TAG, "Input string too small");
            return status;
        }
        *freq_khz = card->max_freq_khz;
        status = 1;
    }
    return status;
}
