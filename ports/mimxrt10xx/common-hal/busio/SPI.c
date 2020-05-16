/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Scott Shawcroft
 * Copyright (c) 2019 Artur Pacholec
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "shared-bindings/busio/SPI.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "periph.h"

#include "fsl_lpspi.h"

#include <stdio.h>

//arrays use 0 based numbering: SPI1 is stored at index 0
#define MAX_SPI 4
STATIC bool reserved_spi[MAX_SPI];
STATIC bool never_reset_spi[MAX_SPI];

STATIC void config_periph_pin(const mcu_periph_obj_t *periph) {
    IOMUXC_SetPinMux(
        periph->pin->mux_reg, periph->mux_mode,
        periph->input_reg, periph->input_idx,
        0,
        0);

    IOMUXC_SetPinConfig(0, 0, 0, 0,
        periph->pin->cfg_reg,
        IOMUXC_SW_PAD_CTL_PAD_HYS(0)
            | IOMUXC_SW_PAD_CTL_PAD_PUS(0)
            | IOMUXC_SW_PAD_CTL_PAD_PUE(0)
            | IOMUXC_SW_PAD_CTL_PAD_PKE(1)
            | IOMUXC_SW_PAD_CTL_PAD_ODE(0)
            | IOMUXC_SW_PAD_CTL_PAD_SPEED(2)
            | IOMUXC_SW_PAD_CTL_PAD_DSE(4)
            | IOMUXC_SW_PAD_CTL_PAD_SRE(0));
}

#define LPSPI_MASTER_CLK_FREQ (CLOCK_GetFreq(kCLOCK_Usb1PllPfd0Clk) / (CLOCK_GetDiv(kCLOCK_LpspiDiv)))

void spi_reset(void) {
    for (int i = 0; i < MAX_SPI; i++) {
        reserved_spi[i] = false;
    }
}

