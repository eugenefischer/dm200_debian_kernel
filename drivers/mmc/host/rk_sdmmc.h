/*
 * Synopsys DesignWare Multimedia Card Interface driver
 *  (Based on NXP driver for lpc 31xx)
 *
 * Copyright (C) 2009 NXP Semiconductors
 * Copyright (C) 2009, 2010 Imagination Technologies Ltd.
 *
 * Copyright (C) 2014 Fuzhou Rockchip Electronics Co.Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _DW_MMC_H_
#define _DW_MMC_H_
#include "rk_sdmmc_dbg.h"

#define DW_MMC_240A             0x240a
#define DW_MMC_270A             0x270a

#define SDMMC_CTRL              0x000
#define SDMMC_PWREN             0x004
#define SDMMC_CLKDIV            0x008
#define SDMMC_CLKSRC            0x00c
#define SDMMC_CLKENA            0x010
#define SDMMC_TMOUT             0x014
#define SDMMC_CTYPE             0x018
#define SDMMC_BLKSIZ            0x01c
#define SDMMC_BYTCNT            0x020
#define SDMMC_INTMASK           0x024
#define SDMMC_CMDARG            0x028
#define SDMMC_CMD               0x02c
#define SDMMC_RESP0             0x030
#define SDMMC_RESP1             0x034
#define SDMMC_RESP2             0x038
#define SDMMC_RESP3             0x03c
#define SDMMC_MINTSTS           0x040
#define SDMMC_RINTSTS		0x044
#define SDMMC_STATUS		0x048
#define SDMMC_FIFOTH		0x04c
#define SDMMC_CDETECT		0x050
#define SDMMC_WRTPRT		0x054
#define SDMMC_GPIO              0x058
#define SDMMC_TCBCNT            0x05c
#define SDMMC_TBBCNT		0x060
#define SDMMC_DEBNCE		0x064
#define SDMMC_USRID             0x068
#define SDMMC_VERID             0x06c
#define SDMMC_HCON              0x070
#define SDMMC_UHS_REG		0x074
#define SDMMC_RST_N             0x078
#define SDMMC_BMOD              0x080
#define SDMMC_PLDMND            0x084
#define SDMMC_DBADDR		0x088
#define SDMMC_IDSTS             0x08c
#define SDMMC_IDINTEN           0x090
#define SDMMC_DSCADDR           0x094
#define SDMMC_BUFADDR           0x098
#define SDMMC_CDTHRCTL          0x100
#define SDMMC_DATA(x)           (x)

/*
 * Data offset is difference according to Version
 * Lower than 2.40a : data register offest is 0x100
 */
#define DATA_OFFSET             0x100
#define DATA_240A_OFFSET	0x200

/* shift bit field */
#define _SBF(f, v)      ((v) << (f))


struct sdmmc_reg {
u32   addr;
char    *name;
};


