/*
 * drivers/video/rockchip/rk_fb.c
 *
 * Copyright (C) ROCKCHIP, Inc.
 * Author:yxj<yxj@rock-chips.com>
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <asm/div64.h>
#include <linux/uaccess.h>
#include <linux/rk_fb.h>
#include <linux/linux_logo.h>
#include <linux/dma-mapping.h>
#include <linux/regulator/consumer.h>

#include "bmp_helper.h"

#define CONFIG_LOGO_LINUX_BMP 1 /* for 32bit framebuffer color depth */

#if defined(CONFIG_RK_HDMI)
#include "hdmi/rockchip-hdmi.h"
#endif

#if defined(CONFIG_ROCKCHIP_RGA) || defined(CONFIG_ROCKCHIP_RGA2)
#include "rga/rga.h"
#endif

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <video/of_display_timing.h>
#include <video/display_timing.h>
#include <dt-bindings/rkfb/rk_fb.h>
#endif

#if defined(CONFIG_ION_ROCKCHIP)
#include <linux/rockchip_ion.h>
#include <linux/rockchip-iovmm.h>
#include <linux/dma-buf.h>
#include <linux/highmem.h>
#endif

#define H_USE_FENCE 1
/* #define FB_ROATE_BY_KERNEL 1 */

static int hdmi_switch_complete;
static struct platform_device *fb_pdev;

#if defined(CONFIG_FB_MIRRORING)
int (*video_data_to_mirroring) (struct fb_info *info, u32 yuv_phy[2]);
EXPORT_SYMBOL(video_data_to_mirroring);
#endif

extern phys_addr_t uboot_logo_base;
extern phys_addr_t uboot_logo_size;
extern phys_addr_t uboot_logo_offset;
static struct rk_fb_trsm_ops *trsm_lvds_ops;
static struct rk_fb_trsm_ops *trsm_edp_ops;
static struct rk_fb_trsm_ops *trsm_mipi_ops;
static int uboot_logo_on;

#if 1  /* 2015/06 LCD_CHANGE */
#define WIN0_BUFFER_NUM 0
#define WIN1_BUFFER_NUM 2
#define HWC_BUFFER_NUM  4
#define VIDEO_BUFFER_NUM 6
#define VIDEO_BUFFER_END (VIDEO_BUFFER_NUM + control.alloc_max_num)
#define HWC_LUT_BUFFER_NUM (VIDEO_BUFFER_NUM + control.alloc_max_num)
#define HWC_LUT_BUFFER_END (HWC_LUT_BUFFER_NUM + control.hwc_lut_num)

struct lcd_contorol{
	struct rk_lcdc_driver *dev_drv;

	struct task_struct *bitblt_thread;      /* thread */
	wait_queue_head_t  bitblt_wait;         /* wait queue */
	int                bitblt_finish_flag;  /* wait flag */
	struct mutex       bitblt_lock;         /* compleate flag */

	struct task_struct *flip_thread;        /* thread */
	wait_queue_head_t  flip_wait;           /* wait queue */
	int                flip_finish_flag;    /* wait flag */
	struct mutex       flip_lock;           /* compleate flag */

	struct list_head	bitblt_list;        /* bitblt list */
	struct list_head	empty_bitblt_list;  /* bitblt list */
	int					bitblt_max_num;     /* max bitblt cmd */
	int					alloc_max_num;      /* max bitblt cmd */

	struct flip_action  flip_action;        /* flip action table */
	bool                black;              /* black display flag */
	struct bg_rgb       bg[2];              /* background RGB status */
	struct layer_val    win0[2];            /* win0 status */
	struct layer_val    win1[2];            /* win1 status */
	struct layer_val    hwc[2];             /* hwc  statusl */

	struct bk_buf_num   num;                /* back ground buffer bum */

	int hwc_lut_num;                        /* HWC lut max number */
};

struct ion_table{
	struct ion_handle *hdl;
	struct dma_buf *dma_buf;
	ion_phys_addr_t phy_addr;
	size_t len;
	int fd;
};

static struct ion_table *lcd_buffer;
#endif /* 2015/06 LCD_CHANGE */

int support_uboot_display(void)
{
	return uboot_logo_on;
}

int rk_fb_get_display_policy(void)
{
	struct rk_fb *rk_fb;

	if (fb_pdev) {
		rk_fb = platform_get_drvdata(fb_pdev);
		return rk_fb->disp_policy;
	} else {
		return DISPLAY_POLICY_SDK;
	}
}

int rk_fb_trsm_ops_register(struct rk_fb_trsm_ops *ops, int type)
{
	switch (type) {
	case SCREEN_RGB:
	case SCREEN_LVDS:
	case SCREEN_DUAL_LVDS:
		trsm_lvds_ops = ops;
		break;
	case SCREEN_EDP:
		trsm_edp_ops = ops;
		break;
	case SCREEN_MIPI:
	case SCREEN_DUAL_MIPI:
		trsm_mipi_ops = ops;
		break;
	default:
		printk(KERN_WARNING "%s:un supported transmitter:%d!\n",
		       __func__, type);
		break;
	}
	return 0;
}

struct rk_fb_trsm_ops *rk_fb_trsm_ops_get(int type)
{
	struct rk_fb_trsm_ops *ops;
	switch (type) {
	case SCREEN_RGB:
	case SCREEN_LVDS:
	case SCREEN_DUAL_LVDS:
		ops = trsm_lvds_ops;
		break;
	case SCREEN_EDP:
		ops = trsm_edp_ops;
		break;
	case SCREEN_MIPI:
	case SCREEN_DUAL_MIPI:
		ops = trsm_mipi_ops;
		break;
	default:
		ops = NULL;
		printk(KERN_WARNING "%s:un supported transmitter:%d!\n",
		       __func__, type);
		break;
	}
	return ops;
}

int rk_fb_pixel_width(int data_format)
{
	int pixel_width;
	switch (data_format) {
	case XBGR888:
	case ABGR888:
	case ARGB888:
		pixel_width = 4 * 8;
		break;
	case RGB888:
		pixel_width = 3 * 8;
		break;
	case RGB565:
		pixel_width = 2 * 8;
		break;
	case YUV422:
	case YUV420:
	case YUV444:
		pixel_width = 1 * 8;
		break;
	case YUV422_A:
	case YUV420_A:
	case YUV444_A:
		pixel_width = 8;
		break;
	default:
		printk(KERN_WARNING "%s:un supported format:0x%x\n",
		       __func__, data_format);
		return -EINVAL;
	}
	return pixel_width;
}

static int rk_fb_data_fmt(int data_format, int bits_per_pixel)
{
	int fb_data_fmt;
	if (data_format) {
		switch (data_format) {
		case HAL_PIXEL_FORMAT_RGBX_8888:
			fb_data_fmt = XBGR888;
			break;
		case HAL_PIXEL_FORMAT_RGBA_8888:
			fb_data_fmt = ABGR888;
			break;
		case HAL_PIXEL_FORMAT_BGRA_8888:
			fb_data_fmt = ARGB888;
			break;
		case HAL_PIXEL_FORMAT_RGB_888:
			fb_data_fmt = RGB888;
			break;
		case HAL_PIXEL_FORMAT_RGB_565:
			fb_data_fmt = RGB565;
			break;
		case HAL_PIXEL_FORMAT_YCbCr_422_SP:	/* yuv422 */
			fb_data_fmt = YUV422;
			break;
		case HAL_PIXEL_FORMAT_YCrCb_NV12:	/* YUV420---uvuvuv */
			fb_data_fmt = YUV420;
			break;
		case HAL_PIXEL_FORMAT_YCrCb_444:	/* yuv444 */
			fb_data_fmt = YUV444;
			break;
		case HAL_PIXEL_FORMAT_YCrCb_NV12_10:	/* yuv444 */
			fb_data_fmt = YUV420_A;
			break;
		case HAL_PIXEL_FORMAT_YCbCr_422_SP_10:	/* yuv444 */
			fb_data_fmt = YUV422_A;
			break;
		case HAL_PIXEL_FORMAT_YCrCb_420_SP_10:	/* yuv444 */
			fb_data_fmt = YUV444_A;
			break;
		default:
			printk(KERN_WARNING "%s:un supported format:0x%x\n",
			       __func__, data_format);
			return -EINVAL;
		}
	} else {
		switch (bits_per_pixel) {
		case 32:
			fb_data_fmt = ARGB888;
			break;
		case 24:
			fb_data_fmt = RGB888;
			break;
		case 16:
			fb_data_fmt = RGB565;
			break;
		default:
			printk(KERN_WARNING
			       "%s:un supported bits_per_pixel:%d\n", __func__,
			       bits_per_pixel);
			break;
		}
	}
	return fb_data_fmt;
}

/*
 * rk display power control parse from dts
 */
int rk_disp_pwr_ctr_parse_dt(struct rk_lcdc_driver *dev_drv)
{
	struct device_node *root = of_get_child_by_name(dev_drv->dev->of_node,
							"power_ctr");
	struct device_node *child;
	struct rk_disp_pwr_ctr_list *pwr_ctr;
	struct list_head *pos;
	enum of_gpio_flags flags;
	u32 val = 0;
	u32 debug = 0;
	int ret;

	INIT_LIST_HEAD(&dev_drv->pwrlist_head);
	if (!root) {
		dev_err(dev_drv->dev, "can't find power_ctr node for lcdc%d\n",
			dev_drv->id);
		return -ENODEV;
	}

	for_each_child_of_node(root, child) {
		pwr_ctr = kmalloc(sizeof(struct rk_disp_pwr_ctr_list),
				  GFP_KERNEL);
		strcpy(pwr_ctr->pwr_ctr.name, child->name);
		if (!of_property_read_u32(child, "rockchip,power_type", &val)) {
			if (val == GPIO) {
				pwr_ctr->pwr_ctr.type = GPIO;
				pwr_ctr->pwr_ctr.gpio = of_get_gpio_flags(child, 0, &flags);
				if (!gpio_is_valid(pwr_ctr->pwr_ctr.gpio)) {
					dev_err(dev_drv->dev, "%s ivalid gpio\n",
						child->name);
					return -EINVAL;
				}
				pwr_ctr->pwr_ctr.atv_val = !(flags & OF_GPIO_ACTIVE_LOW);
				ret = gpio_request(pwr_ctr->pwr_ctr.gpio,
						   child->name);
				if (ret) {
					dev_err(dev_drv->dev,
						"request %s gpio fail:%d\n",
						child->name, ret);
				}

			} else {
				pwr_ctr->pwr_ctr.type = REGULATOR;
				pwr_ctr->pwr_ctr.rgl_name = NULL;
				ret = of_property_read_string(child, "rockchip,regulator_name",
							     &(pwr_ctr->pwr_ctr.rgl_name));
				if (ret || IS_ERR_OR_NULL(pwr_ctr->pwr_ctr.rgl_name))
					dev_err(dev_drv->dev, "get regulator name failed!\n");
				if (!of_property_read_u32(child, "rockchip,regulator_voltage", &val))
					pwr_ctr->pwr_ctr.volt = val;
				else
					pwr_ctr->pwr_ctr.volt = 0;
			}
		};

		if (!of_property_read_u32(child, "rockchip,delay", &val))
			pwr_ctr->pwr_ctr.delay = val;
		else
			pwr_ctr->pwr_ctr.delay = 0;
		list_add_tail(&pwr_ctr->list, &dev_drv->pwrlist_head);
	}

	of_property_read_u32(root, "rockchip,debug", &debug);

	if (debug) {
		list_for_each(pos, &dev_drv->pwrlist_head) {
			pwr_ctr = list_entry(pos, struct rk_disp_pwr_ctr_list,
					     list);
			printk(KERN_INFO "pwr_ctr_name:%s\n"
			       "pwr_type:%s\n"
			       "gpio:%d\n"
			       "atv_val:%d\n"
			       "delay:%d\n\n",
			       pwr_ctr->pwr_ctr.name,
			       (pwr_ctr->pwr_ctr.type == GPIO) ? "gpio" : "regulator",
			       pwr_ctr->pwr_ctr.gpio,
			       pwr_ctr->pwr_ctr.atv_val,
			       pwr_ctr->pwr_ctr.delay);
		}
	}

	return 0;

}

int rk_disp_pwr_enable(struct rk_lcdc_driver *dev_drv)
{
	struct list_head *pos;
	struct rk_disp_pwr_ctr_list *pwr_ctr_list;
	struct pwr_ctr *pwr_ctr;
	struct regulator *regulator_lcd = NULL;
	int count = 10;

	if (list_empty(&dev_drv->pwrlist_head))
		return 0;
	list_for_each(pos, &dev_drv->pwrlist_head) {
		pwr_ctr_list = list_entry(pos, struct rk_disp_pwr_ctr_list,
					  list);
		pwr_ctr = &pwr_ctr_list->pwr_ctr;
		if (pwr_ctr->type == GPIO) {
			gpio_direction_output(pwr_ctr->gpio, pwr_ctr->atv_val);
			mdelay(pwr_ctr->delay);
		} else if (pwr_ctr->type == REGULATOR) {
			if (pwr_ctr->rgl_name)
				regulator_lcd = regulator_get(NULL, pwr_ctr->rgl_name);
			if (regulator_lcd == NULL) {
				dev_err(dev_drv->dev,
					"%s: regulator get failed,regulator name:%s\n",
					__func__, pwr_ctr->rgl_name);
				continue;
			}
			regulator_set_voltage(regulator_lcd, pwr_ctr->volt, pwr_ctr->volt);
			while (!regulator_is_enabled(regulator_lcd)) {
				if (regulator_enable(regulator_lcd) == 0 || count == 0)
					break;
				else
					dev_err(dev_drv->dev,
						"regulator_enable failed,count=%d\n",
						count);
				count--;
			}
			regulator_put(regulator_lcd);
			msleep(pwr_ctr->delay);
		}
	}

	return 0;
}

int rk_disp_pwr_disable(struct rk_lcdc_driver *dev_drv)
{
	struct list_head *pos;
	struct rk_disp_pwr_ctr_list *pwr_ctr_list;
	struct pwr_ctr *pwr_ctr;
	struct regulator *regulator_lcd = NULL;
	int count = 10;

	if (list_empty(&dev_drv->pwrlist_head))
		return 0;
	list_for_each(pos, &dev_drv->pwrlist_head) {
		pwr_ctr_list = list_entry(pos, struct rk_disp_pwr_ctr_list,
					  list);
		pwr_ctr = &pwr_ctr_list->pwr_ctr;
		if (pwr_ctr->type == GPIO) {
			gpio_set_value(pwr_ctr->gpio, !pwr_ctr->atv_val);
		} else if (pwr_ctr->type == REGULATOR) {
			if (pwr_ctr->rgl_name)
				regulator_lcd = regulator_get(NULL, pwr_ctr->rgl_name);
			if (regulator_lcd == NULL) {
				dev_err(dev_drv->dev,
					"%s: regulator get failed,regulator name:%s\n",
					__func__, pwr_ctr->rgl_name);
				continue;
			}
			while (regulator_is_enabled(regulator_lcd) > 0) {
				if (regulator_disable(regulator_lcd) == 0 || count == 0)
					break;
				else
					dev_err(dev_drv->dev,
						"regulator_disable failed,count=%d\n",
						count);
				count--;
			}
			regulator_put(regulator_lcd);
		}
	}
	return 0;
}

int rk_fb_video_mode_from_timing(const struct display_timing *dt,
				 struct rk_screen *screen)
{
	screen->mode.pixclock = dt->pixelclock.typ;
	screen->mode.left_margin = dt->hback_porch.typ;
	screen->mode.right_margin = dt->hfront_porch.typ;
	screen->mode.xres = dt->hactive.typ;
	screen->mode.hsync_len = dt->hsync_len.typ;
	screen->mode.upper_margin = dt->vback_porch.typ;
	screen->mode.lower_margin = dt->vfront_porch.typ;
	screen->mode.yres = dt->vactive.typ;
	screen->mode.vsync_len = dt->vsync_len.typ;
	screen->type = dt->screen_type;
	screen->lvds_format = dt->lvds_format;
	screen->face = dt->face;
	screen->color_mode = dt->color_mode;
	screen->dsp_lut = dt->dsp_lut;

	if (dt->flags & DISPLAY_FLAGS_PIXDATA_POSEDGE)
		screen->pin_dclk = 1;
	else
		screen->pin_dclk = 0;
	if (dt->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		screen->pin_hsync = 1;
	else
		screen->pin_hsync = 0;
	if (dt->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		screen->pin_vsync = 1;
	else
		screen->pin_vsync = 0;
	if (dt->flags & DISPLAY_FLAGS_DE_HIGH)
		screen->pin_den = 1;
	else
		screen->pin_den = 0;

	return 0;

}

int rk_fb_prase_timing_dt(struct device_node *np, struct rk_screen *screen)
{
	struct display_timings *disp_timing;
	struct display_timing *dt;
	disp_timing = of_get_display_timings(np);
	if (!disp_timing) {
		pr_err("parse display timing err\n");
		return -EINVAL;
	}
	dt = display_timings_get(disp_timing, disp_timing->native_mode);
	rk_fb_video_mode_from_timing(dt, screen);
	return 0;

}

int rk_fb_calc_fps(struct rk_screen *screen, u32 pixclock)
{
	int x, y;
	unsigned long long hz;
	if (!screen) {
		printk(KERN_ERR "%s:null screen!\n", __func__);
		return 0;
	}
	x = screen->mode.xres + screen->mode.left_margin +
	    screen->mode.right_margin + screen->mode.hsync_len;
	y = screen->mode.yres + screen->mode.upper_margin +
	    screen->mode.lower_margin + screen->mode.vsync_len;

	hz = 1000000000000ULL;	/* 1e12 picoseconds per second */

	hz += (x * y) / 2;
	do_div(hz, x * y);	/* divide by x * y with rounding */

	hz += pixclock / 2;
	do_div(hz, pixclock);	/* divide by pixclock with rounding */

	return hz;
}

char *get_format_string(enum data_format format, char *fmt)
{
	if (!fmt)
		return NULL;
	switch (format) {
	case ARGB888:
		strcpy(fmt, "ARGB888");
		break;
	case RGB888:
		strcpy(fmt, "RGB888");
		break;
	case RGB565:
		strcpy(fmt, "RGB565");
		break;
	case YUV420:
		strcpy(fmt, "YUV420");
		break;
	case YUV422:
		strcpy(fmt, "YUV422");
		break;
	case YUV444:
		strcpy(fmt, "YUV444");
		break;
	case YUV420_A:
		strcpy(fmt, "YUV420_A");
		break;
	case YUV422_A:
		strcpy(fmt, "YUV422_A");
		break;
	case YUV444_A:
		strcpy(fmt, "YUV444_A");
		break;
	case XRGB888:
		strcpy(fmt, "XRGB888");
		break;
	case XBGR888:
		strcpy(fmt, "XBGR888");
		break;
	case ABGR888:
		strcpy(fmt, "ABGR888");
		break;
	default:
		strcpy(fmt, "invalid");
		break;
	}

	return fmt;

}

/*
 * this is for hdmi
 * name: lcdc device name ,lcdc0 , lcdc1
 */
struct rk_lcdc_driver *rk_get_lcdc_drv(char *name)
{
	struct rk_fb *inf = NULL;
	struct rk_lcdc_driver *dev_drv = NULL;
	int i = 0;

        if (likely(fb_pdev))
                inf = platform_get_drvdata(fb_pdev);
        else
                return NULL;

	for (i = 0; i < inf->num_lcdc; i++) {
		if (!strcmp(inf->lcdc_dev_drv[i]->name, name)) {
			dev_drv = inf->lcdc_dev_drv[i];
			break;
		}
	}

	return dev_drv;
}

static struct rk_lcdc_driver *rk_get_prmry_lcdc_drv(void)
{
	struct rk_fb *inf = NULL;
	struct rk_lcdc_driver *dev_drv = NULL;
	int i = 0;

	if (likely(fb_pdev))
		inf = platform_get_drvdata(fb_pdev);
	else
		return NULL;

	for (i = 0; i < inf->num_lcdc; i++) {
		if (inf->lcdc_dev_drv[i]->prop == PRMRY) {
			dev_drv = inf->lcdc_dev_drv[i];
			break;
		}
	}

	return dev_drv;
}

static __maybe_unused struct rk_lcdc_driver *rk_get_extend_lcdc_drv(void)
{
	struct rk_fb *inf = NULL;
	struct rk_lcdc_driver *dev_drv = NULL;
	int i = 0;

	if (likely(fb_pdev))
		inf = platform_get_drvdata(fb_pdev);
	else
		return NULL;

	for (i = 0; i < inf->num_lcdc; i++) {
		if (inf->lcdc_dev_drv[i]->prop == EXTEND) {
			dev_drv = inf->lcdc_dev_drv[i];
			break;
		}
	}

	return dev_drv;
}

/*
 * get one frame time of the prmry screen, unit: us
 */
u32 rk_fb_get_prmry_screen_ft(void)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();
	uint32_t htotal, vtotal, pixclock_ps;
	u64 pix_total, ft_us;

	if (unlikely(!dev_drv))
		return 0;

	pixclock_ps = dev_drv->pixclock;

	vtotal = (dev_drv->cur_screen->mode.upper_margin +
		 dev_drv->cur_screen->mode.lower_margin +
		 dev_drv->cur_screen->mode.yres +
		 dev_drv->cur_screen->mode.vsync_len);
	htotal = (dev_drv->cur_screen->mode.left_margin +
		 dev_drv->cur_screen->mode.right_margin +
		 dev_drv->cur_screen->mode.xres +
		 dev_drv->cur_screen->mode.hsync_len);
	pix_total = htotal * vtotal;
	ft_us = pix_total * pixclock_ps;
	do_div(ft_us, 1000000);
	if (dev_drv->frame_time.ft == 0)
		dev_drv->frame_time.ft = ft_us;

	ft_us = dev_drv->frame_time.framedone_t - dev_drv->frame_time.last_framedone_t;
	do_div(ft_us, 1000);
	ft_us = min(dev_drv->frame_time.ft, (u32)ft_us);
	if (ft_us != 0)
		dev_drv->frame_time.ft = ft_us;

	return dev_drv->frame_time.ft;
}

/*
 * get the vblanking time of the prmry screen, unit: us
 */
u32 rk_fb_get_prmry_screen_vbt(void)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();
	uint32_t htotal, vblank, pixclock_ps;
	u64 pix_blank, vbt_us;

