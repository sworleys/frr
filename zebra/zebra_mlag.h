/* Zebra mlag header.
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *                    Donald Sharp
 *
 * This file is part of FRR.
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * FRR is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FRR; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#ifndef __ZEBRA_MLAG_H__
#define __ZEBRA_MLAG_H__

#include "mlag.h"

#define ZEBRA_MLAG_BUF_LIMIT 2048

extern uint8_t mlag_wr_buffer[ZEBRA_MLAG_BUF_LIMIT];
extern uint8_t mlag_rd_buffer[ZEBRA_MLAG_BUF_LIMIT];
extern uint32_t mlag_wr_buf_ptr;

static inline void zebra_mlag_reset_write_buffer(void)
{
	memset(mlag_wr_buffer, 0, ZEBRA_MLAG_BUF_LIMIT);
	mlag_wr_buf_ptr = 0;
}

enum zebra_mlag_state {
	MLAG_UP = 1,
	MLAG_DOWN = 2,
};

void zebra_mlag_init(void);
void zebra_mlag_terminate(void);

enum mlag_role zebra_mlag_get_role(void);

void zebra_mlag_send_register(void);

void zebra_mlag_send_deregister(void);

void zebra_mlag_handle_process_state(enum zebra_mlag_state state);

void zebra_mlag_process_mlag_data(uint8_t *data, uint32_t len);

#endif