/* Control register defines */
#define SDMMC_CTRL_USE_IDMAC		BIT(25)
#define SDMMC_CTRL_CEATA_INT_EN		BIT(11)
#define SDMMC_CTRL_SEND_AS_CCSD		BIT(10)
#define SDMMC_CTRL_SEND_CCSD		BIT(9)
#define SDMMC_CTRL_ABRT_READ_DATA	BIT(8)
#define SDMMC_CTRL_SEND_IRQ_RESP	BIT(7)
#define SDMMC_CTRL_READ_WAIT		BIT(6)
#define SDMMC_CTRL_DMA_ENABLE		BIT(5)
#define SDMMC_CTRL_INT_ENABLE		BIT(4)
#define SDMMC_CTRL_DMA_RESET		BIT(2)
#define SDMMC_CTRL_FIFO_RESET		BIT(1)
#define SDMMC_CTRL_RESET		BIT(0)
/* Clock Enable register defines */
#define SDMMC_CLKEN_LOW_PWR		BIT(16)
#define SDMMC_CLKEN_ENABLE		BIT(0)
/* time-out register defines */
#define SDMMC_TMOUT_DATA(n)		_SBF(8, (n))
#define SDMMC_TMOUT_DATA_MSK		0xFFFFFF00
#define SDMMC_TMOUT_RESP(n)		((n) & 0xFF)
#define SDMMC_TMOUT_RESP_MSK		0xFF
/* card-type register defines */
#define SDMMC_CTYPE_8BIT		BIT(16)
#define SDMMC_CTYPE_4BIT		BIT(0)
#define SDMMC_CTYPE_1BIT		0
/* Interrupt status & mask register defines */
#define SDMMC_INT_SDIO(n)		BIT(16 + (n))
#define SDMMC_INT_EBE			BIT(15)
#define SDMMC_INT_ACD			BIT(14)
#define SDMMC_INT_SBE			BIT(13)
#define SDMMC_INT_HLE			BIT(12)
#define SDMMC_INT_FRUN			BIT(11)
#define SDMMC_INT_HTO			BIT(10)
#define SDMMC_INT_VSI                   SDMMC_INT_HTO   // VSI => Voltage Switch Interrupt,Volt_Switch_int
#define SDMMC_INT_DRTO			BIT(9)
#define SDMMC_INT_RTO			BIT(8)
#define SDMMC_INT_DCRC			BIT(7)
#define SDMMC_INT_RCRC			BIT(6)
#define SDMMC_INT_RXDR			BIT(5)
#define SDMMC_INT_TXDR			BIT(4)
#define SDMMC_INT_DATA_OVER		BIT(3)
#define SDMMC_INT_CMD_DONE		BIT(2)
#define SDMMC_INT_RESP_ERR		BIT(1)
#define SDMMC_INT_CD			BIT(0)
#define SDMMC_INT_ERROR			0xbfc2
/* Command register defines */
#define SDMMC_CMD_START                 BIT(31)
#define SDMMC_CMD_USE_HOLD_REG          BIT(29)
#define SDMMC_CMD_VOLT_SWITCH           BIT(28)      //Voltage switch bit
#define SDMMC_CMD_VOLT_SWITCH           BIT(28)      //Voltage switch bit
#define SDMMC_CMD_BOOT_MODE             BIT(27)      //set boot mode.
#define SDMMC_CMD_DISABLE_BOOT          BIT(26)      //disable boot.
#define SDMMC_CMD_EXPECT_BOOT_ACK       BIT(25)      //Expect Boot Acknowledge.
#define SDMMC_CMD_ENABLE_BOOT           BIT(24)      //be set only for mandatory boot mode.
#define SDMMC_CMD_CCS_EXP		BIT(23)
#define SDMMC_CMD_CEATA_RD		BIT(22)
#define SDMMC_CMD_UPD_CLK		BIT(21)
#define SDMMC_CMD_INIT			BIT(15)
#define SDMMC_CMD_STOP			BIT(14)
#define SDMMC_CMD_PRV_DAT_WAIT		BIT(13)
#define SDMMC_CMD_SEND_STOP		BIT(12)
#define SDMMC_CMD_STRM_MODE		BIT(11)
#define SDMMC_CMD_DAT_WR		BIT(10)
#define SDMMC_CMD_DAT_EXP		BIT(9)
#define SDMMC_CMD_RESP_CRC		BIT(8)
#define SDMMC_CMD_RESP_LONG		BIT(7)
#define SDMMC_CMD_RESP_EXP		BIT(6)
#define SDMMC_CMD_INDX(n)		((n) & 0x1F)
/* Status register defines */
#define SDMMC_GET_FCNT(x)               (((x)>>17) & 0x1FFF)
#define SDMMC_STAUTS_MC_BUSY            BIT(10)
#define SDMMC_STAUTS_DATA_BUSY          BIT(9)          //Card busy
#define SDMMC_CMD_FSM_MASK              (0x0F << 4)	    //Command FSM status mask
#define SDMMC_CMD_FSM_IDLE              (0x00)			//CMD FSM is IDLE
#define SDMMC_STAUTS_FIFO_FULL          BIT(3)          //FIFO is full status
#define SDMMC_STAUTS_FIFO_EMPTY         BIT(2)          //FIFO is empty status

/* Control SDMMC_UHS_REG defines (base+ 0x74)*/
#define SDMMC_UHS_DDR_MODE              BIT(16)     // 0--Non DDR Mode; 1--DDR mode 
#define SDMMC_UHS_VOLT_REG_18           BIT(0)      // 0--3.3v; 1--1.8V

/* FIFOTH register defines */
#define SDMMC_SET_FIFOTH(m, r, t)	(((m) & 0x7) << 28 | \
					 ((r) & 0xFFF) << 16 | \
					 ((t) & 0xFFF))
/* Internal DMAC interrupt defines */
#define SDMMC_IDMAC_INT_AI		BIT(9)
#define SDMMC_IDMAC_INT_NI		BIT(8)
#define SDMMC_IDMAC_INT_CES		BIT(5)
#define SDMMC_IDMAC_INT_DU		BIT(4)
#define SDMMC_IDMAC_INT_FBE		BIT(2)
#define SDMMC_IDMAC_INT_RI		BIT(1)
#define SDMMC_IDMAC_INT_TI		BIT(0)
/* Internal DMAC bus mode bits */
#define SDMMC_IDMAC_ENABLE		BIT(7)
#define SDMMC_IDMAC_FB			BIT(1)
#define SDMMC_IDMAC_SWRESET		BIT(0)
/* Version ID register define */
#define SDMMC_GET_VERID(x)		((x) & 0xFFFF)
/* Card read threshold */
#define SDMMC_SET_RD_THLD(v, x)		(((v) & 0x1FFF) << 16 | (x))

