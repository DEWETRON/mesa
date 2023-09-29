// Copyright (c) 2004-2020 Microchip Technology Inc. and its subsidiaries.
// SPDX-License-Identifier: MIT


#ifndef _MSCC_APPL_PTP_H_
#define _MSCC_APPL_PTP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* Initialize PTP */
void mscc_appl_ptp_init(mscc_appl_init_t *init);

/* Configure PTP transparent clock */
mesa_rc mscc_appl_ptp_setup_tc(const mesa_inst_t inst);

#ifdef __cplusplus
}
#endif

#endif /* _MSCC_APPL_PTP_H_ */