	if (unlikely(!dev_drv))
		return 0;

	pixclock_ps = dev_drv->pixclock;

	htotal = (dev_drv->cur_screen->mode.left_margin +
		 dev_drv->cur_screen->mode.right_margin +
		 dev_drv->cur_screen->mode.xres +
		 dev_drv->cur_screen->mode.hsync_len);
	vblank = (dev_drv->cur_screen->mode.upper_margin +
		 dev_drv->cur_screen->mode.lower_margin +
		 dev_drv->cur_screen->mode.vsync_len);
	pix_blank = htotal * vblank;
	vbt_us = pix_blank * pixclock_ps;
	do_div(vbt_us, 1000000);
	return (u32)vbt_us;
}

/*
 * get the frame done time of the prmry screen, unit: us
 */
u64 rk_fb_get_prmry_screen_framedone_t(void)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();

	if (unlikely(!dev_drv))
		return 0;
	else
		return dev_drv->frame_time.framedone_t;
}

/*
 * set prmry screen status
 */
int rk_fb_set_prmry_screen_status(int status)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();
	struct rk_screen *screen;

	if (unlikely(!dev_drv))
		return 0;

	screen = dev_drv->cur_screen;
	switch (status) {
	case SCREEN_PREPARE_DDR_CHANGE:
		if (screen->type == SCREEN_MIPI
			|| screen->type == SCREEN_DUAL_MIPI) {
			if (dev_drv->trsm_ops->dsp_pwr_off)
				dev_drv->trsm_ops->dsp_pwr_off();
		}
		break;
	case SCREEN_UNPREPARE_DDR_CHANGE:
		if (screen->type == SCREEN_MIPI
			|| screen->type == SCREEN_DUAL_MIPI) {
			if (dev_drv->trsm_ops->dsp_pwr_on)
				dev_drv->trsm_ops->dsp_pwr_on();
		}
		break;
	default:
		break;
	}

	return 0;
}

u32 rk_fb_get_prmry_screen_pixclock(void)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();

	if (unlikely(!dev_drv))
		return 0;
	else
		return dev_drv->pixclock;
}

int rk_fb_poll_prmry_screen_vblank(void)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();

	if (likely(dev_drv)) {
		if (dev_drv->ops->poll_vblank)
			return dev_drv->ops->poll_vblank(dev_drv);
		else
			return RK_LF_STATUS_NC;
	} else {
		return RK_LF_STATUS_NC;
	}
}

bool rk_fb_poll_wait_frame_complete(void)
{
	uint32_t timeout = RK_LF_MAX_TIMEOUT;
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();

	if (likely(dev_drv)) {
		if (dev_drv->ops->set_irq_to_cpu)
			dev_drv->ops->set_irq_to_cpu(dev_drv, 0);
	}

	if (rk_fb_poll_prmry_screen_vblank() == RK_LF_STATUS_NC) {
		if (likely(dev_drv)) {
			if (dev_drv->ops->set_irq_to_cpu)
				dev_drv->ops->set_irq_to_cpu(dev_drv, 1);
		}
		return false;
	}
	while (!(rk_fb_poll_prmry_screen_vblank() == RK_LF_STATUS_FR) && --timeout)
		;
	while (!(rk_fb_poll_prmry_screen_vblank() == RK_LF_STATUS_FC) && --timeout)
		;
	if (likely(dev_drv)) {
		if (dev_drv->ops->set_irq_to_cpu)
			dev_drv->ops->set_irq_to_cpu(dev_drv, 1);
	}

	return true;
}


/* rk_fb_get_sysmmu_device_by_compatible()
 * @compt: dts device compatible name
 * return value: success: pointer to the device inside of platform device
 *               fail: NULL
 */
struct device *rk_fb_get_sysmmu_device_by_compatible(const char *compt)
{
        struct device_node *dn = NULL;
        struct platform_device *pd = NULL;
        struct device *ret = NULL ;

        dn = of_find_compatible_node(NULL, NULL, compt);
        if (!dn) {
                printk("can't find device node %s \r\n", compt);
                return NULL;
	}

        pd = of_find_device_by_node(dn);
        if (!pd) {
                printk("can't find platform device in device node %s \r\n", compt);
                return  NULL;
        }
        ret = &pd->dev;

        return ret;
}

#ifdef CONFIG_IOMMU_API
void rk_fb_platform_set_sysmmu(struct device *sysmmu, struct device *dev)
{
        dev->archdata.iommu = sysmmu;
}
#else
void rk_fb_platform_set_sysmmu(struct device *sysmmu, struct device *dev)
{

}
#endif

static int rk_fb_open(struct fb_info *info, int user)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	int win_id;

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	fb_par->state++;
	/* if this win aready opened ,no need to reopen */
	if (dev_drv->win[win_id]->state)
		return 0;
	else
		dev_drv->ops->open(dev_drv, win_id, 1);
	return 0;
}

static int rk_fb_close(struct fb_info *info, int user)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_lcdc_win *win = NULL;
	int win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);

	if (win_id >= 0) {
		win = dev_drv->win[win_id];
		fb_par->state--;
		if (!fb_par->state) {
			if (fb_par->fb_phy_base > 0)
				info->fix.smem_start = fb_par->fb_phy_base;
			info->var.xres = dev_drv->screen0->mode.xres;
			info->var.yres = dev_drv->screen0->mode.yres;
			/*
			info->var.grayscale |=
			    (info->var.xres << 8) + (info->var.yres << 20);
			*/
			info->var.xres_virtual = info->var.xres;
			info->var.yres_virtual = info->var.yres;
#if defined(CONFIG_LOGO_LINUX_BMP)
			info->var.bits_per_pixel = 32;
#else
			info->var.bits_per_pixel = 16;
#endif
			info->fix.line_length =
			    (info->var.xres_virtual) * (info->var.bits_per_pixel >> 3);
			info->var.width = dev_drv->screen0->width;
			info->var.height = dev_drv->screen0->height;
			info->var.pixclock = dev_drv->pixclock;
			info->var.left_margin = dev_drv->screen0->mode.left_margin;
			info->var.right_margin = dev_drv->screen0->mode.right_margin;
			info->var.upper_margin = dev_drv->screen0->mode.upper_margin;
			info->var.lower_margin = dev_drv->screen0->mode.lower_margin;
			info->var.vsync_len = dev_drv->screen0->mode.vsync_len;
			info->var.hsync_len = dev_drv->screen0->mode.hsync_len;
		}
	}

	return 0;
}

#if defined(FB_ROATE_BY_KERNEL)

#if defined(CONFIG_RK29_IPP)
static int get_ipp_format(int fmt)
{
	int ipp_fmt = IPP_XRGB_8888;
	switch (fmt) {
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
	case HAL_PIXEL_FORMAT_RGB_888:
		ipp_fmt = IPP_XRGB_8888;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
		ipp_fmt = IPP_RGB_565;
		break;
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
		ipp_fmt = IPP_Y_CBCR_H2V1;
		break;
	case HAL_PIXEL_FORMAT_YCrCb_NV12:
		ipp_fmt = IPP_Y_CBCR_H2V2;
		break;
	case HAL_PIXEL_FORMAT_YCrCb_444:
		ipp_fmt = IPP_Y_CBCR_H1V1;
		break;
	default:
		ipp_fmt = IPP_IMGTYPE_LIMIT;
		break;
	}

	return ipp_fmt;
}

static void ipp_win_check(int *dst_w, int *dst_h, int *dst_vir_w,
			  int rotation, int fmt)
{
	int align16 = 2;
	int align64 = 8;

	if (fmt == IPP_XRGB_8888) {
		align16 = 1;
		align64 = 2;
	} else if (fmt == IPP_RGB_565) {
		align16 = 1;
		align64 = 4;
	} else {
		align16 = 2;
		align64 = 8;
	}
	align16 -= 1;		/*for YUV, 1 */
	align64 -= 1;		/*for YUV, 7 */

	if (rotation == IPP_ROT_0) {
		if (fmt > IPP_RGB_565) {
			if ((*dst_w & 1) != 0)
				*dst_w = *dst_w + 1;
			if ((*dst_h & 1) != 0)
				*dst_h = *dst_h + 1;
			if (*dst_vir_w < *dst_w)
				*dst_vir_w = *dst_w;
		}
	} else {
		if ((*dst_w & align64) != 0)
			*dst_w = (*dst_w + align64) & (~align64);
		if ((fmt > IPP_RGB_565) && ((*dst_h & 1) == 1))
			*dst_h = *dst_h + 1;
		if (*dst_vir_w < *dst_w)
			*dst_vir_w = *dst_w;
	}
}

static void fb_copy_by_ipp(struct fb_info *dst_info,
				struct fb_info *src_info)
{
	struct rk29_ipp_req ipp_req;
	uint32_t rotation = 0;
	int dst_w, dst_h, dst_vir_w;
	int ipp_fmt;
	u8 data_format = (dst_info->var.nonstd) & 0xff;
	struct rk_fb_par *fb_par = (struct rk_fb_par *)dst_info->par;
	struct rk_lcdc_driver *ext_dev_drv = fb_par->lcdc_drv;
	u16 orientation = ext_dev_drv->rotate_mode;

	memset(&ipp_req, 0, sizeof(struct rk29_ipp_req));

	switch (orientation) {
	case 0:
		rotation = IPP_ROT_0;
		break;
	case ROTATE_90:
		rotation = IPP_ROT_90;
		break;
	case ROTATE_180:
		rotation = IPP_ROT_180;
		break;
	case ROTATE_270:
		rotation = IPP_ROT_270;
		break;
	default:
		rotation = IPP_ROT_270;
		break;
	}

	dst_w = dst_info->var.xres;
	dst_h = dst_info->var.yres;
	dst_vir_w = dst_info->var.xres_virtual;
	ipp_fmt = get_ipp_format(data_format);
	ipp_win_check(&dst_w, &dst_h, &dst_vir_w, rotation, ipp_fmt);
	ipp_req.src0.YrgbMst = src_info->fix.smem_start + offset;
	ipp_req.src0.w = src_info->var.xres;
	ipp_req.src0.h = src_info->var.yres;
	ipp_req.src_vir_w = src_info->var.xres_virtual;
	ipp_req.src0.fmt = ipp_fmt;

	ipp_req.dst0.YrgbMst = dst_info->fix.smem_start + offset;
	ipp_req.dst0.w = dst_w;
	ipp_req.dst0.h = dst_h;
	ipp_req.dst_vir_w = dst_vir_w;
	ipp_req.dst0.fmt = ipp_fmt;

	ipp_req.timeout = 100;
	ipp_req.flag = rotation;
	ipp_blit_sync(&ipp_req);
}

#endif

#if defined(CONFIG_ROCKCHIP_RGA) || defined(CONFIG_ROCKCHIP_RGA2)
static int get_rga_format(int fmt)
{
	int rga_fmt = 0;

	switch (fmt) {
	case XBGR888:
		rga_fmt = RK_FORMAT_RGBX_8888;
		break;
	case ABGR888:
		rga_fmt = RK_FORMAT_RGBA_8888;
		break;
	case ARGB888:
		rga_fmt = RK_FORMAT_BGRA_8888;
		break;
	case RGB888:
		rga_fmt = RK_FORMAT_RGB_888;
		break;
	case RGB565:
		rga_fmt = RK_FORMAT_RGB_565;
		break;
	case YUV422:
		rga_fmt = RK_FORMAT_YCbCr_422_SP;
		break;
	case YUV420:
		rga_fmt = RK_FORMAT_YCbCr_420_SP;
		break;
	default:
		rga_fmt = RK_FORMAT_RGBA_8888;
		break;
	}

	return rga_fmt;
}

static void rga_win_check(struct rk_lcdc_win *dst_win,
			  struct rk_lcdc_win *src_win)
{
	int format = 0;

	format = get_rga_format(src_win->area[0].format);
	/* width and height must be even number */
	if (format >= RK_FORMAT_YCbCr_422_SP &&
	    format <= RK_FORMAT_YCrCb_420_P) {
		if ((src_win->area[0].xact % 2) != 0)
			src_win->area[0].xact += 1;
		if ((src_win->area[0].yact % 2) != 0)
			src_win->area[0].yact += 1;
	}
	if (src_win->area[0].xvir < src_win->area[0].xact)
		src_win->area[0].xvir = src_win->area[0].xact;
	if (src_win->area[0].yvir < src_win->area[0].yact)
		src_win->area[0].yvir = src_win->area[0].yact;

	format = get_rga_format(dst_win->area[0].format);
	if (format >= RK_FORMAT_YCbCr_422_SP &&
	    format <= RK_FORMAT_YCrCb_420_P) {
		if ((dst_win->area[0].xact % 2) != 0)
			dst_win->area[0].xact += 1;
		if ((dst_win->area[0].yact % 2) != 0)
			dst_win->area[0].yact += 1;
	}
	if (dst_win->area[0].xvir < dst_win->area[0].xact)
		dst_win->area[0].xvir = dst_win->area[0].xact;
	if (dst_win->area[0].yvir < dst_win->area[0].yact)
		dst_win->area[0].yvir = dst_win->area[0].yact;
}

static void win_copy_by_rga(struct rk_lcdc_win *dst_win,
			    struct rk_lcdc_win *src_win,
			    u16 orientation, int iommu_en)
{
	struct rga_req Rga_Request;
	long ret = 0;
	/* int fd = 0; */

	memset(&Rga_Request, 0, sizeof(Rga_Request));
	rga_win_check(dst_win, src_win);

	switch (orientation) {
	case ROTATE_90:
		Rga_Request.rotate_mode = 1;
		Rga_Request.sina = 65536;
		Rga_Request.cosa = 0;
		Rga_Request.dst.act_w = dst_win->area[0].yact;
		Rga_Request.dst.act_h = dst_win->area[0].xact;
		Rga_Request.dst.x_offset = dst_win->area[0].xact - 1;
		Rga_Request.dst.y_offset = 0;
		break;
	case ROTATE_180:
		Rga_Request.rotate_mode = 1;
		Rga_Request.sina = 0;
		Rga_Request.cosa = -65536;
		Rga_Request.dst.act_w = dst_win->area[0].xact;
		Rga_Request.dst.act_h = dst_win->area[0].yact;
		Rga_Request.dst.x_offset = dst_win->area[0].xact - 1;
		Rga_Request.dst.y_offset = dst_win->area[0].yact - 1;
		break;
	case ROTATE_270:
		Rga_Request.rotate_mode = 1;
		Rga_Request.sina = -65536;
		Rga_Request.cosa = 0;
		Rga_Request.dst.act_w = dst_win->area[0].yact;
		Rga_Request.dst.act_h = dst_win->area[0].xact;
		Rga_Request.dst.x_offset = 0;
		Rga_Request.dst.y_offset = dst_win->area[0].yact - 1;
		break;
	default:
		Rga_Request.rotate_mode = 0;
		Rga_Request.dst.act_w = dst_win->area[0].xact;
		Rga_Request.dst.act_h = dst_win->area[0].yact;
		Rga_Request.dst.x_offset = dst_win->area[0].xact - 1;
		Rga_Request.dst.y_offset = dst_win->area[0].yact - 1;
		break;
	}

/*
	fd = ion_share_dma_buf_fd(rk_fb->ion_client, src_win->area[0].ion_hdl);
	Rga_Request.src.yrgb_addr = fd;
	fd = ion_share_dma_buf_fd(rk_fb->ion_client, dst_win->area[0].ion_hdl);
	Rga_Request.dst.yrgb_addr = fd;
*/
	Rga_Request.src.yrgb_addr = 0;
	Rga_Request.src.uv_addr =
	    src_win->area[0].smem_start + src_win->area[0].y_offset;
	Rga_Request.src.v_addr = 0;

	Rga_Request.dst.yrgb_addr = 0;
	Rga_Request.dst.uv_addr =
	    dst_win->area[0].smem_start + dst_win->area[0].y_offset;
	Rga_Request.dst.v_addr = 0;

	Rga_Request.src.vir_w = src_win->area[0].xvir;
	Rga_Request.src.vir_h = src_win->area[0].yvir;
	Rga_Request.src.format = get_rga_format(src_win->area[0].format);
	Rga_Request.src.act_w = src_win->area[0].xact;
	Rga_Request.src.act_h = src_win->area[0].yact;
	Rga_Request.src.x_offset = 0;
	Rga_Request.src.y_offset = 0;

	Rga_Request.dst.vir_w = dst_win->area[0].xvir;
	Rga_Request.dst.vir_h = dst_win->area[0].yvir;
	Rga_Request.dst.format = get_rga_format(dst_win->area[0].format);

	Rga_Request.clip.xmin = 0;
	Rga_Request.clip.xmax = dst_win->area[0].xact - 1;
	Rga_Request.clip.ymin = 0;
	Rga_Request.clip.ymax = dst_win->area[0].yact - 1;
	Rga_Request.scale_mode = 0;
#if defined(CONFIG_ROCKCHIP_IOMMU)
	if (iommu_en) {
		Rga_Request.mmu_info.mmu_en = 1;
		Rga_Request.mmu_info.mmu_flag = 1;
	} else {
		Rga_Request.mmu_info.mmu_en = 0;
		Rga_Request.mmu_info.mmu_flag = 0;
	}
#else
	Rga_Request.mmu_info.mmu_en = 0;
	Rga_Request.mmu_info.mmu_flag = 0;
#endif

	ret = rga_ioctl_kernel(&Rga_Request);
}

/*
 * This function is used for copying fb by RGA Module
 * RGA only support copy RGB to RGB
 * RGA2 support copy RGB to RGB and YUV to YUV
 */
