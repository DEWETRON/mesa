// Copyright (c) 2004-2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: MIT


#define VTSS_TRACE_GROUP VTSS_TRACE_GROUP_L2
#include "vtss_lan966x_cil.h"

#if defined(VTSS_ARCH_LAN966X)

/* Convert to PGID in chip */
static u32 lan966x_chip_pgid(vtss_state_t *vtss_state, u32 pgid)
{
    if (pgid < vtss_state->port_count) {
        return VTSS_CHIP_PORT(pgid);
    } else {
        return (pgid + VTSS_CHIP_PORTS - vtss_state->port_count);
    }
}

/* Convert from PGID in chip */
static u32 lan966x_vtss_pgid(vtss_state_t *vtss_state, u32 pgid)
{
    vtss_port_no_t port_no;

    if (pgid < VTSS_CHIP_PORTS) {
        for (port_no = 0; port_no < vtss_state->port_count; port_no++)
            if (VTSS_CHIP_PORT(port_no) == pgid)
                break;
        return port_no;
    } else {
        return (vtss_state->port_count + pgid - VTSS_CHIP_PORTS);
    }
}

static vtss_rc lan966x_pgid_mask_write(vtss_state_t *vtss_state,
                                       u32 pgid, BOOL member[VTSS_PORT_ARRAY_SIZE],
                                       BOOL cpu_copy, vtss_packet_rx_queue_t cpu_queue)
{
    u32 mask = vtss_lan966x_port_mask(vtss_state, member);
    u32 queue = 0;

    if (cpu_copy) {
        mask |= VTSS_BIT(VTSS_CHIP_PORT_CPU);
        queue = cpu_queue;
    }
    REG_WR(ANA_PGID(pgid),
           ANA_PGID_PGID(mask) |
           ANA_PGID_CPUQ_DST_PGID(queue));

    return VTSS_RC_OK;
}

static vtss_rc lan966x_pgid_table_write(vtss_state_t *vtss_state,
                                        u32 pgid, BOOL member[VTSS_PORT_ARRAY_SIZE])
{
    vtss_pgid_entry_t *pgid_entry = &vtss_state->l2.pgid_table[pgid];

    return lan966x_pgid_mask_write(vtss_state, lan966x_chip_pgid(vtss_state, pgid), member,
                                   pgid_entry->cpu_copy, pgid_entry->cpu_queue);
}

static vtss_rc lan966x_pgid_update(vtss_state_t *vtss_state, u32 pgid, BOOL member[])
{
    vtss_port_no_t    port_no;
    vtss_pgid_entry_t *pgid_entry;

    pgid = lan966x_vtss_pgid(vtss_state, pgid);
    pgid_entry = &vtss_state->l2.pgid_table[pgid];
    pgid_entry->resv = 1;
    pgid_entry->references = 1;
    for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
        pgid_entry->member[port_no] = member[port_no];
    }
    return lan966x_pgid_table_write(vtss_state, pgid, member);
}

static vtss_rc lan966x_aggr_table_write(vtss_state_t *vtss_state,
                                        u32 ac, BOOL member[VTSS_PORT_ARRAY_SIZE])
{
    return lan966x_pgid_mask_write(vtss_state, PGID_AGGR + ac, member, 0, 0);
}

static vtss_rc lan966x_src_table_write(vtss_state_t *vtss_state,
                                       vtss_port_no_t port_no, BOOL member[VTSS_PORT_ARRAY_SIZE])
{
    return lan966x_pgid_mask_write(vtss_state, PGID_SRC + VTSS_CHIP_PORT(port_no), member, 0, 0);
}

static vtss_rc lan966x_aggr_mode_set(vtss_state_t *vtss_state)
{
    vtss_aggr_mode_t *mode = &vtss_state->l2.aggr_mode;

    REG_WR(ANA_AGGR_CFG,
           (mode->sip_dip_enable ? ANA_AGGR_CFG_AC_IP4_SIPDIP_ENA_M : 0) |
           (mode->sport_dport_enable ? ANA_AGGR_CFG_AC_IP4_TCPUDP_ENA_M : 0) |
           (mode->dmac_enable ? ANA_AGGR_CFG_AC_DMAC_ENA_M : 0) |
           (mode->smac_enable ? ANA_AGGR_CFG_AC_SMAC_ENA_M : 0));
    return VTSS_RC_OK;
}

