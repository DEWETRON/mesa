// Copyright (c) 2004-2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: MIT

#include "trace.h"
#include "cli.h"
#include "ptp.h"
#include <microchip/ethernet/switch/api/types.h>
#include <microchip/ethernet/switch/api/security.h>
#include <stdio.h>


#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

static mesa_vid_t  vid = 100;
static mesa_ace_id_t acl_id = 1;


extern meba_inst_t meba_global_inst;

void log_rc_error(const char* func, mesa_rc rc)
{
    fprintf(stderr, "%s : %d", func, rc);
    cli_printf("%s : %d", func, rc);
}


void dbg_print_mesa_acl_frame_key_t(mesa_acl_port_conf_t *acl_port_cfg);

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
    mesa_rc rc;
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

    for (int port_no=0; port_no<port_cnt;++port_no) {
        cli_printf("   %i  ", port_no);

        // Enabled
        {
            //cli_printf("  FALSE");
        }
        
        // PTP-State
        {
            mepa_ts_ptp_clock_conf_t ptpclock_conf;
            rc = meba_phy_ts_rx_clock_conf_get(meba_global_inst, port_no, 0, &ptpclock_conf);
            //log_rc_error("meba_phy_ts_rx_clock_conf_get", rc);

            if (ptpclock_conf.enable) {
                cli_printf("  TRUE ");

                if  (ptpclock_conf.delaym_type == MEPA_TS_PTP_DELAYM_E2E) 
                {
                    cli_printf("  e2et");
                }
                else {
                    cli_printf("  p2pt");
                }
            }
            else {
                cli_printf("  FALSE");

                cli_printf("  dsbl");
            }
	                
        }
        
        // Internal
        {
            mesa_ts_operation_mode_t mode;
            rc = mesa_ts_operation_mode_get(0, port_no, &mode);
            //log_rc_error("mesa_ts_operation_mode_get", rc);
            switch(mode.mode) {
            case MESA_TS_MODE_NONE:     cli_printf("       None"); break;
            case MESA_TS_MODE_EXTERNAL: cli_printf("       FALSE"); break;
            case MESA_TS_MODE_INTERNAL: cli_printf("       TRUE"); break;
            default: cli_printf("       UNKN"); break;
            }
            {
                mepa_ts_init_conf_t init_conf;
                rc = meba_phy_ts_init_conf_get(meba_global_inst, 0, &init_conf);
                log_rc_error("meba_phy_ts_init_conf_set", rc);
                //cli_printf(" %d", init_conf.clk_src);
            }
        }

        // EOL
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
    log_rc_error("mesa_ts_external_clock_mode_get", rc);

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



static void cli_cmd_ptp_setup_transparent_clock(cli_req_t *req)
{
    mesa_rc rc;

    rc = mscc_appl_ptp_setup_tc(NULL);
        
    log_rc_error("mesa_ace_add", rc);

    // for (int port_no=0; port_no<4;++port_no)
    // {
    //     mesa_acl_port_conf_t acl_port_cfg;
    //     rc = mesa_acl_port_conf_get(NULL, port_no, &acl_port_cfg);
    //     log_rc_error("mesa_acl_port_conf_get", rc);

    //     dbg_print_mesa_acl_frame_key_t(&acl_port_cfg);
    // }

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
    {
        "Ptp tc [<clock>]",
        "Configure transparent clock",
        cli_cmd_ptp_setup_transparent_clock
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



mesa_rc mscc_appl_ptp_setup_tc(const mesa_inst_t inst)
{
    mesa_rc rc;
    mesa_port_list_t  port_list;
    mesa_ace_t        ace;

    mesa_port_list_clear(&port_list);
    mesa_port_list_set(&port_list, 0, TRUE);
    mesa_port_list_set(&port_list, 1, TRUE);
    mesa_port_list_set(&port_list, 2, TRUE);
    mesa_port_list_set(&port_list, 3, TRUE);
    mesa_vlan_port_members_set(NULL, vid, &port_list);

    {
        // configure transparent clock PTP Ethernet
        rc = mesa_ace_init(NULL, MESA_ACE_TYPE_ETYPE, &ace);
        ace.id = acl_id;
        ace.port_list = port_list;
        ace.action.ptp_action = MESA_ACL_PTP_ACTION_ONE_STEP;
        rc = mesa_ace_add(NULL, MESA_ACE_ID_LAST, &ace);
        //log_rc_error("mesa_ace_add", rc);
    }
    {
        // configure transparent clock PTP UDP

        rc = mesa_ace_init(NULL, MESA_ACE_TYPE_IPV4, &ace);
        ace.id = acl_id+1;
        ace.port_list = port_list;
        ace.action.ptp_action = MESA_ACL_PTP_ACTION_ONE_STEP;
        // UDP
        ace.frame.ipv4.proto.value = 17;  
        ace.frame.ipv4.proto.mask = 0xff;
        ace.frame.ipv4.sport.in_range = 1;
        // PTP Port
        ace.frame.ipv4.sport.low = 319;
        ace.frame.ipv4.sport.high = 319;
        ace.frame.ipv4.dport = ace.frame.ipv4.sport;

        rc = mesa_ace_add(NULL, MESA_ACE_ID_LAST, &ace);
        //log_rc_error("mesa_ace_add", rc);
    }
    return rc;
}


void dbg_print_mesa_acl_ptp_action_conf_t(mesa_acl_ptp_action_conf_t* ptp)
{
    cli_printf("ptp\n");
    cli_printf("response = %s\n", ptp->response == MESA_ACL_PTP_RSP_NONE ? "MESA_ACL_PTP_RSP_NONE" :
        ptp->response == MESA_ACL_PTP_RSP_DLY_REQ_RSP_TS_UPD ? "MESA_ACL_PTP_RSP_DLY_REQ_RSP_TS_UPD" : "MESA_ACL_PTP_RSP_DLY_REQ_RSP_NO_TS");

    cli_printf("log_message_interval = %d\n", ptp->log_message_interval);
    cli_printf("copy_smac_to_dmac = %d\n", ptp->copy_smac_to_dmac);
    cli_printf("set_smac_to_port_mac = %d\n", ptp->set_smac_to_port_mac);
    cli_printf("dom_sel = %d\n", ptp->dom_sel);
    cli_printf("sport = %d\n", ptp->sport);
    cli_printf("dport = %d\n", ptp->dport);

}

void dbg_print_mesa_acl_action_t(mesa_acl_action_t *action)
{
    cli_printf("action\n");
    cli_printf("cpu = %d\n", action->cpu);
    cli_printf("cpu_once = %d\n", action->cpu_once);
    cli_printf("cpu_disable = %d\n", action->cpu_disable);
    cli_printf("cpu_queue = %d\n", action->cpu_queue);
    cli_printf("police = %d\n", action->police);
    cli_printf("policer_no = %d\n", action->policer_no);
    cli_printf("evc_police = %d\n", action->evc_police);
    cli_printf("evc_policer_id = %d\n", action->evc_policer_id);
    cli_printf("learn = %d\n", action->learn);

    cli_printf("port_action = %s\n", action->port_action == MESA_ACL_PORT_ACTION_NONE ? "MESA_ACL_PORT_ACTION_NONE" :
        action->port_action == MESA_ACL_PORT_ACTION_FILTER ? "MESA_ACL_PORT_ACTION_FILTER" : "MESA_ACL_PORT_ACTION_REDIR");
    
    // port_list TODO

    cli_printf("mirror = %d\n", action->mirror);
    cli_printf("ptp_action = %s\n", action->ptp_action == MESA_ACL_PTP_ACTION_NONE ? "MESA_ACL_PTP_ACTION_NONE" :
        action->ptp_action == MESA_ACL_PTP_ACTION_ONE_STEP ? "MESA_ACL_PTP_ACTION_ONE_STEP" :
        action->ptp_action == MESA_ACL_PTP_ACTION_ONE_STEP_ADD_DELAY ? "MESA_ACL_PTP_ACTION_ONE_STEP_ADD_DELAY" :
        action->ptp_action == MESA_ACL_PTP_ACTION_ONE_STEP_SUB_DELAY_1 ? "MESA_ACL_PTP_ACTION_ONE_STEP_SUB_DELAY_1" :
        action->ptp_action == MESA_ACL_PTP_ACTION_ONE_STEP_SUB_DELAY_2 ? "MESA_ACL_PTP_ACTION_ONE_STEP_SUB_DELAY_2" :
        action->ptp_action == MESA_ACL_PTP_ACTION_ONE_AND_TWO_STEP ? "MESA_ACL_PTP_ACTION_ONE_AND_TWO_STEP" :
        "MESA_ACL_PTP_ACTION_TWO_STEP");
        
    dbg_print_mesa_acl_ptp_action_conf_t(&action->ptp);

    // addr TODO
    cli_printf("lm_cnt_disable = %d\n", action->lm_cnt_disable);
    cli_printf("mac_swap = %d\n", action->mac_swap);
    cli_printf("ifh_flag = %d\n", action->ifh_flag);
}

void dbg_print_mesa_acl_frame_key_t(mesa_acl_port_conf_t *acl_port_cfg)
{
    cli_printf("mesa_acl_port_conf_t: %u\n", acl_port_cfg->policy_no);
   
    cli_printf("arp =  %s\n", acl_port_cfg->key.arp == MESA_ACL_KEY_DEFAULT ? "MESA_ACL_KEY_DEFAULT" :
        acl_port_cfg->key.arp == MESA_ACL_KEY_ETYPE ? "MESA_ACL_KEY_ETYPE" : "MESA_ACL_KEY_EXT");
    cli_printf("ipv4 = %s\n", acl_port_cfg->key.ipv4 == MESA_ACL_KEY_DEFAULT ? "MESA_ACL_KEY_DEFAULT" :
        acl_port_cfg->key.ipv4 == MESA_ACL_KEY_ETYPE ? "MESA_ACL_KEY_ETYPE" : "MESA_ACL_KEY_EXT");
    cli_printf("ipv6 = %s\n", acl_port_cfg->key.ipv6 == MESA_ACL_KEY_DEFAULT ? "MESA_ACL_KEY_DEFAULT" :
        acl_port_cfg->key.ipv6 == MESA_ACL_KEY_ETYPE ? "MESA_ACL_KEY_ETYPE" : "MESA_ACL_KEY_EXT");

    dbg_print_mesa_acl_action_t(&acl_port_cfg->action);
}
