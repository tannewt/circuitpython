/*
 * External nRF70 firmware loader: reads the firmware blob from a flash
 * partition instead of embedding it in the application binary.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf70_fw_ext, LOG_LEVEL_INF);

/* Forward declarations — avoids pulling in the deep nrf_wifi header chain. */
enum nrf_wifi_status {
    NRF_WIFI_STATUS_SUCCESS = 0,
    NRF_WIFI_STATUS_FAIL = -1,
};
struct nrf_wifi_fmac_fw_info;

extern enum nrf_wifi_status nrf_wifi_fmac_fw_parse(void *fmac_dev_ctx,
    const void *fw_data,
    unsigned int fw_size,
    struct nrf_wifi_fmac_fw_info *fw_info);

extern enum nrf_wifi_status nrf_wifi_fmac_fw_load(void *fmac_dev_ctx,
    struct nrf_wifi_fmac_fw_info *fmac_fw);

#define NRF70_FW_PARTITION_ID FIXED_PARTITION_ID(nrf70_fw_partition)

/* Size of nrf_wifi_fmac_fw_info is opaque; allocate generously. */
#define FW_INFO_SIZE 256

enum nrf_wifi_status nrf_wifi_fw_load(void *rpu_ctx) {
    const struct flash_area *fa;
    int rc;
    char *buf;
    char fw_info_buf[FW_INFO_SIZE];
    struct nrf_wifi_fmac_fw_info *fw_info = (void *)fw_info_buf;
    enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;

    memset(fw_info_buf, 0, sizeof(fw_info_buf));

    rc = flash_area_open(NRF70_FW_PARTITION_ID, &fa);
    if (rc) {
        LOG_ERR("Failed to open nrf70_fw partition: %d", rc);
        return NRF_WIFI_STATUS_FAIL;
    }

    buf = k_malloc(fa->fa_size);
    if (!buf) {
        LOG_ERR("Failed to allocate %u bytes for nRF70 FW", fa->fa_size);
        flash_area_close(fa);
        return NRF_WIFI_STATUS_FAIL;
    }

    rc = flash_area_read(fa, 0, buf, fa->fa_size);
    flash_area_close(fa);
    if (rc) {
        LOG_ERR("Failed to read nRF70 FW from flash: %d", rc);
        k_free(buf);
        return NRF_WIFI_STATUS_FAIL;
    }

    status = nrf_wifi_fmac_fw_parse(rpu_ctx, buf, fa->fa_size, fw_info);
    if (status != NRF_WIFI_STATUS_SUCCESS) {
        LOG_ERR("nrf_wifi_fmac_fw_parse failed");
        k_free(buf);
        return status;
    }

    status = nrf_wifi_fmac_fw_load(rpu_ctx, fw_info);
    k_free(buf);

    if (status != NRF_WIFI_STATUS_SUCCESS) {
        LOG_ERR("nrf_wifi_fmac_fw_load failed");
    }

    return status;
}