static void fb_copy_by_rga(struct fb_info *dst_info,
				struct fb_info *src_info)
{
	struct rk_fb_par *src_fb_par = (struct rk_fb_par *)src_info->par;
	struct rk_fb_par *dst_fb_par = (struct rk_fb_par *)dst_info->par;
	struct rk_lcdc_driver *dev_drv = src_fb_par->lcdc_drv;
	struct rk_lcdc_driver *ext_dev_drv = dst_fb_par->lcdc_drv;
	int win_id = 0, ext_win_id;
	struct rk_lcdc_win *src_win, *dst_win;

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, src_info->fix.id);
	src_win = dev_drv->win[win_id];

	ext_win_id =
	    ext_dev_drv->ops->fb_get_win_id(ext_dev_drv, dst_info->fix.id);
	dst_win = ext_dev_drv->win[ext_win_id];

	win_copy_by_rga(dst_win, src_win, ext_dev_drv->rotate_mode,
			ext_dev_drv->iommu_enabled);
}
#endif

static int rk_fb_rotate(struct fb_info *dst_info,
			  struct fb_info *src_info)
{

#if defined(CONFIG_RK29_IPP)
	fb_copy_by_ipp(dst_info, src_info);
#elif defined(CONFIG_ROCKCHIP_RGA) || defined(CONFIG_ROCKCHIP_RGA2)
	fb_copy_by_rga(dst_info, src_info);
#else
	return -1;
#endif
	return 0;
}

static int __maybe_unused rk_fb_win_rotate(struct rk_lcdc_win *dst_win,
					    struct rk_lcdc_win *src_win,
					    u16 rotate, int iommu_en)
{
#if defined(CONFIG_ROCKCHIP_RGA) || defined(CONFIG_ROCKCHIP_RGA2)
	win_copy_by_rga(dst_win, src_win, rotate, iommu_en);
#else
	return -1;
#endif
	return 0;
}

#endif

