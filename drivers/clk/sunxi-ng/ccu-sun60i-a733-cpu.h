/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Tim Michals <tcmichals@gmail.com>
 */

#ifndef _CCU_SUN60I_A733_CPU_H_
#define _CCU_SUN60I_A733_CPU_H_

#include <dt-bindings/clock/allwinner,sun60i-a733-cpu-ccu.h>

#define CLK_PLL_CPU_BACK	0
#define CLK_PLL_CPU_L		1
#define CLK_PLL_CPU_DSU		2
#define CLK_PLL_CPU_B		3
#define CLK_DIV_CPU_L		4
#define CLK_DIV_CPU_DSU		5
#define CLK_DIV_CPU_B		6

#define CLK_NUMBER		(CLK_CPU_B + 1)

#endif /* _CCU_SUN60I_A733_CPU_H_ */
