// Copyright (c) 2004-2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: MIT


#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#include "microchip/ethernet/switch/api.h"
#include "microchip/ethernet/board/api.h"
#include "main.h"
#include "trace.h"

typedef struct {
    int fd;
    int freq;
    int padding;
} spi_conf_t;

static spi_conf_t spi_conf[SPI_USER_CNT];

static mscc_appl_trace_module_t trace_module = {
    .name = "spi"
};

enum {
    TRACE_GROUP_DEFAULT,
    TRACE_GROUP_CNT
};

static mscc_appl_trace_group_t trace_groups[TRACE_GROUP_CNT] = {
    // TRACE_GROUP_DEFAULT
    {
        .name = "default",
        .level = MESA_TRACE_LEVEL_ERROR
    },
};

/* MEBA callouts */
#define TO_SPI(_a_)     (_a_ & 0x007FFFFF) /* 23 bit SPI address */
#define SPI_NR_BYTES     7                 /* Number of bytes to transmit or receive */
#define SPI_PADDING_MAX 15                 /* Maximum number of optional padding bytes */

#if 1

//TODO: utilize nexdaq haeder

typedef uint64_t t_uint64;
typedef uint32_t t_uint32;
typedef uint16_t t_uint16;
typedef uint8_t t_uint8;

enum t_ioctl_data_type
{
    IOCTL_TYPE_UNKNOWN,
    IOCTL_TYPE_NO_DATA,
    IOCTL_TYPE_REGISTER_DATA,
    IOCTL_TYPE_DMA_BUFFER_CONFIG,
    IOCTL_TYPE_PARAMETER_DEV_DATA,
    IOCTL_TYPE_MISC_DATA,
    IOCTL_TYPE_STRING,
    IOCTL_TYPE_AVALON_FIRMWARE_BLOCK,
    IOCTL_TYPE_ASYNC_GET_FRAMES,
    IOCTL_TYPE_DMA_BUFFER_SELECT,
};

struct t_bar_data
{
    t_uint32 bar;
    t_uint32 offset;
    t_uint32 value;
};

struct t_dma_buffer_config
{
    t_uint32 buffer_address_low;
    t_uint32 buffer_address_high;
    t_uint32 scan_size;
    t_uint32 block_size;
    t_uint32 block_count;
    t_uint32 dma_channel;
};

struct t_param_data
{
    t_uint32 param_id;  // also used for dma_channel
    t_uint32 value1;
    t_uint32 value2;
};

struct t_misc_data
{
    t_uint32 value1;
    t_uint32 value2;
    t_uint32 value3;
    t_uint32 value4;
    t_uint32 value5;
};


struct t_string
{
    char string[24];
};


struct t_avalon_firmware_block
{
    t_uint32 buffer_address_low;
    t_uint32 buffer_address_high;
    t_uint32 offset;
    t_uint32 size;
};

struct t_dma_select
{
    t_uint32 dma_channel;
};

#define NEXIO_RETURN_CODE_STARTS     0x200

#define NEXIO_RETURN_CODE_SECTION_1  0x300

/**
 * DRV->API Return code values
 */
typedef enum _nx_return_code
{
    /** Section 0 error codes */
    NXApiSuccess = NEXIO_RETURN_CODE_STARTS,

    NXApiAddressConversionError = NEXIO_RETURN_CODE_SECTION_1,
    NXApiMemoryError,
    NXApiIoctlError,              // ioctl general failure -> undefined ...
       
} NX_RETURN_CODE;

struct t_ioctl_data
{
    NX_RETURN_CODE return_code;
    enum t_ioctl_data_type payload_type;

    union {
        struct t_bar_data bar_data;
        struct t_dma_buffer_config dma_buffer_cfg;
        struct t_param_data param_data;
        struct t_misc_data misc_data;
        struct t_string string;
        struct t_avalon_firmware_block avalon_firmware_block;
        struct t_dma_select dma_sel;
        // 24 bytes wide payload, set in stone.
        t_uint8 _reserve_size[24];
    } payload;
};

#define MGT_IOCTL_DATA(name, typeid) struct t_ioctl_data name;  \
    memset(&name, 0, sizeof(name));                             \
    name.payload_type = typeid;

//LAN-SPI
#define BAR5 5

#  define NEXIO_IOCTL_NUMBER(num) _IOWR(/*magic=*/666, num, struct t_ioctl_data *)

enum nexio_ioctl
{
    NEXIO_REG_SET                                       = NEXIO_IOCTL_NUMBER(10),
    NEXIO_REG_GET                                       = NEXIO_IOCTL_NUMBER(11),
};