static vtss_rc lan966x_pmap_table_write(vtss_state_t *vtss_state,
                                        vtss_port_no_t port_no, vtss_port_no_t l_port_no)
{
    REG_WRM(ANA_PORT_CFG(VTSS_CHIP_PORT(port_no)),
             ANA_PORT_CFG_PORTID_VAL(VTSS_CHIP_PORT(l_port_no)),
             ANA_PORT_CFG_PORTID_VAL_M);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_learn_state_set(vtss_state_t *vtss_state,
                                       const BOOL member[VTSS_PORT_ARRAY_SIZE])
{
    vtss_port_no_t port_no;

    for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
        REG_WRM_CTL(ANA_PORT_CFG(VTSS_CHIP_PORT(port_no)),
                    member[port_no], ANA_PORT_CFG_LEARN_ENA_M);
    }
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  MAC address table
 * ================================================================= */
#define MAC_TYPE_NORMAL  0 /* Normal entry */
#define MAC_TYPE_LOCKED  1 /* Locked entry */
#define MAC_TYPE_IPV4_MC 2 /* IPv4 MC entry */
#define MAC_TYPE_IPV6_MC 3 /* IPv6 MC entry */

static vtss_rc lan966x_mac_table_idle(vtss_state_t *vtss_state)
{
    u32 cmd, cnt = 0;

    do {
        REG_RD(ANA_MACACCESS, &cmd);
        cnt++;
        if (cnt == 100000) {
            VTSS_E("Timeout! cmd =  %u", cmd);
        }
    } while (ANA_MACACCESS_MAC_TABLE_CMD_X(cmd) != MACACCESS_CMD_IDLE);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mac_table_add(vtss_state_t *vtss_state,
                                     const vtss_mac_table_entry_t *const entry, u32 pgid)
{
    u32  mach, macl, mask, idx = 0, aged = 0, fwd_kill = 0, type;
    BOOL copy_to_cpu = entry->copy_to_cpu_smac;

    vtss_mach_macl_get(&entry->vid_mac, &mach, &macl);
    VTSS_D("address 0x%08x%08x", mach, macl);

    if (pgid == VTSS_PGID_NONE) {
        /* IPv4/IPv6 MC entry, encode port mask directly */
        mask = vtss_lan966x_port_mask(vtss_state, vtss_state->l2.pgid_table[pgid].member);
        if (entry->vid_mac.mac.addr[0] == 0x01) {
            type = MAC_TYPE_IPV4_MC;
            macl = ((macl & 0x00ffffff) | ((mask << 24) & 0xff000000));
            mach = ((mach & 0xffff0000) | ((mask >> 8) & 0x0000ffff));
        } else {
            type = MAC_TYPE_IPV6_MC;
            mach = ((mach & 0xffff0000) | (mask & 0x0000ffff));
        }
    } else {
        /* Not IP MC entry */
        /* Set FWD_KILL to make the switch discard frames in SMAC lookup */
        fwd_kill = (copy_to_cpu || pgid != vtss_state->l2.pgid_drop ? 0 : 1);
        idx = lan966x_chip_pgid(vtss_state, pgid);
        type = (entry->locked ? MAC_TYPE_LOCKED : MAC_TYPE_NORMAL);
    }

    /* Insert/learn new entry into the MAC table  */
    REG_WR(ANA_MACHDATA, mach);
    REG_WR(ANA_MACLDATA, macl);
    REG_WR(ANA_MACACCESS,
           (copy_to_cpu ? ANA_MACACCESS_MAC_CPU_COPY_M : 0) |
           (fwd_kill ? ANA_MACACCESS_SRC_KILL_M : 0) |
           (aged ? ANA_MACACCESS_AGED_FLAG_M : 0) |
           ANA_MACACCESS_VALID_M |
           ANA_MACACCESS_ENTRYTYPE(type) |
           ANA_MACACCESS_DEST_IDX(idx) |
           ANA_MACACCESS_MAC_TABLE_CMD(MACACCESS_CMD_LEARN));

    /* Wait until MAC operation is finished */
    return lan966x_mac_table_idle(vtss_state);
}

static u32 lan966x_mac_type(const vtss_vid_mac_t *vid_mac)
{
    return (VTSS_MAC_IPV4_MC(vid_mac->mac.addr) ? MAC_TYPE_IPV4_MC :
            VTSS_MAC_IPV6_MC(vid_mac->mac.addr) ? MAC_TYPE_IPV6_MC : MAC_TYPE_NORMAL);
}

static vtss_rc lan966x_mac_table_cmd(vtss_state_t *vtss_state, const vtss_vid_mac_t *const vid_mac, u32 type, u32 cmd)
{
    u32 mach, macl;

    vtss_mach_macl_get(vid_mac, &mach, &macl);
    VTSS_D("cmd: %u, address 0x%08x%08x", cmd, mach, macl);

    /* Run command for MAC entry */
    REG_WR(ANA_MACHDATA, mach);
    REG_WR(ANA_MACLDATA, macl);
    REG_WR(ANA_MACACCESS,
           ANA_MACACCESS_VALID_M |
           ANA_MACACCESS_ENTRYTYPE(type) |
           ANA_MACACCESS_MAC_TABLE_CMD(cmd));

    /* Wait until MAC operation is finished */
    return lan966x_mac_table_idle(vtss_state);
}

static vtss_rc lan966x_mac_table_del(vtss_state_t *vtss_state, const vtss_vid_mac_t *const vid_mac)
{
    u32 type = lan966x_mac_type(vid_mac);

    VTSS_RC(lan966x_mac_table_cmd(vtss_state, vid_mac, type, MACACCESS_CMD_FORGET));
    if (type != MAC_TYPE_NORMAL) {
        /* IPMC entries may be encoded as NORMAL */
        VTSS_RC(lan966x_mac_table_cmd(vtss_state, vid_mac, MAC_TYPE_NORMAL, MACACCESS_CMD_FORGET));
    }

    /* Wait until MAC operation is finished */
    return lan966x_mac_table_idle(vtss_state);
}

static vtss_rc lan966x_mac_table_result(vtss_state_t *vtss_state,
                                        vtss_mac_table_entry_t *const entry, u32 *pgid)
{
    u32               value, mask, mach, macl, idx, type;
    vtss_port_no_t    port_no;
    vtss_pgid_entry_t *pgid_entry;
    u8                *mac = &entry->vid_mac.mac.addr[0];

    REG_RD(ANA_MACACCESS, &value);

    /* Check if entry is valid */
    if (!(value & ANA_MACACCESS_VALID_M)) {
        VTSS_D("not valid");
        return VTSS_RC_ERROR;
    }

    type = ANA_MACACCESS_ENTRYTYPE_X(value);
    idx = ANA_MACACCESS_DEST_IDX_X(value);
    entry->aged = VTSS_BOOL(value & ANA_MACACCESS_AGED_FLAG_M);
    entry->copy_to_cpu = 0;
    entry->copy_to_cpu_smac = VTSS_BOOL(value & ANA_MACACCESS_MAC_CPU_COPY_M);
    entry->locked = (type == MAC_TYPE_NORMAL ? 0 : 1);

    REG_RD(ANA_MACHDATA, &mach);
    REG_RD(ANA_MACLDATA, &macl);
    if (type == MAC_TYPE_IPV4_MC || type == MAC_TYPE_IPV6_MC) {
        /* IPv4/IPv6 MC entry, decode port mask from entry */
        *pgid = VTSS_PGID_NONE;
        if (type == MAC_TYPE_IPV6_MC) {
            mask = (mach & 0xffff);
            mach = ((mach & 0xffff0000) | 0x00003333);
        } else {
            mask = (((mach << 8) & 0xffff00) | ((macl >> 24) & 0xff));
            mach = ((mach & 0xffff0000) | 0x00000100);
            macl = ((macl & 0x00ffffff) | 0x5e000000);
        }

        /* Convert port mask */
        pgid_entry = &vtss_state->l2.pgid_table[*pgid];
        for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
            pgid_entry->member[port_no] = VTSS_BOOL(mask & (1 << VTSS_CHIP_PORT(port_no)));
        }
    } else {
        *pgid = lan966x_vtss_pgid(vtss_state, idx);
    }
    entry->vid_mac.vid = ((mach>>16) & 0xFFF);
    mac[0] = ((mach>>8)  & 0xff);
    mac[1] = ((mach>>0)  & 0xff);
    mac[2] = ((macl>>24) & 0xff);
    mac[3] = ((macl>>16) & 0xff);
    mac[4] = ((macl>>8)  & 0xff);
    mac[5] = ((macl>>0)  & 0xff);
    VTSS_D("found 0x%08x%08x", mach, macl);

    return VTSS_RC_OK;
}

static vtss_rc lan966x_mac_table_get(vtss_state_t *vtss_state,
                                     vtss_mac_table_entry_t *const entry, u32 *pgid)
{
    vtss_rc rc;
    u32     type = lan966x_mac_type(&entry->vid_mac);

    VTSS_RC(lan966x_mac_table_cmd(vtss_state, &entry->vid_mac, type, MACACCESS_CMD_READ));
    rc = lan966x_mac_table_result(vtss_state, entry, pgid);
    if (type != MAC_TYPE_NORMAL && rc != VTSS_RC_OK) {
        /* IPMC entries may be encoded as NORMAL */
        VTSS_RC(lan966x_mac_table_cmd(vtss_state, &entry->vid_mac, MAC_TYPE_NORMAL, MACACCESS_CMD_READ));
        rc = lan966x_mac_table_result(vtss_state, entry, pgid);
    }
    return rc;
}

static vtss_rc lan966x_mac_table_get_next(vtss_state_t *vtss_state,
                                          vtss_mac_table_entry_t *const entry, u32 *pgid)
{
    VTSS_RC(lan966x_mac_table_cmd(vtss_state, &entry->vid_mac, MAC_TYPE_NORMAL, MACACCESS_CMD_GET_NEXT));
    return lan966x_mac_table_result(vtss_state, entry, pgid);
}

static vtss_rc lan966x_mac_table_age_time_set(vtss_state_t *vtss_state)
{
    u32 time;

    /* Scan two times per age time */
    time = (vtss_state->l2.mac_age_time/2);
    if (time > 0xfffff)
        time = 0xfffff;
    REG_WR(ANA_AUTOAGE, ANA_AUTOAGE_AGE_PERIOD(time));
    return VTSS_RC_OK;
}

/* Maximum FID value */
#define LAN966X_FID_MAX 63

static vtss_rc lan966x_mac_table_age(vtss_state_t *vtss_state,
                                     BOOL             pgid_age,
                                     u32              pgid,
                                     BOOL             vid_age,
                                     const vtss_vid_t vid)
{
    vtss_vid_t fid = vtss_state->l2.vlan_table[vid].conf.fid;

    if (fid == 0 || fid > LAN966X_FID_MAX) {
        fid = vid;
    }

    /* Selective aging */
    REG_WR(ANA_ANAGEFIL,
           (pgid_age ? ANA_ANAGEFIL_PID_EN_M : 0) |
           (vid_age ? ANA_ANAGEFIL_VID_EN_M : 0) |
           ANA_ANAGEFIL_PID_VAL(lan966x_chip_pgid(vtss_state, pgid)) |
           ANA_ANAGEFIL_VID_VAL(fid));

    /* Do the aging */
    REG_WR(ANA_MACACCESS,
           ANA_MACACCESS_MAC_TABLE_CMD(MACACCESS_CMD_AGE));
    VTSS_RC(lan966x_mac_table_idle(vtss_state));

    /* Clear age filter again to avoid affecting automatic ageing */
    REG_WR(ANA_ANAGEFIL, 0);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mac_table_status_get(vtss_state_t *vtss_state,
                                            vtss_mac_table_status_t *status)
{
    u32 value;

    /* Read and clear sticky register */
    REG_RD(ANA_ANEVENTS, &value);
    REG_WR(ANA_ANEVENTS,
           value & (ANA_ANEVENTS_AUTO_MOVED_M |
                    ANA_ANEVENTS_AUTO_LEARNED_M |
                    ANA_ANEVENTS_LEARN_REMOVE_M |
                    ANA_ANEVENTS_AGED_ENTRY_M));

    /* Detect events */
    status->learned = VTSS_BOOL(value & ANA_ANEVENTS_AUTO_LEARNED_M);
    status->replaced = VTSS_BOOL(value & ANA_ANEVENTS_LEARN_REMOVE_M);
    status->moved = VTSS_BOOL(value & ANA_ANEVENTS_AUTO_MOVED_M);
    status->aged = VTSS_BOOL(value & ANA_ANEVENTS_AGED_ENTRY_M);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_learn_port_mode_set(vtss_state_t *vtss_state,
                                           const vtss_port_no_t port_no)
{
    vtss_learn_mode_t *mode = &vtss_state->l2.learn_mode[port_no];
    u32               value, port = VTSS_CHIP_PORT(port_no);

    REG_WRM(ANA_PORT_CFG(port),
            (mode->discard ? ANA_PORT_CFG_LEARNDROP_M : 0) |
            (mode->automatic ? ANA_PORT_CFG_LEARNAUTO_M : ANA_PORT_CFG_LOCKED_PORTMOVE_CPU_M) |
            (mode->cpu ? ANA_PORT_CFG_LEARNCPU_M : 0) |
            ANA_PORT_CFG_LOCKED_PORTMOVE_DROP_M,
            ANA_PORT_CFG_LEARNDROP_M |
            ANA_PORT_CFG_LEARNAUTO_M |
            ANA_PORT_CFG_LOCKED_PORTMOVE_CPU_M |
            ANA_PORT_CFG_LEARNCPU_M |
            ANA_PORT_CFG_LOCKED_PORTMOVE_DROP_M);

    if (!mode->automatic) {
        /* Flush entries previously learned on port to avoid continuous refreshing */
        REG_RD(ANA_PORT_CFG(port), &value);
        REG_WRM_CLR(ANA_PORT_CFG(port), ANA_PORT_CFG_LEARN_ENA_M);
        VTSS_RC(lan966x_mac_table_age(vtss_state, 1, port_no, 0, 0));
        VTSS_RC(lan966x_mac_table_age(vtss_state, 1, port_no, 0, 0));
        REG_WR(ANA_PORT_CFG(port), value);
    }
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  Layer 2 - VLAN
 * ================================================================= */

static vtss_rc lan966x_vlan_conf_apply(vtss_state_t *vtss_state, BOOL ports)
{
    u32            etype = vtss_state->l2.vlan_conf.s_etype;
    vtss_port_no_t port_no;

    /* BZ4513: Type 0x8100 can not be used so we use 0x88a8 */
    if (etype == VTSS_ETYPE_TAG_C)
        etype = VTSS_ETYPE_TAG_S;
    REG_WR(ANA_VLAN_ETYPE_CFG, ANA_VLAN_ETYPE_CFG_S_TAG_VAL(etype));

    /* Update ports */
    for (port_no = 0; ports && port_no < vtss_state->port_count; port_no++) {
        VTSS_RC(vtss_cmn_vlan_port_conf_set(vtss_state, port_no));
    }

    return VTSS_RC_OK;
}

static vtss_rc lan966x_vlan_conf_set(vtss_state_t *vtss_state)
{
    return lan966x_vlan_conf_apply(vtss_state, 1);
}

/* Types for REW:TAG_CFG:TAG_CFG */
#define TAG_CFG_DISABLE       0 /* Port tagging disabled */
#define TAG_CFG_ALL_NPV_NNUL  1 /* Tag all frames, except when VID=PORT_VLAN_CFG.PORT_VID or VID=0 */
#define TAG_CFG_ALL_NNUL      2 /* Tag all frames, except when VID=0 */
#define TAG_CFG_ALL           3 /* Tag all frames */

/* Types for REW_PORT:TAG_CFG:TAG_TPID_CFG */
#define TAG_TPID_CFG_0x8100   0  /* Use 0x8100 */
#define TAG_TPID_CFG_0x88A8   1  /* Use 0x88A8 */
#define TAG_TPID_CFG_PTPID    2  /* Use custom from PORT_VLAN_CFG.PORT_TPID */
#define TAG_TPID_CFG_PTPID_NC 3  /* As above, but unless ingress tag was a C-tag */

static vtss_rc lan966x_vlan_port_conf_apply(vtss_state_t *vtss_state,
                                            u32                   port,
                                            vtss_vlan_port_conf_t *conf)
{
    u32                   tpid = TAG_TPID_CFG_0x8100;
    u32                   value, etype = vtss_state->l2.vlan_conf.s_etype;
    vtss_vid_t            uvid = conf->untagged_vid;
    BOOL                  tagged, untagged, aware = 1, c_port = 0, s_port = 0;
    vtss_vlan_port_type_t type = conf->port_type;

    /* BZ4513: If the custom TPID is 0x8100, we treat S-custom ports as C-ports */
    if (etype == VTSS_ETYPE_TAG_C && type == VTSS_VLAN_PORT_TYPE_S_CUSTOM)
        type = VTSS_VLAN_PORT_TYPE_C;

    /* Check port type */
    switch (type) {
    case VTSS_VLAN_PORT_TYPE_UNAWARE:
        aware = 0;
        break;
    case VTSS_VLAN_PORT_TYPE_C:
        c_port = 1;
        break;
    case VTSS_VLAN_PORT_TYPE_S:
        s_port = 1;
        tpid = TAG_TPID_CFG_0x88A8;
        break;
    case VTSS_VLAN_PORT_TYPE_S_CUSTOM:
        s_port = 1;
        tpid = TAG_TPID_CFG_PTPID;
        break;
    default:
        return VTSS_RC_ERROR;
    }

    /* Port VLAN Configuration */
    REG_WRM(ANA_VLAN_CFG(port),
            ANA_VLAN_CFG_VLAN_VID(conf->pvid) |
            (aware ? ANA_VLAN_CFG_VLAN_AWARE_ENA_M : 0) |
            ANA_VLAN_CFG_VLAN_POP_CNT(aware ? 1 : 0),
            ANA_VLAN_CFG_VLAN_VID_M |
            ANA_VLAN_CFG_VLAN_AWARE_ENA_M |
            ANA_VLAN_CFG_VLAN_POP_CNT_M);

    /* Drop Configuration based on port type and frame type */
    tagged = (conf->frame_type == VTSS_VLAN_FRAME_TAGGED);
    untagged = (conf->frame_type == VTSS_VLAN_FRAME_UNTAGGED);
    value = ANA_DROP_CFG_DROP_MC_SMAC_ENA_M;
    if (tagged && aware) {
        /* Discard untagged and priority-tagged if aware and tagged-only allowed */
        value |= ANA_DROP_CFG_DROP_UNTAGGED_ENA_M;
        value |= ANA_DROP_CFG_DROP_PRIO_C_TAGGED_ENA_M;
        value |= ANA_DROP_CFG_DROP_PRIO_S_TAGGED_ENA_M;
    }
    if ((untagged && c_port) || (tagged && s_port)) {
        /* Discard C-tagged if C-port and untagged-only OR S-port and tagged-only */
        value |= ANA_DROP_CFG_DROP_C_TAGGED_ENA_M;
    }
    if ((untagged && s_port) || (tagged && c_port)) {
        /* Discard S-tagged if S-port and untagged-only OR C-port and tagged-only */
        value |= ANA_DROP_CFG_DROP_S_TAGGED_ENA_M;
    }
    REG_WR(ANA_DROP_CFG(port), value);

    /* Ingress filtering */
    REG_WRM_CTL(ANA_VLANMASK, conf->ingress_filter, 1 << port);

    /* Rewriter VLAN tag configuration */
    value = (REW_TAG_CFG_TAG_TPID_CFG(tpid) |
             REW_TAG_CFG_TAG_CFG(
                 uvid == VTSS_VID_ALL ? TAG_CFG_DISABLE :
                 uvid == VTSS_VID_NULL ? TAG_CFG_ALL : TAG_CFG_ALL_NPV_NNUL));
    REG_WRM(REW_TAG_CFG(port), value,
            REW_TAG_CFG_TAG_TPID_CFG_M |
            REW_TAG_CFG_TAG_CFG_M);
    REG_WRM(REW_PORT_VLAN_CFG(port),
            REW_PORT_VLAN_CFG_PORT_TPID(conf->s_etype ? conf->s_etype : etype) |
            REW_PORT_VLAN_CFG_PORT_VID(uvid),
            REW_PORT_VLAN_CFG_PORT_TPID_M |
            REW_PORT_VLAN_CFG_PORT_VID_M);

    return VTSS_RC_OK;
}

static vtss_rc lan966x_vlan_port_conf_update(vtss_state_t *vtss_state,
                                             vtss_port_no_t port_no, vtss_vlan_port_conf_t *conf)
{
    /* Update maximum tags allowed */
    VTSS_RC(vtss_lan966x_port_max_tags_set(vtss_state, port_no));
    return lan966x_vlan_port_conf_apply(vtss_state, VTSS_CHIP_PORT(port_no), conf);
}

static vtss_rc lan966x_vlan_table_idle(vtss_state_t *vtss_state)
{
    u32 value;

    do {
        REG_RD(ANA_VLANACCESS, &value);
    } while (ANA_VLANACCESS_VLAN_TBL_CMD_X(value) != VLANACCESS_CMD_IDLE);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_vlan_mask_update(vtss_state_t *vtss_state,
                                        vtss_vid_t vid, BOOL member[VTSS_PORT_ARRAY_SIZE])
{
    vtss_vlan_entry_t    *vlan_entry = &vtss_state->l2.vlan_table[vid];
    vtss_vlan_vid_conf_t *conf = &vlan_entry->conf;
    BOOL                 learning = (vlan_entry->evc_learning && conf->learning ? 1 : 0);

    /* Index and properties */
    REG_WR(ANA_VLANTIDX,
           ANA_VLANTIDX_V_INDEX(vid) |
           (vlan_entry->isolated ? ANA_VLANTIDX_VLAN_PRIV_VLAN_M : 0) |
           (learning ? 0 : ANA_VLANTIDX_VLAN_LEARN_DISABLED_M) |
           (conf->mirror ? ANA_VLANTIDX_VLAN_MIRROR_M : 0));

    /* VLAN mask */
    REG_WR(ANA_VLANACCESS,
           ANA_VLANACCESS_VLAN_PORT_MASK(vtss_lan966x_port_mask(vtss_state, member)) |
           ANA_VLANACCESS_VLAN_TBL_CMD(VLANACCESS_CMD_WRITE));

    /* FID */
    REG_WR(ANA_FID_MAP(vid),
           ANA_FID_MAP_FID_VAL(conf->fid > LAN966X_FID_MAX ? 0 : conf->fid));

    return lan966x_vlan_table_idle(vtss_state);
}

/* ================================================================= *
 *  Layer 2 - PVLAN / Isolated ports
 * ================================================================= */

static vtss_rc lan966x_isolated_port_members_set(vtss_state_t *vtss_state)
{
    u32 mask = vtss_lan966x_port_mask(vtss_state, vtss_state->l2.isolated_port);

    /* Isolated ports: cleared (add CPU as not isolated) */
    mask = ((~mask) & VTSS_CHIP_PORT_MASK) | VTSS_BIT(VTSS_CHIP_PORT_CPU);
    REG_WR(ANA_ISOLATED_PORTS, mask);
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  Layer 2 - IP Multicast
 * ================================================================= */

static vtss_rc lan966x_flood_conf_set(vtss_state_t *vtss_state)
{
    u32 pgid = (vtss_state->l2.ipv6_mc_scope ? PGID_MCIPV6 : PGID_MC);

    /* Unicast flood mask */
    VTSS_RC(lan966x_pgid_update(vtss_state, PGID_UC, vtss_state->l2.uc_flood));

    /* Multicast flood mask */
    VTSS_RC(lan966x_pgid_update(vtss_state, PGID_MC, vtss_state->l2.mc_flood));

    /* IPv4 flood mask */
    VTSS_RC(lan966x_pgid_update(vtss_state, PGID_MCIPV4, vtss_state->l2.ipv4_mc_flood));

    /* IPv6 flood mask */
    VTSS_RC(lan966x_pgid_update(vtss_state, PGID_MCIPV6, vtss_state->l2.ipv6_mc_flood));

    /* IPv6 flood scope */
    REG_WRM(ANA_FLOODING_IPMC,
            ANA_FLOODING_IPMC_FLD_MC6_CTRL(pgid),
            ANA_FLOODING_IPMC_FLD_MC6_CTRL_M);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_ip_mc_update(vtss_state_t *vtss_state,
                                    vtss_ipmc_data_t *ipmc, vtss_ipmc_cmd_t cmd)
{
    vtss_vcap_obj_t       *obj = &vtss_state->vcap.is2.obj;
    int                   i, user = (ipmc->src.ssm ? VTSS_IS2_USER_SSM : VTSS_IS2_USER_ASM);
    vtss_vcap_key_size_t  key_size = (ipmc->ipv6 ? VTSS_VCAP_KEY_SIZE_FULL : VTSS_VCAP_KEY_SIZE_HALF);
    vtss_res_chg_t        res;
    vtss_vcap_data_t      data;
    vtss_is2_entry_t      entry;
    vtss_ace_t            *ace = &entry.ace;
    vtss_ace_frame_ipv4_t *ipv4 = &ace->frame.ipv4;
    vtss_ace_frame_ipv6_t *ipv6 = &ace->frame.ipv6;
    vtss_port_no_t        port_no;

    if (cmd == VTSS_IPMC_CMD_CHECK) {
        memset(&res, 0, sizeof(res));
        if (ipmc->dst_add) {
            res.add_key[key_size] = 1;
        }
        return vtss_cmn_vcap_res_check(obj, &res);
    }

    if (cmd == VTSS_IPMC_CMD_DEL) {
        return vtss_vcap_del(vtss_state, obj, user, ipmc->dst.id);
    }

    /* Add entry */
    vtss_vcap_is2_init(&data, &entry);
    data.key_size = key_size;
    ace->vlan.vid.value = ipmc->src.vid;
    ace->vlan.vid.mask = 0xfff;
    if (ipmc->ipv6) {
        ace->type = VTSS_ACE_TYPE_IPV6;
        for (i = 0; i < 16; i++) {
            ipv6->sip.value[i] = ipmc->src.sip.ipv6.addr[i];
            ipv6->sip.mask[i] = (ipmc->src.ssm ? 0xff : 0);
            ipv6->dip.value[i] = ipmc->dst.dip.ipv6.addr[i];
            ipv6->dip.mask[i] = 0xff;
        }
    } else {
        ace->type = VTSS_ACE_TYPE_IPV4;
        ipv4->sip.value = ipmc->src.sip.ipv4;
        ipv4->sip.mask = (ipmc->src.ssm ? 0xffffffff : 0);
        ipv4->dip.value = ipmc->dst.dip.ipv4;
        ipv4->dip.mask = 0xffffffff;
    }
    ace->action.learn = TRUE;
    ace->action.port_action = VTSS_ACL_PORT_ACTION_FILTER;
    for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
        ace->port_list[port_no] = 1;
        ace->action.port_list[port_no] = VTSS_PORT_BF_GET(ipmc->dst.member, port_no);
    }
    return vtss_vcap_add(vtss_state, obj, user, ipmc->dst.id, ipmc->id_next, &data, 0);
}

/* ================================================================= *
 *  Layer 2 - Mirror
 * ================================================================= */

static vtss_rc lan966x_mirror_cpu_ingress_set(vtss_state_t *vtss_state)
{
    REG_WRM_CTL(ANA_PORT_CFG(VTSS_CHIP_PORT_CPU), vtss_state->l2.mirror_cpu_ingress, ANA_PORT_CFG_SRC_MIRROR_ENA_M);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mirror_cpu_egress_set(vtss_state_t *vtss_state)
{
    REG_WRM_CTL(ANA_AGENCTRL, vtss_state->l2.mirror_cpu_egress, ANA_AGENCTRL_MIRROR_CPU_M);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mirror_port_set(vtss_state_t *vtss_state)
{
    BOOL           member[VTSS_PORT_ARRAY_SIZE];
    vtss_port_no_t port_no;

    for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
        member[port_no] = (port_no == vtss_state->l2.mirror_conf.port_no);
    }
    REG_WR(ANA_MIRRORPORTS, vtss_lan966x_port_mask(vtss_state, member));

    /* Update all VLANs */
    return vtss_cmn_vlan_update_all(vtss_state);
}

static vtss_rc lan966x_mirror_ingress_set(vtss_state_t *vtss_state)
{
    vtss_port_no_t port_no;
    u32            port;

    for (port_no = 0; port_no < vtss_state->port_count; port_no++) {
        port = VTSS_CHIP_PORT(port_no);
        REG_WRM_CTL(ANA_PORT_CFG(port), vtss_state->l2.mirror_ingress[port_no], ANA_PORT_CFG_SRC_MIRROR_ENA_M);
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mirror_egress_set(vtss_state_t *vtss_state)
{
    REG_WR(ANA_EMIRRORPORTS, vtss_lan966x_port_mask(vtss_state, vtss_state->l2.mirror_egress));
    return VTSS_RC_OK;
}

#if 0
static vtss_rc lan966x_vcap_port_conf_set(vtss_state_t *vtss_state, const vtss_port_no_t port_no)
{
    return VTSS_RC_OK;
}
#endif

static vtss_rc lan966x_sdx_update(vtss_state_t *vtss_state, vtss_sdx_entry_t *sdx)
{
    vtss_iflow_conf_t       *conf;
    vtss_frer_iflow_conf_t  *frer;
    vtss_psfp_iflow_conf_t  *psfp;
    vtss_psfp_filter_conf_t *filter;
    vtss_is1_action_t       act;

    if (sdx == NULL) {
        return VTSS_RC_ERROR;
    }

    conf = &sdx->conf;
    frer = &conf->frer;
    psfp = &conf->psfp;
    memset(&act, 0, sizeof(act));
    act.isdx = sdx->sdx;
    act.dlb_enable = conf->dlb_enable;
    act.dlb = conf->dlb_id;
    act.mstream_enable = frer->mstream_enable;
    act.mstream = frer->mstream_id;
    if (psfp->filter_enable) {
        filter = &vtss_state->l2.psfp.filter[psfp->filter_id];
        act.sfid_enable = psfp->filter_enable;
        act.sfid = psfp->filter_id;
        act.sgid_enable = filter->gate_enable;
        act.sgid = filter->gate_id;
    }
    act.ct_disable = conf->cut_through_disable;
    VTSS_I("sdx: %u, dlb: %u/%u, mstream: %u/%u, filter: %u/%u, gate: %u/%u, ct_disable: %u",
           act.isdx, act.dlb_enable, act.dlb, act.mstream_enable, act.mstream,
           act.sfid_enable, act.sfid, act.sgid_enable, act.sgid, act.ct_disable);
    return vtss_vcap_is1_update(vtss_state, &act);
}

static vtss_rc lan966x_iflow_conf_set(vtss_state_t *vtss_state, const vtss_iflow_id_t id)
{
    return lan966x_sdx_update(vtss_state, vtss_iflow_lookup(vtss_state, id));
}

vtss_rc lan966x_counters_update(vtss_state_t *vtss_state, vtss_stat_idx_t *stat_idx, BOOL clr)
{
    vtss_sdx_counters_t *c;
    vtss_sdx_entry_t    *sdx;
    u32                 base, *p = &base;
    u16                 idx;

    /* Update ingress counters, if active */
    idx = stat_idx->idx;
    if (idx != 0) {
        /* ISDX counters */
        c = &vtss_state->l2.sdx_info.sdx_table[idx];
        REG_WR(SYS_STAT_CFG, SYS_STAT_CFG_STAT_VIEW(idx + 640));
        base = 0x280; // TBD
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_green.bytes, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_green.frames, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_yellow.bytes, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_yellow.frames, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_red.bytes, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_red.frames, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_discard.bytes, clr));  // Green drops
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_discard.frames, clr)); // Green drops
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_discard.bytes, clr));  // Yellow drops
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_discard.frames, clr)); // Yellow drops

        // PSFP counters
        if ((sdx = vtss_iflow_lookup(vtss_state, idx)) != NULL && sdx->conf.psfp.filter_enable) {
            REG_WR(SYS_STAT_CFG, SYS_STAT_CFG_STAT_VIEW(sdx->conf.psfp.filter_id + 512));
            base = 0x200;
            VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_match, clr));
            VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_gate_discard, clr));
            VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->rx_sdu_discard, clr));
        }
    }

    /* Update egress counters, if active */
    idx = stat_idx->edx;
    if (idx != 0) {
        c = &vtss_state->l2.sdx_info.sdx_table[idx];
        REG_WR(SYS_STAT_CFG, SYS_STAT_CFG_STAT_VIEW(idx + 768));
        base = 0x300; // TBD
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_green.bytes, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_green.frames, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_yellow.bytes, clr));
        VTSS_RC(vtss_lan966x_counter_update(vtss_state, p, &c->tx_yellow.frames, clr));
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_icnt_get(vtss_state_t *vtss_state, u16 idx, vtss_ingress_counters_t *counters)
{
    vtss_stat_idx_t     sidx;
    vtss_sdx_counters_t *c = &vtss_state->l2.sdx_info.sdx_table[idx];

    sidx.idx = idx;
    sidx.edx = 0;
    VTSS_RC(lan966x_counters_update(vtss_state, &sidx, counters == NULL));

    if (counters != NULL) {
        counters->rx_green.frames = c->rx_green.frames.value;
        counters->rx_green.bytes = c->rx_green.bytes.value;
        counters->rx_yellow.frames = c->rx_yellow.frames.value;
        counters->rx_yellow.bytes = c->rx_yellow.bytes.value;
        counters->rx_red.frames = c->rx_red.frames.value;
        counters->rx_red.bytes = c->rx_red.bytes.value;
        counters->rx_discard.frames = (c->rx_discard.frames.value + c->tx_discard.frames.value); // Green and yellow
        counters->rx_discard.bytes = (c->rx_discard.bytes.value + c->tx_discard.bytes.value);    // Green and yellow
        counters->tx_discard.frames = 0;
        counters->tx_discard.bytes = 0;

        // PSFP counters
        counters->rx_match = c->rx_match.value;
        counters->rx_sdu_discard = c->rx_sdu_discard.value;
        counters->rx_sdu_pass = (counters->rx_match - counters->rx_sdu_discard);
        counters->rx_gate_discard = c->rx_gate_discard.value;
        counters->rx_gate_pass = (counters->rx_sdu_pass - counters->rx_gate_discard);
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_ecnt_get(vtss_state_t *vtss_state, u16 idx, vtss_egress_counters_t *counters)
{
    vtss_stat_idx_t     sidx;
    vtss_sdx_counters_t *c = &vtss_state->l2.sdx_info.sdx_table[idx];

    sidx.idx = 0;
    sidx.edx = idx;
    VTSS_RC(lan966x_counters_update(vtss_state, &sidx, counters == NULL));

    if (counters != NULL) {
        counters->tx_green.frames = c->tx_green.frames.value;
        counters->tx_green.bytes = c->tx_green.bytes.value;
        counters->tx_yellow.frames = c->tx_yellow.frames.value;
        counters->tx_yellow.bytes = c->tx_yellow.bytes.value;
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_policer_update(vtss_state_t *vtss_state, u16 idx)
{
    return VTSS_RC_OK;
}

static vtss_rc lan966x_isdx_update(vtss_state_t *vtss_state, vtss_sdx_entry_t *sdx)
{
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  FRER
 * ================================================================= */
static vtss_rc lan966x_cstream_conf_set(vtss_state_t *vtss_state, const vtss_frer_cstream_id_t id)
{
    vtss_frer_stream_conf_t *conf = &vtss_state->l2.cstream_conf[id];
    BOOL                    vector = (conf->alg == VTSS_FRER_RECOVERY_ALG_VECTOR);

    REG_WR(QSYS_FRER_CFG_CMP(id),
           QSYS_FRER_CFG_CMP_TAKE_NO_SEQUENCE(conf->take_no_seq ? 1 : 0) |
           QSYS_FRER_CFG_CMP_VECTOR_ALGORITHM(vector) |
           QSYS_FRER_CFG_CMP_HISTORY_LENGTH(conf->hlen < 2 ? 1 : (conf->hlen - 1)) |
           QSYS_FRER_CFG_CMP_RESET_TICKS(conf->reset_time ? conf->reset_time : 1) |
           QSYS_FRER_CFG_CMP_RESET(1) |
           QSYS_FRER_CFG_CMP_ENABLE(conf->recovery));
    return VTSS_RC_OK;
}

static vtss_rc lan966x_mstream_conf_set(vtss_state_t *vtss_state, const u16 idx)
{
    vtss_frer_stream_conf_t *conf = &vtss_state->l2.mstream_conf[idx];
    BOOL                    vector = (conf->alg == VTSS_FRER_RECOVERY_ALG_VECTOR);

    REG_WR(QSYS_FRER_CFG_MBM(idx),
           QSYS_FRER_CFG_MBM_TAKE_NO_SEQUENCE(conf->take_no_seq ? 1 : 0) |
           QSYS_FRER_CFG_MBM_VECTOR_ALGORITHM(vector) |
           QSYS_FRER_CFG_MBM_HISTORY_LENGTH(conf->hlen < 2 ? 1 : (conf->hlen - 1)) |
           QSYS_FRER_CFG_MBM_RESET_TICKS(conf->reset_time ? conf->reset_time : 1) |
           QSYS_FRER_CFG_MBM_RESET(1) |
           QSYS_FRER_CFG_MBM_ENABLE(conf->recovery) |
           QSYS_FRER_CFG_MBM_COMPOUND_HANDLE(conf->cstream_id));
    return VTSS_RC_OK;
}

#define FRER_CNT(name, i, cnt, clr)              \
{                                                \
    u32 value;                                   \
    REG_RD(QSYS_CNT_##name(i), &value);   \
    vtss_cmn_counter_32_update(value, cnt, clr); \
}

static vtss_rc lan966x_frer_cnt_get(vtss_frer_chip_counters_t *c,
                                    vtss_frer_counters_t *counters)
{
    if (counters != NULL) {
        counters->out_of_order_packets = c->out_of_order_packets.value;
        counters->rogue_packets = c->rogue_packets.value;
        counters->passed_packets = c->passed_packets.value;
        counters->discarded_packets = c->discarded_packets.value;
        counters->lost_packets = c->lost_packets.value;
        counters->tagless_packets = c->tagless_packets.value;
        counters->resets = c->resets.value;
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_cstream_cnt_update(vtss_state_t *vtss_state,
                                          const vtss_frer_cstream_id_t id,
                                          vtss_frer_counters_t *counters,
                                          BOOL clr)
{
    vtss_frer_chip_counters_t *c = &vtss_state->l2.cs_counters[id];

    FRER_CNT(CMP_OO, id, &c->out_of_order_packets, clr);
    FRER_CNT(CMP_RG, id, &c->rogue_packets, clr);
    FRER_CNT(CMP_PS, id, &c->passed_packets, clr);
    FRER_CNT(CMP_DC, id, &c->discarded_packets, clr);
    FRER_CNT(CMP_LS, id, &c->lost_packets, clr);
    FRER_CNT(CMP_TL, id, &c->tagless_packets, clr);
    FRER_CNT(CMP_RS, id, &c->resets, clr);
    return lan966x_frer_cnt_get(c, counters);
}

static vtss_rc lan966x_cstream_cnt_get(vtss_state_t *vtss_state,
                                       const vtss_frer_cstream_id_t id,
                                       vtss_frer_counters_t *counters)
{
    return lan966x_cstream_cnt_update(vtss_state, id, counters, counters == NULL);
}

static vtss_rc lan966x_mstream_cnt_update(vtss_state_t *vtss_state,
                                          const u16 idx,
                                          vtss_frer_counters_t *counters,
                                          BOOL clr)
{
    vtss_frer_chip_counters_t *c = &vtss_state->l2.ms_counters[idx];

    FRER_CNT(MBM_OO, idx, &c->out_of_order_packets, clr);
    FRER_CNT(MBM_RG, idx, &c->rogue_packets, clr);
    FRER_CNT(MBM_PS, idx, &c->passed_packets, clr);
    FRER_CNT(MBM_DC, idx, &c->discarded_packets, clr);
    FRER_CNT(MBM_LS, idx, &c->lost_packets, clr);
    FRER_CNT(MBM_TL, idx, &c->tagless_packets, clr);
    FRER_CNT(MBM_RS, idx, &c->resets, clr);
    return lan966x_frer_cnt_get(c, counters);
}

static vtss_rc lan966x_mstream_cnt_get(vtss_state_t *vtss_state,
                                       const u16 idx,
                                       vtss_frer_counters_t *counters)
{
    return lan966x_mstream_cnt_update(vtss_state, idx, counters, counters == NULL);
}

/* ================================================================= *
 *  PSFP
 * ================================================================= */
static u32 lan966x_psfp_prio(vtss_opt_prio_t *prio)
{
    return (prio->value + (prio->enable ? 0x8 : 0));
}

static vtss_rc lan966x_gate_conf_set(vtss_state_t *vtss_state, const vtss_psfp_gate_id_t id)
{
    vtss_psfp_state_t     *psfp = &vtss_state->l2.psfp;
    vtss_psfp_gate_conf_t *conf = &psfp->gate[id];
    vtss_psfp_gcl_conf_t  *gcl_conf = &conf->config;
    vtss_psfp_gcl_t       *gcl = &psfp->admin_gcl[id];
    vtss_psfp_gce_t       *gce;
    u32                   i, t = 0;

    REG_WR(ANA_SG_ACCESS_CTRL, ANA_SG_ACCESS_CTRL_SGID(id));
    REG_WR(ANA_SG_CFG_1, gcl_conf->base_time.nanoseconds);
    REG_WR(ANA_SG_CFG_2, gcl_conf->base_time.seconds);
    REG_WR(ANA_SG_CFG_3,
           ANA_SG_CFG_3_BASE_TIME_SEC_MSB(gcl_conf->base_time.sec_msb) |
           ANA_SG_CFG_3_LIST_LENGTH(gcl->gcl_length) |
           ANA_SG_CFG_3_GATE_ENABLE(conf->enable) |
           ANA_SG_CFG_3_INIT_IPS(lan966x_psfp_prio(&conf->prio)) |
           ANA_SG_CFG_3_INIT_GATE_STATE(conf->gate_open ? 1 : 0) |
           ANA_SG_CFG_3_INVALID_RX_ENA(conf->close_invalid_rx.enable ? 1 : 0) |
           ANA_SG_CFG_3_INVALID_RX(conf->close_invalid_rx.value ? 1 : 0) |
           ANA_SG_CFG_3_OCTETS_EXCEEDED_ENA(conf->close_octets_exceeded.enable ? 1 : 0) |
           ANA_SG_CFG_3_OCTETS_EXCEEDED(conf->close_octets_exceeded.value ? 1 : 0));
    REG_WR(ANA_SG_CFG_4, gcl_conf->cycle_time);
    REG_WR(ANA_SG_CFG_5, gcl_conf->cycle_time_ext);
    for (i = 0; i < gcl->gcl_length; i++) {
        gce = &gcl->gce[i];
        REG_WR(ANA_SG_GCL_GS_CFG(i),
               ANA_SG_GCL_GS_CFG_IPS(lan966x_psfp_prio(&gce->prio)) |
               ANA_SG_GCL_GS_CFG_GATE_STATE(gce->gate_open ? 1 : 0));
        t += gce->time_interval;
        REG_WR(ANA_SG_GCL_TI_CFG(i), t);
        REG_WR(ANA_SG_GCL_OCT_CFG(i), gce->octet_max);
    }
    if (conf->config_change) {
        REG_WR(ANA_SG_ACCESS_CTRL,
               ANA_SG_ACCESS_CTRL_SGID(id) |
               ANA_SG_ACCESS_CTRL_CONFIG_CHANGE(1));
    }
    return VTSS_RC_OK;
}

static vtss_rc lan966x_gate_status_get(vtss_state_t *vtss_state,
                                       const vtss_psfp_gate_id_t id,
                                       vtss_psfp_gate_status_t *const status)
{
    u32 value, prio;
#if defined(VTSS_FEATURE_TIMESTAMP)
    u64 tc;
#endif

    REG_WR(ANA_SG_ACCESS_CTRL, ANA_SG_ACCESS_CTRL_SGID(id));
    REG_RD(ANA_SG_STS_1, &status->config_change_time.nanoseconds);
    REG_RD(ANA_SG_STS_2, &status->config_change_time.seconds);
    REG_RD(ANA_SG_STS_3, &value);
    status->config_change_time.sec_msb = ANA_SG_STS_3_CFG_CHG_TIME_SEC_MSB_X(value);
    status->gate_open = ANA_SG_STS_3_GATE_STATE_X(value);
    prio = ANA_SG_STS_3_IPS_X(value);
    status->prio.enable = (prio & 0x8 ? 1 : 0);
    status->prio.value = (prio & 0x7);
    status->config_pending = ANA_SG_STS_3_CONFIG_PENDING_X(value);
#if defined(VTSS_FEATURE_TIMESTAMP)
    return VTSS_FUNC(ts.timeofday_get, &status->current_time, &tc);
#else
    return VTSS_RC_OK;
#endif
}

static vtss_rc lan966x_filter_conf_set(vtss_state_t *vtss_state, const vtss_psfp_filter_id_t id)
{
    vtss_psfp_filter_conf_t *conf = &vtss_state->l2.psfp.filter[id];
    vtss_sdx_entry_t        *sdx;

    REG_WR(ANA_SFIDTIDX, ANA_SFIDTIDX_SFID_INDEX(id));
    REG_WR(ANA_SFIDACCESS,
           ANA_SFIDACCESS_B_O_FRM(conf->block_oversize.value ? 1 : 0) |
           ANA_SFIDACCESS_B_O_FRM_ENA(conf->block_oversize.enable ? 1 : 0) |
           ANA_SFIDACCESS_MAX_SDU_LEN(conf->max_sdu) |
           ANA_SFIDACCESS_SFID_TBL_CMD(2)); // WRITE

    // Update IS1 mappings to gate
    for (sdx = vtss_state->l2.sdx_info.iflow; sdx != NULL; sdx = sdx->next) {
        if (sdx->conf.psfp.filter_enable && sdx->conf.psfp.filter_id == id) {
            VTSS_RC(lan966x_sdx_update(vtss_state, sdx));
        }
    }
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  SFLOW
 * ================================================================= */

/* sFlow H/W-related min/max */
#define LAN966X_SFLOW_MIN_SAMPLE_RATE    0 /**< Minimum allowable sampling rate for sFlow */
#define LAN966X_SFLOW_MAX_SAMPLE_RATE 4095 /**< Maximum allowable sampling rate for sFlow */

static u32 next_power_of_two(u32 x)
{
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return ++x;
}

static u32 lan966x_sflow_hw_rate(const u32 desired_sw_rate, u32 *const realizable_sw_rate)
{
    u32 hw_rate = desired_sw_rate ? VTSS_ROUNDING_DIVISION(LAN966X_SFLOW_MAX_SAMPLE_RATE + 1, desired_sw_rate) : 0;
    hw_rate = (hw_rate > LAN966X_SFLOW_MIN_SAMPLE_RATE ? hw_rate - 1 : hw_rate);
    *realizable_sw_rate = VTSS_ROUNDING_DIVISION(LAN966X_SFLOW_MAX_SAMPLE_RATE + 1, hw_rate + 1);
    return hw_rate;
}

static vtss_rc lan966x_sflow_sampling_rate_convert(struct vtss_state_s *const state,
                                                   const BOOL power2, const u32 rate_in, u32 *const rate_out)
{
    u32 modified_rate_in;
    // Could happen that two threads call this function simultaneously at boot, but we take the risk.
    // Once sflow_max_power_of_two has been computed, it's no longer a problem with simultaneous access.
    u32 sflow_max_power_of_two = state->l2.sflow_max_power_of_two;

    if (sflow_max_power_of_two == 0) {
        sflow_max_power_of_two = next_power_of_two(LAN966X_SFLOW_MAX_SAMPLE_RATE);
        if ((LAN966X_SFLOW_MAX_SAMPLE_RATE & sflow_max_power_of_two) == 0) {
            sflow_max_power_of_two >>= 1;
        }
        state->l2.sflow_max_power_of_two = sflow_max_power_of_two;
    }

    // Compute the actual sampling rate given the user input.
    if (rate_in != 0 && power2) {
        // Round off to the nearest power of two.
        u32 temp1 = next_power_of_two(rate_in);
        u32 temp2 = temp1 >> 1;
        if (temp1 - rate_in < rate_in-temp2) {
            modified_rate_in = temp1;
        } else {
            modified_rate_in = temp2;
        }
        if (modified_rate_in == 0) {
            modified_rate_in = 1;
        } else if (modified_rate_in > sflow_max_power_of_two) {
            modified_rate_in = sflow_max_power_of_two;
        }
    } else {
        modified_rate_in = rate_in;
    }

    (void)lan966x_sflow_hw_rate(modified_rate_in, rate_out);
    return VTSS_RC_OK;
}

static vtss_rc lan966x_sflow_port_conf_set(vtss_state_t *vtss_state,
                                           const vtss_port_no_t port_no,
                                           const vtss_sflow_port_conf_t *const new_conf)
{
    u32 hw_rate;
    vtss_sflow_port_conf_t *cur_conf = &vtss_state->l2.sflow_conf[port_no];
    vtss_sflow_type_t type = (new_conf->sampling_rate == 0 ? VTSS_SFLOW_TYPE_NONE : new_conf->type);

    *cur_conf = *new_conf;
    hw_rate = lan966x_sflow_hw_rate(new_conf->sampling_rate, &cur_conf->sampling_rate);
    REG_WR(ANA_SFLOW_CFG(VTSS_CHIP_PORT(port_no)),
           ANA_SFLOW_CFG_SF_RATE(hw_rate) |
           (type == VTSS_SFLOW_TYPE_ALL || type == VTSS_SFLOW_TYPE_RX ? ANA_SFLOW_CFG_SF_SAMPLE_RX_M : 0) |
           (type == VTSS_SFLOW_TYPE_ALL || type == VTSS_SFLOW_TYPE_TX ? ANA_SFLOW_CFG_SF_SAMPLE_TX_M : 0));
    return VTSS_RC_OK;
}

/* ================================================================= *
 *  Debug print
 * ================================================================= */
static vtss_rc lan966x_debug_mac_table(vtss_state_t *vtss_state,
                                       const vtss_debug_printf_t pr,
                                       const vtss_debug_info_t   *const info)
{
    u32  value, port;
    BOOL header = 1;

    for (port = 0; port < VTSS_CHIP_PORTS; port++) {
        if (vtss_cmn_port2port_no(vtss_state, info, port) == VTSS_PORT_NO_NONE)
            continue;
        if (header)
            pr("Port  ID  Learn  L_Auto  L_CPU  L_DROP  PM_DROP PM_CPU Mirror\n");
        header = 0;

        REG_RD(ANA_PORT_CFG(port), &value);
        pr("%-4u  %-2u  %-5u  %-6u  %-5u  %-6u  %-7u  %-6u  %u\n",
           port,
           ANA_PORT_CFG_PORTID_VAL_X(value),
           VTSS_BOOL(value & ANA_PORT_CFG_LEARN_ENA_M),
           VTSS_BOOL(value & ANA_PORT_CFG_LEARNAUTO_M),
           VTSS_BOOL(value & ANA_PORT_CFG_LEARNCPU_M),
           VTSS_BOOL(value & ANA_PORT_CFG_LEARNDROP_M),
           VTSS_BOOL(value & ANA_PORT_CFG_LOCKED_PORTMOVE_DROP_M),
           VTSS_BOOL(value & ANA_PORT_CFG_LOCKED_PORTMOVE_CPU_M),
           VTSS_BOOL(value & ANA_PORT_CFG_SRC_MIRROR_ENA_M));
    }
    if (!header)
        pr("\n");

    /* Read and clear analyzer sticky bits */
    REG_RD(ANA_ANEVENTS, &value);
    REG_WR(ANA_ANEVENTS, value);
    vtss_debug_print_sticky(pr, "FLOOD_DISCARD", value, ANA_ANEVENTS_FLOOD_DISCARD_M);
    vtss_debug_print_sticky(pr, "MSTI_DROP", value, ANA_ANEVENTS_MSTI_DROP_M);
    vtss_debug_print_sticky(pr, "ACLKILL", value, ANA_ANEVENTS_ACLKILL_M);
    vtss_debug_print_sticky(pr, "ACLUSED", value, ANA_ANEVENTS_ACLUSED_M);
    vtss_debug_print_sticky(pr, "AUTOAGE", value, ANA_ANEVENTS_AUTOAGE_M);
    vtss_debug_print_sticky(pr, "LEARN_DROP", value, ANA_ANEVENTS_LEARN_DROP_M);
    vtss_debug_print_sticky(pr, "AGED_ENTRY", value, ANA_ANEVENTS_AGED_ENTRY_M);
    vtss_debug_print_sticky(pr, "CPU_LEARN_FAILED", value, ANA_ANEVENTS_CPU_LEARN_FAILED_M);
    vtss_debug_print_sticky(pr, "AUTO_LEARN_FAILED", value, ANA_ANEVENTS_AUTO_LEARN_FAILED_M);
    vtss_debug_print_sticky(pr, "LEARN_REMOVE", value, ANA_ANEVENTS_LEARN_REMOVE_M);
    vtss_debug_print_sticky(pr, "AUTO_LEARNED", value, ANA_ANEVENTS_AUTO_LEARNED_M);
    vtss_debug_print_sticky(pr, "AUTO_MOVED", value, ANA_ANEVENTS_AUTO_MOVED_M);
    vtss_debug_print_sticky(pr, "CLASSIFIED_DROP", value, ANA_ANEVENTS_CLASSIFIED_DROP_M);
    vtss_debug_print_sticky(pr, "CLASSIFIED_COPY", value, ANA_ANEVENTS_CLASSIFIED_COPY_M);
    vtss_debug_print_sticky(pr, "VLAN_DISCARD", value, ANA_ANEVENTS_VLAN_DISCARD_M);
    vtss_debug_print_sticky(pr, "FWD_DISCARD", value, ANA_ANEVENTS_FWD_DISCARD_M);
    vtss_debug_print_sticky(pr, "MULTICAST_FLOOD", value, ANA_ANEVENTS_MULTICAST_FLOOD_M);
    vtss_debug_print_sticky(pr, "UNICAST_FLOOD", value, ANA_ANEVENTS_UNICAST_FLOOD_M);
    vtss_debug_print_sticky(pr, "DEST_KNOWN", value, ANA_ANEVENTS_DEST_KNOWN_M);
    vtss_debug_print_sticky(pr, "BUCKET3_MATCH", value, ANA_ANEVENTS_BUCKET3_MATCH_M);
    vtss_debug_print_sticky(pr, "BUCKET2_MATCH", value, ANA_ANEVENTS_BUCKET2_MATCH_M);
    vtss_debug_print_sticky(pr, "BUCKET1_MATCH", value, ANA_ANEVENTS_BUCKET1_MATCH_M);
    vtss_debug_print_sticky(pr, "BUCKET0_MATCH", value, ANA_ANEVENTS_BUCKET0_MATCH_M);
    vtss_debug_print_sticky(pr, "CPU_OPERATION", value, ANA_ANEVENTS_CPU_OPERATION_M);
    vtss_debug_print_sticky(pr, "DMAC_LOOKUP", value, ANA_ANEVENTS_DMAC_LOOKUP_M);
    vtss_debug_print_sticky(pr, "SMAC_LOOKUP", value, ANA_ANEVENTS_SMAC_LOOKUP_M);
    vtss_debug_print_sticky(pr, "SEQ_GEN_ERR_0", value, ANA_ANEVENTS_SEQ_GEN_ERR_0_M);
    vtss_debug_print_sticky(pr, "SEQ_GEN_ERR_1", value, ANA_ANEVENTS_SEQ_GEN_ERR_1_M);
    pr("\n");

    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_src_table(vtss_state_t *vtss_state,
                                       const vtss_debug_printf_t pr,
                                       const vtss_debug_info_t   *const info)
{
    u32 port, mask;

    vtss_debug_print_header(pr, "Source Masks");
    vtss_lan966x_debug_print_port_header(vtss_state, pr, "Port  ");
    for (port = 0; port <= VTSS_CHIP_PORTS;  port++) {
        REG_RD(ANA_PGID(PGID_SRC + port), &mask);
        mask = ANA_PGID_PGID_X(mask);
        pr("%-4u  ", port);
        vtss_lan966x_debug_print_mask(pr, mask);
    }
    pr("\n");

    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_aggr(vtss_state_t *vtss_state,
                                  const vtss_debug_printf_t pr,
                                  const vtss_debug_info_t   *const info)
{
    u32 i, mask, value;

    VTSS_RC(lan966x_debug_src_table(vtss_state, pr, info));

    vtss_debug_print_header(pr, "Aggregation Masks");
    vtss_lan966x_debug_print_port_header(vtss_state, pr, "AC    ");
    for (i = 0; i < LAN966X_ACS; i++) {
        REG_RD(ANA_PGID(PGID_AGGR + i), &mask);
        mask = ANA_PGID_PGID_X(mask);
        pr("%-4u  ", i);
        vtss_lan966x_debug_print_mask(pr, mask);
    }
    pr("\n");

    vtss_debug_print_header(pr, "Destination Masks");
    vtss_lan966x_debug_print_port_header(vtss_state, pr, "PGID  CPU  Queue  ");
    for (i = 0; i < VTSS_PGIDS; i++) {
        REG_RD(ANA_PGID(i), &value);
        mask = ANA_PGID_PGID_X(value);
        pr("%-4u  %-3u  %-5u  ",
           i,
           mask & VTSS_BIT(VTSS_CHIP_PORT_CPU) ? 1 : 0,
           ANA_PGID_CPUQ_DST_PGID_X(value));
        vtss_lan966x_debug_print_mask(pr, mask);
    }
    pr("\n");

    vtss_debug_print_header(pr, "Flooding PGIDs");
    for (i = 0; i < VTSS_PRIOS; i++) {
        REG_RD(ANA_FLOODING(i), &value);
        pr("UNICAST_%u  : %u\n", i, ANA_FLOODING_FLD_UNICAST_X(value));
        pr("MULTICAST_%u: %u\n", i, ANA_FLOODING_FLD_MULTICAST_X(value));
        pr("BROADCAST_%u: %u\n", i, ANA_FLOODING_FLD_BROADCAST_X(value));
    }
    REG_RD(ANA_FLOODING_IPMC, &value);
    pr("MC4_CTRL   : %u\n", ANA_FLOODING_IPMC_FLD_MC4_CTRL_X(value));
    pr("MC4_DATA   : %u\n", ANA_FLOODING_IPMC_FLD_MC4_DATA_X(value));
    pr("MC6_CTRL   : %u\n", ANA_FLOODING_IPMC_FLD_MC6_CTRL_X(value));
    pr("MC6_DATA   : %u\n", ANA_FLOODING_IPMC_FLD_MC6_DATA_X(value));
    pr("\n");

    vtss_lan966x_debug_reg_header(pr, "Aggr. Mode");
    vtss_lan966x_debug_reg(vtss_state, pr, REG_ADDR(ANA_AGGR_CFG), "AGGR_CFG");
    pr("\n");

    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_stp(vtss_state_t *vtss_state,
                                 const vtss_debug_printf_t pr,
                                 const vtss_debug_info_t   *const info)
{
    u32  port;
    BOOL header = 1;

    for (port = 0; port < VTSS_CHIP_PORTS; port++) {
        if (vtss_cmn_port2port_no(vtss_state, info, port) == VTSS_PORT_NO_NONE)
            continue;
        if (header)
            vtss_lan966x_debug_reg_header(pr, "ANA");
        header = 0;
        vtss_lan966x_debug_reg_inst(vtss_state, pr, REG_ADDR(ANA_PORT_CFG(port)), port, "PORT_CFG");
    }
    if (!header)
        pr("\n");

    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_vxlat(vtss_state_t *vtss_state,
                                   const vtss_debug_printf_t pr,
                                   const vtss_debug_info_t   *const info)
{
    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_vlan(vtss_state_t *vtss_state,
                                  const vtss_debug_printf_t pr,
                                  const vtss_debug_info_t   *const info)
{
    vtss_vid_t        vid;
    vtss_vlan_entry_t *vlan_entry;
    BOOL              header = 1;
    vtss_port_no_t    port_no;
    u32               port, value, mask = 0, fid;
    char              buf[32];

    for (port = 0; port < (VTSS_CHIP_PORTS + 2); port++) {
        if (port < VTSS_CHIP_PORTS) {
            /* Normal ports */
            if ((port_no = vtss_cmn_port2port_no(vtss_state, info, port)) == VTSS_PORT_NO_NONE)
                continue;
            sprintf(buf, "Port %u (%u)", port, port_no);
        } else {
            /* CPU ports */
            if (!info->full)
                continue;
            sprintf(buf, "Port %u (CPU)", port);
        }

        vtss_lan966x_debug_reg_header(pr, buf);
        if (port != VTSS_CHIP_PORT_CPU_1) {
            vtss_lan966x_debug_reg_inst(vtss_state, pr, REG_ADDR(ANA_VLAN_CFG(port)), port, "ANA:VLAN_CFG");
            vtss_lan966x_debug_reg_inst(vtss_state, pr, REG_ADDR(ANA_DROP_CFG(port)), port, "ANA:DROP_CFG");
        }
        vtss_lan966x_debug_reg_inst(vtss_state, pr, REG_ADDR(REW_PORT_VLAN_CFG(port)), port, "REW:VLAN_CFG");
        vtss_lan966x_debug_reg_inst(vtss_state, pr, REG_ADDR(REW_TAG_CFG(port)), port, "REW:TAG_CFG");
        pr("\n");
    }
    vtss_lan966x_debug_reg(vtss_state, pr, REG_ADDR(ANA_VLAN_ETYPE_CFG), "ETYPE_CFG");
    vtss_lan966x_debug_reg(vtss_state, pr, REG_ADDR(ANA_ADVLEARN), "ADVLEARN");
    vtss_lan966x_debug_reg(vtss_state, pr, REG_ADDR(ANA_VLANMASK), "VLANMASK");
    pr("\n");

    for (vid = VTSS_VID_NULL; vid < VTSS_VIDS; vid++) {
        vlan_entry = &vtss_state->l2.vlan_table[vid];
        if (!vlan_entry->enabled && !info->full)
            continue;

        REG_WR(ANA_VLANTIDX, ANA_VLANTIDX_V_INDEX(vid));
        REG_WR(ANA_VLANACCESS, ANA_VLANACCESS_VLAN_TBL_CMD(VLANACCESS_CMD_READ));
        if (lan966x_vlan_table_idle(vtss_state) != VTSS_RC_OK)
            continue;
        REG_RD(ANA_VLANACCESS, &value);
        mask = ANA_VLANACCESS_VLAN_PORT_MASK_X(value);
        REG_RD(ANA_VLANTIDX, &value);
        REG_RD(ANA_FID_MAP(vid), &fid);
        fid = ANA_FID_MAP_FID_VAL_X(fid);
        if (header)
            vtss_lan966x_debug_print_port_header(vtss_state, pr, "VID   FID  Lrn  Mir  Prv  ");
        header = 0;

        pr("%-6u%-5u%-5u%-5u%-5u", vid, fid,
           value & ANA_VLANTIDX_VLAN_LEARN_DISABLED_M ? 0 : 1,
           value & ANA_VLANTIDX_VLAN_MIRROR_M ? 1 : 0,
           value & ANA_VLANTIDX_VLAN_PRIV_VLAN_M ? 1 : 0);
        vtss_lan966x_debug_print_mask(pr, mask);

        /* Leave critical region briefly */
        VTSS_EXIT_ENTER();
    }
    if (!header)
        pr("\n");

    return VTSS_RC_OK;
}

static vtss_rc lan966x_debug_pvlan(vtss_state_t *vtss_state,
                                   const vtss_debug_printf_t pr,
                                   const vtss_debug_info_t   *const info)
{
    vtss_lan966x_debug_reg_header(pr, "ANA");
    vtss_lan966x_debug_reg(vtss_state, pr, REG_ADDR(ANA_ISOLATED_PORTS), "ISOLATED_PORTS");
    pr("\n");

    return lan966x_debug_src_table(vtss_state, pr, info);
}

static vtss_rc lan966x_debug_mirror(vtss_state_t *vtss_state,
                                    const vtss_debug_printf_t pr,
                                    const vtss_debug_info_t   *const info)
{
    u32 port, value, mask = 0;

    /* Calculate ingress mirror mask */
    for (port = 0; port <= VTSS_CHIP_PORTS; port++) {
        REG_RD(ANA_PORT_CFG(port), &value);
        if (value & ANA_PORT_CFG_SRC_MIRROR_ENA_M)
            mask |= VTSS_BIT(port);
    }
    vtss_lan966x_debug_print_port_header(vtss_state, pr, "Mirror   ");
    pr("Ingress  ");
    vtss_lan966x_debug_print_mask(pr, mask);
    REG_RD(ANA_EMIRRORPORTS, &mask);
    pr("Egress   ");
    vtss_lan966x_debug_print_mask(pr, mask);
    REG_RD(ANA_MIRRORPORTS, &mask);
    pr("Ports    ");
    vtss_lan966x_debug_print_mask(pr, mask);
    pr("\n");

    return VTSS_RC_OK;
}


vtss_rc vtss_lan966x_l2_debug_print(vtss_state_t *vtss_state,
                                    const vtss_debug_printf_t pr,
                                    const vtss_debug_info_t   *const info)
{
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_VLAN, lan966x_debug_vlan, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_PVLAN, lan966x_debug_pvlan, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_MAC_TABLE, lan966x_debug_mac_table, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_VXLAT, lan966x_debug_vxlat, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_AGGR, lan966x_debug_aggr, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_STP, lan966x_debug_stp, vtss_state, pr, info));
    VTSS_RC(vtss_debug_print_group(VTSS_DEBUG_GROUP_MIRROR, lan966x_debug_mirror, vtss_state, pr, info));
    return VTSS_RC_OK;
}

/* - Initialization ------------------------------------------------ */

static vtss_rc lan966x_l2_port_map_set(vtss_state_t *vtss_state)
{
    u32 i;

    /* Set the number of available PGIDs */
    vtss_state->l2.pgid_count = (VTSS_PGIDS - VTSS_CHIP_PORTS + vtss_state->port_count);

    /* Reserve PGIDs for flooding */
    VTSS_RC(lan966x_flood_conf_set(vtss_state));

    /* Setup flooding PGIDs */
    for (i = 0; i < VTSS_PRIOS; i++) {
        REG_WR(ANA_FLOODING(i),
               ANA_FLOODING_FLD_UNICAST(PGID_UC) |
               ANA_FLOODING_FLD_BROADCAST(PGID_MC) |
               ANA_FLOODING_FLD_MULTICAST(PGID_MC));
    }
    REG_WR(ANA_FLOODING_IPMC,
           ANA_FLOODING_IPMC_FLD_MC4_CTRL(PGID_MC) |
           ANA_FLOODING_IPMC_FLD_MC4_DATA(PGID_MCIPV4) |
           ANA_FLOODING_IPMC_FLD_MC6_CTRL(PGID_MC) |
           ANA_FLOODING_IPMC_FLD_MC6_DATA(PGID_MCIPV6));
    return VTSS_RC_OK;
}

static vtss_rc lan966x_l2_init(vtss_state_t *vtss_state)
{
    vtss_vid_t vid;
    u32        port, value;

    /* Clear MAC table */
    REG_WR(ANA_MACACCESS, ANA_MACACCESS_MAC_TABLE_CMD(MACACCESS_CMD_INIT));

    /* Clear VLAN table */
    REG_WR(ANA_VLANACCESS, ANA_VLANACCESS_VLAN_TBL_CMD(VLANACCESS_CMD_INIT));

    /* Setup chip ports */
    for (port = 0; port < VTSS_CHIP_PORTS; port++) {
        /* Default VLAN port configuration */
        VTSS_RC(lan966x_vlan_port_conf_apply(vtss_state, port,
                                             &vtss_state->l2.vlan_port_conf[0]));
    }

    /* Clear VLAN table masks */
    for (vid = VTSS_VID_NULL; vid < VTSS_VIDS; vid++) {
        if (vid == VTSS_VID_DEFAULT) /* Default VLAN includes all ports */
            continue;
        REG_WR(ANA_VLANTIDX, ANA_VLANTIDX_V_INDEX(vid));
        REG_WR(ANA_VLANACCESS, ANA_VLANACCESS_VLAN_TBL_CMD(VLANACCESS_CMD_WRITE));
        VTSS_RC(lan966x_vlan_table_idle(vtss_state));
    }

    /* Setup VLAN configuration */
    VTSS_RC(lan966x_vlan_conf_apply(vtss_state, 0));

    /* Setup aggregation mode */
    VTSS_RC(lan966x_aggr_mode_set(vtss_state));

    /* Set MAC age time to default value */
    VTSS_RC(lan966x_mac_table_age_time_set(vtss_state));

    /* Disable learning for frames discarded by VLAN ingress filtering */
    REG_WR(ANA_ADVLEARN, ANA_ADVLEARN_VLAN_CHK_M);

    /* Ignore CPU copy flag in DMAC lookup */
    REG_WRM_SET(ANA_AGENCTRL, ANA_AGENCTRL_IGNORE_DMAC_FLAGS_M);

    /* Setup frame ageing to 2 seconds (6.5 us units) */
    REG_WR(SYS_FRM_AGING,
           SYS_FRM_AGING_AGE_TX_ENA_M |
           SYS_FRM_AGING_MAX_AGE(2*2000000/13));

    /* Set FRER TicksPerSecond to 1000 */
    value = (1000000000/(8*256*vtss_lan966x_clk_period_ps(vtss_state)));
    REG_WR(QSYS_FRER_CFG, QSYS_FRER_CFG_WATCHDOG_PRESCALER(value));

    return VTSS_RC_OK;
}

static vtss_rc lan966x_l2_poll(vtss_state_t *vtss_state)
{
    vtss_l2_state_t *state = &vtss_state->l2;
    vtss_stat_idx_t sidx;
    u32             idx;

    /* Poll counters for one SDX entry, giving 256 seconds between each poll.
       This ensures that any counter can wrap only once between each poll.
       On a 10Gbps port, a 32-bit frame counter would take about
       0xffffffff/14.880.000.000 = 288 seconds to wrap. */
    idx = state->sdx_info.poll_idx;
    sidx.idx = idx;
    sidx.edx = idx;
    VTSS_RC(lan966x_counters_update(vtss_state, &sidx, FALSE));
    idx++;
    state->sdx_info.poll_idx = (idx < VTSS_EVC_STAT_CNT ? idx : 0);
    return VTSS_RC_OK;
}

vtss_rc vtss_lan966x_l2_init(vtss_state_t *vtss_state, vtss_init_cmd_t cmd)
{
    vtss_l2_state_t *state = &vtss_state->l2;

    switch (cmd) {
    case VTSS_INIT_CMD_CREATE:
        state->mac_table_add = lan966x_mac_table_add;
        state->mac_table_del = lan966x_mac_table_del;
        state->mac_table_get = lan966x_mac_table_get;
        state->mac_table_get_next = lan966x_mac_table_get_next;
        state->mac_table_age_time_set = lan966x_mac_table_age_time_set;
        state->mac_table_age = lan966x_mac_table_age;
        state->mac_table_status_get = lan966x_mac_table_status_get;
        state->learn_port_mode_set = lan966x_learn_port_mode_set;
        state->learn_state_set = lan966x_learn_state_set;
        state->mstp_state_set = vtss_cmn_mstp_state_set;
        state->mstp_vlan_msti_set = vtss_cmn_vlan_members_set;
        state->erps_vlan_member_set = vtss_cmn_erps_vlan_member_set;
        state->erps_port_state_set = vtss_cmn_erps_port_state_set;
        state->pgid_table_write = lan966x_pgid_table_write;
        state->src_table_write = lan966x_src_table_write;
        state->aggr_table_write = lan966x_aggr_table_write;
        state->aggr_mode_set = lan966x_aggr_mode_set;
        state->pmap_table_write = lan966x_pmap_table_write;
        state->vlan_conf_set = lan966x_vlan_conf_set;
        state->vlan_port_conf_set = vtss_cmn_vlan_port_conf_set;
        state->vlan_port_conf_update = lan966x_vlan_port_conf_update;
        state->vlan_port_members_set = vtss_cmn_vlan_members_set;
        state->vlan_mask_update = lan966x_vlan_mask_update;
        state->vlan_tx_tag_set = vtss_cmn_vlan_tx_tag_set;
        state->isolated_vlan_set = vtss_cmn_vlan_members_set;
        state->isolated_port_members_set = lan966x_isolated_port_members_set;
        state->flood_conf_set = lan966x_flood_conf_set;
        state->ipv4_mc_add = vtss_cmn_ipv4_mc_add;
        state->ipv4_mc_del = vtss_cmn_ipv4_mc_del;
        state->ipv6_mc_add = vtss_cmn_ipv6_mc_add;
        state->ipv6_mc_del = vtss_cmn_ipv6_mc_del;
        state->ip_mc_update = lan966x_ip_mc_update;
        state->mirror_port_set = lan966x_mirror_port_set;
        state->mirror_ingress_set = lan966x_mirror_ingress_set;
        state->mirror_egress_set = lan966x_mirror_egress_set;
        state->mirror_cpu_ingress_set = lan966x_mirror_cpu_ingress_set;
        state->mirror_cpu_egress_set = lan966x_mirror_cpu_egress_set;
        state->eps_port_set = vtss_cmn_eps_port_set;
        state->sflow_port_conf_set = lan966x_sflow_port_conf_set;
        state->sflow_sampling_rate_convert = lan966x_sflow_sampling_rate_convert;
        // TBD: state->vcl_port_conf_set = vtss_lan966x_vcap_port_conf_set;
        state->vce_add = vtss_cmn_vce_add;
        state->vce_del = vtss_cmn_vce_del;
        state->vlan_trans_group_add = vtss_cmn_vlan_trans_group_add;
        state->vlan_trans_group_del = vtss_cmn_vlan_trans_group_del;
        state->vlan_trans_group_get = vtss_cmn_vlan_trans_group_get;
        state->vlan_trans_port_conf_set = vtss_cmn_vlan_trans_port_conf_set;
        state->vlan_trans_port_conf_get = vtss_cmn_vlan_trans_port_conf_get;
        // TBD: state->vcap_port_conf_set = lan966x_vcap_port_conf_set;
        state->iflow_conf_set = lan966x_iflow_conf_set;
        state->icnt_get = lan966x_icnt_get;
        state->ecnt_get = lan966x_ecnt_get;
        state->policer_update = lan966x_policer_update;
        state->counters_update = lan966x_counters_update;
        state->isdx_update = lan966x_isdx_update;
        state->cstream_conf_set = lan966x_cstream_conf_set;
        state->mstream_conf_set = lan966x_mstream_conf_set;
        state->cstream_cnt_get = lan966x_cstream_cnt_get;
        state->mstream_cnt_get = lan966x_mstream_cnt_get;
        state->psfp_gate_conf_set = lan966x_gate_conf_set;
        state->psfp_gate_status_get = lan966x_gate_status_get;
        state->psfp_filter_conf_set = lan966x_filter_conf_set;
        state->ac_count = LAN966X_ACS;
        state->sdx_info.max_count = VTSS_SDX_CNT;
        break;

    case VTSS_INIT_CMD_INIT:
        VTSS_RC(lan966x_l2_init(vtss_state));
        break;

    case VTSS_INIT_CMD_PORT_MAP:
        VTSS_RC(lan966x_l2_port_map_set(vtss_state));
        break;

    case VTSS_INIT_CMD_POLL:
        VTSS_RC(lan966x_l2_poll(vtss_state));
        break;

    default:
        break;
    }
    return VTSS_RC_OK;
}
#endif /* VTSS_ARCH_LAN966X */