static int rk_fb_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct fb_fix_screeninfo *fix = &info->fix;
	int win_id = 0;
	struct rk_lcdc_win *win = NULL;
	struct rk_screen *screen = dev_drv->cur_screen;
	u32 xoffset = var->xoffset;
	u32 yoffset = var->yoffset;
	u32 xvir = var->xres_virtual;
	u8 pixel_width;
	u32 vir_width_bit;
	u32 stride, uv_stride;
	u32 stride_32bit_1;
	u32 stride_32bit_2;
	u16 uv_x_off, uv_y_off, uv_y_act;
	u8 is_pic_yuv = 0;

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	if (win_id < 0)
		return -ENODEV;
	else
		win = dev_drv->win[win_id];

	pixel_width = rk_fb_pixel_width(win->area[0].format);
	vir_width_bit = pixel_width * xvir;
	stride_32bit_1 = ALIGN_N_TIMES(vir_width_bit, 32) / 8;
	stride_32bit_2 = ALIGN_N_TIMES(vir_width_bit * 2, 32) / 8;

	switch (win->area[0].format) {
	case YUV422:
	case YUV422_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1 >> 1;
		uv_x_off = xoffset >> 1;
		uv_y_off = yoffset;
		fix->line_length = stride;
		uv_y_act = win->area[0].yact >> 1;
		break;
	case YUV420:		/* 420sp */
	case YUV420_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1;
		uv_x_off = xoffset;
		uv_y_off = yoffset >> 1;
		fix->line_length = stride;
		uv_y_act = win->area[0].yact >> 1;
		break;
	case YUV444:
	case YUV444_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_2;
		uv_x_off = xoffset * 2;
		uv_y_off = yoffset;
		fix->line_length = stride << 2;
		uv_y_act = win->area[0].yact;
		break;
	default:
		stride = stride_32bit_1;	/* default rgb */
		fix->line_length = stride;
		break;
	}

	/* x y mirror ,jump line */
	if (screen->y_mirror == 1) {
		if (screen->interlace == 1) {
			win->area[0].y_offset = yoffset * stride * 2 +
			    ((win->area[0].yact - 1) * 2 + 1) * stride +
			    xoffset * pixel_width / 8;
		} else {
			win->area[0].y_offset = yoffset * stride +
			    (win->area[0].yact - 1) * stride +
			    xoffset * pixel_width / 8;
		}
	} else {
		if (screen->interlace == 1) {
			win->area[0].y_offset =
			    yoffset * stride * 2 + xoffset * pixel_width / 8;
		} else {
			win->area[0].y_offset =
			    yoffset * stride + xoffset * pixel_width / 8;
		}
	}
	if (is_pic_yuv == 1) {
		if (screen->y_mirror == 1) {
			if (screen->interlace == 1) {
				win->area[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    ((uv_y_act - 1) * 2 + 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			} else {
				win->area[0].c_offset = uv_y_off * uv_stride +
				    (uv_y_act - 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		} else {
			if (screen->interlace == 1) {
				win->area[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    uv_x_off * pixel_width / 8;
			} else {
				win->area[0].c_offset =
				    uv_y_off * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		}
	}

	win->area[0].smem_start = fix->smem_start;
	win->area[0].cbr_start = fix->mmio_start;
	win->area[0].state = 1;
	win->area_num = 1;

	dev_drv->ops->pan_display(dev_drv, win_id);

#ifdef	CONFIG_FB_MIRRORING
	if (video_data_to_mirroring)
		video_data_to_mirroring(info, NULL);
#endif
	dev_drv->ops->cfg_done(dev_drv);

	return 0;
}

static int rk_fb_get_list_stat(struct rk_lcdc_driver *dev_drv)
{
	int i, j;

	i = list_empty(&dev_drv->update_regs_list);
	j = list_empty(&dev_drv->saved_list);
	return i == j ? 0 : 1;
}

void rk_fd_fence_wait(struct rk_lcdc_driver *dev_drv, struct sync_fence *fence)
{
	int err = sync_fence_wait(fence, 1000);

	if (err >= 0)
		return;

	if (err == -ETIME)
		err = sync_fence_wait(fence, 10 * MSEC_PER_SEC);

	if (err < 0)
		printk("error waiting on fence\n");
}
#if 0
static int rk_fb_copy_from_loader(struct fb_info *info)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	void *dst = info->screen_base;
	u32 dsp_addr[4];
	u32 src;
	u32 i,size;
	int win_id;
	struct rk_lcdc_win *win;
	
	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	win = dev_drv->win[win_id];
	size = (win->area[0].xact) * (win->area[0].yact) << 2;
	dev_drv->ops->get_dsp_addr(dev_drv, dsp_addr);
	src = dsp_addr[win_id];
	dev_info(info->dev, "copy fb data %d x %d  from  dst_addr:%08x\n",
		 win->area[0].xact, win->area[0].yact, src);
	for (i = 0; i < size; i += PAGE_SIZE) {
		void *page = phys_to_page(i + src);
		void *from_virt = kmap(page);
		void *to_virt = dst + i;
		memcpy(to_virt, from_virt, PAGE_SIZE);
	}
	dev_drv->ops->direct_set_addr(dev_drv, win_id,
				      info->fix.smem_start);
	return 0;
}
#endif
#ifdef CONFIG_ROCKCHIP_IOMMU
static int g_last_addr[4];
int g_last_timeout;
u32 freed_addr[10];
u32 freed_index;

#define DUMP_CHUNK 256
char buf[PAGE_SIZE];

int rk_fb_sysmmu_fault_handler(struct device *dev,
			       enum rk_iommu_inttype itype,
			       unsigned long pgtable_base,
			       unsigned long fault_addr, unsigned int status)
{
	struct rk_lcdc_driver *dev_drv = rk_get_prmry_lcdc_drv();
	int i = 0;
	static int page_fault_cnt;
	if ((page_fault_cnt++) >= 10)
		return 0;
	pr_err
	    ("PAGE FAULT occurred at 0x%lx (Page table base: 0x%lx),status=%d\n",
	     fault_addr, pgtable_base, status);
	printk("last config addr:\n" "win0:0x%08x\n" "win1:0x%08x\n"
	       "win2:0x%08x\n" "win3:0x%08x\n", g_last_addr[0], g_last_addr[1],
	       g_last_addr[2], g_last_addr[3]);
	printk("last freed buffer:\n");
	for (i = 0; (freed_addr[i] != 0xfefefefe) && freed_addr[i]; i++)
		printk("%d:0x%08x\n", i, freed_addr[i]);
	printk("last timeout:%d\n", g_last_timeout);
	dev_drv->ops->get_disp_info(dev_drv, buf, 0);
	for (i = 0; i < PAGE_SIZE; i += DUMP_CHUNK) {
		if ((PAGE_SIZE - i) > DUMP_CHUNK) {
			char c = buf[i + DUMP_CHUNK];
			buf[i + DUMP_CHUNK] = 0;
			pr_cont("%s", buf + i);
			buf[i + DUMP_CHUNK] = c;
		} else {
			buf[PAGE_SIZE - 1] = 0;
			pr_cont("%s", buf + i);
		}
	}

	return 0;
}
#endif

void rk_fb_free_dma_buf(struct rk_lcdc_driver *dev_drv,
			struct rk_fb_reg_win_data *reg_win_data)
{
	int i, index_buf;
	struct rk_fb_reg_area_data *area_data;
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);

	for (i = 0; i < reg_win_data->area_num; i++) {
		area_data = &reg_win_data->reg_area_data[i];
		index_buf = area_data->index_buf;
#if defined(CONFIG_ROCKCHIP_IOMMU)
		if (dev_drv->iommu_enabled) {
			if (rk_fb->disp_policy != DISPLAY_POLICY_BOX)
				ion_unmap_iommu(dev_drv->dev, rk_fb->ion_client,
						area_data->ion_handle);
			freed_addr[freed_index++] = area_data->smem_start;
		}
#endif
		if (area_data->ion_handle != NULL)
			ion_free(rk_fb->ion_client, area_data->ion_handle);

		if (area_data->acq_fence)
			sync_fence_put(area_data->acq_fence);
	}
	memset(reg_win_data, 0, sizeof(struct rk_fb_reg_win_data));
}

static void rk_fb_update_win(struct rk_lcdc_driver *dev_drv,
                                struct rk_lcdc_win *win,
				struct rk_fb_reg_win_data *reg_win_data)
{
	int i = 0;
        struct rk_fb *inf = platform_get_drvdata(fb_pdev);
        struct rk_screen *cur_screen;
        struct rk_screen primary_screen;

        if (unlikely(!inf) || unlikely(!dev_drv) ||
            unlikely(!win) || unlikely(!reg_win_data))
                return;

        cur_screen = dev_drv->cur_screen;
        rk_fb_get_prmry_screen(&primary_screen);

	win->area_num = reg_win_data->area_num;
	win->id = reg_win_data->win_id;
	win->z_order = reg_win_data->z_order;

	if (reg_win_data->reg_area_data[0].smem_start > 0) {
		win->state = 1;
		win->area_num = reg_win_data->area_num;
		win->id = reg_win_data->win_id;
		win->z_order = reg_win_data->z_order;
		win->area[0].uv_vir_stride =
		    reg_win_data->reg_area_data[0].uv_vir_stride;
		win->area[0].cbr_start =
		    reg_win_data->reg_area_data[0].cbr_start;
		win->area[0].c_offset = reg_win_data->reg_area_data[0].c_offset;
		win->alpha_en = reg_win_data->alpha_en;
		win->alpha_mode = reg_win_data->alpha_mode;
		win->g_alpha_val = reg_win_data->g_alpha_val;
		win->mirror_en = reg_win_data->mirror_en;
		win->area[0].fbdc_en =
			reg_win_data->reg_area_data[0].fbdc_en;
		win->area[0].fbdc_cor_en =
			reg_win_data->reg_area_data[0].fbdc_cor_en;
		win->area[0].fbdc_data_format =
			reg_win_data->reg_area_data[0].fbdc_data_format;
		for (i = 0; i < RK_WIN_MAX_AREA; i++) {
			if (reg_win_data->reg_area_data[i].smem_start > 0) {
				win->area[i].format =
					reg_win_data->reg_area_data[i].data_format;
				if (inf->disp_policy != DISPLAY_POLICY_BOX)
					win->area[i].ion_hdl =
					reg_win_data->reg_area_data[i].ion_handle;
				win->area[i].smem_start =
					reg_win_data->reg_area_data[i].smem_start;
                                if (inf->disp_mode == DUAL ||
                                    inf->disp_policy == DISPLAY_POLICY_BOX) {
				        win->area[i].xpos =
				                reg_win_data->reg_area_data[i].xpos;
				        win->area[i].ypos =
				                reg_win_data->reg_area_data[i].ypos;
				        win->area[i].xsize =
				                reg_win_data->reg_area_data[i].xsize;
				        win->area[i].ysize =
				                reg_win_data->reg_area_data[i].ysize;
                                } else {
                                        win->area[i].xpos =
                                                reg_win_data->reg_area_data[i].xpos *
                                                cur_screen->mode.xres /
                                                primary_screen.mode.xres;
	                                win->area[i].ypos =
                                                reg_win_data->reg_area_data[i].ypos *
                                                cur_screen->mode.yres /
                                                primary_screen.mode.yres;
	                                win->area[i].xsize =
                                                reg_win_data->reg_area_data[i].xsize *
                                                cur_screen->mode.xres /
                                                primary_screen.mode.xres;
	                                win->area[i].ysize =
                                                reg_win_data->reg_area_data[i].ysize *
                                                cur_screen->mode.yres /
                                                primary_screen.mode.yres;

					/* recalc display size if set hdmi scaler when at ONE_DUAL mode */
					if (inf->disp_mode == ONE_DUAL && hdmi_switch_complete) {
						if (cur_screen->xsize > 0 &&
						    cur_screen->xsize <= cur_screen->mode.xres) {
							win->area[i].xpos =
								((cur_screen->mode.xres - cur_screen->xsize) >> 1) +
								cur_screen->xsize * win->area[i].xpos / cur_screen->mode.xres;
							win->area[i].xsize =
								win->area[i].xsize * cur_screen->xsize / cur_screen->mode.xres;
						}
						if (cur_screen->ysize > 0 && cur_screen->ysize <= cur_screen->mode.yres) {
							win->area[i].ypos =
								((cur_screen->mode.yres - cur_screen->ysize) >> 1) +
								cur_screen->ysize * win->area[i].ypos / cur_screen->mode.yres;
							win->area[i].ysize =
								win->area[i].ysize * cur_screen->ysize / cur_screen->mode.yres;
						}
					}
                                }
				win->area[i].xact =
				    reg_win_data->reg_area_data[i].xact;
				win->area[i].yact =
				    reg_win_data->reg_area_data[i].yact;
				win->area[i].xvir =
				    reg_win_data->reg_area_data[i].xvir;
				win->area[i].yvir =
				    reg_win_data->reg_area_data[i].yvir;
				win->area[i].xoff =
				    reg_win_data->reg_area_data[i].xoff;
				win->area[i].yoff =
				    reg_win_data->reg_area_data[i].yoff;
				win->area[i].y_offset =
				    reg_win_data->reg_area_data[i].y_offset;
				win->area[i].y_vir_stride =
				    reg_win_data->reg_area_data[i].y_vir_stride;
				win->area[i].state = 1;
			} else {
				win->area[i].state = 0;
			}
		}
	} else {
	/*
		win->state = 0;
		win->z_order = -1;
	*/
	}
}

static struct rk_fb_reg_win_data *rk_fb_get_win_data(struct rk_fb_reg_data
						     *regs, int win_id)
{
	int i;
	struct rk_fb_reg_win_data *win_data = NULL;
	for (i = 0; i < regs->win_num; i++) {
		if (regs->reg_win_data[i].win_id == win_id) {
			win_data = &(regs->reg_win_data[i]);
			break;
		}
	}

	return win_data;
}

static void rk_fb_update_reg(struct rk_lcdc_driver *dev_drv,
			     struct rk_fb_reg_data *regs)
{
	int i, j;
	struct rk_lcdc_win *win;
	ktime_t timestamp = dev_drv->vsync_info.timestamp;
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct rk_fb_reg_win_data *win_data;
	bool wait_for_vsync;
	int count = 100;
	unsigned int dsp_addr[4];
	long timeout;

	/* acq_fence wait */
	for (i = 0; i < regs->win_num; i++) {
		win_data = &regs->reg_win_data[i];
		for (j = 0; j < RK_WIN_MAX_AREA; j++) {
			if (win_data->reg_area_data[j].acq_fence) {
				/* printk("acq_fence wait!!!!!\n"); */
				rk_fd_fence_wait(dev_drv, win_data->reg_area_data[j].acq_fence);
			}
		}
	}

	for (i = 0; i < dev_drv->lcdc_win_num; i++) {
		win = dev_drv->win[i];
		win_data = rk_fb_get_win_data(regs, i);
		if (win_data) {
			if (rk_fb->disp_policy == DISPLAY_POLICY_BOX &&
			    (win_data->reg_area_data[0].data_format == YUV420 ||
			     win_data->reg_area_data[0].data_format == YUV420_A))
				continue;
			mutex_lock(&dev_drv->win_config);
			rk_fb_update_win(dev_drv, win, win_data);
			win->state = 1;
			dev_drv->ops->set_par(dev_drv, i);
			dev_drv->ops->pan_display(dev_drv, i);
			mutex_unlock(&dev_drv->win_config);
#if defined(CONFIG_ROCKCHIP_IOMMU)
			if (dev_drv->iommu_enabled) {
				g_last_addr[i] = win_data->reg_area_data[0].smem_start +
					win_data->reg_area_data[0].y_offset;
			}
#endif
		} else {
			win->z_order = -1;
			win->state = 0;
		}
	}
	dev_drv->ops->ovl_mgr(dev_drv, 0, 1);
	dev_drv->ops->cfg_done(dev_drv);

	do {
		timestamp = dev_drv->vsync_info.timestamp;
		timeout = wait_event_interruptible_timeout(dev_drv->vsync_info.wait,
				ktime_compare(dev_drv->vsync_info.timestamp, timestamp) > 0,
				msecs_to_jiffies(25));

		dev_drv->ops->get_dsp_addr(dev_drv, dsp_addr);
		wait_for_vsync = false;
		for (i = 0; i < dev_drv->lcdc_win_num; i++) {
			if (dev_drv->win[i]->state == 1) {
				if (rk_fb->disp_policy == DISPLAY_POLICY_BOX &&
				    (!strcmp(dev_drv->win[i]->name, "hwc"))) {
					continue;
				} else {
					u32 new_start =
					    dev_drv->win[i]->area[0].smem_start +
					    dev_drv->win[i]->area[0].y_offset;
					u32 reg_start = dsp_addr[i];

					if ((rk_fb->disp_policy ==
					     DISPLAY_POLICY_BOX) &&
					    (dev_drv->suspend_flag))
						continue;
					if (unlikely(new_start != reg_start)) {
						wait_for_vsync = true;
						dev_info(dev_drv->dev,
						       "win%d:new_addr:0x%08x cur_addr:0x%08x--%d\n",
						       i, new_start, reg_start, 101 - count);
						break;
					}
				}
			}
		}
	} while (wait_for_vsync && count--);
#ifdef H_USE_FENCE
	sw_sync_timeline_inc(dev_drv->timeline, 1);
#endif
	if (dev_drv->front_regs) {
#if defined(CONFIG_ROCKCHIP_IOMMU)
		if (dev_drv->iommu_enabled) {
			if (dev_drv->ops->mmu_en)
				dev_drv->ops->mmu_en(dev_drv);
			freed_index = 0;
			g_last_timeout = timeout;
		}
#endif

		mutex_lock(&dev_drv->front_lock);

		for (i = 0; i < dev_drv->front_regs->win_num; i++) {
			win_data = &dev_drv->front_regs->reg_win_data[i];
			rk_fb_free_dma_buf(dev_drv, win_data);
		}
		kfree(dev_drv->front_regs);

		mutex_unlock(&dev_drv->front_lock);

#if defined(CONFIG_ROCKCHIP_IOMMU)
		if (dev_drv->iommu_enabled)
			freed_addr[freed_index] = 0xfefefefe;
#endif
	}

	mutex_lock(&dev_drv->front_lock);

	dev_drv->front_regs = regs;

	mutex_unlock(&dev_drv->front_lock);
}

static void rk_fb_update_regs_handler(struct kthread_work *work)
{
	struct rk_lcdc_driver *dev_drv =
	    container_of(work, struct rk_lcdc_driver, update_regs_work);
	struct rk_fb_reg_data *data, *next;

	mutex_lock(&dev_drv->update_regs_list_lock);
	dev_drv->saved_list = dev_drv->update_regs_list;
	list_replace_init(&dev_drv->update_regs_list, &dev_drv->saved_list);
	mutex_unlock(&dev_drv->update_regs_list_lock);

	list_for_each_entry_safe(data, next, &dev_drv->saved_list, list) {
		rk_fb_update_reg(dev_drv, data);
		list_del(&data->list);
	}

	if (dev_drv->wait_fs && list_empty(&dev_drv->update_regs_list))
		wake_up(&dev_drv->update_regs_wait);
}

static int rk_fb_check_config_var(struct rk_fb_area_par *area_par,
				  struct rk_screen *screen)
{
	if ((area_par->x_offset + area_par->xact > area_par->xvir) ||
	    (area_par->xact <= 0) || (area_par->yact <= 0) ||
	    (area_par->xvir <= 0) || (area_par->yvir <= 0)) {
		pr_err("check config var fail 0:\n"
		       "x_offset=%d,xact=%d,xvir=%d\n",
		       area_par->x_offset, area_par->xact, area_par->xvir);
		return -EINVAL;
	}

	if ((area_par->xpos + area_par->xsize > screen->mode.xres) ||
	    (area_par->ypos + area_par->ysize > screen->mode.yres) ||
	    (area_par->xsize <= 0) || (area_par->ysize <= 0)) {
		pr_warn("check config var fail 1:\n"
		       "xpos=%d,xsize=%d,xres=%d\n"
		       "ypos=%d,ysize=%d,yres=%d\n",
		       area_par->xpos, area_par->xsize, screen->mode.xres,
		       area_par->ypos, area_par->ysize, screen->mode.yres);
		return -EINVAL;
	}
	return 0;
}

static int rk_fb_set_win_buffer(struct fb_info *info,
				struct rk_fb_win_par *win_par,
				struct rk_fb_reg_win_data *reg_win_data)
{
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
	struct fb_fix_screeninfo *fix = &info->fix;
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_screen *screen = dev_drv->cur_screen;
	struct fb_info *fbi;
	int i, ion_fd, acq_fence_fd;
	u32 xvir, yvir;
	u32 xoffset, yoffset;

	struct ion_handle *hdl;
	size_t len;
	int index_buf;
	u8 fb_data_fmt;
	u8 pixel_width;
	u32 vir_width_bit;
	u32 stride, uv_stride;
	u32 stride_32bit_1;
	u32 stride_32bit_2;
	u16 uv_x_off, uv_y_off, uv_y_act;
	u8 is_pic_yuv = 0;
	u8 ppixel_a = 0, global_a = 0;
	ion_phys_addr_t phy_addr;
	int ret = 0;

	reg_win_data->reg_area_data[0].smem_start = -1;
	reg_win_data->area_num = 0;
	fbi = rk_fb->fb[reg_win_data->win_id];
	if (win_par->area_par[0].phy_addr == 0) {
		for (i = 0; i < RK_WIN_MAX_AREA; i++) {
			ion_fd = win_par->area_par[i].ion_fd;
			if (ion_fd > 0) {
				hdl =
				    ion_import_dma_buf(rk_fb->ion_client,
						       ion_fd);
				if (IS_ERR(hdl)) {
					pr_info("%s: Could not import handle:"
						" %ld\n", __func__, (long)hdl);
					/*return -EINVAL; */
					break;
				}
				reg_win_data->area_num++;
				reg_win_data->reg_area_data[i].ion_handle = hdl;
#ifndef CONFIG_ROCKCHIP_IOMMU
				ret = ion_phys(rk_fb->ion_client, hdl, &phy_addr,
						&len);
#else
				if (dev_drv->iommu_enabled)
					ret = ion_map_iommu(dev_drv->dev,
								rk_fb->ion_client,
								hdl,
								(unsigned long *)&phy_addr,
								(unsigned long *)&len);
				else
					ret = ion_phys(rk_fb->ion_client, hdl,
							&phy_addr, &len);
#endif
				if (ret < 0) {
					dev_err(fbi->dev, "ion map to get phy addr failed\n");
					ion_free(rk_fb->ion_client, hdl);
					return -ENOMEM;
				}
				reg_win_data->reg_area_data[i].smem_start = phy_addr;
				reg_win_data->area_buf_num++;
				reg_win_data->reg_area_data[i].index_buf = 1;
			}
		}
	} else {
		reg_win_data->reg_area_data[0].smem_start =
		    win_par->area_par[0].phy_addr;
		reg_win_data->area_num = 1;
		fbi->screen_base = phys_to_virt(win_par->area_par[0].phy_addr);
	}

	if (reg_win_data->area_num == 0)
		return 0;

	for (i = 0; i < reg_win_data->area_num; i++) {
		acq_fence_fd = win_par->area_par[i].acq_fence_fd;
		index_buf = reg_win_data->reg_area_data[i].index_buf;
		if ((acq_fence_fd > 0) && (index_buf == 1)) {
			reg_win_data->reg_area_data[i].acq_fence =
			    sync_fence_fdget(win_par->area_par[i].acq_fence_fd);
		}
	}
	if (reg_win_data->reg_area_data[0].smem_start > 0) {
		reg_win_data->z_order = win_par->z_order;
		reg_win_data->win_id = win_par->win_id;
	} else {
		reg_win_data->z_order = -1;
		reg_win_data->win_id = -1;
	}

	reg_win_data->mirror_en = win_par->mirror_en;
	reg_win_data->reg_area_data[0].fbdc_en = win_par->area_par[0].fbdc_en;
	reg_win_data->reg_area_data[0].fbdc_cor_en =
		win_par->area_par[0].fbdc_cor_en;
	reg_win_data->reg_area_data[0].fbdc_data_format =
		win_par->area_par[0].fbdc_data_format;
	for (i = 0; i < reg_win_data->area_num; i++) {
		rk_fb_check_config_var(&win_par->area_par[i], screen);

		fb_data_fmt = rk_fb_data_fmt(win_par->area_par[i].data_format, 0);
		reg_win_data->reg_area_data[i].data_format = fb_data_fmt;
		pixel_width = rk_fb_pixel_width(fb_data_fmt);

		ppixel_a |= ((fb_data_fmt == ARGB888) ||
			     (fb_data_fmt == ABGR888)) ? 1 : 0;
		/* visiable pos in panel */
		reg_win_data->reg_area_data[i].xpos = win_par->area_par[i].xpos;
		reg_win_data->reg_area_data[i].ypos = win_par->area_par[i].ypos;

		/* realy size in panel */
		reg_win_data->reg_area_data[i].xsize = win_par->area_par[i].xsize;
		reg_win_data->reg_area_data[i].ysize = win_par->area_par[i].ysize;

		/* realy size in panel */
		reg_win_data->reg_area_data[i].xact = win_par->area_par[i].xact;
		reg_win_data->reg_area_data[i].yact = win_par->area_par[i].yact;

		xoffset = win_par->area_par[i].x_offset;	/* buf offset */
		yoffset = win_par->area_par[i].y_offset;
		reg_win_data->reg_area_data[i].xoff = xoffset;
		reg_win_data->reg_area_data[i].yoff = yoffset;

		xvir = win_par->area_par[i].xvir;
		reg_win_data->reg_area_data[i].xvir = xvir;
		yvir = win_par->area_par[i].yvir;
		reg_win_data->reg_area_data[i].yvir = yvir;

		vir_width_bit = pixel_width * xvir;
		/* pixel_width = byte_num*8 */
		stride_32bit_1 = ((vir_width_bit + 31) & (~31)) / 8;
		stride_32bit_2 = ((vir_width_bit * 2 + 31) & (~31)) / 8;

		stride = stride_32bit_1;	/* default rgb */
		fix->line_length = stride;
		reg_win_data->reg_area_data[i].y_vir_stride = stride >> 2;

		/* x y mirror ,jump line
		 * reg_win_data->reg_area_data[i].y_offset =
		 *		yoffset*stride+xoffset*pixel_width/8;
		 */
		if ((screen->y_mirror == 1) || (reg_win_data->mirror_en)) {
			if (screen->interlace == 1) {
				reg_win_data->reg_area_data[i].y_offset =
				    yoffset * stride * 2 +
				    ((reg_win_data->reg_area_data[i].yact - 1) * 2 + 1) * stride +
				    xoffset * pixel_width / 8;
			} else {
				reg_win_data->reg_area_data[i].y_offset =
				    yoffset * stride +
				    (reg_win_data->reg_area_data[i].yact - 1) * stride +
				    xoffset * pixel_width / 8;
			}
		} else {
			if (screen->interlace == 1) {
				reg_win_data->reg_area_data[i].y_offset =
				    yoffset * stride * 2 +
				    xoffset * pixel_width / 8;
			} else {
				reg_win_data->reg_area_data[i].y_offset =
				    yoffset * stride +
				    xoffset * pixel_width / 8;
			}
		}
	}

	global_a = (win_par->g_alpha_val == 0) ? 0 : 1;
	reg_win_data->alpha_en = ppixel_a | global_a;
	reg_win_data->g_alpha_val = win_par->g_alpha_val;
	reg_win_data->alpha_mode = win_par->alpha_mode;

	switch (fb_data_fmt) {
	case YUV422:
	case YUV422_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1 >> 1;
		uv_x_off = xoffset >> 1;
		uv_y_off = yoffset;
		fix->line_length = stride;
		uv_y_act = win_par->area_par[0].yact >> 1;
		break;
	case YUV420:		/* 420sp */
	case YUV420_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1;
		uv_x_off = xoffset;
		uv_y_off = yoffset >> 1;
		fix->line_length = stride;
		uv_y_act = win_par->area_par[0].yact >> 1;
		break;
	case YUV444:
	case YUV444_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_2;
		uv_x_off = xoffset * 2;
		uv_y_off = yoffset;
		fix->line_length = stride << 2;
		uv_y_act = win_par->area_par[0].yact;
		break;
	default:
		break;
	}
	if (is_pic_yuv == 1) {
		reg_win_data->reg_area_data[0].cbr_start =
		    reg_win_data->reg_area_data[0].smem_start + xvir * yvir;
		reg_win_data->reg_area_data[0].uv_vir_stride = uv_stride >> 2;
		if ((screen->y_mirror == 1) || (reg_win_data->mirror_en)) {
			if (screen->interlace == 1) {
				reg_win_data->reg_area_data[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    ((uv_y_act - 1) * 2 + 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			} else {
				reg_win_data->reg_area_data[0].c_offset =
				    uv_y_off * uv_stride +
				    (uv_y_act - 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		} else {
			if (screen->interlace == 1) {
				reg_win_data->reg_area_data[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    uv_x_off * pixel_width / 8;
			} else {
				reg_win_data->reg_area_data[0].c_offset =
				    uv_y_off * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		}
	}

	/* record buffer information for rk_fb_disp_scale to prevent fence timeout
	 * because rk_fb_disp_scale will call function info->fbops->fb_set_par(info);
	 */
	info->var.yoffset = yoffset;
	info->var.xoffset = xoffset;
	return 0;
}

static int rk_fb_set_win_config(struct fb_info *info,
				struct rk_fb_win_cfg_data *win_data)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_fb_reg_data *regs;
#ifdef H_USE_FENCE
	struct sync_fence *release_fence[RK_MAX_BUF_NUM];
	struct sync_fence *retire_fence;
	struct sync_pt *release_sync_pt[RK_MAX_BUF_NUM];
	struct sync_pt *retire_sync_pt;
	char fence_name[20];
#endif
	int ret = 0, i, j = 0;
	int list_is_empty = 0;

	regs = kzalloc(sizeof(struct rk_fb_reg_data), GFP_KERNEL);
	if (!regs) {
		printk(KERN_INFO "could not allocate rk_fb_reg_data\n");
		ret = -ENOMEM;
		return ret;
	}

/*
	regs->post_cfg.xpos = win_data->post_cfg.xpos;
	regs->post_cfg.ypos = win_data->post_cfg.ypos;
	regs->post_cfg.xsize = win_data->post_cfg.xsize;
	regs->post_cfg.ysize = win_data->post_cfg.xsize;
*/

	for (i = 0; i < dev_drv->lcdc_win_num; i++) {
		if (win_data->win_par[i].win_id < dev_drv->lcdc_win_num) {
			if (rk_fb_set_win_buffer(info, &win_data->win_par[i],
							&regs->reg_win_data[j]))
				return -ENOMEM;
			if (regs->reg_win_data[j].area_num > 0) {
				regs->win_num++;
				regs->buf_num +=
				    regs->reg_win_data[j].area_buf_num;
			}
			j++;
		} else {
			printk(KERN_INFO "error:win_id bigger than lcdc_win_num\n");
			printk(KERN_INFO "i=%d,win_id=%d\n", i,
			       win_data->win_par[i].win_id);
		}
	}

	mutex_lock(&dev_drv->output_lock);
	if (!(dev_drv->suspend_flag == 0)) {
		rk_fb_update_reg(dev_drv, regs);
		printk(KERN_INFO "suspend_flag = 1\n");
		goto err;
	}

	dev_drv->timeline_max++;
#ifdef H_USE_FENCE
	for (i = 0; i < RK_MAX_BUF_NUM; i++) {
		if (i < regs->buf_num) {
			sprintf(fence_name, "fence%d", i);
			win_data->rel_fence_fd[i] = get_unused_fd();
			if (win_data->rel_fence_fd[i] < 0) {
				printk(KERN_INFO "rel_fence_fd=%d\n",
				       win_data->rel_fence_fd[i]);
				ret = -EFAULT;
				goto err;
			}
			release_sync_pt[i] =
			    sw_sync_pt_create(dev_drv->timeline,
					      dev_drv->timeline_max);
			release_fence[i] =
			    sync_fence_create(fence_name, release_sync_pt[i]);
			sync_fence_install(release_fence[i],
					   win_data->rel_fence_fd[i]);
		} else {
			win_data->rel_fence_fd[i] = -1;
		}
	}

	win_data->ret_fence_fd = get_unused_fd();
	if (win_data->ret_fence_fd < 0) {
		printk("ret_fence_fd=%d\n", win_data->ret_fence_fd);
		ret = -EFAULT;
		goto err;
	}
	retire_sync_pt =
	    sw_sync_pt_create(dev_drv->timeline, dev_drv->timeline_max);
	retire_fence = sync_fence_create("ret_fence", retire_sync_pt);
	sync_fence_install(retire_fence, win_data->ret_fence_fd);
#else
	for (i = 0; i < RK_MAX_BUF_NUM; i++)
		win_data->rel_fence_fd[i] = -1;

	win_data->ret_fence_fd = -1;
#endif
	if (dev_drv->wait_fs == 0) {
		mutex_lock(&dev_drv->update_regs_list_lock);
		list_add_tail(&regs->list, &dev_drv->update_regs_list);
		mutex_unlock(&dev_drv->update_regs_list_lock);
		queue_kthread_work(&dev_drv->update_regs_worker,
				   &dev_drv->update_regs_work);
	} else {
		mutex_lock(&dev_drv->update_regs_list_lock);
		list_is_empty = list_empty(&dev_drv->update_regs_list) &&
					list_empty(&dev_drv->saved_list);
		mutex_unlock(&dev_drv->update_regs_list_lock);
		if (!list_is_empty) {
			ret = wait_event_timeout(dev_drv->update_regs_wait,
				list_empty(&dev_drv->update_regs_list) && list_empty(&dev_drv->saved_list),
				msecs_to_jiffies(60));
			if (ret > 0)
				rk_fb_update_reg(dev_drv, regs);
			else
				printk("%s: wait update_regs_wait timeout\n", __func__);
		} else if (ret == 0) {
			rk_fb_update_reg(dev_drv, regs);
		}
	}

err:
	mutex_unlock(&dev_drv->output_lock);
	return ret;
}

#if 1
static int cfgdone_distlist[10] = { 0 };

static int cfgdone_index;
static int cfgdone_lasttime;

int rk_get_real_fps(int before)
{
	struct timespec now;
	int dist_curr;
	int dist_total = 0;
	int dist_count = 0;
	int dist_first = 0;

	int index = cfgdone_index;
	int i = 0, fps = 0;
	int total;

	if (before > 100)
		before = 100;
	if (before < 0)
		before = 0;

	getnstimeofday(&now);
	dist_curr = (now.tv_sec * 1000000 + now.tv_nsec / 1000) -
			cfgdone_lasttime;
	total = dist_curr;
	/*
	   printk("fps: ");
	 */
	for (i = 0; i < 10; i++) {
		if (--index < 0)
			index = 9;
		total += cfgdone_distlist[index];
		if (i == 0)
			dist_first = cfgdone_distlist[index];
		if (total < (before * 1000)) {
			/*
			   printk("[%d:%d] ", dist_count, cfgdone_distlist[index]);
			 */
			dist_total += cfgdone_distlist[index];
			dist_count++;
		} else {
			break;
		}
	}

	/*
	   printk("total %d, count %d, curr %d, ", dist_total, dist_count, dist_curr);
	 */
	dist_curr = (dist_curr > dist_first) ? dist_curr : dist_first;
	dist_total += dist_curr;
	dist_count++;

	if (dist_total > 0)
		fps = (1000000 * dist_count) / dist_total;
	else
		fps = 60;

	/*
	   printk("curr2 %d, fps=%d\n", dist_curr, fps);
	 */
	return fps;
}
EXPORT_SYMBOL(rk_get_real_fps);

#endif
#ifdef CONFIG_ROCKCHIP_IOMMU
#define ION_MAX 10
static struct ion_handle *ion_hanle[ION_MAX];
static struct ion_handle *ion_hwc[1];
#endif

#if 1  /* 2015/06 LCD_CHANGE */
static int buffer_create(struct fb_info *info, struct ion_table* buf, size_t len)
{
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	int ret;
	if (dev_drv->iommu_enabled)
		buf->hdl = ion_alloc(rk_fb->ion_client, len, 0,
						   ION_HEAP(ION_VMALLOC_HEAP_ID), ION_FLAG_STRAIGH_BUF/* ION_FLAG_CACHED */);
	else
		buf->hdl = ion_alloc(rk_fb->ion_client, len, 0,
						   ION_HEAP(ION_CMA_HEAP_ID), 0);
	if (IS_ERR(buf->hdl)) {
		dev_err(info->dev, "failed to ion_alloc:%ld rk_fb->ion_client\n",
				PTR_ERR(buf->hdl));
		goto end;
	}

	if (dev_drv->iommu_enabled && dev_drv->mmu_dev)
		ret = ion_map_iommu(dev_drv->dev, rk_fb->ion_client, buf->hdl,
							(unsigned long *)&buf->phy_addr,
							(unsigned long *)&buf->len);
	else
		ret = ion_phys(rk_fb->ion_client, buf->hdl, &buf->phy_addr, &buf->len);

	/* fb get */
	buf->fd = ion_share_dma_buf_fd(rk_fb->ion_client, buf->hdl);
	if (buf->fd < 0) {
		dev_err(info->dev, "ion_share_dma_buf_fd failed\n");
		goto err;
	}
	return 0;
err:
	ion_free(rk_fb->ion_client, buf->hdl);
end:
	return -ENOMEM;
}
static int buffer_delete(struct fb_info *info, struct ion_table* buf)
{
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	ion_unmap_iommu(dev_drv->dev, rk_fb->ion_client, buf->hdl);
	/* free buffer */
	ion_free(rk_fb->ion_client, buf->hdl);
	/* clear table */
	memset(buf, 0, sizeof(struct ion_table));
	return 0;
}

static struct ion_table * find_ion_table(int fd, int start, int end)
{
	int i;
	struct ion_table *p = &lcd_buffer[start];
	for(i = start; i < end; i++, p++){
		if(p->fd == fd){
			return p;
		}
	}
	return NULL;
}

static int bitblt_func(struct rk_lcdc_driver *dev_drv, BITBLT_LIST val)
{
	struct rga_req req;
	int pixel;

	if(val.mode == TYPE_RGB565)
		pixel = RK_FORMAT_RGB_565;
	else if(val.mode == TYPE_8BPP)
		pixel = RK_FORMAT_BPP8;
	else
		return -1;

	memset(&req, 0, sizeof(struct rga_req));

    req.src.act_w    = val.width;
    req.src.act_h    = val.height;
    req.src.vir_w    = val.src_w;
    req.src.vir_h    = val.src_h;
    req.src.x_offset = val.src_x;
    req.src.y_offset = val.src_y;
    req.src.yrgb_addr = 0;
	if(dev_drv->ops->iova_to_phy){
		req.src.uv_addr = dev_drv->ops->iova_to_phy(dev_drv, (void*)(val.src_addr));
		req.src.uv_addr += val.src_addr_offset;
	}
    req.src.v_addr = 0;
    req.src.format = pixel;

    req.dst.act_w    = val.width;
    req.dst.act_h    = val.height;
    req.dst.vir_w    = val.dst_w;
    req.dst.vir_h    = val.dst_h;
    req.dst.x_offset = val.dst_x;
    req.dst.y_offset = val.dst_y;
    req.dst.yrgb_addr = 0;
	if(dev_drv->ops->iova_to_phy){
		req.dst.uv_addr = dev_drv->ops->iova_to_phy(dev_drv, (void*)(val.dst_addr));
		req.dst.uv_addr += val.dst_addr_offset;
	}
    req.dst.v_addr = 0;
    req.dst.format = pixel;
	req.render_mode = bitblt_mode;

    req.clip.xmin = 0;
    req.clip.xmax = 1024;
    req.clip.ymin = 0;
    req.clip.ymax = 600;

	rga_ioctl_kernel(&req);

	return 0;
}


static int rk_fb_lcd_bitblt(void *argv)
{
	struct lcd_contorol *control = (struct lcd_contorol*)argv;
	struct rk_lcdc_driver *dev_drv = control->dev_drv;
	int timeout;
	struct list_data* data;

	while (1) {
		control->bitblt_finish_flag = 0;
		mutex_unlock(&control->bitblt_lock);
bitblt_wait:
		timeout = wait_event_interruptible_timeout(control->bitblt_wait,
												   control->bitblt_finish_flag,
												   msecs_to_jiffies(1));
		if (kthread_should_stop())
			break;
		if(timeout == 0)
			goto bitblt_wait;

		/* bitblt? */
		while(!list_empty(&(control->bitblt_list)))
		{
			/* get list */
			data = list_entry(control->bitblt_list.next, struct list_data, list);
			bitblt_func(dev_drv, data->req);
			list_del(&data->list);
			list_add_tail(&data->list, &control->empty_bitblt_list);
		}
	}
	return 0;
}

static int rk_fb_lcd_flip(void *argv)
{
	struct lcd_contorol *control = (struct lcd_contorol*)argv;
	struct rk_lcdc_driver *dev_drv = control->dev_drv;
	static bool black_old = true;
	int timeout;

	while (1) {
		control->flip_finish_flag = 0;
		mutex_unlock(&control->flip_lock);
flip_wait:
		timeout = wait_event_interruptible_timeout(control->flip_wait,
												   control->flip_finish_flag && 
												   (!control->bitblt_finish_flag),
												   msecs_to_jiffies(1));
		if (kthread_should_stop())
			break;
		if(timeout == 0)
			goto flip_wait;

		/* set Black Disp */
		if(control->black != black_old){
			if(dev_drv->ops->dsp_black){
				dev_drv->ops->dsp_black(dev_drv, control->black);
			}
			black_old = control->black;
		}

		/* disable irq */
		local_irq_disable();
		/* wait vblank */
		if(!black_old)
			rk_fb_poll_wait_frame_complete();

		/* set Back Ground Color */
		if(control->flip_action.bg)
		{
			if(dev_drv->ops->set_bg_color){
				dev_drv->ops->set_bg_color(dev_drv,
										   control->bg[control->num.bg].R,
										   control->bg[control->num.bg].G,
										   control->bg[control->num.bg].B);
				control->num.bg ^= 1;
			}
		}
		if(control->flip_action.win0)
		{
			if (dev_drv->ops->win0_dsp_st)
				dev_drv->ops->win0_dsp_st(dev_drv,
										  control->win0[control->num.win0].positon.x,
										  control->win0[control->num.win0].positon.y);
				
			/* flip & enable*/
			if(dev_drv->ops->win0_enable)
				dev_drv->ops->win0_enable(dev_drv,
										  control->win0[control->num.win0].layer_enable,
										  lcd_buffer[WIN0_BUFFER_NUM + control->num.win0].phy_addr);
			/* change num */
			control->num.win0 ^= 1;
		}
		if(control->flip_action.win1)
		{
			if (dev_drv->ops->win1_dsp_st)
				dev_drv->ops->win1_dsp_st(dev_drv,
										  control->win1[control->num.win1].positon.x,
										  control->win1[control->num.win1].positon.y);
			/* flip & enable*/
			if(dev_drv->ops->win1_enable)
				dev_drv->ops->win1_enable(dev_drv,
										  control->win1[control->num.win1].layer_enable,
										  lcd_buffer[WIN1_BUFFER_NUM + control->num.win1].phy_addr);
			/* change num */
			control->num.win1 ^= 1;
		}
		if(control->flip_action.hwc)
		{
			if (dev_drv->ops->hwc_dsp_st)
				dev_drv->ops->hwc_dsp_st(dev_drv,
										 control->hwc[control->num.hwc].positon.x,
										 control->hwc[control->num.hwc].positon.y);
			/* flip & enable*/
			if(dev_drv->ops->hwc_enable)
				dev_drv->ops->hwc_enable(dev_drv,
										 control->hwc[control->num.hwc].layer_enable,
										 lcd_buffer[HWC_BUFFER_NUM + control->num.hwc].phy_addr);
			control->num.hwc ^= 1;
		}

		/* enable irq */
		local_irq_enable();
	}
	return 0;
}
#endif  /* 2015/06 LCD_CHANGE */

static int rk_fb_ioctl(struct fb_info *info, unsigned int cmd,
		       unsigned long arg)
{
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct fb_fix_screeninfo *fix = &info->fix;
	struct rk_lcdc_win *win;
	int enable;	/* enable fb:1 enable;0 disable */
	int ovl;	/* overlay:0 win1 on the top of win0;1,win0 on the top of win1 */
	int num_buf;	/* buffer_number */
	int ret;
	struct rk_fb_win_cfg_data win_data;
	unsigned int dsp_addr[4];
	int list_stat;

	int win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);

	void __user *argp = (void __user *)arg;
#if 1  /* 2015/06 LCD_CHANGE */
	static struct lcd_contorol control = {0};
#endif /* 2015/06 LCD_CHANGE */
	win = dev_drv->win[win_id];

	switch (cmd) {
	case RK_FBIOSET_HWC_ADDR:
	{
		u32 hwc_phy[1];
		if (copy_from_user(hwc_phy, argp, 4))
			return -EFAULT;
#ifdef CONFIG_ROCKCHIP_IOMMU
		if (!dev_drv->iommu_enabled) {
#endif
			fix->smem_start = hwc_phy[0];
#ifdef CONFIG_ROCKCHIP_IOMMU
		} else {
			int usr_fd;
			struct ion_handle *hdl;
			ion_phys_addr_t phy_addr;
			size_t len;

			usr_fd = hwc_phy[0];
			if (!usr_fd) {
				fix->smem_start = 0;
				fix->mmio_start = 0;
				dev_drv->ops->open(dev_drv, win_id, 0);
				break;
			}

			if (ion_hwc[0] != 0) {
				ion_free(rk_fb->ion_client, ion_hwc[0]);
				ion_hwc[0] = 0;
			}

			hdl = ion_import_dma_buf(rk_fb->ion_client, usr_fd);
			if (IS_ERR(hdl)) {
				dev_err(info->dev, "failed to get hwc ion handle:%ld\n",
					PTR_ERR(hdl));
				return -EFAULT;
			}

			ret = ion_map_iommu(dev_drv->dev, rk_fb->ion_client, hdl,
						(unsigned long *)&phy_addr,
						(unsigned long *)&len);
			if (ret < 0) {
				dev_err(info->dev, "ion map to get hwc phy addr failed");
				ion_free(rk_fb->ion_client, hdl);
				return -ENOMEM;
			}
			fix->smem_start = phy_addr;
			ion_hwc[0] = hdl;
		}
#endif
		break;
	}
	case RK_FBIOSET_YUV_ADDR:
		{
			u32 yuv_phy[2];

			if (copy_from_user(yuv_phy, argp, 8))
				return -EFAULT;
			#ifdef CONFIG_ROCKCHIP_IOMMU
			if (!dev_drv->iommu_enabled || !strcmp(info->fix.id, "fb0")) {
			#endif
				fix->smem_start = yuv_phy[0];
				fix->mmio_start = yuv_phy[1];
			#ifdef CONFIG_ROCKCHIP_IOMMU
			} else {
				int usr_fd, offset, tmp;
				struct ion_handle *hdl;
				ion_phys_addr_t phy_addr;
				size_t len;

				usr_fd = yuv_phy[0];
				offset = yuv_phy[1] - yuv_phy[0];
				if (!usr_fd) {
					fix->smem_start = 0;
					fix->mmio_start = 0;
					break;
				}

				if (ion_hanle[ION_MAX - 1] != 0) {
					/*ion_unmap_kernel(rk_fb->ion_client, ion_hanle[ION_MAX - 1]);*/
					/*ion_unmap_iommu(dev_drv->dev, rk_fb->ion_client, ion_hanle[ION_MAX - 1]);*/
					ion_free(rk_fb->ion_client, ion_hanle[ION_MAX - 1]);
					ion_hanle[ION_MAX - 1] = 0;
				}

				hdl = ion_import_dma_buf(rk_fb->ion_client, usr_fd);
				if (IS_ERR(hdl)) {
					dev_err(info->dev, "failed to get ion handle:%ld\n",
						PTR_ERR(hdl));
					return -EFAULT;
				}

				ret = ion_map_iommu(dev_drv->dev, rk_fb->ion_client, hdl,
							(unsigned long *)&phy_addr,
							(unsigned long *)&len);
				if (ret < 0) {
					dev_err(info->dev, "ion map to get phy addr failed");
					ion_free(rk_fb->ion_client, hdl);
					return -ENOMEM;
				}
				fix->smem_start = phy_addr;
				fix->mmio_start = phy_addr + offset;
				fix->smem_len = len;
				/*info->screen_base = ion_map_kernel(rk_fb->ion_client, hdl);*/

				ion_hanle[0] = hdl;
				for (tmp = ION_MAX - 1; tmp > 0; tmp--)
					ion_hanle[tmp] = ion_hanle[tmp - 1];
				ion_hanle[0] = 0;
			}
			#endif
			break;
		}
	case RK_FBIOSET_ENABLE:
		if (copy_from_user(&enable, argp, sizeof(enable)))
			return -EFAULT;
				if (enable)
					fb_par->state++;
				else
					fb_par->state--;
		dev_drv->ops->open(dev_drv, win_id, enable);
		break;
	case RK_FBIOGET_ENABLE:
		enable = dev_drv->ops->get_win_state(dev_drv, win_id);
		if (copy_to_user(argp, &enable, sizeof(enable)))
			return -EFAULT;
		break;
	case RK_FBIOSET_OVERLAY_STA:
		if (copy_from_user(&ovl, argp, sizeof(ovl)))
			return -EFAULT;
		dev_drv->ops->ovl_mgr(dev_drv, ovl, 1);
		break;
	case RK_FBIOGET_OVERLAY_STA:
		ovl = dev_drv->ops->ovl_mgr(dev_drv, 0, 0);
		if (copy_to_user(argp, &ovl, sizeof(ovl)))
			return -EFAULT;
		break;
	case RK_FBIOPUT_NUM_BUFFERS:
		if (copy_from_user(&num_buf, argp, sizeof(num_buf)))
			return -EFAULT;
		dev_drv->num_buf = num_buf;
		break;
	case RK_FBIOSET_VSYNC_ENABLE:
		if (copy_from_user(&enable, argp, sizeof(enable)))
			return -EFAULT;
		dev_drv->vsync_info.active = enable;
		break;

	case RK_FBIOGET_DSP_ADDR:
		dev_drv->ops->get_dsp_addr(dev_drv, dsp_addr);
		if (copy_to_user(argp, &dsp_addr, sizeof(dsp_addr)))
			return -EFAULT;
		break;
	case RK_FBIOGET_LIST_STA:
		list_stat = rk_fb_get_list_stat(dev_drv);
		if (copy_to_user(argp, &list_stat, sizeof(list_stat)))
			return -EFAULT;

		break;
	case RK_FBIOGET_IOMMU_STA:
		if (copy_to_user(argp, &dev_drv->iommu_enabled,
				 sizeof(dev_drv->iommu_enabled)))
			return -EFAULT;
		break;
#if defined(CONFIG_ION_ROCKCHIP)
	case RK_FBIOSET_DMABUF_FD:
		{
			int usr_fd;
			struct ion_handle *hdl;
			ion_phys_addr_t phy_addr;
			size_t len;
			if (copy_from_user(&usr_fd, argp, sizeof(usr_fd)))
				return -EFAULT;
			hdl = ion_import_dma_buf(rk_fb->ion_client, usr_fd);
			if (IS_ERR(hdl)) {
				dev_err(info->dev, "failed to ion_alloc:%ld rk_fb->ion_client\n",
						PTR_ERR(hdl));
				return -ENOMEM;
			}
			ion_phys(rk_fb->ion_client, hdl, &phy_addr, &len);
			fix->smem_start = phy_addr;
			break;
		}
	case RK_FBIOGET_DMABUF_FD:
		{
			int fd = -1;

			if (IS_ERR_OR_NULL(fb_par->ion_hdl)) {
				dev_err(info->dev,
					"get dma_buf fd failed,ion handle is err\n");
				return PTR_ERR(fb_par->ion_hdl);
			}
			fd = ion_share_dma_buf_fd(rk_fb->ion_client,
						  fb_par->ion_hdl);
			if (fd < 0) {
				dev_err(info->dev,
					"ion_share_dma_buf_fd failed\n");
				return fd;
			}
			if (copy_to_user(argp, &fd, sizeof(fd)))
				return -EFAULT;
			break;
		}
#endif
	case RK_FBIOSET_CLEAR_FB:
		memset(fb_par->fb_virt_base, 0, fb_par->fb_size);
		break;
	case RK_FBIOSET_CONFIG_DONE:
		{
			int curr = 0;
			struct timespec now;

			getnstimeofday(&now);
			curr = now.tv_sec * 1000000 + now.tv_nsec / 1000;
			cfgdone_distlist[cfgdone_index++] =
			    curr - cfgdone_lasttime;
			/*
			   printk("%d ", curr - cfgdone_lasttime);
			 */
			cfgdone_lasttime = curr;
			if (cfgdone_index >= 10)
				cfgdone_index = 0;
		}
		if (copy_from_user(&win_data,
				   (struct rk_fb_win_cfg_data __user *)argp,
				   sizeof(win_data))) {
			return -EFAULT;
		};

		dev_drv->wait_fs = win_data.wait_fs;
		rk_fb_set_win_config(info, &win_data);

		if (copy_to_user((struct rk_fb_win_cfg_data __user *)arg,
				 &win_data, sizeof(win_data))) {
			return -EFAULT;
		}
		memset(&win_data, 0, sizeof(struct rk_fb_win_cfg_data));
		break;

#if 1 /* 2015/06 LCD_CHANGE */
	case RK_FB_VIDEO_INIT:
	{
		VIDEO_OUTPUT_INI config;
		int disp_size;
		int hwc_size;
		int i;
		int ret;
		if (copy_from_user(&config, argp, sizeof(config)))
			return -EFAULT;

		memset(&control, 0x00, sizeof(struct lcd_contorol));

		disp_size = config.video_width*config.video_height*2;
		hwc_size = (config.hwc_size == SIZE_32x32) ? 32*32: 64*64;

		control.hwc_lut_num = config.hwc_lut_num;
		control.alloc_max_num = config.alloc_max;
		control.bitblt_max_num = config.bitblt_max;

		control.black = true;

		lcd_buffer = kmalloc(HWC_LUT_BUFFER_END * sizeof(struct ion_table), GFP_KERNEL);
		memset(lcd_buffer, 0, HWC_LUT_BUFFER_END * sizeof(struct ion_table));

		/* create FB */
		ret = buffer_create(info, &lcd_buffer[WIN0_BUFFER_NUM], disp_size);
		if(ret != 0) return -ENOMEM;
		ret = buffer_create(info, &lcd_buffer[WIN0_BUFFER_NUM + 1], disp_size);
		if(ret != 0) return -ENOMEM;
		ret = buffer_create(info, &lcd_buffer[WIN1_BUFFER_NUM], disp_size);
		if(ret != 0) return -ENOMEM;
		ret = buffer_create(info, &lcd_buffer[WIN1_BUFFER_NUM + 1], disp_size);
		if(ret != 0) return -ENOMEM;
		ret = buffer_create(info, &lcd_buffer[HWC_BUFFER_NUM], hwc_size);
		if(ret != 0) return -ENOMEM;
		ret = buffer_create(info, &lcd_buffer[HWC_BUFFER_NUM + 1], hwc_size);
		if(ret != 0) return -ENOMEM;

		/* create LUT buf */
		for(i = 0; i < control.hwc_lut_num; i++){
			ret = buffer_create(info, &lcd_buffer[HWC_LUT_BUFFER_NUM + i], 256 * 4);
			if(ret != 0) return -ENOMEM;
		}

		/* Black display ON */
		if(dev_drv->ops->dsp_black){
			dev_drv->ops->dsp_black(dev_drv, 1);
		}
		if(dev_drv->ops->win0_enable)
			dev_drv->ops->win0_enable(dev_drv,
									  1,
									  lcd_buffer[WIN0_BUFFER_NUM + 0].phy_addr);


		dev_drv->cur_screen = dev_drv->screen0;
		dev_drv->ops->load_screen(dev_drv, 1);


		/* init Win0 */
		if (dev_drv->ops->win0_act_info &&
			dev_drv->ops->win0_dsp_info &&
			dev_drv->ops->win0_vir) {
			dev_drv->ops->win0_act_info(dev_drv,
										config.win0_width,
										config.win0_height);
			dev_drv->ops->win0_dsp_info(dev_drv,
										config.win0_width,
										config.win0_height);
			dev_drv->ops->win0_vir(dev_drv,
								   config.video_width,
								   config.video_height);
		}

		/* init Win1 */
		if (dev_drv->ops->win1_act_info &&
			dev_drv->ops->win1_dsp_info &&
			dev_drv->ops->win1_vir) {
			dev_drv->ops->win1_act_info(dev_drv,
										config.win1_width,
										config.win1_height);
			dev_drv->ops->win1_dsp_info(dev_drv,
										config.win1_width,
										config.win1_height);
			dev_drv->ops->win1_vir(dev_drv,
								   config.video_width,
								   config.video_height);
		}

		/* init HWC */
		if(dev_drv->ops->hwc_size){
			dev_drv->ops->hwc_size(dev_drv, config.hwc_size);
		}
		if(dev_drv->ops->hwc_alpah_en){
			dev_drv->ops->hwc_alpah_en(dev_drv, 1);
		}

		/* create event task */
		mutex_init(&control.bitblt_lock);
		mutex_lock(&control.bitblt_lock);

		mutex_init(&control.flip_lock);
		mutex_lock(&control.flip_lock);

		control.num.bg   = 1;
		control.num.win0 = 1;
		control.num.win1 = 1;
		control.num.hwc  = 1;

		control.win0[0].layer_enable = 1;
		control.win0[1].layer_enable = 1;

		control.bitblt_finish_flag = 0;
		control.flip_finish_flag = 0;
		init_waitqueue_head( &(control.bitblt_wait) );
		init_waitqueue_head( &(control.flip_wait) );

		INIT_LIST_HEAD(&(control.bitblt_list));
		INIT_LIST_HEAD(&(control.empty_bitblt_list));

		for(i = 0; i < control.bitblt_max_num; i++){
			struct list_data* data;
			data = kzalloc(sizeof(struct list_data), GFP_KERNEL);
			if(data == NULL){
				return -ENOMEM;
			}
			list_add_tail(&data->list, &control.empty_bitblt_list);
		}

		/* rga buffer init */
		cmd_reg_memory_init(control.bitblt_max_num);

		control.dev_drv = dev_drv;
		control.bitblt_thread = kthread_run(rk_fb_lcd_bitblt, &control,
						"bitblt_thread");
		control.flip_thread = kthread_run(rk_fb_lcd_flip, &control,
						"flip_thread");

		break;
	}
	case RK_FB_VIDEO_EXIT:
	{
		int i;

		/* stop thread */
		kthread_stop(control.flip_thread);
		kthread_stop(control.bitblt_thread);

		/* RGA buffer deinit */
		cmd_reg_memory_exit();

		/* bitblt buffer deinit */

		while(!list_empty(&(control.empty_bitblt_list))){
			struct list_data* data;
			data = list_entry(control.empty_bitblt_list.next, struct list_data, list);
			list_del(&data->list);
			kfree(data);
		}

		while(!list_empty(&(control.empty_bitblt_list))){
			struct list_data* data;
			data = list_entry(control.empty_bitblt_list.next, struct list_data, list);
			list_del(&data->list);
			kfree(data);
		}

		list_del_init(&control.bitblt_list);
		list_del_init(&control.empty_bitblt_list);

		/* mutex deinit */
		mutex_destroy(&control.bitblt_lock);
		mutex_destroy(&control.flip_lock);

		/* buffer free */
		for(i = 0; i < HWC_LUT_BUFFER_END; i++)
			if(lcd_buffer[i].fd != 0)
				buffer_delete(info, &lcd_buffer[i]);
		kfree(lcd_buffer);

		memset(&control, 0x00, sizeof(struct lcd_contorol));

		break;
	}
	case RK_FB_SET_BLACK_DISP:
		if (copy_from_user(&(control.black), argp, sizeof(enable))){
			return -EFAULT;
		}
		break;
	case RK_FB_SET_BITBLT:
	{
		struct list_data* data;
		struct ion_table *src;
		struct ion_table *dst;

		/*  */
		if(list_empty(&(control.empty_bitblt_list))){
			dev_err(info->dev, "bitblt add failed(bitblt max[%d])\n",
					control.bitblt_max_num);
			return -EFAULT;
		}

		/* add wait bitblt cmd num */
		data = list_entry(control.empty_bitblt_list.next, struct list_data, list);
		list_del(&data->list);

		if (copy_from_user(&data->req, argp, sizeof(BITBLT_LIST))){
			kfree(data);
			return -EFAULT;
		}

		src = find_ion_table(data->req.src_addr, WIN0_BUFFER_NUM, VIDEO_BUFFER_END);
		dst = find_ion_table(data->req.dst_addr, WIN0_BUFFER_NUM, VIDEO_BUFFER_END);
		if(src == NULL || dst == NULL){
			dev_err(info->dev, "find_ion_table failed\n");
			kfree(data);
			return -EINVAL;
		}

		data->req.src_addr = src->phy_addr;
		data->req.dst_addr = dst->phy_addr;

		list_add_tail(&data->list, &control.bitblt_list);
		
		break;
	}

	case RK_FB_BITBLT_START:
		mutex_lock(&control.bitblt_lock);
		control.bitblt_finish_flag = 1;
		wake_up_interruptible( &(control.bitblt_wait) );
		break;
	case RK_FB_BITBLT_WAIT:
		/* wait */
		mutex_lock(&control.bitblt_lock);
		mutex_unlock(&control.bitblt_lock);
		break;
	case RK_FB_VIDEO_BUF_GET:
	{
		struct buffer_st buf_st;
		struct ion_table *tbl;

		if (copy_from_user(&buf_st, argp, sizeof(struct buffer_st)))
			return -EFAULT;

		if((tbl = find_ion_table(0, VIDEO_BUFFER_NUM, VIDEO_BUFFER_END)) == NULL){
			dev_err(info->dev, "find_ion_table failed\n");
			return -ENOMEM;
		}
		if(buffer_create(info, tbl, buf_st.mem_size) != 0)
			return -ENOMEM;

		buf_st.fd = tbl->fd;

		if (copy_to_user(argp, &buf_st, sizeof(struct buffer_st)))
			return -EFAULT;

		break;
	}
	case RK_FB_VIDEO_BUF_PUT:
	{
		int usr_fd;
		struct ion_table *tbl;

		if (copy_from_user(&usr_fd, argp, sizeof(usr_fd)))
			return -EFAULT;

		if((tbl = find_ion_table(usr_fd, VIDEO_BUFFER_NUM, VIDEO_BUFFER_END)) == NULL){
			dev_err(info->dev, "find_ion_table failed\n");
			return -EINVAL;
		}
		/* free */
		buffer_delete(info, tbl);
		break;
	}
	case RK_FB_FRAME_BUFFER_GET:
	{
		struct double_buf fd;

		fd.win0_0 = lcd_buffer[WIN0_BUFFER_NUM].fd;
		fd.win0_1 = lcd_buffer[WIN0_BUFFER_NUM + 1].fd;
		fd.win1_0 = lcd_buffer[WIN1_BUFFER_NUM].fd;
		fd.win1_1 = lcd_buffer[WIN1_BUFFER_NUM + 1].fd;
		fd.hwc_0  = lcd_buffer[HWC_BUFFER_NUM].fd;
		fd.hwc_1  = lcd_buffer[HWC_BUFFER_NUM + 1].fd;

		if (copy_to_user(argp, &fd, sizeof(fd)))
			return -EFAULT;
		break;
	}
	case RK_FB_FRAME_BUFFER_PUT:
		/* do nothing */
		break;
	case RK_FB_BACK_BUFFER_NUM_GET:
	{
		struct bk_buf_num num;
		num.win0 =  control.num.win0;
		num.win1 =  control.num.win1;
		num.hwc  =  control.num.hwc;
		num.bg  =  control.num.bg;
		if (copy_to_user(argp, &num, sizeof(struct bk_buf_num)))
			return -EFAULT;
		break;
	}
	case RK_FB_SET_BACKGROUND_COROL:
		if (copy_from_user(&(control.bg[control.num.bg]), argp, sizeof(control.bg)))
			return -EFAULT;
		break;
	case RK_FB_SET_WIN0_ENABLE:
		if (copy_from_user(&(control.win0[control.num.win0].layer_enable), argp,
						   sizeof(control.win0[control.num.win0].layer_enable)))
			return -EFAULT;
		break;
	case RK_FB_SET_WIN0_CONDITION:
		if (copy_from_user(&(control.win0[control.num.win0].positon), argp,
						   sizeof(control.win0[control.num.win0].positon)))
			return -EFAULT;
		break;
	case RK_FB_FLIP_START:
		if (copy_from_user(&(control.flip_action), argp, 
						   sizeof(control.flip_action)))
			return -EFAULT;
		mutex_lock(&control.flip_lock);
		control.flip_finish_flag = 1;
		wake_up_interruptible( &(control.flip_wait) );
		break;
	case RK_FB_FLIP_WAIT:
		mutex_lock(&control.flip_lock);
		mutex_unlock(&control.flip_lock);
		break;
	case RK_FB_SET_WIN1_ENABLE:
		if (copy_from_user(&(control.win1[control.num.win1].layer_enable), argp,
						   sizeof(control.win1[control.num.win1].layer_enable)))
			return -EFAULT;
		break;
	case RK_FB_SET_WIN1_CONDITION:
		if (copy_from_user(&(control.win1[control.num.win1].positon), argp,
						   sizeof(control.win1[control.num.win1].positon)))
			return -EFAULT;
		break;
	case RK_FB_SET_HWC_ENABLE:
		if (copy_from_user(&(control.hwc[control.num.hwc].layer_enable), argp,
						   sizeof(control.hwc[control.num.hwc].layer_enable)))
			return -EFAULT;
		break;
	case RK_FB_SET_HWC_CONDITION:
		if (copy_from_user(&(control.hwc[control.num.hwc].positon), argp,
						   sizeof(control.hwc[control.num.hwc].positon)))
			return -EFAULT;
		break;
	case RK_FB_SET_HWC_LUT_NUM:
	{
		int lut_num;
		int *hwc_lut;
		struct ion_handle *hdl;

		/* get LUT number */
		if (copy_from_user(&lut_num, argp, sizeof(lut_num)))
			return -EFAULT;
		if (lut_num > control.hwc_lut_num || lut_num == 0){
			dev_err(info->dev, "req num[%d] > max lut num[%d]\n",
					lut_num, control.hwc_lut_num);
			return -EFAULT;
		}
		/* serch LUT buffer */
		hdl = lcd_buffer[HWC_LUT_BUFFER_NUM + lut_num - 1].hdl;

		hwc_lut = ion_map_kernel(rk_fb->ion_client, hdl);

		/* set HWC LUT */
		if (dev_drv->ops->set_hwc_lut)
			dev_drv->ops->set_hwc_lut(dev_drv, hwc_lut, 1);

		ion_unmap_kernel(rk_fb->ion_client, hdl);

		break;
	}

	case RK_FB_SET_HWC_LUT_BUF_GET:
	{
		int lut_num;
		if (copy_from_user(&lut_num, argp, sizeof(lut_num)))
			return -EFAULT;

		if (lut_num > control.hwc_lut_num || lut_num == 0){
			dev_err(info->dev, "req num[%d] > max lut num[%d]\n",
					lut_num, control.hwc_lut_num);
			return -EFAULT;
		}
		
		if (copy_to_user(argp, &lcd_buffer[HWC_LUT_BUFFER_NUM + lut_num - 1].fd,
						 sizeof(lcd_buffer[HWC_LUT_BUFFER_NUM + lut_num - 1].fd)))
			return -EFAULT;
		break;
	}
	case RK_FB_SET_HWC_LUT_BUF_PUT:
		/* do nothing */
		break;
	case RK_BUFFER_SYNC:
	{		
		extern int ion_sync_for_device(struct ion_client *client, int fd);
		int fd;
		if (copy_from_user(&fd, argp, sizeof(fd)))
			return -EFAULT;

		ion_sync_for_device(rk_fb->ion_client, fd);
		break;
	}

	case 0x8000:
	{
		int i;
		for(i=0;i<VIDEO_BUFFER_END;i++){
			struct ion_table *p = &lcd_buffer[i];
			if(p->fd != 0){
				printk("lcd_buffer[%d]\n", i);
				printk("    p->hdl     [%p]\n", p->hdl);
				printk("    p->dma_buf [%p]\n", p->dma_buf);
				printk("    p->phy_addr[%08x]\n", p->phy_addr);
				printk("    p->len     [%d]\n", p->len);
				printk("    p->fd      [%d]\n", p->fd);
				printk("    p->phy_top [%08x]\n", dev_drv->ops->iova_to_phy(dev_drv, (void*)(p->phy_addr)));
			}
		}
	}
	break;

#endif /* 2015/06 LCD_CHANGE */

	default:
		dev_drv->ops->ioctl(dev_drv, cmd, arg, win_id);
		break;
	}

	return 0;
}

static int rk_fb_blank(int blank_mode, struct fb_info *info)
{
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct fb_fix_screeninfo *fix = &info->fix;
	int win_id;
#if defined(CONFIG_RK_HDMI)
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
#endif

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, fix->id);
	if (win_id < 0)
		return -ENODEV;
#if defined(CONFIG_RK_HDMI)
	if ((rk_fb->disp_mode == ONE_DUAL) &&
	    (hdmi_get_hotplug() == HDMI_HPD_ACTIVED)) {
		printk(KERN_INFO "hdmi is connect , not blank lcdc\n");
	} else
#endif
	{
		dev_drv->ops->blank(dev_drv, win_id, blank_mode);
	}
	return 0;
}

static int rk_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if ((0 == var->xres_virtual) || (0 == var->yres_virtual) ||
	    (0 == var->xres) || (0 == var->yres) || (var->xres < 16) ||
	    ((16 != var->bits_per_pixel) &&
	    (32 != var->bits_per_pixel) &&
	    (24 != var->bits_per_pixel))) {
		dev_err(info->dev, "%s check var fail 1:\n"
			"xres_vir:%d>>yres_vir:%d\n"
			"xres:%d>>yres:%d\n"
			"bits_per_pixel:%d\n",
			info->fix.id,
			var->xres_virtual,
			var->yres_virtual,
			var->xres, var->yres, var->bits_per_pixel);
		return -EINVAL;
	}

	if (((var->xoffset + var->xres) > var->xres_virtual) ||
	    ((var->yoffset + var->yres) > (var->yres_virtual))) {
		dev_err(info->dev, "%s check_var fail 2:\n"
			"xoffset:%d>>xres:%d>>xres_vir:%d\n"
			"yoffset:%d>>yres:%d>>yres_vir:%d\n",
			info->fix.id,
			var->xoffset,
			var->xres,
			var->xres_virtual,
			var->yoffset, var->yres, var->yres_virtual);
		return -EINVAL;
	}

	return 0;
}

static ssize_t rk_fb_read(struct fb_info *info, char __user *buf,
			  size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	u8 *buffer, *dst;
	u8 __iomem *src;
	int c, cnt = 0, err = 0;
	unsigned long total_size;
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_lcdc_win *win = NULL;
	int win_id = 0;

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	if (win_id < 0)
		return -ENODEV;
	else
		win = dev_drv->win[win_id];

	/* only read the current frame buffer */
	if (win->area[0].format == RGB565) {
		total_size = win->area[0].y_vir_stride * win->area[0].yact << 1;
	} else if (win->area[0].format == YUV420) {
		total_size =
		    (win->area[0].y_vir_stride * win->area[0].yact * 6);
	} else {
		total_size = win->area[0].y_vir_stride * win->area[0].yact << 2;
	}
	if (p >= total_size)
		return 0;

	if (count >= total_size)
		count = total_size;

	if (count + p > total_size)
		count = total_size - p;

	buffer = kmalloc((count > PAGE_SIZE) ? PAGE_SIZE : count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	src = (u8 __iomem *)(info->screen_base + p + win->area[0].y_offset);

	while (count) {
		c = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		dst = buffer;
		fb_memcpy_fromfb(dst, src, c);
		dst += c;
		src += c;

		if (copy_to_user(buf, buffer, c)) {
			err = -EFAULT;
			break;
		}
		*ppos += c;
		buf += c;
		cnt += c;
		count -= c;
	}

	kfree(buffer);

	return (err) ? err : cnt;
}

static ssize_t rk_fb_write(struct fb_info *info, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	u8 *buffer, *src;
	u8 __iomem *dst;
	int c, cnt = 0, err = 0;
	unsigned long total_size;
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_lcdc_win *win = NULL;
	int win_id = 0;

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	if (win_id < 0)
		return -ENODEV;
	else
		win = dev_drv->win[win_id];

	/* write the current frame buffer */
	if (win->area[0].format == RGB565)
		total_size = win->area[0].xact * win->area[0].yact << 1;
	else
		total_size = win->area[0].xact * win->area[0].yact << 2;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	buffer = kmalloc((count > PAGE_SIZE) ? PAGE_SIZE : count, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	dst = (u8 __iomem *)(info->screen_base + p + win->area[0].y_offset);

	while (count) {
		c = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		src = buffer;

		if (copy_from_user(src, buf, c)) {
			err = -EFAULT;
			break;
		}

		fb_memcpy_tofb(dst, src, c);
		dst += c;
		src += c;
		*ppos += c;
		buf += c;
		cnt += c;
		count -= c;
	}

	kfree(buffer);

	return (cnt) ? cnt : err;
}

static int rk_fb_set_par(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &info->var;
	struct fb_fix_screeninfo *fix = &info->fix;
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_fb *rk_fb = dev_get_drvdata(info->device);
	struct rk_lcdc_win *win = NULL;
	struct rk_screen *screen = dev_drv->cur_screen;
	int win_id = 0;
	u32 cblen = 0, crlen = 0;
	u16 xsize = 0, ysize = 0;	/* winx display window height/width --->LCDC_WINx_DSP_INFO */
	u32 xoffset = var->xoffset;	/* offset from virtual to visible */
	u32 yoffset = var->yoffset;
	u16 xpos = (var->nonstd >> 8) & 0xfff;	/*visiable pos in panel */
	u16 ypos = (var->nonstd >> 20) & 0xfff;
	u32 xvir = var->xres_virtual;
	u32 yvir = var->yres_virtual;
	u8 data_format = var->nonstd & 0xff;
	u8 fb_data_fmt;
	u8 pixel_width;
	u32 vir_width_bit;
	u32 stride, uv_stride;
	u32 stride_32bit_1;
	u32 stride_32bit_2;
	u16 uv_x_off, uv_y_off, uv_y_act;
	u8 is_pic_yuv = 0;

	var->pixclock = dev_drv->pixclock;
	win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
	if (win_id < 0)
		return -ENODEV;
	else
		win = dev_drv->win[win_id];

	/* if the application has specific the horizontal and vertical display size */
	if (var->grayscale >> 8) {
		xsize = (var->grayscale >> 8) & 0xfff;
		ysize = (var->grayscale >> 20) & 0xfff;
		if (xsize > screen->mode.xres)
			xsize = screen->mode.xres;
		if (ysize > screen->mode.yres)
			ysize = screen->mode.yres;
	} else {		/*ohterwise  full  screen display */
		xsize = screen->mode.xres;
		ysize = screen->mode.yres;
	}

	fb_data_fmt = rk_fb_data_fmt(data_format, var->bits_per_pixel);
	pixel_width = rk_fb_pixel_width(fb_data_fmt);
	vir_width_bit = pixel_width * xvir;
	/* pixel_width = byte_num * 8 */
	stride_32bit_1 = ALIGN_N_TIMES(vir_width_bit, 32) / 8;
	stride_32bit_2 = ALIGN_N_TIMES(vir_width_bit * 2, 32) / 8;

	switch (fb_data_fmt) {
	case YUV422:
	case YUV422_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1 >> 1;
		uv_x_off = xoffset >> 1;
		uv_y_off = yoffset;
		fix->line_length = stride;
		cblen = crlen = (xvir * yvir) >> 1;
		uv_y_act = win->area[0].yact >> 1;
		break;
	case YUV420:		/* 420sp */
	case YUV420_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_1;
		uv_x_off = xoffset;
		uv_y_off = yoffset >> 1;
		fix->line_length = stride;
		cblen = crlen = (xvir * yvir) >> 2;
		uv_y_act = win->area[0].yact >> 1;
		break;
	case YUV444:
	case YUV444_A:
		is_pic_yuv = 1;
		stride = stride_32bit_1;
		uv_stride = stride_32bit_2;
		uv_x_off = xoffset * 2;
		uv_y_off = yoffset;
		fix->line_length = stride << 2;
		cblen = crlen = (xvir * yvir);
		uv_y_act = win->area[0].yact;
		break;
	default:
		stride = stride_32bit_1;	/* default rgb */
		fix->line_length = stride;
		break;
	}

	/* x y mirror ,jump line */
	if (screen->y_mirror == 1) {
		if (screen->interlace == 1) {
			win->area[0].y_offset = yoffset * stride * 2 +
			    ((win->area[0].yact - 1) * 2 + 1) * stride +
			    xoffset * pixel_width / 8;
		} else {
			win->area[0].y_offset = yoffset * stride +
			    (win->area[0].yact - 1) * stride +
			    xoffset * pixel_width / 8;
		}
	} else {
		if (screen->interlace == 1) {
			win->area[0].y_offset =
			    yoffset * stride * 2 + xoffset * pixel_width / 8;
		} else {
			win->area[0].y_offset =
			    yoffset * stride + xoffset * pixel_width / 8;
		}
	}
	if (is_pic_yuv == 1) {
		if (screen->y_mirror == 1) {
			if (screen->interlace == 1) {
				win->area[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    ((uv_y_act - 1) * 2 + 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			} else {
				win->area[0].c_offset = uv_y_off * uv_stride +
				    (uv_y_act - 1) * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		} else {
			if (screen->interlace == 1) {
				win->area[0].c_offset =
				    uv_y_off * uv_stride * 2 +
				    uv_x_off * pixel_width / 8;
			} else {
				win->area[0].c_offset =
				    uv_y_off * uv_stride +
				    uv_x_off * pixel_width / 8;
			}
		}
	}

	win->area[0].format = fb_data_fmt;
	win->area[0].y_vir_stride = stride >> 2;
	win->area[0].uv_vir_stride = uv_stride >> 2;
	win->area[0].xpos = xpos;
	win->area[0].ypos = ypos;
	win->area[0].xsize = xsize;
	win->area[0].ysize = ysize;
	win->area[0].xact = var->xres;	/* winx active window height,is a wint of vir */
	win->area[0].yact = var->yres;
	win->area[0].xvir = var->xres_virtual;	/* virtual resolution  stride --->LCDC_WINx_VIR */
	win->area[0].yvir = var->yres_virtual;
	win->area[0].xoff = xoffset;
	win->area[0].yoff = yoffset;

	win->area_num = 1;
	win->alpha_mode = 4;	/* AB_SRC_OVER; */
	win->alpha_en = ((win->area[0].format == ARGB888) ||
			 (win->area[0].format == ABGR888)) ? 1 : 0;
	win->g_alpha_val = 0;

	if (rk_fb->disp_policy == DISPLAY_POLICY_BOX &&
	    (win->area[0].format == YUV420 || win->area[0].format == YUV420_A))
	    win->state = 1;

	dev_drv->ops->set_par(dev_drv, win_id);

	return 0;
}

static inline unsigned int chan_to_field(unsigned int chan,
					 struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int fb_setcolreg(unsigned regno,
			unsigned red, unsigned green, unsigned blue,
			unsigned transp, struct fb_info *info)
{
	unsigned int val;

	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		/* true-colour, use pseudo-palette */
		if (regno < 16) {
			u32 *pal = info->pseudo_palette;
			val = chan_to_field(red, &info->var.red);
			val |= chan_to_field(green, &info->var.green);
			val |= chan_to_field(blue, &info->var.blue);
			pal[regno] = val;
		}
		break;
	default:
		return -1;	/* unknown type */
	}

	return 0;
}

static int rk_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)info->par;
	struct ion_handle *handle = fb_par->ion_hdl;
	struct dma_buf *dma_buf = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		dev_err(info->dev, "failed to get ion handle:%ld\n",
			PTR_ERR(handle));
		return -ENOMEM;
	}
	dma_buf = ion_share_dma_buf(rk_fb->ion_client, handle);
	if (IS_ERR_OR_NULL(dma_buf)) {
		printk("get ion share dma buf failed\n");
		return -ENOMEM;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return dma_buf_mmap(dma_buf, vma, 0);
}

static struct fb_ops fb_ops = {
	.owner = THIS_MODULE,
	.fb_open = rk_fb_open,
	.fb_release = rk_fb_close,
	.fb_check_var = rk_fb_check_var,
	.fb_set_par = rk_fb_set_par,
	.fb_blank = rk_fb_blank,
	.fb_ioctl = rk_fb_ioctl,
	.fb_pan_display = rk_fb_pan_display,
	.fb_read = rk_fb_read,
	.fb_write = rk_fb_write,
	.fb_setcolreg = fb_setcolreg,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

static struct fb_var_screeninfo def_var = {
#if defined(CONFIG_LOGO_LINUX_BMP)
	.red = {16, 8, 0},
	.green = {8, 8, 0},
	.blue = {0, 8, 0},
	.transp = {0, 0, 0},
	.nonstd = HAL_PIXEL_FORMAT_BGRA_8888,
#else
	.red = {11, 5, 0},
	.green = {5, 6, 0},
	.blue = {0, 5, 0},
	.transp = {0, 0, 0},
	.nonstd = HAL_PIXEL_FORMAT_RGB_565,	/* (ypos<<20+xpos<<8+format) format */
#endif
	.grayscale = 0,		/* (ysize<<20+xsize<<8) */
	.activate = FB_ACTIVATE_NOW,
	.accel_flags = 0,
	.vmode = FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo def_fix = {
	.type = FB_TYPE_PACKED_PIXELS,
	.type_aux = 0,
	.xpanstep = 1,
	.ypanstep = 1,
	.ywrapstep = 0,
	.accel = FB_ACCEL_NONE,
	.visual = FB_VISUAL_TRUECOLOR,

};

static int rk_fb_wait_for_vsync_thread(void *data)
{
	struct rk_lcdc_driver *dev_drv = data;
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct fb_info *fbi = rk_fb->fb[0];

	while (!kthread_should_stop()) {
		ktime_t timestamp = dev_drv->vsync_info.timestamp;
		int ret = wait_event_interruptible(dev_drv->vsync_info.wait,
				!ktime_equal(timestamp, dev_drv->vsync_info.timestamp) &&
				(dev_drv->vsync_info.active || dev_drv->vsync_info.irq_stop));

		if (!ret)
			sysfs_notify(&fbi->dev->kobj, NULL, "vsync");
	}

	return 0;
}

static ssize_t rk_fb_vsync_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)fbi->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			 ktime_to_ns(dev_drv->vsync_info.timestamp));
}

static DEVICE_ATTR(vsync, S_IRUGO, rk_fb_vsync_show, NULL);

/*
 * this two function is for other module that in the kernel which
 * need show image directly through fb
 * fb_id:we have 4 fb here,default we use fb0 for ui display
 */
struct fb_info *rk_get_fb(int fb_id)
{
	struct rk_fb *inf = platform_get_drvdata(fb_pdev);
	struct fb_info *fb = inf->fb[fb_id];
	return fb;
}
EXPORT_SYMBOL(rk_get_fb);

void rk_direct_fb_show(struct fb_info *fbi)
{
	rk_fb_set_par(fbi);
	rk_fb_pan_display(&fbi->var, fbi);
}
EXPORT_SYMBOL(rk_direct_fb_show);

int rk_fb_dpi_open(bool open)
{
	struct rk_lcdc_driver *dev_drv = NULL;
	dev_drv = rk_get_prmry_lcdc_drv();

	if (dev_drv->ops->dpi_open)
		dev_drv->ops->dpi_open(dev_drv, open);
	return 0;
}

int rk_fb_dpi_win_sel(int win_id)
{
	struct rk_lcdc_driver *dev_drv = NULL;
	dev_drv = rk_get_prmry_lcdc_drv();

	if (dev_drv->ops->dpi_win_sel)
		dev_drv->ops->dpi_win_sel(dev_drv, win_id);
	return 0;
}

int rk_fb_dpi_status(void)
{
	int ret = 0;
	struct rk_lcdc_driver *dev_drv = NULL;

	dev_drv = rk_get_prmry_lcdc_drv();
	if (dev_drv->ops->dpi_status)
		ret = dev_drv->ops->dpi_status(dev_drv);

	return ret;
}

/*
 * function: this function will be called by display device, enable/disable lcdc
 * @screen: screen timing to be set to lcdc
 * @enable: 0 disable lcdc; 1 enable change lcdc timing; 2 just enable dclk
 * @lcdc_id: the lcdc id the display device attached ,0 or 1
 */
int rk_fb_switch_screen(struct rk_screen *screen, int enable, int lcdc_id)
{
	struct rk_fb *rk_fb =  platform_get_drvdata(fb_pdev);
	struct fb_info *info = NULL;
	struct rk_fb_par *fb_par = NULL;
	struct rk_lcdc_driver *dev_drv = NULL;
	char name[6] = {0};
	int i, win_id, load_screen = 0;

	if (unlikely(!rk_fb) || unlikely(!screen))
		return -ENODEV;

	hdmi_switch_complete = 0;
	/* get lcdc driver */
	sprintf(name, "lcdc%d", lcdc_id);
	if (rk_fb->disp_mode != DUAL)
		dev_drv = rk_fb->lcdc_dev_drv[0];
	else
		dev_drv = rk_get_lcdc_drv(name);

	if (dev_drv == NULL) {
		printk(KERN_ERR "%s driver not found!", name);
		return -ENODEV;
	}
	if (screen->type == SCREEN_HDMI)
		printk("hdmi %s lcdc%d\n", enable ? "connect to" : "remove from",
               		dev_drv->id);
        else if (screen->type == SCREEN_TVOUT ||
		     screen->type == SCREEN_TVOUT_TEST )
        	printk("cvbs %s lcdc%d\n", enable ? "connect to" : "remove from",
               		dev_drv->id);
	if (enable == 2 /*&& dev_drv->enable*/)
		return 0;

	if (rk_fb->disp_mode == ONE_DUAL) {
		if (dev_drv->ops->dsp_black)
			dev_drv->ops->dsp_black(dev_drv, 1);
		if (dev_drv->ops->set_screen_scaler)
			dev_drv->ops->set_screen_scaler(dev_drv, dev_drv->screen0, 0);
	}
	if (dev_drv->uboot_logo && (screen->type != dev_drv->cur_screen->type))
               dev_drv->uboot_logo = 0;
	if (!enable) {
		/* if screen type is different, we do not disable lcdc. */
		if (dev_drv->cur_screen->type != screen->type)
			return 0;

		/* if used one lcdc to dual disp, no need to close win */
		if (rk_fb->disp_mode == ONE_DUAL) {
			dev_drv->cur_screen = dev_drv->screen0;
			dev_drv->ops->load_screen(dev_drv, 1);

			/* force modify dsp size */
			info = rk_fb->fb[dev_drv->fb_index_base];
			info->var.grayscale &= 0xff;
			info->var.grayscale |=
				(dev_drv->cur_screen->mode.xres << 8) +
				(dev_drv->cur_screen->mode.yres << 20);
			mutex_lock(&dev_drv->win_config);
			info->fbops->fb_set_par(info);
			info->fbops->fb_pan_display(&info->var, info);
			mutex_unlock(&dev_drv->win_config);

			if (dev_drv->ops->dsp_black)
				dev_drv->ops->dsp_black(dev_drv, 0);
		} else if (rk_fb->num_lcdc > 1 && rk_fb->disp_policy == DISPLAY_POLICY_BOX) {
			/* If there is more than one lcdc device, we disable
			   the layer which attached to this device */
			for (i = 0; i < dev_drv->lcdc_win_num; i++) {
				if (dev_drv->win[i] && dev_drv->win[i]->state)
					dev_drv->ops->open(dev_drv, i, 0);
			}
		}

		hdmi_switch_complete = 0;
		return 0;
	} else {
		if (dev_drv->screen1)
			dev_drv->cur_screen = dev_drv->screen1;
		memcpy(dev_drv->cur_screen, screen, sizeof(struct rk_screen));
		dev_drv->cur_screen->xsize = dev_drv->cur_screen->mode.xres;
		dev_drv->cur_screen->ysize = dev_drv->cur_screen->mode.yres;
		dev_drv->cur_screen->x_mirror = dev_drv->rotate_mode & X_MIRROR;
		dev_drv->cur_screen->y_mirror = dev_drv->rotate_mode & Y_MIRROR;
	}
	if ((!dev_drv->uboot_logo) ||
	    (rk_fb->disp_policy != DISPLAY_POLICY_BOX)) {
		for (i = 0; i < dev_drv->lcdc_win_num; i++) {
			info = rk_fb->fb[dev_drv->fb_index_base + i];
			fb_par = (struct rk_fb_par *)info->par;
			win_id = dev_drv->ops->fb_get_win_id(dev_drv, info->fix.id);
			if (dev_drv->win[win_id]) {
				if (fb_par->state) {
					if (!dev_drv->win[win_id]->state)
						dev_drv->ops->open(dev_drv, win_id, 1);
					if (!load_screen) {
						dev_drv->ops->load_screen(dev_drv, 1);
						load_screen = 1;
					}
					info->var.activate |= FB_ACTIVATE_FORCE;
					if (rk_fb->disp_mode == ONE_DUAL) {
						info->var.grayscale &= 0xff;
						info->var.grayscale |=
							(dev_drv->cur_screen->xsize << 8) +
							(dev_drv->cur_screen->ysize << 20);
					}

					mutex_lock(&dev_drv->win_config);
					info->fbops->fb_set_par(info);
					info->fbops->fb_pan_display(&info->var, info);
					mutex_unlock(&dev_drv->win_config);
				}
			}
		}
	}else {
		dev_drv->uboot_logo = 0;
	}
	hdmi_switch_complete = 1;
	if (rk_fb->disp_mode == ONE_DUAL) {
		if (dev_drv->ops->set_screen_scaler)
			dev_drv->ops->set_screen_scaler(dev_drv, dev_drv->screen0, 1);
		if (dev_drv->ops->dsp_black)
			dev_drv->ops->dsp_black(dev_drv, 0);
	}
	return 0;
}

/*
 * function:this function current only called by hdmi for
 *	scale the display
 * scale_x: scale rate of x resolution
 * scale_y: scale rate of y resolution
 * lcdc_id: the lcdc id the hdmi attached ,0 or 1
 */
int rk_fb_disp_scale(u8 scale_x, u8 scale_y, u8 lcdc_id)
{
	struct rk_fb *inf = platform_get_drvdata(fb_pdev);
	struct fb_info *info = NULL;
	struct fb_info *pmy_info = NULL;
	struct fb_var_screeninfo *var = NULL;
	struct rk_lcdc_driver *dev_drv = NULL;
	u16 screen_x, screen_y;
	u16 xpos, ypos;
	char name[6];
	struct rk_screen primary_screen;
	rk_fb_get_prmry_screen(&primary_screen);
	if (primary_screen.type == SCREEN_HDMI) {
		return 0;
	}

	sprintf(name, "lcdc%d", lcdc_id);

	if (inf->disp_mode == DUAL) {
		dev_drv = rk_get_lcdc_drv(name);
		if (dev_drv == NULL) {
			printk(KERN_ERR "%s driver not found!", name);
			return -ENODEV;
		}
	} else {
		dev_drv = inf->lcdc_dev_drv[0];
	}

	if (inf->num_lcdc == 1) {
		info = inf->fb[0];
	} else if (inf->num_lcdc == 2) {
		info = inf->fb[dev_drv->lcdc_win_num];
		pmy_info = inf->fb[0];
	}

	var = &info->var;
	screen_x = dev_drv->cur_screen->mode.xres;
	screen_y = dev_drv->cur_screen->mode.yres;

	if (inf->disp_mode != DUAL && dev_drv->screen1) {
		dev_drv->cur_screen->xpos =
		    (screen_x - screen_x * scale_x / 100) >> 1;
		dev_drv->cur_screen->ypos =
		    (screen_y - screen_y * scale_y / 100) >> 1;
		dev_drv->cur_screen->xsize = screen_x * scale_x / 100;
		dev_drv->cur_screen->ysize = screen_y * scale_y / 100;
	} else {
		xpos = (screen_x - screen_x * scale_x / 100) >> 1;
		ypos = (screen_y - screen_y * scale_y / 100) >> 1;
		dev_drv->cur_screen->xsize = screen_x * scale_x / 100;
		dev_drv->cur_screen->ysize = screen_y * scale_y / 100;
		if (inf->disp_mode == ONE_DUAL) {
			var->nonstd &= 0xff;
			var->nonstd |= (xpos << 8) + (ypos << 20);
			var->grayscale &= 0xff;
			var->grayscale |=
				(dev_drv->cur_screen->xsize << 8) +
				(dev_drv->cur_screen->ysize << 20);
		}
	}

	mutex_lock(&dev_drv->win_config);
	info->fbops->fb_set_par(info);
	dev_drv->ops->cfg_done(dev_drv);
	mutex_unlock(&dev_drv->win_config);

	return 0;
}

#if defined(CONFIG_ION_ROCKCHIP)
static int rk_fb_alloc_buffer_by_ion(struct fb_info *fbi,
				     struct rk_lcdc_win *win,
				     unsigned long fb_mem_size)
{
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)fbi->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct ion_handle *handle;
	ion_phys_addr_t phy_addr;
	size_t len;
	int ret = 0;

	if (dev_drv->iommu_enabled)
		handle = ion_alloc(rk_fb->ion_client, (size_t) fb_mem_size, 0,
				   ION_HEAP(ION_VMALLOC_HEAP_ID), 0);
	else
		handle = ion_alloc(rk_fb->ion_client, (size_t) fb_mem_size, 0,
				   ION_HEAP(ION_CMA_HEAP_ID), 0);
	if (IS_ERR(handle)) {
		dev_err(fbi->device, "failed to ion_alloc:%ld\n",
			PTR_ERR(handle));
		return -ENOMEM;
	}

	fb_par->ion_hdl = handle;
	win->area[0].dma_buf = ion_share_dma_buf(rk_fb->ion_client, handle);
	if (IS_ERR_OR_NULL(win->area[0].dma_buf)) {
		printk("ion_share_dma_buf() failed\n");
		goto err_share_dma_buf;
	}
	win->area[0].ion_hdl = handle;
        if (dev_drv->prop == PRMRY)
	        fbi->screen_base = ion_map_kernel(rk_fb->ion_client, handle);
#ifdef CONFIG_ROCKCHIP_IOMMU
	if (dev_drv->iommu_enabled && dev_drv->mmu_dev)
		ret = ion_map_iommu(dev_drv->dev, rk_fb->ion_client, handle,
					(unsigned long *)&phy_addr,
					(unsigned long *)&len);
	else
		ret = ion_phys(rk_fb->ion_client, handle, &phy_addr, &len);
#else
	ret = ion_phys(rk_fb->ion_client, handle, &phy_addr, &len);
#endif
	if (ret < 0) {
		dev_err(fbi->dev, "ion map to get phy addr failed\n");
		goto err_share_dma_buf;
	}
	fbi->fix.smem_start = phy_addr;
	fbi->fix.smem_len = len;
	printk(KERN_INFO "alloc_buffer:ion_phy_addr=0x%lx\n", phy_addr);
	return 0;

err_share_dma_buf:
	ion_free(rk_fb->ion_client, handle);
	return -ENOMEM;
}
#endif

static int rk_fb_alloc_buffer(struct fb_info *fbi)
{
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct rk_fb_par *fb_par = (struct rk_fb_par *)fbi->par;
	struct rk_lcdc_driver *dev_drv = fb_par->lcdc_drv;
	struct rk_lcdc_win *win = NULL;
	int win_id;
	int ret = 0;
	unsigned long fb_mem_size;
#if !defined(CONFIG_ION_ROCKCHIP)
	dma_addr_t fb_mem_phys;
	void *fb_mem_virt;
#endif

	win_id = dev_drv->ops->fb_get_win_id(dev_drv, fbi->fix.id);
	if (win_id < 0)
		return -ENODEV;
	else
		win = dev_drv->win[win_id];

	if (!strcmp(fbi->fix.id, "fb0")) {
		fb_mem_size = get_fb_size();
#if defined(CONFIG_ION_ROCKCHIP)
		if (rk_fb_alloc_buffer_by_ion(fbi, win, fb_mem_size) < 0)
			return -ENOMEM;
#else
		fb_mem_virt = dma_alloc_writecombine(fbi->dev, fb_mem_size,
						     &fb_mem_phys, GFP_KERNEL);
		if (!fb_mem_virt) {
			pr_err("%s: Failed to allocate framebuffer\n",
			       __func__);
			return -ENOMEM;
		}
		fbi->fix.smem_len = fb_mem_size;
		fbi->fix.smem_start = fb_mem_phys;
		fbi->screen_base = fb_mem_virt;
#endif
		memset(fbi->screen_base, 0, fbi->fix.smem_len);
	} else {
		fbi->fix.smem_start = rk_fb->fb[0]->fix.smem_start;
		fbi->fix.smem_len = rk_fb->fb[0]->fix.smem_len;
		fbi->screen_base = rk_fb->fb[0]->screen_base;
	}

	fbi->screen_size = fbi->fix.smem_len;
	fb_par->fb_phy_base = fbi->fix.smem_start;
	fb_par->fb_virt_base = fbi->screen_base;
	fb_par->fb_size = fbi->fix.smem_len;

	pr_info("%s:phy:%lx>>vir:%p>>len:0x%x\n", fbi->fix.id,
		       fbi->fix.smem_start, fbi->screen_base,
		       fbi->fix.smem_len);
	return ret;
}

#if 0
static int rk_release_fb_buffer(struct fb_info *fbi)
{
	/* buffer for fb1 and fb3 are alloc by android */
	if (!strcmp(fbi->fix.id, "fb1") || !strcmp(fbi->fix.id, "fb3"))
		return 0;
	iounmap(fbi->screen_base);
	release_mem_region(fbi->fix.smem_start, fbi->fix.smem_len);
	return 0;
}
#endif

static int init_lcdc_win(struct rk_lcdc_driver *dev_drv,
			 struct rk_lcdc_win *def_win)
{
	int i;
	int lcdc_win_num = dev_drv->lcdc_win_num;

	for (i = 0; i < lcdc_win_num; i++) {
		struct rk_lcdc_win *win = NULL;
		win = kzalloc(sizeof(struct rk_lcdc_win), GFP_KERNEL);
		if (!win) {
			dev_err(dev_drv->dev, "kzmalloc for win fail!");
			return -ENOMEM;
		}

		strcpy(win->name, def_win[i].name);
		win->id = def_win[i].id;
		win->support_3d = def_win[i].support_3d;
		dev_drv->win[i] = win;
	}

	return 0;
}

static int init_lcdc_device_driver(struct rk_fb *rk_fb,
				   struct rk_lcdc_win *def_win, int index)
{
	struct rk_lcdc_driver *dev_drv = rk_fb->lcdc_dev_drv[index];
	struct rk_screen *screen = devm_kzalloc(dev_drv->dev,
						sizeof(struct rk_screen),
						GFP_KERNEL);

	if (!screen) {
		dev_err(dev_drv->dev, "malloc screen for lcdc%d fail!",
			dev_drv->id);
		return -ENOMEM;
	}

	screen->screen_id = 0;
	screen->lcdc_id = dev_drv->id;
	screen->overscan.left = 100;
	screen->overscan.top = 100;
	screen->overscan.right = 100;
	screen->overscan.bottom = 100;

	screen->x_mirror = dev_drv->rotate_mode & X_MIRROR;
	screen->y_mirror = dev_drv->rotate_mode & Y_MIRROR;

	dev_drv->screen0 = screen;
	dev_drv->cur_screen = screen;
	/* devie use one lcdc + rk61x scaler for dual display */
	if (rk_fb->disp_mode == ONE_DUAL) {
		struct rk_screen *screen1 =
				devm_kzalloc(dev_drv->dev,
					     sizeof(struct rk_screen),
					     GFP_KERNEL);
		if (!screen1) {
			dev_err(dev_drv->dev, "malloc screen1 for lcdc%d fail!",
				dev_drv->id);
			return -ENOMEM;
		}
		screen1->screen_id = 1;
		screen1->lcdc_id = 1;
		dev_drv->screen1 = screen1;
	}
	sprintf(dev_drv->name, "lcdc%d", dev_drv->id);
	init_lcdc_win(dev_drv, def_win);
	init_completion(&dev_drv->frame_done);
	spin_lock_init(&dev_drv->cpl_lock);
	mutex_init(&dev_drv->fb_win_id_mutex);
	mutex_init(&dev_drv->win_config);
	mutex_init(&dev_drv->front_lock);
	dev_drv->ops->fb_win_remap(dev_drv, dev_drv->fb_win_map);
	dev_drv->first_frame = 1;
	dev_drv->overscan.left = 100;
	dev_drv->overscan.top = 100;
	dev_drv->overscan.right = 100;
	dev_drv->overscan.bottom = 100;
	rk_disp_pwr_ctr_parse_dt(dev_drv);
	if (dev_drv->prop == PRMRY) {
		if (dev_drv->ops->set_dsp_cabc)
			dev_drv->ops->set_dsp_cabc(dev_drv, dev_drv->cabc_mode);
		rk_fb_set_prmry_screen(screen);
		rk_fb_get_prmry_screen(screen);
	}
	dev_drv->trsm_ops = rk_fb_trsm_ops_get(screen->type);
	if (dev_drv->prop != PRMRY)
		rk_fb_get_prmry_screen(screen);
	dev_drv->output_color = screen->color_mode;

	return 0;
}

#ifdef CONFIG_LOGO_LINUX_BMP
static struct linux_logo *bmp_logo;
static int fb_prewine_bmp_logo(struct fb_info *info, int rotate)
{
	bmp_logo = fb_find_logo(24);
	if (bmp_logo == NULL) {
		printk(KERN_INFO "%s error\n", __func__);
		return 0;
	}
	return 1;
}

static void fb_show_bmp_logo(struct fb_info *info, int rotate)
{
	unsigned char *src = bmp_logo->data;
	unsigned char *dst = info->screen_base;
	int i;
	unsigned int Needwidth = (*(src - 24) << 8) | (*(src - 23));
	unsigned int Needheight = (*(src - 22) << 8) | (*(src - 21));

	for (i = 0; i < Needheight; i++)
		memcpy(dst + info->var.xres * i * 4,
		       src + bmp_logo->width * i * 4, Needwidth * 4);
}
#endif

/*
 * check if the primary lcdc has registerd,
 * the primary lcdc mas register first
 */
bool is_prmry_rk_lcdc_registered(void)
{
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);

	if (rk_fb->lcdc_dev_drv[0])
		return true;
	else
		return false;
}

int rk_fb_register(struct rk_lcdc_driver *dev_drv,
		   struct rk_lcdc_win *win, int id)
{
	struct rk_fb *rk_fb = platform_get_drvdata(fb_pdev);
	struct fb_info *fbi;
	struct rk_fb_par *fb_par = NULL;
	int i = 0, ret = 0, index = 0;

	if (rk_fb->num_lcdc == RK30_MAX_LCDC_SUPPORT)
		return -ENXIO;

	for (i = 0; i < RK30_MAX_LCDC_SUPPORT; i++) {
		if (!rk_fb->lcdc_dev_drv[i]) {
			rk_fb->lcdc_dev_drv[i] = dev_drv;
			rk_fb->lcdc_dev_drv[i]->id = id;
			rk_fb->num_lcdc++;
			break;
		}
	}

	index = i;
	init_lcdc_device_driver(rk_fb, win, index);
	dev_drv->fb_index_base = rk_fb->num_fb;
	for (i = 0; i < dev_drv->lcdc_win_num; i++) {
		fbi = framebuffer_alloc(0, &fb_pdev->dev);
		if (!fbi) {
			dev_err(&fb_pdev->dev, "fb framebuffer_alloc fail!");
			ret = -ENOMEM;
		}
		fb_par = devm_kzalloc(&fb_pdev->dev, sizeof(struct rk_fb_par),
				      GFP_KERNEL);
		if (!fb_par) {
			dev_err(&fb_pdev->dev, "malloc fb_par for fb%d fail!",
				rk_fb->num_fb);
			return -ENOMEM;
		}
		fb_par->id = rk_fb->num_fb;
		fb_par->lcdc_drv = dev_drv;
		fbi->par = (void *)fb_par;
		fbi->var = def_var;
		fbi->fix = def_fix;
		sprintf(fbi->fix.id, "fb%d", rk_fb->num_fb);
		fb_videomode_to_var(&fbi->var, &dev_drv->cur_screen->mode);
		fbi->var.grayscale |=
		    (fbi->var.xres << 8) + (fbi->var.yres << 20);
#if defined(CONFIG_LOGO_LINUX_BMP)
		fbi->var.bits_per_pixel = 32;
#else
		fbi->var.bits_per_pixel = 16;
#endif
		fbi->fix.line_length =
		    (fbi->var.xres_virtual) * (fbi->var.bits_per_pixel >> 3);
		fbi->var.width = dev_drv->cur_screen->width;
		fbi->var.height = dev_drv->cur_screen->height;
		fbi->var.pixclock = dev_drv->pixclock;
		if (dev_drv->iommu_enabled)
			fb_ops.fb_mmap = rk_fb_mmap;
		fbi->fbops = &fb_ops;
		fbi->flags = FBINFO_FLAG_DEFAULT;
		fbi->pseudo_palette = dev_drv->win[i]->pseudo_pal;
		ret = register_framebuffer(fbi);
		if (ret < 0) {
			dev_err(&fb_pdev->dev,
				"%s fb%d register_framebuffer fail!\n",
				__func__, rk_fb->num_fb);
			return ret;
		}
		rkfb_create_sysfs(fbi);
		rk_fb->fb[rk_fb->num_fb] = fbi;
		dev_info(fbi->dev, "rockchip framebuffer registerd:%s\n",
			 fbi->fix.id);
		rk_fb->num_fb++;

		if (i == 0) {
			init_waitqueue_head(&dev_drv->vsync_info.wait);
			init_waitqueue_head(&dev_drv->update_regs_wait);
			ret = device_create_file(fbi->dev, &dev_attr_vsync);
			if (ret)
				dev_err(fbi->dev,
					"failed to create vsync file\n");
			dev_drv->vsync_info.thread =
			    kthread_run(rk_fb_wait_for_vsync_thread, dev_drv,
					"fb-vsync");
			if (dev_drv->vsync_info.thread == ERR_PTR(-ENOMEM)) {
				dev_err(fbi->dev,
					"failed to run vsync thread\n");
				dev_drv->vsync_info.thread = NULL;
			}
			dev_drv->vsync_info.active = 1;

			mutex_init(&dev_drv->output_lock);

			INIT_LIST_HEAD(&dev_drv->update_regs_list);
			mutex_init(&dev_drv->update_regs_list_lock);
			init_kthread_worker(&dev_drv->update_regs_worker);

			dev_drv->update_regs_thread =
			    kthread_run(kthread_worker_fn,
					&dev_drv->update_regs_worker, "rk-fb");
			if (IS_ERR(dev_drv->update_regs_thread)) {
				int err = PTR_ERR(dev_drv->update_regs_thread);
				dev_drv->update_regs_thread = NULL;

				printk("failed to run update_regs thread\n");
				return err;
			}
			init_kthread_work(&dev_drv->update_regs_work,
					  rk_fb_update_regs_handler);

			dev_drv->timeline =
			    sw_sync_timeline_create("fb-timeline");
			dev_drv->timeline_max = 1;
		}
	}

	/* show logo for primary display device */
	if (dev_drv->prop == PRMRY) {
		u16 xact, yact;
		int format;
		u32 dsp_addr;
		struct fb_info *main_fbi = rk_fb->fb[0];
		main_fbi->fbops->fb_open(main_fbi, 1);

#if defined(CONFIG_ROCKCHIP_IOMMU)
		if (dev_drv->iommu_enabled) {
			if (dev_drv->mmu_dev)
				rockchip_iovmm_set_fault_handler(dev_drv->dev,
						rk_fb_sysmmu_fault_handler);
		}
#endif

		rk_fb_alloc_buffer(main_fbi);	/* only alloc memory for main fb */
		dev_drv->uboot_logo = support_uboot_display();

		if (dev_drv->uboot_logo &&
		    uboot_logo_offset && uboot_logo_base) {
			int width, height, bits;
			phys_addr_t start = uboot_logo_base + uboot_logo_offset;
			unsigned int size = uboot_logo_size - uboot_logo_offset;
			unsigned int nr_pages;
			struct page **pages;
			char *vaddr;
			int i = 0;

			if (dev_drv->ops->get_dspbuf_info)
				dev_drv->ops->get_dspbuf_info(dev_drv, &xact,
					&yact, &format,	&dsp_addr);
			nr_pages = size >> PAGE_SHIFT;
			pages = kzalloc(sizeof(struct page) * nr_pages,
					GFP_KERNEL);
			while (i < nr_pages) {
				pages[i] = phys_to_page(start);
				start += PAGE_SIZE;
				i++;
			}
			vaddr = vmap(pages, nr_pages, VM_MAP,
					pgprot_writecombine(PAGE_KERNEL));
			if (!vaddr) {
				pr_err("failed to vmap phy addr %x\n",
					uboot_logo_base + uboot_logo_offset);
				return -1;
			}

			if(bmpdecoder(vaddr, main_fbi->screen_base, &width,
				      &height, &bits)) {
				kfree(pages);
				vunmap(vaddr);
				return 0;
			}
			kfree(pages);
			vunmap(vaddr);
			if (dev_drv->uboot_logo &&
			    (width != xact || height != yact)) {
				pr_err("can't support uboot kernel logo use different size [%dx%d] != [%dx%d]\n",
					xact, yact, width, height);
				return 0;
			}

			if (dev_drv->ops->post_dspbuf) {
				dev_drv->ops->post_dspbuf(dev_drv,
					main_fbi->fix.smem_start,
					rk_fb_data_fmt(0, bits),
					width, height, width * bits >> 5);
			}
			if (dev_drv->iommu_enabled) {
				rk_fb_poll_wait_frame_complete();
				if (dev_drv->ops->mmu_en)
					dev_drv->ops->mmu_en(dev_drv);
				freed_index = 0;
			}

			return 0;
		} else if (dev_drv->uboot_logo && uboot_logo_base) {
			phys_addr_t start = uboot_logo_base;
			int logo_len, i=0;
			unsigned int nr_pages;
			struct page **pages;
			char *vaddr;

			dev_drv->ops->get_dspbuf_info(dev_drv, &xact,
					              &yact, &format,
						      &start);
			logo_len = rk_fb_pixel_width(format) * xact * yact >> 3;
			if (logo_len > uboot_logo_size ||
			    logo_len > main_fbi->fix.smem_len) {
				pr_err("logo size > uboot reserve buffer size\n");
				return -1;
			}

			nr_pages = uboot_logo_size >> PAGE_SHIFT;
			pages = kzalloc(sizeof(struct page) * nr_pages,
					GFP_KERNEL);
			while (i < nr_pages) {
				pages[i] = phys_to_page(start);
				start += PAGE_SIZE;
				i++;
			}
			vaddr = vmap(pages, nr_pages, VM_MAP,
					pgprot_writecombine(PAGE_KERNEL));
			if (!vaddr) {
				pr_err("failed to vmap phy addr %x\n",
					uboot_logo_base);
				return -1;
			}

			memcpy(main_fbi->screen_base, vaddr, logo_len);

			kfree(pages);
			vunmap(vaddr);

			dev_drv->ops->post_dspbuf(dev_drv,
					main_fbi->fix.smem_start,
					format,	xact, yact,
					xact * rk_fb_pixel_width(format) >> 5);
			if (dev_drv->iommu_enabled) {
				rk_fb_poll_wait_frame_complete();
				if (dev_drv->ops->mmu_en)
					dev_drv->ops->mmu_en(dev_drv);
				freed_index = 0;
			}
			return 0;
		}
#if defined(CONFIG_LOGO)
		main_fbi->fbops->fb_set_par(main_fbi);
#if  defined(CONFIG_LOGO_LINUX_BMP)
		if (fb_prewine_bmp_logo(main_fbi, FB_ROTATE_UR)) {
			fb_set_cmap(&main_fbi->cmap, main_fbi);
			fb_show_bmp_logo(main_fbi, FB_ROTATE_UR);
		}
#else
		if (fb_prepare_logo(main_fbi, FB_ROTATE_UR)) {
			fb_set_cmap(&main_fbi->cmap, main_fbi);
			fb_show_logo(main_fbi, FB_ROTATE_UR);
		}
#endif
		main_fbi->fbops->fb_pan_display(&main_fbi->var, main_fbi);
#endif
	} else {
		struct fb_info *extend_fbi = rk_fb->fb[rk_fb->num_fb >> 1];

		rk_fb_alloc_buffer(extend_fbi);
	}
	return 0;
}

int rk_fb_unregister(struct rk_lcdc_driver *dev_drv)
{
	struct rk_fb *fb_inf = platform_get_drvdata(fb_pdev);
	struct fb_info *fbi;
	int fb_index_base = dev_drv->fb_index_base;
	int fb_num = dev_drv->lcdc_win_num;
	int i = 0;

	if (fb_inf->lcdc_dev_drv[i]->vsync_info.thread) {
		fb_inf->lcdc_dev_drv[i]->vsync_info.irq_stop = 1;
		kthread_stop(fb_inf->lcdc_dev_drv[i]->vsync_info.thread);
	}

	for (i = 0; i < fb_num; i++)
		kfree(dev_drv->win[i]);

	for (i = fb_index_base; i < (fb_index_base + fb_num); i++) {
		fbi = fb_inf->fb[i];
		unregister_framebuffer(fbi);
		/* rk_release_fb_buffer(fbi); */
		framebuffer_release(fbi);
	}
	fb_inf->lcdc_dev_drv[dev_drv->id] = NULL;
	fb_inf->num_lcdc--;

	return 0;
}

static int rk_fb_probe(struct platform_device *pdev)
{
	struct rk_fb *rk_fb = NULL;
	struct device_node *np = pdev->dev.of_node;
	u32 mode;

	if (!np) {
		dev_err(&pdev->dev, "Missing device tree node.\n");
		return -EINVAL;
	}

	rk_fb = devm_kzalloc(&pdev->dev, sizeof(struct rk_fb), GFP_KERNEL);
	if (!rk_fb) {
		dev_err(&pdev->dev, "kmalloc for rk fb fail!");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, rk_fb);

	if (!of_property_read_u32(np, "rockchip,disp-mode", &mode)) {
		rk_fb->disp_mode = mode;

	} else {
		dev_err(&pdev->dev, "no disp-mode node found!");
		return -ENODEV;
	}
	
	if (!of_property_read_u32(np, "rockchip,disp-policy", &mode)) {
		rk_fb->disp_policy = mode;
		pr_info("fb disp policy is %s\n", rk_fb->disp_policy ? "box":"sdk");
	}

	if (!of_property_read_u32(np, "rockchip,uboot-logo-on", &uboot_logo_on))
		printk(KERN_DEBUG "uboot-logo-on:%d\n", uboot_logo_on);

	dev_set_name(&pdev->dev, "rockchip-fb");
#if defined(CONFIG_ION_ROCKCHIP)
	rk_fb->ion_client = rockchip_ion_client_create("rk_fb");
	if (IS_ERR(rk_fb->ion_client)) {
		dev_err(&pdev->dev, "failed to create ion client for rk fb");
		return PTR_ERR(rk_fb->ion_client);
	} else {
		dev_info(&pdev->dev, "rk fb ion client create success!\n");
	}
#endif

	fb_pdev = pdev;
	dev_info(&pdev->dev, "rockchip framebuffer driver probe\n");
	return 0;
}

#if defined(CONFIG_PM)
static int rk_fb_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}
static int rk_fb_resume(struct platform_device *pdev)
{
	return 0;
}
#endif
static int rk_fb_remove(struct platform_device *pdev)
{
	struct rk_fb *rk_fb = platform_get_drvdata(pdev);

	kfree(rk_fb);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static void rk_fb_shutdown(struct platform_device *pdev)
{
	struct rk_fb *rk_fb = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < rk_fb->num_lcdc; i++) {
		if (!rk_fb->lcdc_dev_drv[i])
			continue;
	}
}

static const struct of_device_id rkfb_dt_ids[] = {
	{.compatible = "rockchip,rk-fb",},
	{}
};

static struct platform_driver rk_fb_driver = {
	.probe = rk_fb_probe,
	.remove = rk_fb_remove,
	.driver = {
		   .name = "rk-fb",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(rkfb_dt_ids),
		   },
#if defined(CONFIG_PM)
	.suspend = rk_fb_suspend,
	.resume = rk_fb_resume,
#endif
	.shutdown = rk_fb_shutdown,
};

static int __init rk_fb_init(void)
{
	return platform_driver_register(&rk_fb_driver);
}

static void __exit rk_fb_exit(void)
{
	platform_driver_unregister(&rk_fb_driver);
}

fs_initcall(rk_fb_init);
module_exit(rk_fb_exit);