static NX_RETURN_CODE do_ioctl(
    int handle,
    int request,
    struct t_ioctl_data *data)
{
    int err = ioctl(handle, request, data);
    return err? NXApiIoctlError: data->return_code;
}


mesa_rc spi_read(spi_user_t     user,
                 const uint32_t addr,
                 uint32_t       *const value)
{
    NX_RETURN_CODE rc;
    MGT_IOCTL_DATA(ioctl_data, IOCTL_TYPE_REGISTER_DATA);

    spi_conf_t *conf = &spi_conf[user];

    if (!conf->fd)
    {
        T_E("spi_read, no file-descriptor");
        return MESA_RC_ERROR;
    }

    ioctl_data.payload.bar_data.bar    = BAR5;
    ioctl_data.payload.bar_data.offset = addr << 2; //<<2 from nexdac code

    rc = do_ioctl(conf->fd, NEXIO_REG_GET, &ioctl_data);
    if (rc != NXApiSuccess)
    {
        T_E("spi_read failed (1) %d", rc);
        return MESA_RC_ERROR;
    }
    if (ioctl_data.return_code == NXApiSuccess)
    {
        T_D("spi_read: %04X = %04X", addr, ioctl_data.payload.bar_data.value);
        *value = ioctl_data.payload.bar_data.value;
    }
    else
    {
        T_E("spi_read failed (2) %d", ioctl_data.return_code);
    }
    
    return MESA_RC_OK;
}

mesa_rc spi_write(spi_user_t     user,
                  const uint32_t addr,
                  const uint32_t value)
{
    NX_RETURN_CODE rc;
    MGT_IOCTL_DATA(ioctl_data, IOCTL_TYPE_REGISTER_DATA);

    spi_conf_t *conf = &spi_conf[user];

    if (!conf->fd)
    {
        T_E("spi_read, no file-descriptor");
        return MESA_RC_ERROR;
    }

    T_D("spi_write: %04x %04x",
        addr, value);

#if 1        

    ioctl_data.payload.bar_data.bar    = BAR5;
    ioctl_data.payload.bar_data.offset = addr << 2; //<<2 from nexdac code;
    ioctl_data.payload.bar_data.value  = value;
    return do_ioctl(conf->fd, NEXIO_REG_SET, &ioctl_data) == NXApiSuccess ?
        MESA_RC_OK : MESA_RC_ERROR;
#else
    return MESA_RC_OK;
#endif        

}

mesa_rc spi_reg_read(const mesa_chip_no_t chip_no,
                     const uint32_t       addr,
                     uint32_t             *const value)
{
    return spi_read(SPI_USER_REG, addr, value);
}

mesa_rc spi_reg_write(const mesa_chip_no_t chip_no,
                      const uint32_t       addr,
                      const uint32_t       value)
{
    return spi_write(SPI_USER_REG, addr, value);
}

mesa_rc spi_io_init(spi_user_t user, const char *device, int freq, int padding)
{
    spi_conf_t *conf;
    int fd, ret, mode = 0;

    if (user >= SPI_USER_CNT) {
        T_E("Invalid spi user %d", user);
        exit(1);
    }

#if 0
    if (padding > SPI_PADDING_MAX) {
        T_E("Invalid spi_padding %d, Range is 0..%d",
            padding, SPI_PADDING_MAX);
        exit(1);
    }
#endif    

    //fd = open(device, O_RDWR | O_NONBLOCK);
    fd = open(device, O_RDWR);
    if (fd < 0) {
        T_E("%s: %s", device, strerror(errno));
        exit(1);
    }

#if 0
    // TODO, delte this once it has been fixed in the DTS
    ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
    if (ret < 0) {
        T_E("Error setting spi wr-mode");
        close(fd);
        return MESA_RC_ERROR;
    }

    // TODO, delte this once it has been fixed in the DTS
    ret = ioctl(fd, SPI_IOC_RD_MODE, &mode);
    if (ret < 0) {
        T_E("Error setting spi wr-mode");
        close(fd);
        return MESA_RC_ERROR;
    }
#endif    

    T_D("spi: %s opened", device);
    conf = &spi_conf[user];
    conf->fd = fd;
    conf->freq = freq;
    conf->padding = padding;

    return MESA_RC_OK;
}
#else
mesa_rc spi_read(spi_user_t     user,
                 const uint32_t addr,
                 uint32_t       *const value)
{
    uint8_t tx[SPI_NR_BYTES + SPI_PADDING_MAX] = { 0 };
    uint8_t rx[sizeof(tx)] = { 0 };
    uint32_t siaddr = TO_SPI(addr);
    spi_conf_t *conf = &spi_conf[user];
    int spi_padding = conf->padding;
    int ret;

    memset(tx, 0xff, sizeof(tx));
    tx[0] = (uint8_t)(siaddr >> 16);
    tx[1] = (uint8_t)(siaddr >> 8);
    tx[2] = (uint8_t)(siaddr >> 0);

    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long) tx,
        .rx_buf = (unsigned long) rx,
        .len = SPI_NR_BYTES + spi_padding,
        .delay_usecs = 0,
        .speed_hz = conf->freq,
        .bits_per_word = 8,
    };

    ret = ioctl(conf->fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 1) {
        T_E("spi_read: %s", strerror(errno));
        return MESA_RC_ERROR;
    }

    uint32_t rxword =
            (rx[3 + spi_padding] << 24) |
            (rx[4 + spi_padding] << 16) |
            (rx[5 + spi_padding] << 8) |
            (rx[6 + spi_padding] << 0);

    *value = rxword;

    T_D("RX: %02x %02x %02x-%02x %02x %02x %02x",
        tx[0], tx[1], tx[2],
        rx[3 + spi_padding],
        rx[4 + spi_padding],
        rx[5 + spi_padding],
        rx[6 + spi_padding]);

    return MESA_RC_OK;
}

