#
# * Copyright (C) 2010 ARM Limited. All rights reserved.
# *
# * This program is free software and is provided to you under the terms of the GNU General Public License version 2
# * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
# *
# * A copy of the licence is included with the program, and can also be obtained from Free Software
# * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

#
# Makefile for the Mali drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

ccflags-y = -Iinclude/drm -D__devinit= -D__devexit= -D__devexit_p=
mali_drm-y := mali_drv.o

obj-$(CONFIG_DRM_MALI)   += mali_drm.o