/* Register access macros */
#define mci_readl(dev, reg)			\
	__raw_readl((dev)->regs + SDMMC_##reg)
#define mci_writel(dev, reg, value)			\
	__raw_writel((value), (dev)->regs + SDMMC_##reg)
#define mci_readreg(dev, addr)			\
        __raw_readl((dev)->regs + addr)
#define mci_writereg(dev, addr, value)		\
  __raw_writel((value), (dev)->regs + addr)


/* 16-bit FIFO access macros */
#define mci_readw(dev, reg)			\
	__raw_readw((dev)->regs + SDMMC_##reg)
#define mci_writew(dev, reg, value)			\
	__raw_writew((value), (dev)->regs + SDMMC_##reg)

/* 64-bit FIFO access macros */
#ifdef readq
#define mci_readq(dev, reg)			\
	__raw_readq((dev)->regs + SDMMC_##reg)
#define mci_writeq(dev, reg, value)			\
	__raw_writeq((value), (dev)->regs + SDMMC_##reg)
#else
/*
 * Dummy readq implementation for architectures that don't define it.
 *
 * We would assume that none of these architectures would configure
 * the IP block with a 64bit FIFO width, so this code will never be
 * executed on those machines. Defining these macros here keeps the
 * rest of the code free from ifdefs.
 */
#define mci_readq(dev, reg)			\
	(*(volatile u64 __force *)((dev)->regs + SDMMC_##reg))
#define mci_writeq(dev, reg, value)			\
	(*(volatile u64 __force *)((dev)->regs + SDMMC_##reg) = (value))
#endif
extern void rk_send_wakeup_key(void);
extern int dw_mci_probe(struct dw_mci *host);
extern void dw_mci_remove(struct dw_mci *host);
#ifdef CONFIG_PM
extern int dw_mci_suspend(struct dw_mci *host);
extern int dw_mci_resume(struct dw_mci *host);
#endif

/**
 * struct dw_mci_slot - MMC slot state
 * @mmc: The mmc_host representing this slot.
 * @host: The MMC controller this slot is using.
 * @quirks: Slot-level quirks (DW_MCI_SLOT_QUIRK_XXX)
 * @wp_gpio: If gpio_is_valid() we'll use this to read write protect.
 * @ctype: Card type for this slot.
 * @mrq: mmc_request currently being processed or waiting to be
 *	processed, or NULL when the slot is idle.
 * @queue_node: List node for placing this node in the @queue list of
 *	&struct dw_mci.
 * @clock: Clock rate configured by set_ios(). Protected by host->lock.
 * @__clk_old: The last updated clock with reflecting clock divider.
 *	Keeping track of this helps us to avoid spamming the console
 *	with CONFIG_MMC_CLKGATE.
 * @flags: Random state bits associated with the slot.
 * @id: Number of this slot.
 * @last_detect_state: Most recently observed card detect state.
 */
struct dw_mci_slot {
	struct mmc_host		*mmc;
	struct dw_mci		*host;

	int			quirks;
	int			wp_gpio;
	int                     cd_gpio;
        int                     pwr_en_gpio;
	u32			ctype;
	u32         pre_ctype;

	struct mmc_request	*mrq;
	struct list_head	queue_node;

	unsigned int		clock;
	unsigned int		__clk_old;

	unsigned long		flags;
#define DW_MMC_CARD_PRESENT	0
#define DW_MMC_CARD_NEED_INIT	1
	int			id;
	int			last_detect_state;
};

struct dw_mci_tuning_data {
	const u8 *blk_pattern;
	unsigned int blksz;

	u8 con_id;
	u8 tuning_type;
};

/**
 * dw_mci driver data - dw-mshc implementation specific driver data.
 * @caps: mmc subsystem specified capabilities of the controller(s).
 * @hold_reg_flag: Fixed the value of HOLG_REG
 * @init: early implementation specific initialization.
 * @setup_clock: implementation specific clock configuration.
 * @prepare_command: handle CMD register extensions.
 * @set_ios: handle bus specific extensions.
 * @parse_dt: parse implementation specific device tree properties.
 *
 * Provide controller implementation specific extensions. The usage of this
 * data structure is fully optional and usage of each member in this structure
 * is optional as well.
 */
struct dw_mci_drv_data {
	unsigned long	*caps;
	unsigned int    *hold_reg_flag;
	int		(*init)(struct dw_mci *host);
	int		(*setup_clock)(struct dw_mci *host);
	void		(*prepare_command)(struct dw_mci *host, u32 *cmdr);
	void		(*set_ios)(struct dw_mci *host, struct mmc_ios *ios);
	int		(*parse_dt)(struct dw_mci *host);
	int		(*execute_tuning)(struct dw_mci_slot *slot, u32 opcode,
					struct dw_mci_tuning_data *tuning_data);
};
#endif /* _DW_MMC_H_ */
