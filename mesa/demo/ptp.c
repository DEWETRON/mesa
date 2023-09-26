// Copyright (c) 2004-2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: MIT

#include "trace.h"
#include "cli.h"
#include "ptp.h"
#include <stdio.h>


/*
# show ptp
PTP RS422 clock mode: Disable, delay : 0, Protocol: Serial (Polyt), port: 1
*/
static void cli_cmd_ptp_show(cli_req_t *req)
{
    cli_printf("Missing\n");

}

static const char* PTP_STATE_ENTRIES[] = {
    "Port", "Enabled", "PTP-State", "Internal", "Link", "Port-Timer",
    "Vlan-forw", "Phy-timestamper", "Peer-delay", 
    NULL
};

/*
# show ptp 0 port-state
Port  Enabled  PTP-State  Internal  Link  Port-Timer  Vlan-forw  Phy-timestamper  Peer-delay
----  -------  ---------  --------  ----  ----------  ---------  ---------------  ----------
   1  TRUE     e2et       FALSE     Up    In Sync     Forward    FALSE            OK        
   2  TRUE     e2et       FALSE     Up    In Sync     Forward    FALSE            OK        
   3  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
   4  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
   5  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
   6  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
   7  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
   8  FALSE    dsbl       FALSE     Down  In Sync     Discard    FALSE            OK        
VirtualPort  Enabled  PTP-State  Io-pin
-----------  -------  ---------  ------
          9  FALSE    dsbl            0     
*/
static void cli_cmd_ptp_port_show(cli_req_t *req)
{
    uint32_t port_cnt = mesa_port_cnt(NULL);

    // Print header
    {
        const char** topic = PTP_STATE_ENTRIES;
        while(*topic != NULL) {
            cli_printf("%s  ", *topic);
            ++topic;
        }
        cli_printf("\n");
    }
    {
        const char** topic = PTP_STATE_ENTRIES;
        while(*topic != NULL) {
            int len = strlen(*topic);
            for (int i=0; i<len; ++i) {
                cli_printf("-");
            }
            cli_printf("  ");
            ++topic;
        }
        cli_printf("\n");
    }

    for (int i=0; i<port_cnt;++i) {
        cli_printf("   %i  ", i);
        cli_printf("\n");
    }

}

/*
PTP External One PPS mode: Disable, Clock output enabled: False, frequency : 1, Preferred adj method: Auto
*/
static void cli_cmd_ptp_ext_clock_show(cli_req_t *req)
{
    mesa_ts_ext_clock_mode_t ext_clock_mode;
    mesa_rc rc;
    rc = mesa_ts_external_clock_mode_get(0, &ext_clock_mode);

    cli_printf("PTP External One PPS mode: ");
    switch (ext_clock_mode.one_pps_mode)
    {
    case MESA_TS_EXT_CLOCK_MODE_ONE_PPS_DISABLE:
        cli_printf("Disable");
        /* code */
        break;
    case MESA_TS_EXT_CLOCK_MODE_ONE_PPS_OUTPUT:
        cli_printf("Output");
        /* code */
        break;
    case MESA_TS_EXT_CLOCK_MODE_ONE_PPS_INPUT:
        cli_printf("Input");
        /* code */
        break;
    case MESA_TS_EXT_CLOCK_MODE_ONE_PPS_OUTPUT_INPUT:
        cli_printf("OutInput");
        /* code */
        break;
    default:
        break;
    }

    cli_printf(", Clock output enabled: %s", ext_clock_mode.enable ? "True" : "False");
    cli_printf(", frequency: %u", ext_clock_mode.freq);
    cli_printf(", Preferred adj method: UNKNOWN");

}

static cli_cmd_t cli_cmd_table[] = {
    {
        "Ptp show",
        "Print ptp settings",
        cli_cmd_ptp_show
    },
    {
        "Ptp port-state [<clock>]",
        "Print ptp port state table",
        cli_cmd_ptp_port_show
    },
    {
        "Ptp ext [<clock>]",
        "Print external clock mode",
        cli_cmd_ptp_ext_clock_show
    },
};

static int cli_parm_phy_value(cli_req_t *req)
{
    //debug_cli_req_t *mreq = req->module_req;
    //return cli_parm_u32(req, &mreq->value, 0, 0xffff);
    return 0;
}


static cli_parm_t cli_parm_table[] = {
    {
        "<clock>",
        "Value to be written to register (0-0xffff)",
        CLI_PARM_FLAG_SET,
        cli_parm_phy_value
    },
};

static void ptp_cli_init(void)
{
    int i;

    /* Register commands */
    for (i = 0; i < sizeof(cli_cmd_table)/sizeof(cli_cmd_t); i++) {
        mscc_appl_cli_cmd_reg(&cli_cmd_table[i]);
    }

    /* Register parameters */
    for (i = 0; i < sizeof(cli_parm_table)/sizeof(cli_parm_t); i++) {
        mscc_appl_cli_parm_reg(&cli_parm_table[i]);
    }
}

void mscc_appl_ptp_init(mscc_appl_init_t *init)
{
    switch (init->cmd) {
    case MSCC_INIT_CMD_INIT:
        ptp_cli_init();
        break;
    default:
        break;
    }
}