mesa_rc spi_write(spi_user_t     user,
                  const uint32_t addr,
                  const uint32_t value)
{
    uint8_t tx[SPI_NR_BYTES] = { 0 };
    uint8_t rx[sizeof(tx)] = { 0 };
    uint32_t siaddr = TO_SPI(addr);
    spi_conf_t *conf = &spi_conf[user];
    int ret;

    tx[0] = (uint8_t)(0x80 | (siaddr >> 16));
    tx[1] = (uint8_t)(siaddr >> 8);
    tx[2] = (uint8_t)(siaddr >> 0);
    tx[3] = (uint8_t)(value >> 24);
    tx[4] = (uint8_t)(value >> 16);
    tx[5] = (uint8_t)(value >> 8);
    tx[6] = (uint8_t)(value >> 0);

    T_D("TX: %02x %02x %02x-%02x %02x %02x %02x",
        tx[0], tx[1], tx[2], tx[3], tx[4], tx[5], tx[6]);

    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long) tx,
        .rx_buf = (unsigned long) rx,
        .len = sizeof(tx),
        .delay_usecs = 0,
        .speed_hz = conf->freq,
        .bits_per_word = 8,
    };

    ret = ioctl(conf->fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 1) {
        T_E("spi_write: %s", strerror(errno));
        return MESA_RC_ERROR;
    }

    return MESA_RC_OK;
}

mesa_rc spi_reg_read(const mesa_chip_no_t chip_no,
                     const uint32_t       addr,
                     uint32_t             *const value)
{
    return spi_read(SPI_USER_REG, addr, value);
}

mesa_rc spi_reg_write(const mesa_chip_no_t chip_no,
                      const uint32_t       addr,
                      const uint32_t       value)
{
    return spi_write(SPI_USER_REG, addr, value);
}

mesa_rc spi_io_init(spi_user_t user, const char *device, int freq, int padding)
{
    spi_conf_t *conf;
    int fd, ret, mode = 0;

    if (user >= SPI_USER_CNT) {
        T_E("Invalid spi user %d", user);
        exit(1);
    }

    if (padding > SPI_PADDING_MAX) {
        T_E("Invalid spi_padding %d, Range is 0..%d",
            padding, SPI_PADDING_MAX);
        exit(1);
    }
    fd = open(device, O_RDWR);
    if (fd < 0) {
        T_E("%s: %s", device, strerror(errno));
        exit(1);
    }

    // TODO, delte this once it has been fixed in the DTS
    ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
    if (ret < 0) {
        T_E("Error setting spi wr-mode");
        close(fd);
        return MESA_RC_ERROR;
    }

    // TODO, delte this once it has been fixed in the DTS
    ret = ioctl(fd, SPI_IOC_RD_MODE, &mode);
    if (ret < 0) {
        T_E("Error setting spi wr-mode");
        close(fd);
        return MESA_RC_ERROR;
    }

    T_D("spi: %s opened", device);
    conf = &spi_conf[user];
    conf->fd = fd;
    conf->freq = freq;
    conf->padding = padding;

    return MESA_RC_OK;
}
#endif

void mscc_appl_spi_init(mscc_appl_init_t *init)
{
    if (init->cmd == MSCC_INIT_CMD_REG) {
        mscc_appl_trace_register(&trace_module, trace_groups, TRACE_GROUP_CNT);
    }
}