void common_hal_busio_spi_construct(busio_spi_obj_t *self,
        const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
        const mcu_pin_obj_t *miso) {

    const uint32_t sck_count = MP_ARRAY_SIZE(mcu_spi_sck_list);
    const uint32_t miso_count = MP_ARRAY_SIZE(mcu_spi_miso_list);
    const uint32_t mosi_count = MP_ARRAY_SIZE(mcu_spi_mosi_list);
    bool spi_taken = false;

    for (uint i = 0; i < sck_count; i++) {
        if (mcu_spi_sck_list[i].pin != clock) {
            continue;
        }
        //if both MOSI and MISO exist, loop search normally
        if ((mosi != NULL) && (miso != NULL)) {
            for (uint j = 0; j < mosi_count; j++) {
                if ((mcu_spi_mosi_list[i].pin != mosi) 
                    || (mcu_spi_sck_list[i].bank_idx != mcu_spi_mosi_list[j].bank_idx)){
                    continue;
                }
                for (uint k = 0; k < miso_count; k++) {
                    if ((mcu_spi_miso_list[k].pin != miso) //everything needs the same index
                        || (mcu_spi_sck_list[i].bank_idx != mcu_spi_miso_list[k].bank_idx)) {
                        continue;
                    }
                    //keep looking if the SPI is taken, edge case
                    if (reserved_spi[mcu_spi_sck_list[i].bank_idx - 1]) {
                        spi_taken = true;
                        continue;
                    }
                    //store pins if not
                    self->clock = &mcu_spi_sck_list[i];
                    self->mosi = &mcu_spi_mosi_list[j];
                    self->miso = &mcu_spi_miso_list[k];
                    break;
                }
                if (self->clock != NULL) {
                    break; // Multi-level break to pick lowest peripheral
                }
            }
            if (self->clock != NULL) {
                break;
            }
        // if just MISO, reduce search
        } else if (miso != NULL) {
            for (uint j = 0; j < miso_count; j++) {
                if ((mcu_spi_miso_list[j].pin != miso) //only SCK and MISO need the same index
                    || (mcu_spi_sck_list[i].bank_idx != mcu_spi_miso_list[j].bank_idx)) {
                    continue;
                }
                if (reserved_spi[mcu_spi_sck_list[i].bank_idx - 1]) {
                    spi_taken = true;
                    continue;
                }
                self->clock = &mcu_spi_sck_list[i];
                self->mosi = NULL;
                self->miso = &mcu_spi_miso_list[j];
                break;
            }
            if (self->clock != NULL) {
                break;
            }
        // if just MOSI, reduce search
        } else if (mosi != NULL) {
            for (uint j = 0; j < mosi_count; j++) {
                if ((mcu_spi_mosi_list[j].pin != mosi) //only SCK and MOSI need the same index
                    || (mcu_spi_sck_list[i].bank_idx != mcu_spi_mosi_list[j].bank_idx)) {
                    continue;
                }
                if (reserved_spi[mcu_spi_sck_list[i].bank_idx - 1]) {
                    spi_taken = true;
                    continue;
                }
                self->clock = &mcu_spi_sck_list[i];
                self->mosi = &mcu_spi_mosi_list[j];
                self->miso = NULL;
                break;
            }
            if (self->clock != NULL) {
                break;
            }
        } else {
            //throw an error immediately
            mp_raise_ValueError(translate("Must provide MISO or MOSI pin"));
        }
    }

    if (self->clock != NULL && (self->mosi != NULL || self->miso != NULL)) {
        self->spi = mcu_spi_banks[self->clock->bank_idx - 1];
    } else {
        if (spi_taken) {
            mp_raise_ValueError(translate("Hardware busy, try alternative pins"));
        } else {
            mp_raise_ValueError(translate("Invalid SPI pin selection"));
        }
    }

    config_periph_pin(self->clock);
    if (self->mosi != NULL) {
        config_periph_pin(self->mosi);
    }
    if (self->miso != NULL) {
        config_periph_pin(self->miso);
    }
    reserved_spi[self->clock->bank_idx - 1] = true;

    lpspi_master_config_t config = { 0 };
    LPSPI_MasterGetDefaultConfig(&config);

    // Always start at 250khz which is what SD cards need. They are sensitive to
    // SPI bus noise before they are put into SPI mode.
    config.baudRate = 250000;
    LPSPI_MasterInit(self->spi, &config, LPSPI_MASTER_CLK_FREQ);
    LPSPI_Enable(self->spi, false);
    uint32_t tcrPrescaleValue;
    self->baudrate = LPSPI_MasterSetBaudRate(self->spi, config.baudRate, LPSPI_MASTER_CLK_FREQ, &tcrPrescaleValue);
    LPSPI_Enable(self->spi, true);

    claim_pin(self->clock->pin);
    if (self->mosi != NULL) {
        claim_pin(self->mosi->pin);
    }
    if (self->miso != NULL) {
        claim_pin(self->miso->pin);
    }
}

void common_hal_busio_spi_never_reset(busio_spi_obj_t *self) {
    // TODO
}

bool common_hal_busio_spi_deinited(busio_spi_obj_t *self) {
    return self->clock == NULL;
}

void common_hal_busio_spi_deinit(busio_spi_obj_t *self) {
    if (common_hal_busio_spi_deinited(self)) {
        return;
    }

    self->clock = NULL;
}

