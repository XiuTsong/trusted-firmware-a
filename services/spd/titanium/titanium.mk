#
#
# Copyright (c) 2013-2019, ARM Limited and Contributors. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

TITANIUM_DIR		:=	services/spd/titanium
SPD_INCLUDES		:=

SPD_SOURCES		:=	services/spd/titanium/titanium_common.c	\
				services/spd/titanium/titanium_helpers.S	\
				services/spd/titanium/titanium_main.c	\
				services/spd/titanium/titanium_pm.c

NEED_BL32		:=	yes

# required so that optee code can control access to the timer registers
# NS_TIMER_SWITCH		:=	1
NS_TIMER_SWITCH		:=	0