bool common_hal_busio_spi_configure(busio_spi_obj_t *self,
        uint32_t baudrate, uint8_t polarity, uint8_t phase, uint8_t bits) {

    LPSPI_Enable(self->spi, false);
    uint32_t tcrPrescaleValue;
    self->baudrate = LPSPI_MasterSetBaudRate(self->spi, baudrate, LPSPI_MASTER_CLK_FREQ, &tcrPrescaleValue);
    LPSPI_Enable(self->spi, true);

    if ((polarity == common_hal_busio_spi_get_polarity(self)) &&
        (phase == common_hal_busio_spi_get_phase(self)) &&
        (bits == ((self->spi->TCR & LPSPI_TCR_FRAMESZ_MASK) >> LPSPI_TCR_FRAMESZ_SHIFT)) + 1) {
        return true;
    }

    lpspi_master_config_t config = { 0 };
    LPSPI_MasterGetDefaultConfig(&config);

    config.baudRate = baudrate;
    config.cpol = polarity;
    config.cpha = phase;
    config.bitsPerFrame = bits;

    LPSPI_Deinit(self->spi);
    LPSPI_MasterInit(self->spi, &config, LPSPI_MASTER_CLK_FREQ);

    return true;
}

bool common_hal_busio_spi_try_lock(busio_spi_obj_t *self) {
    bool grabbed_lock = false;
//    CRITICAL_SECTION_ENTER()
    if (!self->has_lock) {
        grabbed_lock = true;
        self->has_lock = true;
    }
//    CRITICAL_SECTION_LEAVE();
    return grabbed_lock;
}

bool common_hal_busio_spi_has_lock(busio_spi_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_spi_unlock(busio_spi_obj_t *self) {
    self->has_lock = false;
}

bool common_hal_busio_spi_write(busio_spi_obj_t *self,
        const uint8_t *data, size_t len) {
    if (len == 0) {
        return true;
    }
    if (self->mosi == NULL) {
        mp_raise_ValueError(translate("No MOSI Pin"));
    }

    lpspi_transfer_t xfer = { 0 };
    xfer.txData = (uint8_t*)data;
    xfer.dataSize = len;
    xfer.configFlags = kLPSPI_MasterPcs0;

    const status_t status = LPSPI_MasterTransferBlocking(self->spi, &xfer);
    if (status != kStatus_Success)
        printf("%s: status %ld\r\n", __func__, status);

    return (status == kStatus_Success);
}

bool common_hal_busio_spi_read(busio_spi_obj_t *self,
        uint8_t *data, size_t len, uint8_t write_value) {
    if (len == 0) {
        return true;
    }
    if (self->miso == NULL) {
        mp_raise_ValueError(translate("No MISO Pin"));
    }

    LPSPI_SetDummyData(self->spi, write_value);

    lpspi_transfer_t xfer = { 0 };
    xfer.rxData = data;
    xfer.dataSize = len;

    const status_t status = LPSPI_MasterTransferBlocking(self->spi, &xfer);
    if (status != kStatus_Success)
        printf("%s: status %ld\r\n", __func__, status);

    return (status == kStatus_Success);
}

bool common_hal_busio_spi_transfer(busio_spi_obj_t *self, uint8_t *data_out, uint8_t *data_in, size_t len) {
    if (len == 0) {
        return true;
    }
    if (self->miso == NULL || self->mosi == NULL) {
        mp_raise_ValueError(translate("Missing MISO or MOSI Pin"));
    }

    LPSPI_SetDummyData(self->spi, 0xFF);

    lpspi_transfer_t xfer = { 0 };
    xfer.txData = data_out;
    xfer.rxData = data_in;
    xfer.dataSize = len;

    const status_t status = LPSPI_MasterTransferBlocking(self->spi, &xfer);
    if (status != kStatus_Success)
        printf("%s: status %ld\r\n", __func__, status);

    return (status == kStatus_Success);
}

uint32_t common_hal_busio_spi_get_frequency(busio_spi_obj_t* self) {
    return self->baudrate;
}

uint8_t common_hal_busio_spi_get_phase(busio_spi_obj_t* self) {
    return ((self->spi->TCR & LPSPI_TCR_CPHA_MASK) == LPSPI_TCR_CPHA_MASK);
}

uint8_t common_hal_busio_spi_get_polarity(busio_spi_obj_t* self) {
    return ((self->spi->TCR & LPSPI_TCR_CPOL_MASK) == LPSPI_TCR_CPOL_MASK);
}
