/*
 * DHD Bus Module for PCIE
 *
 * Copyright (C) 1999-2018, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id: dhd_pcie.c 740024 2018-01-10 12:20:47Z $
 */

/* include files */
#include <typedefs.h>
#include <bcmutils.h>
#include <bcmdevs.h>
#include <siutils.h>
#include <hndsoc.h>
#include <hndpmu.h>
#include <etd.h>
#include <hnd_debug.h>
#include <sbchipc.h>
#include <hnd_armtrap.h>
#if defined(DHD_DEBUG)
#include <hnd_cons.h>
#endif /* defined(DHD_DEBUG) */
#include <dngl_stats.h>
#include <pcie_core.h>
#include <dhd.h>
#include <dhd_bus.h>
#include <dhd_flowring.h>
#include <dhd_proto.h>
#include <dhd_dbg.h>
#include <dhd_debug.h>
#include <dhd_daemon.h>
#include <dhdioctl.h>
#include <sdiovar.h>
#include <bcmmsgbuf.h>
#include <pcicfg.h>
#include <dhd_pcie.h>
#include <bcmpcie.h>
#include <bcmendian.h>
#ifdef DHDTCPACK_SUPPRESS
#include <dhd_ip.h>
#endif /* DHDTCPACK_SUPPRESS */
#include <bcmevent.h>

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
#include <linux/pm_runtime.h>
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

#if defined(DEBUGGER) || defined(DHD_DSCOPE)
#include <debugger.h>
#endif /* DEBUGGER || DHD_DSCOPE */

#define MEMBLOCK	2048		/* Block size used for downloading of dongle image */
#define MAX_WKLK_IDLE_CHECK	3	/* times wake_lock checked before deciding not to suspend */

#define ARMCR4REG_BANKIDX	(0x40/sizeof(uint32))
#define ARMCR4REG_BANKPDA	(0x4C/sizeof(uint32))
/* Temporary war to fix precommit till sync issue between trunk & precommit branch is resolved */

/* CTO Prevention Recovery */
#ifdef BCMQT_HW
#define CTO_TO_CLEAR_WAIT_MS 10000
#define CTO_TO_CLEAR_WAIT_MAX_CNT 100
#else
#define CTO_TO_CLEAR_WAIT_MS 1000
#define CTO_TO_CLEAR_WAIT_MAX_CNT 10
#endif // endif

/* Fetch address of a member in the pciedev_shared structure in dongle memory */
#define DHD_PCIE_SHARED_MEMBER_ADDR(bus, member) \
	(bus)->shared_addr + OFFSETOF(pciedev_shared_t, member)

/* Fetch address of a member in rings_info_ptr structure in dongle memory */
#define DHD_RING_INFO_MEMBER_ADDR(bus, member) \
	(bus)->pcie_sh->rings_info_ptr + OFFSETOF(ring_info_t, member)

/* Fetch address of a member in the ring_mem structure in dongle memory */
#define DHD_RING_MEM_MEMBER_ADDR(bus, ringid, member) \
	(bus)->ring_sh[ringid].ring_mem_addr + OFFSETOF(ring_mem_t, member)

#if defined(SUPPORT_MULTIPLE_BOARD_REV)
	extern unsigned int system_rev;
#endif /* SUPPORT_MULTIPLE_BOARD_REV */

int dhd_dongle_memsize;
int dhd_dongle_ramsize;
static int dhdpcie_checkdied(dhd_bus_t *bus, char *data, uint size);
static int dhdpcie_bus_readconsole(dhd_bus_t *bus);
#if defined(DHD_FW_COREDUMP)
struct dhd_bus *g_dhd_bus = NULL;
static int dhdpcie_mem_dump(dhd_bus_t *bus);
#endif /* DHD_FW_COREDUMP */

static int dhdpcie_bus_membytes(dhd_bus_t *bus, bool write, ulong address, uint8 *data, uint size);
static int dhdpcie_bus_doiovar(dhd_bus_t *bus, const bcm_iovar_t *vi, uint32 actionid,
	const char *name, void *params,
	int plen, void *arg, int len, int val_size);
static int dhdpcie_bus_lpback_req(struct  dhd_bus *bus, uint32 intval);
static int dhdpcie_bus_dmaxfer_req(struct  dhd_bus *bus,
	uint32 len, uint32 srcdelay, uint32 destdelay,
	uint32 d11_lpbk, uint32 core_num, uint32 wait);
static int dhdpcie_bus_download_state(dhd_bus_t *bus, bool enter);
static int _dhdpcie_download_firmware(struct dhd_bus *bus);
static int dhdpcie_download_firmware(dhd_bus_t *bus, osl_t *osh);
static int dhdpcie_bus_write_vars(dhd_bus_t *bus);
static bool dhdpcie_bus_process_mailbox_intr(dhd_bus_t *bus, uint32 intstatus);
static bool dhdpci_bus_read_frames(dhd_bus_t *bus);
static int dhdpcie_readshared(dhd_bus_t *bus);
static void dhdpcie_init_shared_addr(dhd_bus_t *bus);
static bool dhdpcie_dongle_attach(dhd_bus_t *bus);
static void dhdpcie_bus_dongle_setmemsize(dhd_bus_t *bus, int mem_size);
static void dhdpcie_bus_release_dongle(dhd_bus_t *bus, osl_t *osh,
	bool dongle_isolation, bool reset_flag);
static void dhdpcie_bus_release_malloc(dhd_bus_t *bus, osl_t *osh);
static int dhdpcie_downloadvars(dhd_bus_t *bus, void *arg, int len);
static uint8 dhdpcie_bus_rtcm8(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_wtcm8(dhd_bus_t *bus, ulong offset, uint8 data);
static void dhdpcie_bus_wtcm16(dhd_bus_t *bus, ulong offset, uint16 data);
static uint16 dhdpcie_bus_rtcm16(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_wtcm32(dhd_bus_t *bus, ulong offset, uint32 data);
static uint32 dhdpcie_bus_rtcm32(dhd_bus_t *bus, ulong offset);
#ifdef DHD_SUPPORT_64BIT
static void dhdpcie_bus_wtcm64(dhd_bus_t *bus, ulong offset, uint64 data) __attribute__ ((used));
static uint64 dhdpcie_bus_rtcm64(dhd_bus_t *bus, ulong offset) __attribute__ ((used));
#endif /* DHD_SUPPORT_64BIT */
static void dhdpcie_bus_cfg_set_bar0_win(dhd_bus_t *bus, uint32 data);
static void dhdpcie_bus_reg_unmap(osl_t *osh, volatile char *addr, int size);
static int dhdpcie_cc_nvmshadow(dhd_bus_t *bus, struct bcmstrbuf *b);
static void dhdpcie_fw_trap(dhd_bus_t *bus);
static void dhd_fillup_ring_sharedptr_info(dhd_bus_t *bus, ring_info_t *ring_info);
extern void dhd_dpc_enable(dhd_pub_t *dhdp);
extern void dhd_dpc_kill(dhd_pub_t *dhdp);

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
static void dhdpcie_handle_mb_data(dhd_bus_t *bus);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

#ifdef IDLE_TX_FLOW_MGMT
static void dhd_bus_check_idle_scan(dhd_bus_t *bus);
static void dhd_bus_idle_scan(dhd_bus_t *bus);
#endif /* IDLE_TX_FLOW_MGMT */

#ifdef EXYNOS_PCIE_DEBUG
extern void exynos_pcie_register_dump(int ch_num);
#endif /* EXYNOS_PCIE_DEBUG */

#define     PCI_VENDOR_ID_BROADCOM          0x14e4

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
#define MAX_D3_ACK_TIMEOUT	100
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

#define DHD_DEFAULT_DOORBELL_TIMEOUT 200	/* ms */
static bool dhdpcie_check_firmware_compatible(uint32 f_api_version, uint32 h_api_version);
static void dhdpcie_cto_error_recovery(struct dhd_bus *bus);

static int dhdpcie_init_d11status(struct dhd_bus *bus);

static void dhdpcie_wrt_rnd(struct dhd_bus *bus);

extern uint16 dhd_prot_get_h2d_max_txpost(dhd_pub_t *dhd);
extern void dhd_prot_set_h2d_max_txpost(dhd_pub_t *dhd, uint16 max_txpost);

/* IOVar table */
enum {
	IOV_INTR = 1,
	IOV_MEMSIZE,
	IOV_SET_DOWNLOAD_STATE,
	IOV_DEVRESET,
	IOV_VARS,
	IOV_MSI_SIM,
	IOV_PCIE_LPBK,
	IOV_CC_NVMSHADOW,
	IOV_RAMSIZE,
	IOV_RAMSTART,
	IOV_SLEEP_ALLOWED,
	IOV_PCIE_DMAXFER,
	IOV_PCIE_SUSPEND,
	IOV_DONGLEISOLATION,
	IOV_LTRSLEEPON_UNLOOAD,
	IOV_METADATA_DBG,
	IOV_RX_METADATALEN,
	IOV_TX_METADATALEN,
	IOV_TXP_THRESHOLD,
	IOV_BUZZZ_DUMP,
	IOV_DUMP_RINGUPD_BLOCK,
	IOV_DMA_RINGINDICES,
	IOV_FORCE_FW_TRAP,
	IOV_DB1_FOR_MB,
	IOV_FLOW_PRIO_MAP,
#ifdef DHD_PCIE_RUNTIMEPM
	IOV_IDLETIME,
#endif /* DHD_PCIE_RUNTIMEPM */
	IOV_RXBOUND,
	IOV_TXBOUND,
	IOV_HANGREPORT,
	IOV_H2D_MAILBOXDATA,
	IOV_INFORINGS,
	IOV_H2D_PHASE,
	IOV_H2D_ENABLE_TRAP_BADPHASE,
	IOV_H2D_TXPOST_MAX_ITEM,
	IOV_TRAPDATA,
	IOV_TRAPDATA_RAW,
	IOV_CTO_PREVENTION,
	IOV_PCIE_WD_RESET,
	IOV_DUMP_DONGLE,
	IOV_IDMA_ENABLE,
	IOV_IFRM_ENABLE,
	IOV_CLEAR_RING,
	IOV_DAR_ENABLE,
	IOV_DNGL_CAPS,   /**< returns string with dongle capabilities */
#if defined(DEBUGGER) || defined(DHD_DSCOPE)
	IOV_GDB_SERVER,  /**< starts gdb server on given interface */
#endif /* DEBUGGER || DHD_DSCOPE */
	IOV_INB_DW_ENABLE,
	IOV_CTO_THRESHOLD,
	IOV_PCIE_LAST /**< unused IOVAR */
};

const bcm_iovar_t dhdpcie_iovars[] = {
	{"intr",	IOV_INTR,	0, 	0, IOVT_BOOL,	0 },
	{"memsize",	IOV_MEMSIZE,	0, 	0, IOVT_UINT32,	0 },
	{"dwnldstate",	IOV_SET_DOWNLOAD_STATE,	0, 	0, IOVT_BOOL,	0 },
	{"vars",	IOV_VARS,	0, 	0, IOVT_BUFFER,	0 },
	{"devreset",	IOV_DEVRESET,	0, 	0, IOVT_UINT8,	0 },
	{"pcie_device_trap", IOV_FORCE_FW_TRAP, 0, 	0, 0,	0 },
	{"pcie_lpbk",	IOV_PCIE_LPBK,	0,	0, IOVT_UINT32,	0 },
	{"cc_nvmshadow", IOV_CC_NVMSHADOW, 0,	0, IOVT_BUFFER, 0 },
	{"ramsize",	IOV_RAMSIZE,	0, 	0, IOVT_UINT32,	0 },
	{"ramstart",	IOV_RAMSTART,	0, 	0, IOVT_UINT32,	0 },
	{"pcie_dmaxfer",	IOV_PCIE_DMAXFER,	0, 	0, IOVT_BUFFER,	3 * sizeof(int32) },
	{"pcie_suspend", IOV_PCIE_SUSPEND,	0,	0, IOVT_UINT32,	0 },
	{"sleep_allowed",	IOV_SLEEP_ALLOWED,	0,	0, IOVT_BOOL,	0 },
	{"dngl_isolation", IOV_DONGLEISOLATION,	0, 	0, IOVT_UINT32,	0 },
	{"ltrsleep_on_unload", IOV_LTRSLEEPON_UNLOOAD,	0,	0, IOVT_UINT32,	0 },
	{"dump_ringupdblk", IOV_DUMP_RINGUPD_BLOCK,	0, 	0, IOVT_BUFFER,	0 },
	{"dma_ring_indices", IOV_DMA_RINGINDICES,	0, 	0, IOVT_UINT32,	0},
	{"metadata_dbg", IOV_METADATA_DBG,	0,	0, IOVT_BOOL,	0 },
	{"rx_metadata_len", IOV_RX_METADATALEN,	0, 	0, IOVT_UINT32,	0 },
	{"tx_metadata_len", IOV_TX_METADATALEN,	0, 	0, IOVT_UINT32,	0 },
	{"db1_for_mb", IOV_DB1_FOR_MB,	0, 	0, IOVT_UINT32,	0 },
	{"txp_thresh", IOV_TXP_THRESHOLD,	0,	0, IOVT_UINT32,	0 },
	{"buzzz_dump", IOV_BUZZZ_DUMP,		0, 	0, IOVT_UINT32,	0 },
	{"flow_prio_map", IOV_FLOW_PRIO_MAP,	0, 	0, IOVT_UINT32,	0 },
#ifdef DHD_PCIE_RUNTIMEPM
	{"idletime",    IOV_IDLETIME,   0,	0, IOVT_INT32,     0 },
#endif /* DHD_PCIE_RUNTIMEPM */
	{"rxbound",     IOV_RXBOUND,    0, 0,	IOVT_UINT32,    0 },
	{"txbound",     IOV_TXBOUND,    0, 0,	IOVT_UINT32,    0 },
	{"fw_hang_report", IOV_HANGREPORT,	0, 0,	IOVT_BOOL,	0 },
	{"h2d_mb_data",     IOV_H2D_MAILBOXDATA,    0, 0,      IOVT_UINT32,    0 },
	{"inforings",   IOV_INFORINGS,    0, 0,      IOVT_UINT32,    0 },
	{"h2d_phase",   IOV_H2D_PHASE,    0, 0,      IOVT_UINT32,    0 },
	{"force_trap_bad_h2d_phase", IOV_H2D_ENABLE_TRAP_BADPHASE,    0, 0,
	IOVT_UINT32,    0 },
	{"h2d_max_txpost",   IOV_H2D_TXPOST_MAX_ITEM,    0, 0,      IOVT_UINT32,    0 },
	{"trap_data",	IOV_TRAPDATA,	0, 0,	IOVT_BUFFER,	0 },
	{"trap_data_raw",	IOV_TRAPDATA_RAW,	0, 0,	IOVT_BUFFER,	0 },
	{"cto_prevention",	IOV_CTO_PREVENTION,	0, 0,	IOVT_UINT32,	0 },
	{"pcie_wd_reset",	IOV_PCIE_WD_RESET,	0,	0, IOVT_BOOL,	0 },
	{"dump_dongle", IOV_DUMP_DONGLE, 0, 0, IOVT_BUFFER,
	MAX(sizeof(dump_dongle_in_t), sizeof(dump_dongle_out_t))},
	{"clear_ring",   IOV_CLEAR_RING,    0, 0,  IOVT_UINT32,    0 },
	{"idma_enable",   IOV_IDMA_ENABLE,    0, 0,  IOVT_UINT32,    0 },
	{"ifrm_enable",   IOV_IFRM_ENABLE,    0, 0,  IOVT_UINT32,    0 },
	{"dar_enable",   IOV_DAR_ENABLE,    0, 0,  IOVT_UINT32,    0 },
	{"cap", IOV_DNGL_CAPS,	0, 0, IOVT_BUFFER,	0},
#if defined(DEBUGGER) || defined(DHD_DSCOPE)
	{"gdb_server", IOV_GDB_SERVER,    0, 0,      IOVT_UINT32,    0 },
#endif /* DEBUGGER || DHD_DSCOPE */
	{"inb_dw_enable",   IOV_INB_DW_ENABLE,    0, 0,  IOVT_UINT32,    0 },
	{"cto_threshold",	IOV_CTO_THRESHOLD,	0,	0, IOVT_UINT32,	0 },
	{NULL, 0, 0, 0, 0, 0 }
};

#define MAX_READ_TIMEOUT	5 * 1000 * 1000

#ifndef DHD_RXBOUND
#define DHD_RXBOUND		64
#endif // endif
#ifndef DHD_TXBOUND
#define DHD_TXBOUND		64
#endif // endif

#define DHD_INFORING_BOUND	32
#define DHD_BTLOGRING_BOUND	32

uint dhd_rxbound = DHD_RXBOUND;
uint dhd_txbound = DHD_TXBOUND;

#if defined(DEBUGGER) || defined(DHD_DSCOPE)
/** the GDB debugger layer will call back into this (bus) layer to read/write dongle memory */
static struct dhd_gdb_bus_ops_s  bus_ops = {
	.read_u16 = dhdpcie_bus_rtcm16,
	.read_u32 = dhdpcie_bus_rtcm32,
	.write_u32 = dhdpcie_bus_wtcm32,
};
#endif /* DEBUGGER || DHD_DSCOPE */

/**
 * Register/Unregister functions are called by the main DHD entry point (eg module insertion) to
 * link with the bus driver, in order to look for or await the device.
 */
int
dhd_bus_register(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	return dhdpcie_bus_register();
}

void
dhd_bus_unregister(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	dhdpcie_bus_unregister();
	return;
}

/** returns a host virtual address */
uint32 *
dhdpcie_bus_reg_map(osl_t *osh, ulong addr, int size)
{
	return (uint32 *)REG_MAP(addr, size);
}

void
dhdpcie_bus_reg_unmap(osl_t *osh, volatile char *addr, int size)
{
	REG_UNMAP(addr);
	return;
}

/**
 * retrun H2D Doorbell registers address
 * use DAR registers instead of enum register for corerev >= 23 (4347B0)
 */
static INLINE uint
dhd_bus_db0_addr_get(struct dhd_bus *bus)
{
	uint addr = PCIH2D_MailBox;
	uint dar_addr = DAR_PCIH2D_DB0_0(bus->sih->buscorerev);

	return ((DAR_ACTIVE(bus->dhd)) ? dar_addr : addr);
}

static INLINE uint
dhd_bus_db0_addr_2_get(struct dhd_bus *bus)
{
	return ((DAR_ACTIVE(bus->dhd)) ? DAR_PCIH2D_DB2_0(bus->sih->buscorerev) : PCIH2D_MailBox_2);
}

static INLINE uint
dhd_bus_db1_addr_get(struct dhd_bus *bus)
{
	return ((DAR_ACTIVE(bus->dhd)) ? DAR_PCIH2D_DB0_1(bus->sih->buscorerev) : PCIH2D_DB1);
}

static INLINE uint
dhd_bus_db1_addr_1_get(struct dhd_bus *bus)
{
	return ((DAR_ACTIVE(bus->dhd)) ? DAR_PCIH2D_DB1_1(bus->sih->buscorerev) : PCIH2D_DB1_1);
}

static INLINE void
dhd_bus_pcie_pwr_req(struct dhd_bus *bus)
{
	si_corereg(bus->sih, bus->sih->buscoreidx,
		DAR_PCIE_PWR_CTRL(bus->sih->buscorerev),
		((SRPWR_DMN0_PCIE_MASK | SRPWR_DMN1_ARMBPSD_MASK) << SRPWR_REQON_SHIFT),
		((SRPWR_DMN0_PCIE_MASK | SRPWR_DMN1_ARMBPSD_MASK) << SRPWR_REQON_SHIFT));

}

bool
dhdpcie_chip_support_msi(dhd_bus_t *bus)
{
	DHD_ERROR(("%s: buscorerev=%d chipid=0x%x\n",
		__FUNCTION__, bus->sih->buscorerev, si_chipid(bus->sih)));
	if (bus->sih->buscorerev <= 14 ||
		si_chipid(bus->sih) == BCM4361_CHIP_ID ||
		si_chipid(bus->sih) == BCM4359_CHIP_ID) {
		return FALSE;
	} else {
		return TRUE;
	}
}

/**
 * Called once for each hardware (dongle) instance that this DHD manages.
 *
 * 'regs' is the host virtual address that maps to the start of the PCIe BAR0 window. The first 4096
 * bytes in this window are mapped to the backplane address in the PCIEBAR0Window register. The
 * precondition is that the PCIEBAR0Window register 'points' at the PCIe core.
 *
 * 'tcm' is the *host* virtual address at which tcm is mapped.
 */
dhd_bus_t* dhdpcie_bus_attach(osl_t *osh,
	volatile char *regs, volatile char *tcm, void *pci_dev)
{
	dhd_bus_t *bus;

	DHD_TRACE(("%s: ENTER\n", __FUNCTION__));

	do {
		if (!(bus = MALLOCZ(osh, sizeof(dhd_bus_t)))) {
			DHD_ERROR(("%s: MALLOC of dhd_bus_t failed\n", __FUNCTION__));
			break;
		}

		bus->regs = regs;
		bus->tcm = tcm;
		bus->osh = osh;
		/* Save pci_dev into dhd_bus, as it may be needed in dhd_attach */
		bus->dev = (struct pci_dev *)pci_dev;

		dll_init(&bus->flowring_active_list);
#ifdef IDLE_TX_FLOW_MGMT
		bus->active_list_last_process_ts = OSL_SYSUPTIME();
#endif /* IDLE_TX_FLOW_MGMT */

		/* Attach pcie shared structure */
		if (!(bus->pcie_sh = MALLOCZ(osh, sizeof(pciedev_shared_t)))) {
			DHD_ERROR(("%s: MALLOC of bus->pcie_sh failed\n", __FUNCTION__));
			break;
		}

		/* dhd_common_init(osh); */

		if (dhdpcie_dongle_attach(bus)) {
			DHD_ERROR(("%s: dhdpcie_probe_attach failed\n", __FUNCTION__));
			break;
		}

		/* software resources */
		if (!(bus->dhd = dhd_attach(osh, bus, PCMSGBUF_HDRLEN))) {
			DHD_ERROR(("%s: dhd_attach failed\n", __FUNCTION__));

			break;
		}
		bus->dhd->busstate = DHD_BUS_DOWN;
		bus->db1_for_mb = TRUE;
		bus->dhd->hang_report = TRUE;
		bus->use_mailbox = FALSE;
		bus->use_d0_inform = FALSE;
#ifdef IDLE_TX_FLOW_MGMT
		bus->enable_idle_flowring_mgmt = FALSE;
#endif /* IDLE_TX_FLOW_MGMT */
		bus->irq_registered = FALSE;

#ifdef DHD_MSI_SUPPORT
		bus->d2h_intr_method = enable_msi && dhdpcie_chip_support_msi(bus) ?
			PCIE_MSI : PCIE_INTX;
#else
		bus->d2h_intr_method = PCIE_INTX;
#endif /* DHD_MSI_SUPPORT */

		DHD_TRACE(("%s: EXIT SUCCESS\n",
			__FUNCTION__));
#ifdef DHD_FW_COREDUMP
		g_dhd_bus = bus;
#endif // endif
		return bus;
	} while (0);

	DHD_TRACE(("%s: EXIT FAILURE\n", __FUNCTION__));

	if (bus && bus->pcie_sh) {
		MFREE(osh, bus->pcie_sh, sizeof(pciedev_shared_t));
	}

	if (bus) {
		MFREE(osh, bus, sizeof(dhd_bus_t));
	}
	return NULL;
}

bool
dhd_bus_skip_clm(dhd_pub_t *dhdp)
{
	switch (dhd_bus_chip_id(dhdp)) {
		default:
			return FALSE;
	}
}

uint
dhd_bus_chip(struct dhd_bus *bus)
{
	ASSERT(bus->sih != NULL);
	return bus->sih->chip;
}

uint
dhd_bus_chiprev(struct dhd_bus *bus)
{
	ASSERT(bus);
	ASSERT(bus->sih != NULL);
	return bus->sih->chiprev;
}

void *
dhd_bus_pub(struct dhd_bus *bus)
{
	return bus->dhd;
}

void *
dhd_bus_sih(struct dhd_bus *bus)
{
	return (void *)bus->sih;
}

void *
dhd_bus_txq(struct dhd_bus *bus)
{
	return &bus->txq;
}

/** Get Chip ID version */
uint dhd_bus_chip_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return  bus->sih->chip;
}

/** Get Chip Rev ID version */
uint dhd_bus_chiprev_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return bus->sih->chiprev;
}

/** Get Chip Pkg ID version */
uint dhd_bus_chippkg_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return bus->sih->chippkg;
}

/** Read and clear intstatus. This should be called with interrupts disabled or inside isr */
uint32
dhdpcie_bus_intstatus(dhd_bus_t *bus)
{
	uint32 intstatus = 0;
	uint32 intmask = 0;

	if (bus->bus_low_power_state == DHD_BUS_D3_ACK_RECIEVED) {
		DHD_ERROR(("%s: trying to clear intstatus after D3 Ack\n", __FUNCTION__));
		return intstatus;
	}
	if ((bus->sih->buscorerev == 6) || (bus->sih->buscorerev == 4) ||
		(bus->sih->buscorerev == 2)) {
		intstatus = dhdpcie_bus_cfg_read_dword(bus, PCIIntstatus, 4);
		dhdpcie_bus_cfg_write_dword(bus, PCIIntstatus, 4, intstatus);
		intstatus &= I_MB;
	} else {
		/* this is a PCIE core register..not a config register... */
		intstatus = si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_int, 0, 0);

		/* this is a PCIE core register..not a config register... */
		intmask = si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_mask, 0, 0);
		intstatus &= intmask;
		/* Is device removed. intstatus & intmask read 0xffffffff */
		if (intstatus == (uint32)-1) {
			DHD_ERROR(("%s: Device is removed or Link is down.\n", __FUNCTION__));
			bus->is_linkdown = TRUE;
			dhd_pcie_debug_info_dump(bus->dhd);
#ifdef CUSTOMER_HW4_DEBUG
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
			bus->dhd->hang_reason = HANG_REASON_PCIE_LINK_DOWN;
			dhd_os_send_hang_message(bus->dhd);
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27) && OEM_ANDROID */
#endif /* CUSTOMER_HW4_DEBUG */
			return intstatus;
		}

		/*
		 * The fourth argument to si_corereg is the "mask" fields of the register to update
		 * and the fifth field is the "value" to update. Now if we are interested in only
		 * few fields of the "mask" bit map, we should not be writing back what we read
		 * By doing so, we might clear/ack interrupts that are not handled yet.
		 */
		si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_int, bus->def_intmask,
			intstatus);

		intstatus &= bus->def_intmask;
	}

	return intstatus;
}

/**
 * Name:  dhdpcie_bus_isr
 * Parameters:
 * 1: IN int irq   -- interrupt vector
 * 2: IN void *arg      -- handle to private data structure
 * Return value:
 * Status (TRUE or FALSE)
 *
 * Description:
 * Interrupt Service routine checks for the status register,
 * disable interrupt and queue DPC if mail box interrupts are raised.
 */
int32
dhdpcie_bus_isr(dhd_bus_t *bus)
{
	uint32 intstatus = 0;

	do {
		DHD_TRACE(("%s: Enter\n", __FUNCTION__));
		/* verify argument */
		if (!bus) {
			DHD_ERROR(("%s : bus is null pointer, exit \n", __FUNCTION__));
			break;
		}

		if (bus->dhd->dongle_reset) {
			break;
		}

		if (bus->dhd->busstate == DHD_BUS_DOWN) {
			break;
		}

		if (PCIECTO_ENAB(bus->dhd)) {
			/* read pci_intstatus */
			intstatus = dhdpcie_bus_cfg_read_dword(bus, PCI_INT_STATUS, 4);

			if (intstatus & PCI_CTO_INT_MASK) {
				/* reset backplane and cto,
				 *  then access through pcie is recovered.
				 */
				dhdpcie_cto_error_recovery(bus);
				return TRUE;
			}
		}

		if (bus->d2h_intr_method == PCIE_MSI) {
			/* For MSI, as intstatus is cleared by firmware, no need to read */
			goto skip_intstatus_read;
		}

		intstatus = dhdpcie_bus_intstatus(bus);

		/* Check if the interrupt is ours or not */
		if (intstatus == 0) {
			break;
		}

		/* save the intstatus */
		/* read interrupt status register!! Status bits will be cleared in DPC !! */
		bus->intstatus = intstatus;

		/* return error for 0xFFFFFFFF */
		if (intstatus == (uint32)-1) {
			dhdpcie_disable_irq_nosync(bus);
			return BCME_ERROR;
		}

skip_intstatus_read:
		/*  Overall operation:
		 *    - Mask further interrupts
		 *    - Read/ack intstatus
		 *    - Take action based on bits and state
		 *    - Reenable interrupts (as per state)
		 */

		/* Count the interrupt call */
		bus->intrcount++;

		bus->ipend = TRUE;

		bus->isr_intr_disable_count++;

		/* For Linux, Macos etc (otherthan NDIS) instead of disabling
		* dongle interrupt by clearing the IntMask, disable directly
		* interrupt from the host side, so that host will not recieve
		* any interrupts at all, even though dongle raises interrupts
		*/
		dhdpcie_disable_irq_nosync(bus); /* Disable interrupt!! */

		bus->intdis = TRUE;

#if defined(PCIE_ISR_THREAD)

		DHD_TRACE(("Calling dhd_bus_dpc() from %s\n", __FUNCTION__));
		DHD_OS_WAKE_LOCK(bus->dhd);
		while (dhd_bus_dpc(bus));
		DHD_OS_WAKE_UNLOCK(bus->dhd);
#else
		bus->dpc_sched = TRUE;
		dhd_sched_dpc(bus->dhd);     /* queue DPC now!! */
#endif /* defined(SDIO_ISR_THREAD) */

		DHD_TRACE(("%s: Exit Success DPC Queued\n", __FUNCTION__));
		return TRUE;

	} while (0);

	DHD_TRACE(("%s: Exit Failure\n", __FUNCTION__));
	return FALSE;
}

int
dhdpcie_set_pwr_state(dhd_bus_t *bus, uint state)
{
	uint32 cur_state = 0;
	uint32 pm_csr = 0;
	osl_t *osh = bus->osh;

	pm_csr = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_PM_CSR, sizeof(uint32));
	cur_state = pm_csr & PCIECFGREG_PM_CSR_STATE_MASK;

	if (cur_state == state) {
		DHD_ERROR(("%s: Already in state %u \n", __FUNCTION__, cur_state));
		return BCME_OK;
	}

	if (state > PCIECFGREG_PM_CSR_STATE_D3_HOT)
		return BCME_ERROR;

	/* Validate the state transition
	* if already in a lower power state, return error
	*/
	if (state != PCIECFGREG_PM_CSR_STATE_D0 &&
			cur_state <= PCIECFGREG_PM_CSR_STATE_D3_COLD &&
			cur_state > state) {
		DHD_ERROR(("%s: Invalid power state transition !\n", __FUNCTION__));
		return BCME_ERROR;
	}

	pm_csr &= ~PCIECFGREG_PM_CSR_STATE_MASK;
	pm_csr |= state;

	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_PM_CSR, sizeof(uint32), pm_csr);

	/* need to wait for the specified mandatory pcie power transition delay time */
	if (state == PCIECFGREG_PM_CSR_STATE_D3_HOT ||
			cur_state == PCIECFGREG_PM_CSR_STATE_D3_HOT)
			OSL_DELAY(DHDPCIE_PM_D3_DELAY);
	else if (state == PCIECFGREG_PM_CSR_STATE_D2 ||
			cur_state == PCIECFGREG_PM_CSR_STATE_D2)
			OSL_DELAY(DHDPCIE_PM_D2_DELAY);

	/* read back the power state and verify */
	pm_csr = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_PM_CSR, sizeof(uint32));
	cur_state = pm_csr & PCIECFGREG_PM_CSR_STATE_MASK;
	if (cur_state != state) {
		DHD_ERROR(("%s: power transition failed ! Current state is %u \n",
				__FUNCTION__, cur_state));
		return BCME_ERROR;
	} else {
		DHD_ERROR(("%s: power transition to %u success \n",
				__FUNCTION__, cur_state));
	}

	return BCME_OK;
}

int
dhdpcie_config_check(dhd_bus_t *bus)
{
	uint32 i, val;
	int ret = BCME_ERROR;

	for (i = 0; i < DHDPCIE_CONFIG_CHECK_RETRY_COUNT; i++) {
		val = OSL_PCI_READ_CONFIG(bus->osh, PCI_CFG_VID, sizeof(uint32));
		if ((val & 0xFFFF) == VENDOR_BROADCOM) {
			ret = BCME_OK;
			break;
		}
		OSL_DELAY(DHDPCIE_CONFIG_CHECK_DELAY_MS * 1000);
	}

	return ret;
}

int
dhdpcie_config_restore(dhd_bus_t *bus, bool restore_pmcsr)
{
	uint32 i;
	osl_t *osh = bus->osh;

	if (BCME_OK != dhdpcie_config_check(bus)) {
		return BCME_ERROR;
	}

	for (i = PCI_CFG_REV >> 2; i < DHDPCIE_CONFIG_HDR_SIZE; i++) {
		OSL_PCI_WRITE_CONFIG(osh, i << 2, sizeof(uint32), bus->saved_config.header[i]);
	}
	OSL_PCI_WRITE_CONFIG(osh, PCI_CFG_CMD, sizeof(uint32), bus->saved_config.header[1]);

	if (restore_pmcsr)
		OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_PM_CSR,
			sizeof(uint32), bus->saved_config.pmcsr);

	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_MSI_CAP, sizeof(uint32), bus->saved_config.msi_cap);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_MSI_ADDR_L, sizeof(uint32),
			bus->saved_config.msi_addr0);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_MSI_ADDR_H,
			sizeof(uint32), bus->saved_config.msi_addr1);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_MSI_DATA,
			sizeof(uint32), bus->saved_config.msi_data);

	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_DEV_STATUS_CTRL,
			sizeof(uint32), bus->saved_config.exp_dev_ctrl_stat);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGGEN_DEV_STATUS_CTRL2,
			sizeof(uint32), bus->saved_config.exp_dev_ctrl_stat2);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_LINK_STATUS_CTRL,
			sizeof(uint32), bus->saved_config.exp_link_ctrl_stat);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_LINK_STATUS_CTRL2,
			sizeof(uint32), bus->saved_config.exp_link_ctrl_stat2);

	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_PML1_SUB_CTRL1,
			sizeof(uint32), bus->saved_config.l1pm0);
	OSL_PCI_WRITE_CONFIG(osh, PCIECFGREG_PML1_SUB_CTRL2,
			sizeof(uint32), bus->saved_config.l1pm1);

	OSL_PCI_WRITE_CONFIG(bus->osh, PCI_BAR0_WIN, sizeof(uint32),
			bus->saved_config.bar0_win);
	OSL_PCI_WRITE_CONFIG(bus->osh, PCI_BAR1_WIN, sizeof(uint32),
			bus->saved_config.bar1_win);

	return BCME_OK;
}

int
dhdpcie_config_save(dhd_bus_t *bus)
{
	uint32 i;
	osl_t *osh = bus->osh;

	if (BCME_OK != dhdpcie_config_check(bus)) {
		return BCME_ERROR;
	}

	for (i = 0; i < DHDPCIE_CONFIG_HDR_SIZE; i++) {
		bus->saved_config.header[i] = OSL_PCI_READ_CONFIG(osh, i << 2, sizeof(uint32));
	}

	bus->saved_config.pmcsr = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_PM_CSR, sizeof(uint32));

	bus->saved_config.msi_cap = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_MSI_CAP,
			sizeof(uint32));
	bus->saved_config.msi_addr0 = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_MSI_ADDR_L,
			sizeof(uint32));
	bus->saved_config.msi_addr1 = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_MSI_ADDR_H,
			sizeof(uint32));
	bus->saved_config.msi_data = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_MSI_DATA,
			sizeof(uint32));

	bus->saved_config.exp_dev_ctrl_stat = OSL_PCI_READ_CONFIG(osh,
			PCIECFGREG_DEV_STATUS_CTRL, sizeof(uint32));
	bus->saved_config.exp_dev_ctrl_stat2 = OSL_PCI_READ_CONFIG(osh,
			PCIECFGGEN_DEV_STATUS_CTRL2, sizeof(uint32));
	bus->saved_config.exp_link_ctrl_stat = OSL_PCI_READ_CONFIG(osh,
			PCIECFGREG_LINK_STATUS_CTRL, sizeof(uint32));
	bus->saved_config.exp_link_ctrl_stat2 = OSL_PCI_READ_CONFIG(osh,
			PCIECFGREG_LINK_STATUS_CTRL2, sizeof(uint32));

	bus->saved_config.l1pm0 = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_PML1_SUB_CTRL1,
			sizeof(uint32));
	bus->saved_config.l1pm1 = OSL_PCI_READ_CONFIG(osh, PCIECFGREG_PML1_SUB_CTRL2,
			sizeof(uint32));

	bus->saved_config.bar0_win = OSL_PCI_READ_CONFIG(osh, PCI_BAR0_WIN,
			sizeof(uint32));
	bus->saved_config.bar1_win = OSL_PCI_READ_CONFIG(osh, PCI_BAR1_WIN,
			sizeof(uint32));

	return BCME_OK;
}

#ifdef EXYNOS_PCIE_LINKDOWN_RECOVERY
dhd_pub_t *link_recovery = NULL;
#endif /* EXYNOS_PCIE_LINKDOWN_RECOVERY */

static void
dhdpcie_bus_intr_init(dhd_bus_t *bus)
{
	uint buscorerev = bus->sih->buscorerev;
	bus->pcie_mailbox_int = PCIMailBoxInt(buscorerev);
	bus->pcie_mailbox_mask = PCIMailBoxMask(buscorerev);
	bus->d2h_mb_mask = PCIE_MB_D2H_MB_MASK(buscorerev);
	bus->def_intmask = PCIE_MB_D2H_MB_MASK(buscorerev);
	if (buscorerev < 64) {
		bus->def_intmask |= PCIE_MB_TOPCIE_FN0_0 | PCIE_MB_TOPCIE_FN0_1;
	}
}

#ifdef DHD_DISABLE_ASPM
void
dhd_bus_aspm_enable(dhd_bus_t *bus, bool enable)
{
	uint val;
	val = dhd_pcie_config_read(bus->osh, PCIECFGREG_LINK_STATUS_CTRL, sizeof(uint32));
	if (enable) {
		dhd_pcie_config_write(bus->osh, PCIECFGREG_LINK_STATUS_CTRL, sizeof(uint32),
				(val | PCIE_ASPM_L1_ENAB));
	} else {
		dhd_pcie_config_write(bus->osh, PCIECFGREG_LINK_STATUS_CTRL, sizeof(uint32),
				(val & (~PCIE_ASPM_L1_ENAB)));
	}
	DHD_ERROR(("%s: %s\n", __FUNCTION__, (enable ? "ENABLE" : "DISABLE")));
}
#endif /* DHD_DISABLE_ASPM */

static bool
dhdpcie_dongle_attach(dhd_bus_t *bus)
{
	osl_t *osh = bus->osh;
	volatile void *regsva = (volatile void*)bus->regs;
	uint16 devid;
	uint32 val;
	sbpcieregs_t *sbpcieregs;
	bool dongle_isolation;

	DHD_TRACE(("%s: ENTER\n", __FUNCTION__));

#ifdef EXYNOS_PCIE_LINKDOWN_RECOVERY
	link_recovery = bus->dhd;
#endif /* EXYNOS_PCIE_LINKDOWN_RECOVERY */

	bus->alp_only = TRUE;
	bus->sih = NULL;

	/* Checking PCIe bus status with reading configuration space */
	val = OSL_PCI_READ_CONFIG(osh, PCI_CFG_VID, sizeof(uint32));
	if ((val & 0xFFFF) != VENDOR_BROADCOM) {
		DHD_ERROR(("%s : failed to read PCI configuration space!\n", __FUNCTION__));
		goto fail;
	}
	devid = (val >> 16) & 0xFFFF;
	bus->cl_devid = devid;

	/* Set bar0 window to si_enum_base */
	dhdpcie_bus_cfg_set_bar0_win(bus, si_enum_base(devid));

	/*
	 * Checking PCI_SPROM_CONTROL register for preventing invalid address access
	 * due to switch address space from PCI_BUS to SI_BUS.
	 */
	val = OSL_PCI_READ_CONFIG(osh, PCI_SPROM_CONTROL, sizeof(uint32));
	if (val == 0xffffffff) {
		DHD_ERROR(("%s : failed to read SPROM control register\n", __FUNCTION__));
		goto fail;
	}

	/* si_attach() will provide an SI handle and scan the backplane */
	if (!(bus->sih = si_attach((uint)devid, osh, regsva, PCI_BUS, bus,
	                           &bus->vars, &bus->varsz))) {
		DHD_ERROR(("%s: si_attach failed!\n", __FUNCTION__));
		goto fail;
	}

	/* Olympic EFI requirement - stop driver load if FW is already running
	*  need to do this here before pcie_watchdog_reset, because
	*  pcie_watchdog_reset will put the ARM back into halt state
	*/
	if (!dhdpcie_is_arm_halted(bus)) {
		DHD_ERROR(("%s: ARM is not halted,FW is already running! Abort.\n",
				__FUNCTION__));
		goto fail;
	}

	BCM_REFERENCE(dongle_isolation);
#ifndef DONGLE_ENABLE_ISOLATION
	/* Enable CLKREQ# */
	dhdpcie_clkreq(bus->osh, 1, 1);

	/*
	 * bus->dhd will be NULL if it is called from dhd_bus_attach, so need to reset
	 * without checking dongle_isolation flag, but if it is called via some other path
	 * like quiesce FLR, then based on dongle_isolation flag, watchdog_reset should
	 * be called.
	 */
	if (bus->dhd == NULL) {
		dongle_isolation = FALSE; /* dhd_attach not yet happened, do watchdog reset */
	} else {
		dongle_isolation = bus->dhd->dongle_isolation;
	}
	/*
	 * Issue CC watchdog to reset all the cores on the chip - similar to rmmod dhd
	 * This is required to avoid spurious interrupts to the Host and bring back
	 * dongle to a sane state (on host soft-reboot / watchdog-reboot).
	 */
	if (dongle_isolation == FALSE) {
		pcie_watchdog_reset(bus->osh, bus->sih,
			WD_ENABLE_MASK, WD_SSRESET_PCIE_F0_EN);
	}
#endif /* !DONGLE_ENABLE_ISOLATION */

	si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	sbpcieregs = (sbpcieregs_t*)(bus->regs);

	/* WAR where the BAR1 window may not be sized properly */
	W_REG(osh, &sbpcieregs->configaddr, 0x4e0);
	val = R_REG(osh, &sbpcieregs->configdata);
	W_REG(osh, &sbpcieregs->configdata, val);

	/* Get info on the ARM and SOCRAM cores... */
	/* Should really be qualified by device id */
	if ((si_setcore(bus->sih, ARM7S_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCM3_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
		bus->armrev = si_corerev(bus->sih);
	} else {
		DHD_ERROR(("%s: failed to find ARM core!\n", __FUNCTION__));
		goto fail;
	}

	if (si_setcore(bus->sih, SYSMEM_CORE_ID, 0)) {
		/* Only set dongle RAMSIZE to default value when BMC vs ARM usage of SYSMEM is not
		 * adjusted.
		 */
		if (!bus->ramsize_adjusted) {
			if (!(bus->orig_ramsize = si_sysmem_size(bus->sih))) {
				DHD_ERROR(("%s: failed to find SYSMEM memory!\n", __FUNCTION__));
				goto fail;
			}
			switch ((uint16)bus->sih->chip) {
				default:
					/* also populate base address */
					bus->dongle_ram_base = CA7_4365_RAM_BASE;
					bus->orig_ramsize = 0x1c0000; /* Reserve 1.75MB for CA7 */
					break;
			}
		}
	} else if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
		if (!(bus->orig_ramsize = si_socram_size(bus->sih))) {
			DHD_ERROR(("%s: failed to find SOCRAM memory!\n", __FUNCTION__));
			goto fail;
		}
	} else {
		/* cr4 has a different way to find the RAM size from TCM's */
		if (!(bus->orig_ramsize = si_tcm_size(bus->sih))) {
			DHD_ERROR(("%s: failed to find CR4-TCM memory!\n", __FUNCTION__));
			goto fail;
		}
		/* also populate base address */
		switch ((uint16)bus->sih->chip) {
		case BCM4339_CHIP_ID:
		case BCM4335_CHIP_ID:
			bus->dongle_ram_base = CR4_4335_RAM_BASE;
			break;
		case BCM4358_CHIP_ID:
		case BCM4354_CHIP_ID:
		case BCM43567_CHIP_ID:
		case BCM43569_CHIP_ID:
		case BCM4350_CHIP_ID:
		case BCM43570_CHIP_ID:
			bus->dongle_ram_base = CR4_4350_RAM_BASE;
			break;
		case BCM4360_CHIP_ID:
			bus->dongle_ram_base = CR4_4360_RAM_BASE;
			break;

		case BCM4364_CHIP_ID:
			bus->dongle_ram_base = CR4_4364_RAM_BASE;
			break;

		CASE_BCM4345_CHIP:
			bus->dongle_ram_base = (bus->sih->chiprev < 6)  /* changed at 4345C0 */
				? CR4_4345_LT_C0_RAM_BASE : CR4_4345_GE_C0_RAM_BASE;
			break;
		CASE_BCM43602_CHIP:
			bus->dongle_ram_base = CR4_43602_RAM_BASE;
			break;
		case BCM4349_CHIP_GRPID:
			/* RAM based changed from 4349c0(revid=9) onwards */
			bus->dongle_ram_base = ((bus->sih->chiprev < 9) ?
				CR4_4349_RAM_BASE : CR4_4349_RAM_BASE_FROM_REV_9);
			break;
		case BCM4347_CHIP_ID:
		case BCM4357_CHIP_ID:
		case BCM4361_CHIP_ID:
			bus->dongle_ram_base = CR4_4347_RAM_BASE;
			break;
		default:
			bus->dongle_ram_base = 0;
			DHD_ERROR(("%s: WARNING: Using default ram base at 0x%x\n",
			           __FUNCTION__, bus->dongle_ram_base));
		}
	}
	bus->ramsize = bus->orig_ramsize;
	if (dhd_dongle_memsize)
		dhdpcie_bus_dongle_setmemsize(bus, dhd_dongle_memsize);

	if (bus->ramsize > DONGLE_TCM_MAP_SIZE) {
		DHD_ERROR(("%s : invalid ramsize %d(0x%x) is returned from dongle\n",
				__FUNCTION__, bus->ramsize, bus->ramsize));
		goto fail;
	}

	DHD_ERROR(("DHD: dongle ram size is set to %d(orig %d) at 0x%x\n",
	           bus->ramsize, bus->orig_ramsize, bus->dongle_ram_base));

	bus->srmemsize = si_socram_srmem_size(bus->sih);

	dhdpcie_bus_intr_init(bus);

	/* Set the poll and/or interrupt flags */
	bus->intr = (bool)dhd_intr;
#ifdef DHD_DISABLE_ASPM
	dhd_bus_aspm_enable(bus, FALSE);
#endif /* DHD_DISABLE_ASPM */

	bus->idma_enabled = TRUE;
	bus->ifrm_enabled = TRUE;
	DHD_TRACE(("%s: EXIT: SUCCESS\n", __FUNCTION__));
	return 0;

fail:
	if (bus->sih != NULL) {
		si_detach(bus->sih);
		bus->sih = NULL;
	}
	DHD_TRACE(("%s: EXIT: FAILURE\n", __FUNCTION__));
	return -1;
}

int
dhpcie_bus_unmask_interrupt(dhd_bus_t *bus)
{
	dhdpcie_bus_cfg_write_dword(bus, PCIIntmask, 4, I_MB);
	return 0;
}
int
dhpcie_bus_mask_interrupt(dhd_bus_t *bus)
{
	dhdpcie_bus_cfg_write_dword(bus, PCIIntmask, 4, 0x0);
	return 0;
}

/* Non atomic function, caller should hold appropriate lock */
void
dhdpcie_bus_intr_enable(dhd_bus_t *bus)
{
	DHD_TRACE(("%s Enter\n", __FUNCTION__));
	if (bus && bus->sih && !bus->is_linkdown) {
		/* Skip after recieving D3 ACK */
		if (bus->bus_low_power_state == DHD_BUS_D3_ACK_RECIEVED) {
			return;
		}
		if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
			(bus->sih->buscorerev == 4)) {
			dhpcie_bus_unmask_interrupt(bus);
		} else {
			si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_mask,
				bus->def_intmask, bus->def_intmask);
		}
	}
	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

/* Non atomic function, caller should hold appropriate lock */
void
dhdpcie_bus_intr_disable(dhd_bus_t *bus)
{
	DHD_TRACE(("%s Enter\n", __FUNCTION__));
	if (bus && bus->sih && !bus->is_linkdown) {
		/* Skip after recieving D3 ACK */
		if (bus->bus_low_power_state == DHD_BUS_D3_ACK_RECIEVED) {
			return;
		}
		if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
			(bus->sih->buscorerev == 4)) {
			dhpcie_bus_mask_interrupt(bus);
		} else {
			si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_mask,
				bus->def_intmask, 0);
		}
	}
	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

/*
 *  dhdpcie_advertise_bus_cleanup advertises that clean up is under progress
 * to other bus user contexts like Tx, Rx, IOVAR, WD etc and it waits for other contexts
 * to gracefully exit. All the bus usage contexts before marking busstate as busy, will check for
 * whether the busstate is DHD_BUS_DOWN or DHD_BUS_DOWN_IN_PROGRESS, if so
 * they will exit from there itself without marking dhd_bus_busy_state as BUSY.
 */
void
dhdpcie_advertise_bus_cleanup(dhd_pub_t	 *dhdp)
{
	unsigned long flags;
	int timeleft;

#ifdef DHD_PCIE_RUNTIMEPM
	dhdpcie_runtime_bus_wake(dhdp, TRUE, dhdpcie_advertise_bus_cleanup);
#endif /* DHD_PCIE_RUNTIMEPM */

	dhdp->dhd_watchdog_ms_backup = dhd_watchdog_ms;
	if (dhdp->dhd_watchdog_ms_backup) {
		DHD_ERROR(("%s: Disabling wdtick before dhd deinit\n",
			__FUNCTION__));
		dhd_os_wd_timer(dhdp, 0);
	}
	if (dhdp->busstate != DHD_BUS_DOWN) {
		DHD_GENERAL_LOCK(dhdp, flags);
		dhdp->busstate = DHD_BUS_DOWN_IN_PROGRESS;
		DHD_GENERAL_UNLOCK(dhdp, flags);
	}

	timeleft = dhd_os_busbusy_wait_negation(dhdp, &dhdp->dhd_bus_busy_state);
	if ((timeleft == 0) || (timeleft == 1)) {
		DHD_ERROR(("%s : Timeout due to dhd_bus_busy_state=0x%x\n",
				__FUNCTION__, dhdp->dhd_bus_busy_state));
		ASSERT(0);
	}

	return;
}

static void
dhdpcie_bus_remove_prep(dhd_bus_t *bus)
{
	unsigned long flags;
	DHD_TRACE(("%s Enter\n", __FUNCTION__));

	DHD_GENERAL_LOCK(bus->dhd, flags);
	bus->dhd->busstate = DHD_BUS_DOWN;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	dhd_os_sdlock(bus->dhd);

	if (bus->sih && !bus->dhd->dongle_isolation) {
		/* Has insmod fails after rmmod issue in Brix Android */

		/* if the pcie link is down, watchdog reset
		* should not be done, as it may hang
		*/

		if (!bus->is_linkdown) {

			pcie_watchdog_reset(bus->osh, bus->sih,
				WD_ENABLE_MASK, WD_SSRESET_PCIE_F0_EN);
		}

		bus->dhd->is_pcie_watchdog_reset = TRUE;
	}

	dhd_os_sdunlock(bus->dhd);

	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

void
dhd_init_bus_lock(dhd_bus_t *bus)
{
	if (!bus->bus_lock) {
		bus->bus_lock = dhd_os_spin_lock_init(bus->dhd->osh);
	}
}

void
dhd_deinit_bus_lock(dhd_bus_t *bus)
{
	if (bus->bus_lock) {
		dhd_os_spin_lock_deinit(bus->dhd->osh, bus->bus_lock);
		bus->bus_lock = NULL;
	}
}

/** Detach and free everything */
void
dhdpcie_bus_release(dhd_bus_t *bus)
{
	bool dongle_isolation = FALSE;
	osl_t *osh = NULL;
	unsigned long flags_bus;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus) {

		osh = bus->osh;
		ASSERT(osh);

		if (bus->dhd) {
#if defined(DEBUGGER) || defined(DHD_DSCOPE)
			debugger_close();
#endif /* DEBUGGER || DHD_DSCOPE */
			dhdpcie_advertise_bus_cleanup(bus->dhd);
			dongle_isolation = bus->dhd->dongle_isolation;
			bus->dhd->is_pcie_watchdog_reset = FALSE;
			dhdpcie_bus_remove_prep(bus);

			if (bus->intr) {
				DHD_BUS_LOCK(bus->bus_lock, flags_bus);
				dhdpcie_bus_intr_disable(bus);
				DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);
				dhdpcie_free_irq(bus);
			}
			dhd_deinit_bus_lock(bus);
			/**
			 * dhdpcie_bus_release_dongle free bus->sih  handle, which is needed to
			 * access Dongle registers.
			 * dhd_detach will communicate with dongle to delete flowring ..etc.
			 * So dhdpcie_bus_release_dongle should be called only after the dhd_detach.
			 */
			dhd_detach(bus->dhd);
			dhdpcie_bus_release_dongle(bus, osh, dongle_isolation, TRUE);
			dhd_free(bus->dhd);
			bus->dhd = NULL;
		}

		/* unmap the regs and tcm here!! */
		if (bus->regs) {
			dhdpcie_bus_reg_unmap(osh, bus->regs, DONGLE_REG_MAP_SIZE);
			bus->regs = NULL;
		}
		if (bus->tcm) {
			dhdpcie_bus_reg_unmap(osh, bus->tcm, DONGLE_TCM_MAP_SIZE);
			bus->tcm = NULL;
		}

		dhdpcie_bus_release_malloc(bus, osh);
		/* Detach pcie shared structure */
		if (bus->pcie_sh) {
			MFREE(osh, bus->pcie_sh, sizeof(pciedev_shared_t));
			bus->pcie_sh = NULL;
		}

		if (bus->console.buf != NULL) {
			MFREE(osh, bus->console.buf, bus->console.bufsize);
		}

		/* Finally free bus info */
		MFREE(osh, bus, sizeof(dhd_bus_t));

	}

	DHD_TRACE(("%s: Exit\n", __FUNCTION__));
} /* dhdpcie_bus_release */

void
dhdpcie_bus_release_dongle(dhd_bus_t *bus, osl_t *osh, bool dongle_isolation, bool reset_flag)
{
	DHD_TRACE(("%s: Enter bus->dhd %p bus->dhd->dongle_reset %d \n", __FUNCTION__,
		bus->dhd, bus->dhd->dongle_reset));

	if ((bus->dhd && bus->dhd->dongle_reset) && reset_flag) {
		DHD_TRACE(("%s Exit\n", __FUNCTION__));
		return;
	}

	if (bus->sih) {

		if (!dongle_isolation &&
		(bus->dhd && !bus->dhd->is_pcie_watchdog_reset)) {
			pcie_watchdog_reset(bus->osh, bus->sih,
				WD_ENABLE_MASK, WD_SSRESET_PCIE_F0_EN);
		}

		if (bus->ltrsleep_on_unload) {
			si_corereg(bus->sih, bus->sih->buscoreidx,
				OFFSETOF(sbpcieregs_t, u.pcie2.ltr_state), ~0, 0);
		}

		if (bus->sih->buscorerev == 13)
			 pcie_serdes_iddqdisable(bus->osh, bus->sih,
			                         (sbpcieregs_t *) bus->regs);

#ifndef DONGLE_ENABLE_ISOLATION
		/* Disable CLKREQ# */
		dhdpcie_clkreq(bus->osh, 1, 0);
#endif /* !DONGLE_ENABLE_ISOLATION */

		if (bus->sih != NULL) {
			si_detach(bus->sih);
			bus->sih = NULL;
		}
		if (bus->vars && bus->varsz)
			MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

uint32
dhdpcie_bus_cfg_read_dword(dhd_bus_t *bus, uint32 addr, uint32 size)
{
	uint32 data = OSL_PCI_READ_CONFIG(bus->osh, addr, size);
	return data;
}

/** 32 bit config write */
void
dhdpcie_bus_cfg_write_dword(dhd_bus_t *bus, uint32 addr, uint32 size, uint32 data)
{
	OSL_PCI_WRITE_CONFIG(bus->osh, addr, size, data);
}

void
dhdpcie_bus_cfg_set_bar0_win(dhd_bus_t *bus, uint32 data)
{
	OSL_PCI_WRITE_CONFIG(bus->osh, PCI_BAR0_WIN, 4, data);
}

void
dhdpcie_bus_dongle_setmemsize(struct dhd_bus *bus, int mem_size)
{
	int32 min_size =  DONGLE_MIN_MEMSIZE;
	/* Restrict the memsize to user specified limit */
	DHD_ERROR(("user: Restrict the dongle ram size to %d, min accepted %d\n",
		dhd_dongle_memsize, min_size));
	if ((dhd_dongle_memsize > min_size) &&
		(dhd_dongle_memsize < (int32)bus->orig_ramsize))
		bus->ramsize = dhd_dongle_memsize;
}

void
dhdpcie_bus_release_malloc(dhd_bus_t *bus, osl_t *osh)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd && bus->dhd->dongle_reset)
		return;

	if (bus->vars && bus->varsz) {
		MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

	DHD_TRACE(("%s: Exit\n", __FUNCTION__));
	return;

}

/** Stop bus module: clear pending frames, disable data flow */
void dhd_bus_stop(struct dhd_bus *bus, bool enforce_mutex)
{
	uint32 status;
	unsigned long flags, flags_bus;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (!bus->dhd)
		return;

	if (bus->dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: already down by net_dev_reset\n", __FUNCTION__));
		goto done;
	}

	DHD_DISABLE_RUNTIME_PM(bus->dhd);

	DHD_GENERAL_LOCK(bus->dhd, flags);
	bus->dhd->busstate = DHD_BUS_DOWN;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
	atomic_set(&bus->dhd->block_bus, TRUE);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

	DHD_BUS_LOCK(bus->bus_lock, flags_bus);
	dhdpcie_bus_intr_disable(bus);
	DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);

	status =  dhdpcie_bus_cfg_read_dword(bus, PCIIntstatus, 4);
	dhdpcie_bus_cfg_write_dword(bus, PCIIntstatus, 4, status);

	if (!dhd_download_fw_on_driverload) {
		dhd_dpc_kill(bus->dhd);
	}

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
	pm_runtime_disable(dhd_bus_to_dev(bus));
	pm_runtime_set_suspended(dhd_bus_to_dev(bus));
	pm_runtime_enable(dhd_bus_to_dev(bus));
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

	/* Clear rx control and wake any waiters */
	dhd_os_set_ioctl_resp_timeout(IOCTL_DISABLE_TIMEOUT);
	dhd_wakeup_ioctl_event(bus->dhd, IOCTL_RETURN_ON_BUS_STOP);

done:
	return;
}

/**
 * Watchdog timer function.
 * @param dhd   Represents a specific hardware (dongle) instance that this DHD manages
 */
bool dhd_bus_watchdog(dhd_pub_t *dhd)
{
	unsigned long flags;
	dhd_bus_t *bus = dhd->bus;

	DHD_GENERAL_LOCK(dhd, flags);
	if (DHD_BUS_CHECK_DOWN_OR_DOWN_IN_PROGRESS(dhd) ||
			DHD_BUS_CHECK_SUSPEND_OR_SUSPEND_IN_PROGRESS(dhd)) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		return FALSE;
	}
	DHD_BUS_BUSY_SET_IN_WD(dhd);
	DHD_GENERAL_UNLOCK(dhd, flags);

#ifdef DHD_PCIE_RUNTIMEPM
	dhdpcie_runtime_bus_wake(dhd, TRUE, __builtin_return_address(0));
#endif /* DHD_PCIE_RUNTIMEPM */

	/* Poll for console output periodically */
	if (dhd->busstate == DHD_BUS_DATA &&
		dhd->dhd_console_ms != 0 &&
		bus->bus_low_power_state == DHD_BUS_NO_LOW_POWER_STATE) {
		bus->console.count += dhd_watchdog_ms;
		if (bus->console.count >= dhd->dhd_console_ms) {
			bus->console.count -= dhd->dhd_console_ms;
			/* Make sure backplane clock is on */
			if (dhdpcie_bus_readconsole(bus) < 0)
				dhd->dhd_console_ms = 0; /* On error, stop trying */
		}
	}

	DHD_GENERAL_LOCK(dhd, flags);
	DHD_BUS_BUSY_CLEAR_IN_WD(dhd);
	dhd_os_busbusy_wake(dhd);
	DHD_GENERAL_UNLOCK(dhd, flags);
	return TRUE;
} /* dhd_bus_watchdog */

#if defined(SUPPORT_MULTIPLE_REVISION)
static int concate_revision_bcm4358(dhd_bus_t *bus, char *fw_path, char *nv_path)
{
	uint32 chiprev;
#if defined(SUPPORT_MULTIPLE_CHIPS)
	char chipver_tag[20] = "_4358";
#else
	char chipver_tag[10] = {0, };
#endif /* SUPPORT_MULTIPLE_CHIPS */

	chiprev = dhd_bus_chiprev(bus);
	if (chiprev == 0) {
		DHD_ERROR(("----- CHIP 4358 A0 -----\n"));
		strcat(chipver_tag, "_a0");
	} else if (chiprev == 1) {
		DHD_ERROR(("----- CHIP 4358 A1 -----\n"));
#if defined(SUPPORT_MULTIPLE_CHIPS) || defined(SUPPORT_MULTIPLE_MODULE_CIS)
		strcat(chipver_tag, "_a1");
#endif /* defined(SUPPORT_MULTIPLE_CHIPS) || defined(SUPPORT_MULTIPLE_MODULE_CIS) */
	} else if (chiprev == 3) {
		DHD_ERROR(("----- CHIP 4358 A3 -----\n"));
#if defined(SUPPORT_MULTIPLE_CHIPS)
		strcat(chipver_tag, "_a3");
#endif /* SUPPORT_MULTIPLE_CHIPS */
	} else {
		DHD_ERROR(("----- Unknown chip version, ver=%x -----\n", chiprev));
	}

	strcat(fw_path, chipver_tag);

#if defined(SUPPORT_MULTIPLE_MODULE_CIS) && defined(USE_CID_CHECK)
	if (chiprev == 1 || chiprev == 3) {
		int ret = dhd_check_module_b85a();
		if ((chiprev == 1) && (ret < 0)) {
			memset(chipver_tag, 0x00, sizeof(chipver_tag));
			strcat(chipver_tag, "_b85");
			strcat(chipver_tag, "_a1");
		}
	}

	DHD_ERROR(("%s: chipver_tag %s \n", __FUNCTION__, chipver_tag));
#endif /* defined(SUPPORT_MULTIPLE_MODULE_CIS) && defined(USE_CID_CHECK) */

#if defined(SUPPORT_MULTIPLE_BOARD_REV)
	if (system_rev >= 10) {
		DHD_ERROR(("----- Board Rev  [%d]-----\n", system_rev));
		strcat(chipver_tag, "_r10");
	}
#endif /* SUPPORT_MULTIPLE_BOARD_REV */
	strcat(nv_path, chipver_tag);

	return 0;
}

static int concate_revision_bcm4359(dhd_bus_t *bus, char *fw_path, char *nv_path)
{
	uint32 chip_ver;
	char chipver_tag[10] = {0, };
#if defined(SUPPORT_MULTIPLE_MODULE_CIS) && defined(USE_CID_CHECK) && \
	defined(SUPPORT_BCM4359_MIXED_MODULES)
	int module_type = -1;
#endif /* SUPPORT_MULTIPLE_MODULE_CIS && USE_CID_CHECK && SUPPORT_BCM4359_MIXED_MODULES */

	chip_ver = bus->sih->chiprev;
	if (chip_ver == 4) {
		DHD_ERROR(("----- CHIP 4359 B0 -----\n"));
		strncat(chipver_tag, "_b0", strlen("_b0"));
	} else if (chip_ver == 5) {
		DHD_ERROR(("----- CHIP 4359 B1 -----\n"));
		strncat(chipver_tag, "_b1", strlen("_b1"));
	} else if (chip_ver == 9) {
		DHD_ERROR(("----- CHIP 4359 C0 -----\n"));
		strncat(chipver_tag, "_c0", strlen("_c0"));
	} else {
		DHD_ERROR(("----- Unknown chip version, ver=%x -----\n", chip_ver));
		return -1;
	}

#if defined(SUPPORT_MULTIPLE_MODULE_CIS) && defined(USE_CID_CHECK) && \
	defined(SUPPORT_BCM4359_MIXED_MODULES)
	module_type =  dhd_check_module_b90();

	switch (module_type) {
		case BCM4359_MODULE_TYPE_B90B:
			strcat(fw_path, chipver_tag);
			break;
		case BCM4359_MODULE_TYPE_B90S:
		default:
			/*
			 * .cid.info file not exist case,
			 * loading B90S FW force for initial MFG boot up.
			*/
			if (chip_ver == 5) {
				strncat(fw_path, "_b90s", strlen("_b90s"));
			}
			strcat(fw_path, chipver_tag);
			strcat(nv_path, chipver_tag);
			break;
	}
#else /* SUPPORT_MULTIPLE_MODULE_CIS && USE_CID_CHECK && SUPPORT_BCM4359_MIXED_MODULES */
	strcat(fw_path, chipver_tag);
	strcat(nv_path, chipver_tag);
#endif /* SUPPORT_MULTIPLE_MODULE_CIS && USE_CID_CHECK && SUPPORT_BCM4359_MIXED_MODULES */

	return 0;
}

#if defined(SUPPORT_BCM4361_MIXED_MODULES) && defined(USE_CID_CHECK)

#define MAX_EXTENSION 20
#define MODULE_BCM4361_INDEX	3
#define CHIP_REV_A0	1
#define CHIP_REV_A1	2
#define CHIP_REV_B0	3
#define CHIP_REV_B1	4
#define CHIP_REV_B2	5
#define CHIP_REV_C0	6
#define BOARD_TYPE_EPA				0x080f
#define BOARD_TYPE_IPA				0x0827
#define BOARD_TYPE_IPA_OLD			0x081a
#define DEFAULT_CIDINFO_FOR_EPA		"r00a_e000_a0_ePA"
#define DEFAULT_CIDINFO_FOR_IPA		"r00a_e000_a0_iPA"
#define DEFAULT_CIDINFO_FOR_A1		"r01a_e30a_a1"
#define DEFAULT_CIDINFO_FOR_B0		"r01i_e32_b0"
#define MAX_VID_LEN					8
#define MAX_VNAME_LEN				30
#define CIS_TUPLE_HDR_LEN			2
#define CIS_TUPLE_START_ADDRESS		0x18011110
#define CIS_TUPLE_END_ADDRESS		0x18011167
#define CIS_TUPLE_MAX_COUNT			(CIS_TUPLE_END_ADDRESS - CIS_TUPLE_START_ADDRESS\
	+ 1) / sizeof(uint32)
#define CIS_TUPLE_TAG_START			0x80
#define CIS_TUPLE_TAG_VENDOR		0x81
#define CIS_TUPLE_TAG_BOARDTYPE		0x1b
#define CIS_TUPLE_TAG_LENGTH		1
#define NVRAM_FEM_MURATA			"_murata"
#define CID_FEM_MURATA				"_mur_"

typedef struct cis_tuple_format {
	uint8	id;
	uint8	len;	/* total length of tag and data */
	uint8	tag;
	uint8	data[1];
} cis_tuple_format_t;

typedef struct {
	char cid_ext[MAX_EXTENSION];
	char nvram_ext[MAX_EXTENSION];
	char fw_ext[MAX_EXTENSION];
} naming_info_t;

naming_info_t bcm4361_naming_table[] = {
	{ {""}, {""}, {""} },
	{ {"r00a_e000_a0_ePA"}, {"_a0_ePA"}, {"_a0_ePA"} },
	{ {"r00a_e000_a0_iPA"}, {"_a0"}, {"_a1"} },
	{ {"r01a_e30a_a1"}, {"_r01a_a1"}, {"_a1"} },
	{ {"r02a_e30a_a1"}, {"_r02a_a1"}, {"_a1"} },
	{ {"r02c_e30a_a1"}, {"_r02c_a1"}, {"_a1"} },
	{ {"r01d_e31_b0"}, {"_r01d_b0"}, {"_b0"} },
	{ {"r01f_e31_b0"}, {"_r01f_b0"}, {"_b0"} },
	{ {"r02g_e31_b0"}, {"_r02g_b0"}, {"_b0"} },
	{ {"r01h_e32_b0"}, {"_r01h_b0"}, {"_b0"} },
	{ {"r01i_e32_b0"}, {"_r01i_b0"}, {"_b0"} },
	{ {"r02j_e32_b0"}, {"_r02j_b0"}, {"_b0"} },
	{ {"r012_1kl_a1"}, {"_r012_a1"}, {"_a1"} },
	{ {"r013_1kl_b0"}, {"_r013_b0"}, {"_b0"} },
	{ {"r013_1kl_b0"}, {"_r013_b0"}, {"_b0"} },
	{ {"r014_1kl_b0"}, {"_r014_b0"}, {"_b0"} },
	{ {"r015_1kl_b0"}, {"_r015_b0"}, {"_b0"} },
	{ {"r020_1kl_b0"}, {"_r020_b0"}, {"_b0"} },
	{ {"r021_1kl_b0"}, {"_r021_b0"}, {"_b0"} },
	{ {"r022_1kl_b0"}, {"_r022_b0"}, {"_b0"} },
	{ {"r023_1kl_b0"}, {"_r023_b0"}, {"_b0"} },
	{ {"r024_1kl_b0"}, {"_r024_b0"}, {"_b0"} },
	{ {"r030_1kl_b0"}, {"_r030_b0"}, {"_b0"} },
	{ {"r031_1kl_b0"}, {"_r030_b0"}, {"_b0"} },	/* exceptional case : r31 -> r30 */
	{ {"r032_1kl_b0"}, {"_r032_b0"}, {"_b0"} },
	{ {"r033_1kl_b0"}, {"_r033_b0"}, {"_b0"} },
	{ {"r034_1kl_b0"}, {"_r034_b0"}, {"_b0"} },
	{ {"r02a_e32a_b2"}, {"_r02a_b2"}, {"_b2"} },
	{ {"r02b_e32a_b2"}, {"_r02b_b2"}, {"_b2"} },
	{ {"r020_1qw_b2"}, {"_r020_b2"}, {"_b2"} },
	{ {"r021_1qw_b2"}, {"_r021_b2"}, {"_b2"} },
	{ {"r022_1qw_b2"}, {"_r022_b2"}, {"_b2"} },
	{ {"r031_1qw_b2"}, {"_r031_b2"}, {"_b2"} },
	{ {"r032_1qw_b2"}, {"_r032_b2"}, {"_b2"} },
	{ {"r041_1qw_b2"}, {"_r041_b2"}, {"_b2"} }
};

static naming_info_t *
dhd_find_naming_info(naming_info_t table[], int table_size, char *module_type)
{
	int index_found = 0, i = 0;

	if (module_type && strlen(module_type) > 0) {
		for (i = 1; i < table_size; i++) {
			if (!strncmp(table[i].cid_ext, module_type, strlen(table[i].cid_ext))) {
				index_found = i;
				break;
			}
		}
	}

	DHD_INFO(("%s: index_found=%d\n", __FUNCTION__, index_found));

	return &table[index_found];
}

static naming_info_t *
dhd_find_naming_info_by_cid(naming_info_t table[], int table_size,
	char *cid_info)
{
	int index_found = 0, i = 0;
	char *ptr;

	/* truncate extension */
	for (i = 1, ptr = cid_info; i < MODULE_BCM4361_INDEX && ptr; i++) {
		ptr = bcmstrstr(ptr, "_");
		if (ptr) {
			ptr++;
		}
	}

	for (i = 1; i < table_size && ptr; i++) {
		if (!strncmp(table[i].cid_ext, ptr, strlen(table[i].cid_ext))) {
			index_found = i;
			break;
		}
	}

	DHD_INFO(("%s: index_found=%d\n", __FUNCTION__, index_found));

	return &table[index_found];
}

static int
dhd_parse_board_information_bcm4361(dhd_bus_t *bus, int *boardtype,
	unsigned char *vid, int *vid_length)
{
	int boardtype_backplane_addr[] = {
		0x18010324, /* OTP Control 1 */
		0x18012618, /* PMU min resource mask */
	};
	int boardtype_backplane_data[] = {
		0x00fa0000,
		0x0e4fffff /* Keep on ARMHTAVAIL */
	};
	int int_val = 0, i = 0;
	cis_tuple_format_t *tuple;
	int totlen, len;
	uint32 raw_data[CIS_TUPLE_MAX_COUNT];

	for (i = 0; i < ARRAYSIZE(boardtype_backplane_addr); i++) {
		/* Write new OTP and PMU configuration */
		if (si_backplane_access(bus->sih, boardtype_backplane_addr[i], sizeof(int),
				&boardtype_backplane_data[i], FALSE) != BCME_OK) {
			DHD_ERROR(("invalid size/addr combination\n"));
			return BCME_ERROR;
		}

		if (si_backplane_access(bus->sih, boardtype_backplane_addr[i], sizeof(int),
				&int_val, TRUE) != BCME_OK) {
			DHD_ERROR(("invalid size/addr combination\n"));
			return BCME_ERROR;
		}

		DHD_INFO(("%s: boardtype_backplane_addr 0x%08x rdata 0x%04x\n",
			__FUNCTION__, boardtype_backplane_addr[i], int_val));
	}

	/* read tuple raw data */
	for (i = 0; i < CIS_TUPLE_MAX_COUNT; i++) {
		if (si_backplane_access(bus->sih, CIS_TUPLE_START_ADDRESS + i * sizeof(uint32),
				sizeof(uint32),	&raw_data[i], TRUE) != BCME_OK) {
			break;
		}
	}

	totlen = i * sizeof(uint32);
	tuple = (cis_tuple_format_t *)raw_data;

	/* check the first tuple has tag 'start' */
	if (tuple->id != CIS_TUPLE_TAG_START) {
		return BCME_ERROR;
	}

	*vid_length = *boardtype = 0;

	/* find tagged parameter */
	while ((totlen >= (tuple->len + CIS_TUPLE_HDR_LEN)) &&
			(*vid_length == 0 || *boardtype == 0)) {
		len = tuple->len;

		if ((tuple->tag == CIS_TUPLE_TAG_VENDOR) &&
				(totlen >= (int)(len + CIS_TUPLE_HDR_LEN))) {
			/* found VID */
			memcpy(vid, tuple->data, tuple->len - CIS_TUPLE_TAG_LENGTH);
			*vid_length = tuple->len - CIS_TUPLE_TAG_LENGTH;
			prhex("OTP VID", tuple->data, tuple->len - CIS_TUPLE_TAG_LENGTH);
		}
		else if ((tuple->tag == CIS_TUPLE_TAG_BOARDTYPE) &&
				(totlen >= (int)(len + CIS_TUPLE_HDR_LEN))) {
			/* found boardtype */
			*boardtype = (int)tuple->data[0];
			prhex("OTP boardtype", tuple->data, tuple->len - CIS_TUPLE_TAG_LENGTH);
		}

		tuple = (cis_tuple_format_t*)((uint8*)tuple + (len + CIS_TUPLE_HDR_LEN));
		totlen -= (len + CIS_TUPLE_HDR_LEN);
	}

	if (*vid_length <= 0 || *boardtype <= 0) {
		DHD_ERROR(("failed to parse information (vid=%d, boardtype=%d)\n",
			*vid_length, *boardtype));
		return BCME_ERROR;
	}

	return BCME_OK;

}

static naming_info_t *
dhd_find_naming_info_by_chip_rev(naming_info_t table[], int table_size,
	dhd_bus_t *bus, bool *is_murata_fem)
{
	int board_type = 0, chip_rev = 0, vid_length = 0;
	unsigned char vid[MAX_VID_LEN];
	naming_info_t *info = &table[0];
	char *cid_info = NULL;

	if (!bus || !bus->sih) {
		DHD_ERROR(("%s:bus(%p) or bus->sih is NULL\n", __FUNCTION__, bus));
		return NULL;
	}
	chip_rev = bus->sih->chiprev;

	if (dhd_parse_board_information_bcm4361(bus, &board_type, vid, &vid_length)
			!= BCME_OK) {
		DHD_ERROR(("%s:failed to parse board information\n", __FUNCTION__));
		return NULL;
	}

	DHD_INFO(("%s:chip version %d\n", __FUNCTION__, chip_rev));

	/* A0 chipset has exception only */
	if (chip_rev == CHIP_REV_A0) {
		if (board_type == BOARD_TYPE_EPA) {
			info = dhd_find_naming_info(table, table_size,
				DEFAULT_CIDINFO_FOR_EPA);
		} else if ((board_type == BOARD_TYPE_IPA) ||
				(board_type == BOARD_TYPE_IPA_OLD)) {
			info = dhd_find_naming_info(table, table_size,
				DEFAULT_CIDINFO_FOR_IPA);
		}
	} else {
		cid_info = dhd_get_cid_info(vid, vid_length);
		if (cid_info) {
			info = dhd_find_naming_info_by_cid(table, table_size, cid_info);
			if (strstr(cid_info, CID_FEM_MURATA)) {
				*is_murata_fem = TRUE;
			}
		}
	}

	return info;
}
#endif /* SUPPORT_BCM4361_MIXED_MODULES && USE_CID_CHECK */

static int
concate_revision_bcm4361(dhd_bus_t *bus, char *fw_path, char *nv_path)
{
	int ret = BCME_OK;
#if defined(SUPPORT_BCM4361_MIXED_MODULES) && defined(USE_CID_CHECK)
	char module_type[MAX_VNAME_LEN];
	naming_info_t *info = NULL;
	bool is_murata_fem = FALSE;

	memset(module_type, 0, sizeof(module_type));

	if (dhd_check_module_bcm4361(module_type,
			MODULE_BCM4361_INDEX, &is_murata_fem) == BCME_OK) {
		info = dhd_find_naming_info(bcm4361_naming_table,
			ARRAYSIZE(bcm4361_naming_table), module_type);
	} else {
		/* in case of .cid.info doesn't exists */
		info = dhd_find_naming_info_by_chip_rev(bcm4361_naming_table,
			ARRAYSIZE(bcm4361_naming_table), bus, &is_murata_fem);
	}

	if (bcmstrnstr(nv_path, PATH_MAX,  "_murata", 7)) {
		is_murata_fem = FALSE;
	}

	if (info) {
		if (is_murata_fem) {
			strncat(nv_path, NVRAM_FEM_MURATA, strlen(NVRAM_FEM_MURATA));
		}
		strncat(nv_path, info->nvram_ext, strlen(info->nvram_ext));
		strncat(fw_path, info->fw_ext, strlen(info->fw_ext));
	} else {
		DHD_ERROR(("%s:failed to find extension for nvram and firmware\n", __FUNCTION__));
		ret = BCME_ERROR;
	}
#else /* SUPPORT_MULTIPLE_MODULE_CIS && USE_CID_CHECK */
	char chipver_tag[10] = {0, };

	strcat(fw_path, chipver_tag);
	strcat(nv_path, chipver_tag);
#endif /* SUPPORT_MULTIPLE_MODULE_CIS && USE_CID_CHECK */

	return ret;
}

int
concate_revision(dhd_bus_t *bus, char *fw_path, char *nv_path)
{
	int res = 0;

	if (!bus || !bus->sih) {
		DHD_ERROR(("%s:Bus is Invalid\n", __FUNCTION__));
		return -1;
	}

	if (!fw_path || !nv_path) {
		DHD_ERROR(("fw_path or nv_path is null.\n"));
		return res;
	}

	switch (si_chipid(bus->sih)) {

	case BCM43569_CHIP_ID:
	case BCM4358_CHIP_ID:
		res = concate_revision_bcm4358(bus, fw_path, nv_path);
		break;
	case BCM4355_CHIP_ID:
	case BCM4359_CHIP_ID:
		res = concate_revision_bcm4359(bus, fw_path, nv_path);
		break;
	case BCM4361_CHIP_ID:
	case BCM4347_CHIP_ID:
		res = concate_revision_bcm4361(bus, fw_path, nv_path);
		break;
	default:
		DHD_ERROR(("REVISION SPECIFIC feature is not required\n"));
		return res;
	}

	return res;
}
#endif /* SUPPORT_MULTIPLE_REVISION */

uint16
dhd_get_chipid(dhd_pub_t *dhd)
{
	dhd_bus_t *bus = dhd->bus;

	if (bus && bus->sih)
		return (uint16)si_chipid(bus->sih);
	else
		return 0;
}

/**
 * Loads firmware given by caller supplied path and nvram image into PCIe dongle.
 *
 * BCM_REQUEST_FW specific :
 * Given the chip type, determines the to be used file paths within /lib/firmware/brcm/ containing
 * firmware and nvm for that chip. If the download fails, retries download with a different nvm file
 *
 * BCMEMBEDIMAGE specific:
 * If bus->fw_path is empty, or if the download of bus->fw_path failed, firmware contained in header
 * file will be used instead.
 *
 * @return BCME_OK on success
 */
int
dhd_bus_download_firmware(struct dhd_bus *bus, osl_t *osh,
                          char *pfw_path, char *pnv_path)
{
	int ret;

	bus->fw_path = pfw_path;
	bus->nv_path = pnv_path;

#if defined(SUPPORT_MULTIPLE_REVISION)
	if (concate_revision(bus, bus->fw_path, bus->nv_path) != 0) {
		DHD_ERROR(("%s: fail to concatnate revison \n",
			__FUNCTION__));
		return BCME_BADARG;
	}
#endif /* SUPPORT_MULTIPLE_REVISION */

#if defined(DHD_BLOB_EXISTENCE_CHECK)
	dhd_set_blob_support(bus->dhd, bus->fw_path);
#endif /* DHD_BLOB_EXISTENCE_CHECK */

	DHD_ERROR(("%s: firmware path=%s, nvram path=%s\n",
		__FUNCTION__, bus->fw_path, bus->nv_path));

	ret = dhdpcie_download_firmware(bus, osh);

	return ret;
}

/**
 * Loads firmware given by 'bus->fw_path' into PCIe dongle.
 *
 * BCM_REQUEST_FW specific :
 * Given the chip type, determines the to be used file paths within /lib/firmware/brcm/ containing
 * firmware and nvm for that chip. If the download fails, retries download with a different nvm file
 *
 * BCMEMBEDIMAGE specific:
 * If bus->fw_path is empty, or if the download of bus->fw_path failed, firmware contained in header
 * file will be used instead.
 *
 * @return BCME_OK on success
 */
static int
dhdpcie_download_firmware(struct dhd_bus *bus, osl_t *osh)
{
	int ret = 0;
#if defined(BCM_REQUEST_FW)
	uint chipid = bus->sih->chip;
	uint revid = bus->sih->chiprev;
	char fw_path[64] = "/lib/firmware/brcm/bcm";	/* path to firmware image */
	char nv_path[64];		/* path to nvram vars file */
	bus->fw_path = fw_path;
	bus->nv_path = nv_path;
	switch (chipid) {
	case BCM43570_CHIP_ID:
		bcmstrncat(fw_path, "43570", 5);
		switch (revid) {
		case 0:
			bcmstrncat(fw_path, "a0", 2);
			break;
		case 2:
			bcmstrncat(fw_path, "a2", 2);
			break;
		default:
			DHD_ERROR(("%s: revid is not found %x\n", __FUNCTION__,
			revid));
			break;
		}
		break;
	default:
		DHD_ERROR(("%s: unsupported device %x\n", __FUNCTION__,
		chipid));
		return 0;
	}
	/* load board specific nvram file */
	snprintf(bus->nv_path, sizeof(nv_path), "%s.nvm", fw_path);
	/* load firmware */
	snprintf(bus->fw_path, sizeof(fw_path), "%s-firmware.bin", fw_path);
#endif /* BCM_REQUEST_FW */

	DHD_OS_WAKE_LOCK(bus->dhd);
	ret = _dhdpcie_download_firmware(bus);

	DHD_OS_WAKE_UNLOCK(bus->dhd);
	return ret;
} /* dhdpcie_download_firmware */

#define DHD_MEMORY_SET_PATTERN 0xAA

/**
 * Downloads a file containing firmware into dongle memory. In case of a .bea file, the DHD
 * is updated with the event logging partitions within that file as well.
 *
 * @param pfw_path    Path to .bin or .bea file
 */
static int
dhdpcie_download_code_file(struct dhd_bus *bus, char *pfw_path)
{
	int bcmerror = BCME_ERROR;
	int offset = 0;
#if defined(DHD_FW_MEM_CORRUPTION)
	uint8 *p_org_fw  = NULL;
	uint32 org_fw_size = 0;
	uint32 fw_write_offset = 0;
#endif /* DHD_FW_MEM_CORRUPTION */
	int len = 0;
	bool store_reset;
	char *imgbuf = NULL;
	uint8 *memblock = NULL, *memptr;
	int offset_end = bus->ramsize;

	DHD_ERROR(("%s: download firmware %s\n", __FUNCTION__, pfw_path));

	/* Should succeed in opening image if it is actually given through registry
	 * entry or in module param.
	 */
	imgbuf = dhd_os_open_image1(bus->dhd, pfw_path);
	if (imgbuf == NULL) {
		goto err;
	}

	memptr = memblock = MALLOC(bus->dhd->osh, MEMBLOCK + DHD_SDALIGN);
	if (memblock == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, MEMBLOCK));
		bcmerror = BCME_NOMEM;
		goto err;
	}
	if ((uint32)(uintptr)memblock % DHD_SDALIGN) {
		memptr += (DHD_SDALIGN - ((uint32)(uintptr)memblock % DHD_SDALIGN));
	}

#if defined(DHD_FW_MEM_CORRUPTION)
	if (dhd_bus_get_fw_mode(bus->dhd) == DHD_FLAG_MFG_MODE) {
		org_fw_size = dhd_os_get_image_size(imgbuf);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_MEMDUMP)
		p_org_fw = (uint8*)DHD_OS_PREALLOC(bus->dhd,
			DHD_PREALLOC_MEMDUMP_RAM, org_fw_size);
#else
		p_org_fw = (uint8*)VMALLOC(bus->dhd->osh, org_fw_size);
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_MEMDUMP */
		if (p_org_fw == NULL) {
			DHD_ERROR(("%s: Failed to allocate memory %d bytes for download check\n",
				__FUNCTION__, org_fw_size));
			bcmerror = BCME_NOMEM;
			goto err;
		} else {
			memset(p_org_fw, 0, org_fw_size);
		}
	}
#endif /* DHD_FW_MEM_CORRUPTION */

	/* check if CR4/CA7 */
	store_reset = (si_setcore(bus->sih, ARMCR4_CORE_ID, 0) ||
			si_setcore(bus->sih, ARMCA7_CORE_ID, 0));
	/* Download image with MEMBLOCK size */
	while ((len = dhd_os_get_image_block((char*)memptr, MEMBLOCK, imgbuf))) {
		if (len < 0) {
			DHD_ERROR(("%s: dhd_os_get_image_block failed (%d)\n", __FUNCTION__, len));
			bcmerror = BCME_ERROR;
			goto err;
		}
		/* if address is 0, store the reset instruction to be written in 0 */
		if (store_reset) {
			ASSERT(offset == 0);
			bus->resetinstr = *(((uint32*)memptr));
			/* Add start of RAM address to the address given by user */
			offset += bus->dongle_ram_base;
			offset_end += offset;
			store_reset = FALSE;
		}

		bcmerror = dhdpcie_bus_membytes(bus, TRUE, offset, (uint8 *)memptr, len);
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
				__FUNCTION__, bcmerror, MEMBLOCK, offset));
			goto err;
		}
		offset += MEMBLOCK;
#if defined(DHD_FW_MEM_CORRUPTION)
		if (dhd_bus_get_fw_mode(bus->dhd) == DHD_FLAG_MFG_MODE) {
			memcpy((p_org_fw + fw_write_offset), memptr, len);
			fw_write_offset += len;
		}
#endif /* DHD_FW_MEM_CORRUPTION */

		if (offset >= offset_end) {
			DHD_ERROR(("%s: invalid address access to %x (offset end: %x)\n",
				__FUNCTION__, offset, offset_end));
			bcmerror = BCME_ERROR;
			goto err;
		}
	}
#ifdef DHD_FW_MEM_CORRUPTION
	/* Read and compare the downloaded code */
	if (dhd_bus_get_fw_mode(bus->dhd) == DHD_FLAG_MFG_MODE) {
		unsigned char *p_readback_buf = NULL;
		uint32 compared_len;
		uint32 remaining_len = 0;

		compared_len = 0;
		p_readback_buf = MALLOC(bus->dhd->osh, MEMBLOCK);
		if (p_readback_buf == NULL) {
			DHD_ERROR(("%s: Failed to allocate memory %d bytes for readback buffer\n",
				__FUNCTION__, MEMBLOCK));
			bcmerror = BCME_NOMEM;
			goto compare_err;
		}
		/* Read image to verify downloaded contents. */
		offset = bus->dongle_ram_base;

		while (compared_len  < org_fw_size) {
			memset(p_readback_buf, DHD_MEMORY_SET_PATTERN, MEMBLOCK);
			remaining_len = org_fw_size - compared_len;

			if (remaining_len >= MEMBLOCK) {
				len = MEMBLOCK;
			} else {
				len = remaining_len;
			}
			bcmerror = dhdpcie_bus_membytes(bus, FALSE, offset,
				(uint8 *)p_readback_buf, len);
			if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d membytes at 0x%08x\n",
					__FUNCTION__, bcmerror, MEMBLOCK, offset));
				goto compare_err;
			}

			if (memcmp((p_org_fw + compared_len), p_readback_buf, len) != 0) {
				DHD_ERROR(("%s: Downloaded image is corrupted. offset %d\n",
					__FUNCTION__, compared_len));
				bcmerror = BCME_ERROR;
				goto compare_err;
			}

			compared_len += len;
			offset += len;
		}
		DHD_ERROR(("%s: Download, Upload and compare succeeded.\n", __FUNCTION__));

compare_err:
		if (p_readback_buf) {
			MFREE(bus->dhd->osh, p_readback_buf, MEMBLOCK);
		}
	}
#endif /* DHD_FW_MEM_CORRUPTION */

err:
#if defined(DHD_FW_MEM_CORRUPTION)
	if (p_org_fw) {
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_MEMDUMP)
		DHD_OS_PREFREE(bus->dhd, p_org_fw, org_fw_size);
#else
		VMFREE(bus->dhd->osh, p_org_fw, org_fw_size);
#endif // endif
	}
#endif /* DHD_FW_MEM_CORRUPTION */
	if (memblock) {
		MFREE(bus->dhd->osh, memblock, MEMBLOCK + DHD_SDALIGN);
	}

	if (imgbuf) {
		dhd_os_close_image1(bus->dhd, imgbuf);
	}

	return bcmerror;
} /* dhdpcie_download_code_file */

#ifdef CUSTOMER_HW4_DEBUG
#define MIN_NVRAMVARS_SIZE 128
#endif /* CUSTOMER_HW4_DEBUG */

static int
dhdpcie_download_nvram(struct dhd_bus *bus)
{
	int bcmerror = BCME_ERROR;
	uint len;
	char * memblock = NULL;
	char *bufp;
	char *pnv_path;
	bool nvram_file_exists;
	bool nvram_uefi_exists = FALSE;
	bool local_alloc = FALSE;
	pnv_path = bus->nv_path;

	nvram_file_exists = ((pnv_path != NULL) && (pnv_path[0] != '\0'));

	/* First try UEFI */
	len = MAX_NVRAMBUF_SIZE;
	dhd_get_download_buffer(bus->dhd, NULL, NVRAM, &memblock, (int *)&len);

	/* If UEFI empty, then read from file system */
	if ((len <= 0) || (memblock == NULL)) {

		if (nvram_file_exists) {
			len = MAX_NVRAMBUF_SIZE;
			dhd_get_download_buffer(bus->dhd, pnv_path, NVRAM, &memblock, (int *)&len);
			if ((len <= 0 || len > MAX_NVRAMBUF_SIZE)) {
				goto err;
			}
		}
		else {
			/* For SROM OTP no external file or UEFI required */
			bcmerror = BCME_OK;
		}
	} else {
		nvram_uefi_exists = TRUE;
	}

	DHD_ERROR(("%s: dhd_get_download_buffer len %d\n", __FUNCTION__, len));

	if (len > 0 && len <= MAX_NVRAMBUF_SIZE && memblock != NULL) {
		bufp = (char *) memblock;

		{
			bufp[len] = 0;
			if (nvram_uefi_exists || nvram_file_exists) {
				len = process_nvram_vars(bufp, len);
			}
		}

		DHD_ERROR(("%s: process_nvram_vars len %d\n", __FUNCTION__, len));
#ifdef CUSTOMER_HW4_DEBUG
		if (len < MIN_NVRAMVARS_SIZE) {
			DHD_ERROR(("%s: invalid nvram size in process_nvram_vars \n",
				__FUNCTION__));
			bcmerror = BCME_ERROR;
			goto err;
		}
#endif /* CUSTOMER_HW4_DEBUG */

		if (len % 4) {
			len += 4 - (len % 4);
		}
		bufp += len;
		*bufp++ = 0;
		if (len)
			bcmerror = dhdpcie_downloadvars(bus, memblock, len + 1);
		if (bcmerror) {
			DHD_ERROR(("%s: error downloading vars: %d\n",
				__FUNCTION__, bcmerror));
		}
	}

err:
	if (memblock) {
		if (local_alloc) {
			MFREE(bus->dhd->osh, memblock, MAX_NVRAMBUF_SIZE);
		} else {
			dhd_free_download_buffer(bus->dhd, memblock, MAX_NVRAMBUF_SIZE);
		}
	}

	return bcmerror;
}

static int
dhdpcie_ramsize_read_image(struct dhd_bus *bus, char *buf, int len)
{
	int bcmerror = BCME_ERROR;
	char *imgbuf = NULL;

	if (buf == NULL || len == 0)
		goto err;

	/* External image takes precedence if specified */
	if ((bus->fw_path != NULL) && (bus->fw_path[0] != '\0')) {
		// opens and seeks to correct file offset:
		imgbuf = dhd_os_open_image1(bus->dhd, bus->fw_path);
		if (imgbuf == NULL) {
			DHD_ERROR(("%s: Failed to open firmware file\n", __FUNCTION__));
			goto err;
		}

		/* Read it */
		if (len != dhd_os_get_image_block(buf, len, imgbuf)) {
			DHD_ERROR(("%s: Failed to read %d bytes data\n", __FUNCTION__, len));
			goto err;
		}

		bcmerror = BCME_OK;
	}

err:
	if (imgbuf)
		dhd_os_close_image1(bus->dhd, imgbuf);

	return bcmerror;
}

/* The ramsize can be changed in the dongle image, for example 4365 chip share the sysmem
 * with BMC and we can adjust how many sysmem belong to CA7 during dongle compilation.
 * So in DHD we need to detect this case and update the correct dongle RAMSIZE as well.
 */
static void
dhdpcie_ramsize_adj(struct dhd_bus *bus)
{
	int i, search_len = 0;
	uint8 *memptr = NULL;
	uint8 *ramsizeptr = NULL;
	uint ramsizelen;
	uint32 ramsize_ptr_ptr[] = {RAMSIZE_PTR_PTR_LIST};
	hnd_ramsize_ptr_t ramsize_info;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	/* Adjust dongle RAMSIZE already called. */
	if (bus->ramsize_adjusted) {
		return;
	}

	/* success or failure,  we don't want to be here
	 * more than once.
	 */
	bus->ramsize_adjusted = TRUE;

	/* Not handle if user restrict dongle ram size enabled */
	if (dhd_dongle_memsize) {
		DHD_ERROR(("%s: user restrict dongle ram size to %d.\n", __FUNCTION__,
			dhd_dongle_memsize));
		return;
	}

	/* Out immediately if no image to download */
	if ((bus->fw_path == NULL) || (bus->fw_path[0] == '\0')) {
		DHD_ERROR(("%s: no fimrware file\n", __FUNCTION__));
		return;
	}

	/* Get maximum RAMSIZE info search length */
	for (i = 0; ; i++) {
		if (ramsize_ptr_ptr[i] == RAMSIZE_PTR_PTR_END)
			break;

		if (search_len < (int)ramsize_ptr_ptr[i])
			search_len = (int)ramsize_ptr_ptr[i];
	}

	if (!search_len)
		return;

	search_len += sizeof(hnd_ramsize_ptr_t);

	memptr = MALLOC(bus->dhd->osh, search_len);
	if (memptr == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, search_len));
		return;
	}

	/* External image takes precedence if specified */
	if (dhdpcie_ramsize_read_image(bus, (char *)memptr, search_len) != BCME_OK) {
		goto err;
	}
	else {
		ramsizeptr = memptr;
		ramsizelen = search_len;
	}

	if (ramsizeptr) {
		/* Check Magic */
		for (i = 0; ; i++) {
			if (ramsize_ptr_ptr[i] == RAMSIZE_PTR_PTR_END)
				break;

			if (ramsize_ptr_ptr[i] + sizeof(hnd_ramsize_ptr_t) > ramsizelen)
				continue;

			memcpy((char *)&ramsize_info, ramsizeptr + ramsize_ptr_ptr[i],
				sizeof(hnd_ramsize_ptr_t));

			if (ramsize_info.magic == HTOL32(HND_RAMSIZE_PTR_MAGIC)) {
				bus->orig_ramsize = LTOH32(ramsize_info.ram_size);
				bus->ramsize = LTOH32(ramsize_info.ram_size);
				DHD_ERROR(("%s: Adjust dongle RAMSIZE to 0x%x\n", __FUNCTION__,
					bus->ramsize));
				break;
			}
		}
	}

err:
	if (memptr)
		MFREE(bus->dhd->osh, memptr, search_len);

	return;
} /* dhdpcie_ramsize_adj */

/**
 * Downloads firmware file given by 'bus->fw_path' into PCIe dongle
 *
 * BCMEMBEDIMAGE specific:
 * If bus->fw_path is empty, or if the download of bus->fw_path failed, firmware contained in header
 * file will be used instead.
 *
 */
static int
_dhdpcie_download_firmware(struct dhd_bus *bus)
{
	int bcmerror = -1;

	bool embed = FALSE;	/* download embedded firmware */
	bool dlok = FALSE;	/* download firmware succeeded */

	/* Out immediately if no image to download */
	if ((bus->fw_path == NULL) || (bus->fw_path[0] == '\0')) {
		DHD_ERROR(("%s: no fimrware file\n", __FUNCTION__));
		return 0;
	}
	/* Adjust ram size */
	dhdpcie_ramsize_adj(bus);

	/* Keep arm in reset */
	if (dhdpcie_bus_download_state(bus, TRUE)) {
		DHD_ERROR(("%s: error placing ARM core in reset\n", __FUNCTION__));
		goto err;
	}

	/* External image takes precedence if specified */
	if ((bus->fw_path != NULL) && (bus->fw_path[0] != '\0')) {
		if (dhdpcie_download_code_file(bus, bus->fw_path)) {
			DHD_ERROR(("%s:%d dongle image file download failed\n", __FUNCTION__,
				__LINE__));
			goto err;
		} else {
			embed = FALSE;
			dlok = TRUE;
		}
	}

	BCM_REFERENCE(embed);
	if (!dlok) {
		DHD_ERROR(("%s:%d dongle image download failed\n", __FUNCTION__, __LINE__));
		goto err;
	}

	/* EXAMPLE: nvram_array */
	/* If a valid nvram_arry is specified as above, it can be passed down to dongle */
	/* dhd_bus_set_nvram_params(bus, (char *)&nvram_array); */

	/* External nvram takes precedence if specified */
	if (dhdpcie_download_nvram(bus)) {
		DHD_ERROR(("%s:%d dongle nvram file download failed\n", __FUNCTION__, __LINE__));
		goto err;
	}

	/* Take arm out of reset */
	if (dhdpcie_bus_download_state(bus, FALSE)) {
		DHD_ERROR(("%s: error getting out of ARM core reset\n", __FUNCTION__));
		goto err;
	}

	bcmerror = 0;

err:
	return bcmerror;
} /* _dhdpcie_download_firmware */

#define CONSOLE_LINE_MAX	192

static int
dhdpcie_bus_readconsole(dhd_bus_t *bus)
{
	dhd_console_t *c = &bus->console;
	uint8 line[CONSOLE_LINE_MAX], ch;
	uint32 n, idx, addr;
	int rv;

	/* Don't do anything until FWREADY updates console address */
	if (bus->console_addr == 0)
		return -1;

	/* Read console log struct */
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, log);

	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)&c->log, sizeof(c->log))) < 0)
		return rv;

	/* Allocate console buffer (one time only) */
	if (c->buf == NULL) {
		c->bufsize = ltoh32(c->log.buf_size);
		if ((c->buf = MALLOC(bus->dhd->osh, c->bufsize)) == NULL)
			return BCME_NOMEM;
	}
	idx = ltoh32(c->log.idx);

	/* Protect against corrupt value */
	if (idx > c->bufsize)
		return BCME_ERROR;

	/* Skip reading the console buffer if the index pointer has not moved */
	if (idx == c->last)
		return BCME_OK;

	/* Read the console buffer */
	addr = ltoh32(c->log.buf);
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, c->buf, c->bufsize)) < 0)
		return rv;

	while (c->last != idx) {
		for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
			if (c->last == idx) {
				/* This would output a partial line.  Instead, back up
				 * the buffer pointer and output this line next time around.
				 */
				if (c->last >= n)
					c->last -= n;
				else
					c->last = c->bufsize - n;
				goto break2;
			}
			ch = c->buf[c->last];
			c->last = (c->last + 1) % c->bufsize;
			if (ch == '\n')
				break;
			line[n] = ch;
		}

		if (n > 0) {
			if (line[n - 1] == '\r')
				n--;
			line[n] = 0;
			DHD_FWLOG(("CONSOLE: %s\n", line));
		}
	}
break2:

	return BCME_OK;
} /* dhdpcie_bus_readconsole */

void
dhd_bus_dump_console_buffer(dhd_bus_t *bus)
{
	uint32 n, i;
	uint32 addr;
	char *console_buffer = NULL;
	uint32 console_ptr, console_size, console_index;
	uint8 line[CONSOLE_LINE_MAX], ch;
	int rv;

	DHD_ERROR(("%s: Dump Complete Console Buffer\n", __FUNCTION__));

	if (bus->is_linkdown) {
		DHD_ERROR(("%s: Skip dump Console Buffer due to PCIe link down\n", __FUNCTION__));
		return;
	}

	addr =	bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log);
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
		(uint8 *)&console_ptr, sizeof(console_ptr))) < 0) {
		goto exit;
	}

	addr =	bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log.buf_size);
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
		(uint8 *)&console_size, sizeof(console_size))) < 0) {
		goto exit;
	}

	addr =	bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log.idx);
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
		(uint8 *)&console_index, sizeof(console_index))) < 0) {
		goto exit;
	}

	console_ptr = ltoh32(console_ptr);
	console_size = ltoh32(console_size);
	console_index = ltoh32(console_index);

	if (console_size > CONSOLE_BUFFER_MAX ||
		!(console_buffer = MALLOC(bus->dhd->osh, console_size))) {
		goto exit;
	}

	if ((rv = dhdpcie_bus_membytes(bus, FALSE, console_ptr,
		(uint8 *)console_buffer, console_size)) < 0) {
		goto exit;
	}

	for (i = 0, n = 0; i < console_size; i += n + 1) {
		for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
			ch = console_buffer[(console_index + i + n) % console_size];
			if (ch == '\n')
				break;
			line[n] = ch;
		}

		if (n > 0) {
			if (line[n - 1] == '\r')
				n--;
			line[n] = 0;
			/* Don't use DHD_ERROR macro since we print
			 * a lot of information quickly. The macro
			 * will truncate a lot of the printfs
			 */

			DHD_FWLOG(("CONSOLE: %s\n", line));
		}
	}

exit:
	if (console_buffer)
		MFREE(bus->dhd->osh, console_buffer, console_size);
	return;
}

/**
 * Opens the file given by bus->fw_path, reads part of the file into a buffer and closes the file.
 *
 * @return BCME_OK on success
 */
static int
dhdpcie_checkdied(dhd_bus_t *bus, char *data, uint size)
{
	int bcmerror = 0;
	uint msize = 512;
	char *mbuffer = NULL;
	uint maxstrlen = 256;
	char *str = NULL;
	pciedev_shared_t *local_pciedev_shared = bus->pcie_sh;
	struct bcmstrbuf strbuf;
	unsigned long flags;
	bool dongle_trap_occured = FALSE;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (DHD_NOCHECKDIED_ON()) {
		return 0;
	}

	if (data == NULL) {
		/*
		 * Called after a rx ctrl timeout. "data" is NULL.
		 * allocate memory to trace the trap or assert.
		 */
		size = msize;
		mbuffer = data = MALLOC(bus->dhd->osh, msize);

		if (mbuffer == NULL) {
			DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, msize));
			bcmerror = BCME_NOMEM;
			goto done;
		}
	}

	if ((str = MALLOC(bus->dhd->osh, maxstrlen)) == NULL) {
		DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, maxstrlen));
		bcmerror = BCME_NOMEM;
		goto done;
	}
	DHD_GENERAL_LOCK(bus->dhd, flags);
	DHD_BUS_BUSY_SET_IN_CHECKDIED(bus->dhd);
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	if ((bcmerror = dhdpcie_readshared(bus)) < 0) {
		goto done;
	}

	bcm_binit(&strbuf, data, size);

	bcm_bprintf(&strbuf, "msgtrace address : 0x%08X\nconsole address  : 0x%08X\n",
	            local_pciedev_shared->msgtrace_addr, local_pciedev_shared->console_addr);

	if ((local_pciedev_shared->flags & PCIE_SHARED_ASSERT_BUILT) == 0) {
		/* NOTE: Misspelled assert is intentional - DO NOT FIX.
		 * (Avoids conflict with real asserts for programmatic parsing of output.)
		 */
		bcm_bprintf(&strbuf, "Assrt not built in dongle\n");
	}

	if ((bus->pcie_sh->flags & (PCIE_SHARED_ASSERT|PCIE_SHARED_TRAP)) == 0) {
		/* NOTE: Misspelled assert is intentional - DO NOT FIX.
		 * (Avoids conflict with real asserts for programmatic parsing of output.)
		 */
		bcm_bprintf(&strbuf, "No trap%s in dongle",
		          (bus->pcie_sh->flags & PCIE_SHARED_ASSERT_BUILT)
		          ?"/assrt" :"");
	} else {
		if (bus->pcie_sh->flags & PCIE_SHARED_ASSERT) {
			/* Download assert */
			bcm_bprintf(&strbuf, "Dongle assert");
			if (bus->pcie_sh->assert_exp_addr != 0) {
				str[0] = '\0';
				if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
					bus->pcie_sh->assert_exp_addr,
					(uint8 *)str, maxstrlen)) < 0) {
					goto done;
				}

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " expr \"%s\"", str);
			}

			if (bus->pcie_sh->assert_file_addr != 0) {
				str[0] = '\0';
				if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
					bus->pcie_sh->assert_file_addr,
					(uint8 *)str, maxstrlen)) < 0) {
					goto done;
				}

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " file \"%s\"", str);
			}

			bcm_bprintf(&strbuf, " line %d ",  bus->pcie_sh->assert_line);
		}

		if (bus->pcie_sh->flags & PCIE_SHARED_TRAP) {
			trap_t *tr = &bus->dhd->last_trap_info;
			dongle_trap_occured = TRUE;
			if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
				bus->pcie_sh->trap_addr, (uint8*)tr, sizeof(trap_t))) < 0) {
				bus->dhd->dongle_trap_occured = TRUE;
				goto done;
			}
			dhd_bus_dump_trap_info(bus, &strbuf);
		}
	}

	if (bus->pcie_sh->flags & (PCIE_SHARED_ASSERT | PCIE_SHARED_TRAP)) {
		DHD_FWLOG(("%s: %s\n", __FUNCTION__, strbuf.origbuf));

		/* wake up IOCTL wait event */
		dhd_wakeup_ioctl_event(bus->dhd, IOCTL_RETURN_ON_TRAP);

		dhd_bus_dump_console_buffer(bus);
		dhd_prot_debug_info_print(bus->dhd);

#if defined(DHD_FW_COREDUMP)
		/* save core dump or write to a file */
		if (bus->dhd->memdump_enabled) {
#ifdef DHD_SSSR_DUMP
			if (bus->dhd->sssr_inited) {
				dhdpcie_sssr_dump(bus->dhd);
			}
#endif /* DHD_SSSR_DUMP */
			bus->dhd->memdump_type = DUMP_TYPE_DONGLE_TRAP;
			dhdpcie_mem_dump(bus);
		}
#endif /* DHD_FW_COREDUMP */

		/* set the trap occured flag only after all the memdump,
		* logdump and sssr dump collection has been scheduled
		*/
		if (dongle_trap_occured)
			bus->dhd->dongle_trap_occured = TRUE;

#ifdef WL_CFGVENDOR_SEND_HANG_EVENT
		copy_hang_info_trap(bus->dhd);
#endif /* WL_CFGVENDOR_SEND_HANG_EVENT */
		dhd_schedule_reset(bus->dhd);

	}

	DHD_GENERAL_LOCK(bus->dhd, flags);
	DHD_BUS_BUSY_CLEAR_IN_CHECKDIED(bus->dhd);
	dhd_os_busbusy_wake(bus->dhd);
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

done:
	if (mbuffer)
		MFREE(bus->dhd->osh, mbuffer, msize);
	if (str)
		MFREE(bus->dhd->osh, str, maxstrlen);

	return bcmerror;
} /* dhdpcie_checkdied */

/* Custom copy of dhdpcie_mem_dump() that can be called at interrupt level */
void dhdpcie_mem_dump_bugcheck(dhd_bus_t *bus, uint8 *buf)
{
	int ret = 0;
	int size; /* Full mem size */
	int start; /* Start address */
	int read_size = 0; /* Read size of each iteration */
	uint8 *databuf = buf;

	if (bus == NULL) {
		return;
	}

	start = bus->dongle_ram_base;
	read_size = 4;
	/* check for dead bus */
	{
		uint test_word = 0;
		ret = dhdpcie_bus_membytes(bus, FALSE, start, (uint8*)&test_word, read_size);
		/* if read error or bus timeout */
		if (ret || (test_word == 0xFFFFFFFF)) {
			return;
		}
	}

	/* Get full mem size */
	size = bus->ramsize;
	/* Read mem content */
	while (size)
	{
		read_size = MIN(MEMBLOCK, size);
		if ((ret = dhdpcie_bus_membytes(bus, FALSE, start, databuf, read_size))) {
			return;
		}

		/* Decrement size and increment start address */
		size -= read_size;
		start += read_size;
		databuf += read_size;
	}
	bus->dhd->soc_ram = buf;
	bus->dhd->soc_ram_length = bus->ramsize;
	return;
}

#if defined(DHD_FW_COREDUMP)
static int
dhdpcie_mem_dump(dhd_bus_t *bus)
{
	int ret = 0;
	int size; /* Full mem size */
	int start = bus->dongle_ram_base; /* Start address */
	int read_size = 0; /* Read size of each iteration */
	uint8 *buf = NULL, *databuf = NULL;

#ifdef EXYNOS_PCIE_DEBUG
	exynos_pcie_register_dump(1);
#endif /* EXYNOS_PCIE_DEBUG */

#ifdef SUPPORT_LINKDOWN_RECOVERY
	if (bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link is down so skip\n", __FUNCTION__));
		return BCME_ERROR;
	}
#endif /* SUPPORT_LINKDOWN_RECOVERY */

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
	if (pm_runtime_get_sync(dhd_bus_to_dev(bus)) < 0)
		return BCME_ERROR;
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

	/* Get full mem size */
	size = bus->ramsize;
	buf = dhd_get_fwdump_buf(bus->dhd, size);
	if (!buf) {
		DHD_ERROR(("%s: Out of memory (%d bytes)\n", __FUNCTION__, size));
		return BCME_ERROR;
	}

	/* Read mem content */
	DHD_TRACE_HW4(("Dump dongle memory\n"));
	databuf = buf;
	while (size)
	{
		read_size = MIN(MEMBLOCK, size);
		if ((ret = dhdpcie_bus_membytes(bus, FALSE, start, databuf, read_size)))
		{
			DHD_ERROR(("%s: Error membytes %d\n", __FUNCTION__, ret));
#ifdef DHD_DEBUG_UART
			bus->dhd->memdump_success = FALSE;
#endif	/* DHD_DEBUG_UART */
			return BCME_ERROR;
		}
		DHD_TRACE(("."));

		/* Decrement size and increment start address */
		size -= read_size;
		start += read_size;
		databuf += read_size;
	}
#ifdef DHD_DEBUG_UART
	bus->dhd->memdump_success = TRUE;
#endif	/* DHD_DEBUG_UART */

	dhd_schedule_memdump(bus->dhd, buf, bus->ramsize);
	/* buf, actually soc_ram free handled in dhd_{free,clear} */

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
	pm_runtime_mark_last_busy(dhd_bus_to_dev(bus));
	pm_runtime_put_autosuspend(dhd_bus_to_dev(bus));
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

	return ret;
}

int
dhd_bus_mem_dump(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	int ret = BCME_ERROR;

	if (dhdp->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s bus is down\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (DHD_BUS_CHECK_SUSPEND_OR_SUSPEND_IN_PROGRESS(dhdp)) {
		DHD_ERROR(("%s: bus is in suspend(%d) or suspending(0x%x) state, so skip\n",
			__FUNCTION__, dhdp->busstate, dhdp->dhd_bus_busy_state));
		return BCME_ERROR;
	}

	DHD_OS_WAKE_LOCK(dhdp);
	ret = dhdpcie_mem_dump(bus);
	DHD_OS_WAKE_UNLOCK(dhdp);
	return ret;
}

int
dhd_dongle_mem_dump(void)
{
	if (!g_dhd_bus) {
		DHD_ERROR(("%s: Bus is NULL\n", __FUNCTION__));
		return -ENODEV;
	}

	dhd_bus_dump_console_buffer(g_dhd_bus);
	dhd_prot_debug_info_print(g_dhd_bus->dhd);

	g_dhd_bus->dhd->memdump_enabled = DUMP_MEMFILE_BUGON;
	g_dhd_bus->dhd->memdump_type = DUMP_TYPE_AP_ABNORMAL_ACCESS;

#ifdef DHD_PCIE_RUNTIMEPM
	dhdpcie_runtime_bus_wake(g_dhd_bus->dhd, TRUE, __builtin_return_address(0));
#endif /* DHD_PCIE_RUNTIMEPM */

	dhd_bus_mem_dump(g_dhd_bus->dhd);
	return 0;
}
EXPORT_SYMBOL(dhd_dongle_mem_dump);
#endif	/* DHD_FW_COREDUMP */

int
dhd_socram_dump(dhd_bus_t *bus)
{
#if defined(DHD_FW_COREDUMP)
	DHD_OS_WAKE_LOCK(bus->dhd);
	dhd_bus_mem_dump(bus->dhd);
	DHD_OS_WAKE_UNLOCK(bus->dhd);
	return 0;
#else
	return -1;
#endif // endif
}

/**
 * Transfers bytes from host to dongle using pio mode.
 * Parameter 'address' is a backplane address.
 */
static int
dhdpcie_bus_membytes(dhd_bus_t *bus, bool write, ulong address, uint8 *data, uint size)
{
	uint dsize;
	int detect_endian_flag = 0x01;
	bool little_endian;

	if (write && bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return BCME_ERROR;
	}

	/* Detect endianness. */
	little_endian = *(char *)&detect_endian_flag;

	/* In remap mode, adjust address beyond socram and redirect
	 * to devram at SOCDEVRAM_BP_ADDR since remap address > orig_ramsize
	 * is not backplane accessible
	 */

	/* Determine initial transfer parameters */
#ifdef DHD_SUPPORT_64BIT
	dsize = sizeof(uint64);
#else /* !DHD_SUPPORT_64BIT */
	dsize = sizeof(uint32);
#endif /* DHD_SUPPORT_64BIT */

	/* Do the transfer(s) */
	if (write) {
		while (size) {
#ifdef DHD_SUPPORT_64BIT
			if (size >= sizeof(uint64) && little_endian &&	!(address % 8)) {
				dhdpcie_bus_wtcm64(bus, address, *((uint64 *)data));
			}
#else /* !DHD_SUPPORT_64BIT */
			if (size >= sizeof(uint32) && little_endian &&	!(address % 4)) {
				dhdpcie_bus_wtcm32(bus, address, *((uint32*)data));
			}
#endif /* DHD_SUPPORT_64BIT */
			else {
				dsize = sizeof(uint8);
				dhdpcie_bus_wtcm8(bus, address, *data);
			}

			/* Adjust for next transfer (if any) */
			if ((size -= dsize)) {
				data += dsize;
				address += dsize;
			}
		}
	} else {
		while (size) {
#ifdef DHD_SUPPORT_64BIT
			if (size >= sizeof(uint64) && little_endian &&	!(address % 8))
			{
				*(uint64 *)data = dhdpcie_bus_rtcm64(bus, address);
			}
#else /* !DHD_SUPPORT_64BIT */
			if (size >= sizeof(uint32) && little_endian &&	!(address % 4))
			{
				*(uint32 *)data = dhdpcie_bus_rtcm32(bus, address);
			}
#endif /* DHD_SUPPORT_64BIT */
			else {
				dsize = sizeof(uint8);
				*data = dhdpcie_bus_rtcm8(bus, address);
			}

			/* Adjust for next transfer (if any) */
			if ((size -= dsize) > 0) {
				data += dsize;
				address += dsize;
			}
		}
	}
	return BCME_OK;
} /* dhdpcie_bus_membytes */

/**
 * Transfers one transmit (ethernet) packet that was queued in the (flow controlled) flow ring queue
 * to the (non flow controlled) flow ring.
 */
int BCMFASTPATH
dhd_bus_schedule_queue(struct dhd_bus  *bus, uint16 flow_id, bool txs)
{
	flow_ring_node_t *flow_ring_node;
	int ret = BCME_OK;
#ifdef DHD_LOSSLESS_ROAMING
	dhd_pub_t *dhdp = bus->dhd;
#endif // endif
	DHD_INFO(("%s: flow_id is %d\n", __FUNCTION__, flow_id));

	/* ASSERT on flow_id */
	if (flow_id >= bus->max_submission_rings) {
		DHD_ERROR(("%s: flow_id is invalid %d, max %d\n", __FUNCTION__,
			flow_id, bus->max_submission_rings));
		return 0;
	}

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flow_id);

#ifdef DHD_LOSSLESS_ROAMING
	if ((dhdp->dequeue_prec_map & (1 << flow_ring_node->flow_info.tid)) == 0) {
		DHD_INFO(("%s: tid %d is not in precedence map. block scheduling\n",
			__FUNCTION__, flow_ring_node->flow_info.tid));
		return BCME_OK;
	}
#endif /* DHD_LOSSLESS_ROAMING */

	{
		unsigned long flags;
		void *txp = NULL;
		flow_queue_t *queue;
#ifdef DHD_LOSSLESS_ROAMING
		struct ether_header *eh;
		uint8 *pktdata;
#endif /* DHD_LOSSLESS_ROAMING */

		queue = &flow_ring_node->queue; /* queue associated with flow ring */

		DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

		if (flow_ring_node->status != FLOW_RING_STATUS_OPEN) {
			DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
			return BCME_NOTREADY;
		}

		while ((txp = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
			PKTORPHAN(txp);

			/*
			 * Modifying the packet length caused P2P cert failures.
			 * Specifically on test cases where a packet of size 52 bytes
			 * was injected, the sniffer capture showed 62 bytes because of
			 * which the cert tests failed. So making the below change
			 * only Router specific.
			 */

#ifdef DHDTCPACK_SUPPRESS
			if (bus->dhd->tcpack_sup_mode != TCPACK_SUP_HOLD) {
				ret = dhd_tcpack_check_xmit(bus->dhd, txp);
				if (ret != BCME_OK) {
					DHD_ERROR(("%s: dhd_tcpack_check_xmit() error.\n",
						__FUNCTION__));
				}
			}
#endif /* DHDTCPACK_SUPPRESS */
#ifdef DHD_LOSSLESS_ROAMING
			pktdata = (uint8 *)PKTDATA(OSH_NULL, txp);
			eh = (struct ether_header *) pktdata;
			if (eh->ether_type == hton16(ETHER_TYPE_802_1X)) {
				uint8 prio = (uint8)PKTPRIO(txp);
				/* Restore to original priority for 802.1X packet */
				if (prio == PRIO_8021D_NC) {
					PKTSETPRIO(txp, dhdp->prio_8021x);
				}
			}
#endif /* DHD_LOSSLESS_ROAMING */
			/* Attempt to transfer packet over flow ring */
			ret = dhd_prot_txdata(bus->dhd, txp, flow_ring_node->flow_info.ifindex);
			if (ret != BCME_OK) { /* may not have resources in flow ring */
				DHD_INFO(("%s: Reinserrt %d\n", __FUNCTION__, ret));
				dhd_prot_txdata_write_flush(bus->dhd, flow_id);
				/* reinsert at head */
				dhd_flow_queue_reinsert(bus->dhd, queue, txp);
				DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

				/* If we are able to requeue back, return success */
				return BCME_OK;
			}
		}

		dhd_prot_txdata_write_flush(bus->dhd, flow_id);

		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
	}

	return ret;
} /* dhd_bus_schedule_queue */

/** Sends an (ethernet) data frame (in 'txp') to the dongle. Callee disposes of txp. */
int BCMFASTPATH
dhd_bus_txdata(struct dhd_bus *bus, void *txp, uint8 ifidx)
{
	uint16 flowid;
#ifdef IDLE_TX_FLOW_MGMT
	uint8	node_status;
#endif /* IDLE_TX_FLOW_MGMT */
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;
	int ret = BCME_OK;
	void *txp_pend = NULL;

	if (!bus->dhd->flowid_allocator) {
		DHD_ERROR(("%s: Flow ring not intited yet  \n", __FUNCTION__));
		goto toss;
	}

	flowid = DHD_PKT_GET_FLOWID(txp);

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);

	DHD_TRACE(("%s: pkt flowid %d, status %d active %d\n",
		__FUNCTION__, flowid, flow_ring_node->status, flow_ring_node->active));

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	if ((flowid >= bus->dhd->num_flow_rings) ||
#ifdef IDLE_TX_FLOW_MGMT
		(!flow_ring_node->active))
#else
		(!flow_ring_node->active) ||
		(flow_ring_node->status == FLOW_RING_STATUS_DELETE_PENDING) ||
		(flow_ring_node->status == FLOW_RING_STATUS_STA_FREEING))
#endif /* IDLE_TX_FLOW_MGMT */
	{
		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
		DHD_INFO(("%s: Dropping pkt flowid %d, status %d active %d\n",
			__FUNCTION__, flowid, flow_ring_node->status,
			flow_ring_node->active));
		ret = BCME_ERROR;
			goto toss;
	}

#ifdef IDLE_TX_FLOW_MGMT
	node_status = flow_ring_node->status;

	/* handle diffrent status states here!! */
	switch (node_status)
	{
		case FLOW_RING_STATUS_OPEN:

			if (bus->enable_idle_flowring_mgmt) {
				/* Move the node to the head of active list */
				dhd_flow_ring_move_to_active_list_head(bus, flow_ring_node);
			}
			break;

		case FLOW_RING_STATUS_SUSPENDED:
			DHD_INFO(("Need to Initiate TX Flow resume\n"));
			/* Issue resume_ring request */
			dhd_bus_flow_ring_resume_request(bus,
					flow_ring_node);
			break;

		case FLOW_RING_STATUS_CREATE_PENDING:
		case FLOW_RING_STATUS_RESUME_PENDING:
			/* Dont do anything here!! */
			DHD_INFO(("Waiting for Flow create/resume! status is %u\n",
				node_status));
			break;

		case FLOW_RING_STATUS_DELETE_PENDING:
		default:
			DHD_ERROR(("Dropping packet!! flowid %u status is %u\n",
				flowid, node_status));
			/* error here!! */
			ret = BCME_ERROR;
			DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
			goto toss;
	}
	/* Now queue the packet */
#endif /* IDLE_TX_FLOW_MGMT */

	queue = &flow_ring_node->queue; /* queue associated with flow ring */

	if ((ret = dhd_flow_queue_enqueue(bus->dhd, queue, txp)) != BCME_OK)
		txp_pend = txp;

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	if (flow_ring_node->status) {
		DHD_INFO(("%s: Enq pkt flowid %d, status %d active %d\n",
		    __FUNCTION__, flowid, flow_ring_node->status,
		    flow_ring_node->active));
		if (txp_pend) {
			txp = txp_pend;
			goto toss;
		}
		return BCME_OK;
	}
	ret = dhd_bus_schedule_queue(bus, flowid, FALSE); /* from queue to flowring */

	/* If we have anything pending, try to push into q */
	if (txp_pend) {
		DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

		if ((ret = dhd_flow_queue_enqueue(bus->dhd, queue, txp_pend)) != BCME_OK) {
			DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
			txp = txp_pend;
			goto toss;
		}

		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
	}

	return ret;

toss:
	DHD_INFO(("%s: Toss %d\n", __FUNCTION__, ret));
	PKTCFREE(bus->dhd->osh, txp, TRUE);
	return ret;
} /* dhd_bus_txdata */

void
dhd_bus_stop_queue(struct dhd_bus *bus)
{
	dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, ON);
}

void
dhd_bus_start_queue(struct dhd_bus *bus)
{
	dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, OFF);
}

/* Device console input function */
int dhd_bus_console_in(dhd_pub_t *dhd, uchar *msg, uint msglen)
{
	dhd_bus_t *bus = dhd->bus;
	uint32 addr, val;
	int rv;
	/* Address could be zero if CONSOLE := 0 in dongle Makefile */
	if (bus->console_addr == 0)
		return BCME_UNSUPPORTED;

	/* Don't allow input if dongle is in reset */
	if (bus->dhd->dongle_reset) {
		return BCME_NOTREADY;
	}

	/* Zero cbuf_index */
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, cbuf_idx);
	val = htol32(0);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	/* Write message into cbuf */
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, cbuf);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)msg, msglen)) < 0)
		goto done;

	/* Write length into vcons_in */
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, vcons_in);
	val = htol32(msglen);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	/* generate an interrupt to dongle to indicate that it needs to process cons command */
	dhdpcie_send_mb_data(bus, H2D_HOST_CONS_INT);
done:
	return rv;
} /* dhd_bus_console_in */

/**
 * Called on frame reception, the frame was received from the dongle on interface 'ifidx' and is
 * contained in 'pkt'. Processes rx frame, forwards up the layer to netif.
 */
void BCMFASTPATH
dhd_bus_rx_frame(struct dhd_bus *bus, void* pkt, int ifidx, uint pkt_count)
{
	dhd_rx_frame(bus->dhd, ifidx, pkt, pkt_count, 0);
}

/** 'offset' is a backplane address */
void
dhdpcie_bus_wtcm8(dhd_bus_t *bus, ulong offset, uint8 data)
{
	W_REG(bus->dhd->osh, (volatile uint8 *)(bus->tcm + offset), data);
}

uint8
dhdpcie_bus_rtcm8(dhd_bus_t *bus, ulong offset)
{
	volatile uint8 data;
	data = R_REG(bus->dhd->osh, (volatile uint8 *)(bus->tcm + offset));
	return data;
}

void
dhdpcie_bus_wtcm32(dhd_bus_t *bus, ulong offset, uint32 data)
{
	W_REG(bus->dhd->osh, (volatile uint32 *)(bus->tcm + offset), data);
}
void
dhdpcie_bus_wtcm16(dhd_bus_t *bus, ulong offset, uint16 data)
{
	W_REG(bus->dhd->osh, (volatile uint16 *)(bus->tcm + offset), data);
}
#ifdef DHD_SUPPORT_64BIT
void
dhdpcie_bus_wtcm64(dhd_bus_t *bus, ulong offset, uint64 data)
{
	W_REG(bus->dhd->osh, (volatile uint64 *)(bus->tcm + offset), data);
}
#endif /* DHD_SUPPORT_64BIT */

uint16
dhdpcie_bus_rtcm16(dhd_bus_t *bus, ulong offset)
{
	volatile uint16 data;
	data = R_REG(bus->dhd->osh, (volatile uint16 *)(bus->tcm + offset));
	return data;
}

uint32
dhdpcie_bus_rtcm32(dhd_bus_t *bus, ulong offset)
{
	volatile uint32 data;
	data = R_REG(bus->dhd->osh, (volatile uint32 *)(bus->tcm + offset));
	return data;
}

#ifdef DHD_SUPPORT_64BIT
uint64
dhdpcie_bus_rtcm64(dhd_bus_t *bus, ulong offset)
{
	volatile uint64 data;
	data = R_REG(bus->dhd->osh, (volatile uint64 *)(bus->tcm + offset));
	return data;
}
#endif /* DHD_SUPPORT_64BIT */

/** A snippet of dongle memory is shared between host and dongle */
void
dhd_bus_cmn_writeshared(dhd_bus_t *bus, void *data, uint32 len, uint8 type, uint16 ringid)
{
	uint64 long_data;
	ulong addr; /* dongle address */

	DHD_INFO(("%s: writing to dongle type %d len %d\n", __FUNCTION__, type, len));

	if (bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return;
	}

	switch (type) {
		case D2H_DMA_SCRATCH_BUF:
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, host_dma_scratch_buffer);
			long_data = HTOL64(*(uint64 *)data);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case D2H_DMA_SCRATCH_BUF_LEN :
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, host_dma_scratch_buffer_len);
			dhdpcie_bus_wtcm32(bus, addr, (uint32) HTOL32(*(uint32 *)data));
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case H2D_DMA_INDX_WR_BUF:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_INFO_MEMBER_ADDR(bus, h2d_w_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case H2D_DMA_INDX_RD_BUF:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_INFO_MEMBER_ADDR(bus, h2d_r_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case D2H_DMA_INDX_WR_BUF:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_INFO_MEMBER_ADDR(bus, d2h_w_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case D2H_DMA_INDX_RD_BUF:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_INFO_MEMBER_ADDR(bus, d2h_r_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case H2D_IFRM_INDX_WR_BUF:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_INFO_MEMBER_ADDR(bus, ifrm_w_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8*) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case RING_ITEM_LEN :
			addr = DHD_RING_MEM_MEMBER_ADDR(bus, ringid, len_items);
			dhdpcie_bus_wtcm16(bus, addr, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_MAX_ITEMS :
			addr = DHD_RING_MEM_MEMBER_ADDR(bus, ringid, max_item);
			dhdpcie_bus_wtcm16(bus, addr, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_BUF_ADDR :
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_RING_MEM_MEMBER_ADDR(bus, ringid, base_addr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *) &long_data, len);
			if (dhd_msg_level & DHD_INFO_VAL) {
				prhex(__FUNCTION__, data, len);
			}
			break;

		case RING_WR_UPD :
			addr = bus->ring_sh[ringid].ring_state_w;
			dhdpcie_bus_wtcm16(bus, addr, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_RD_UPD :
			addr = bus->ring_sh[ringid].ring_state_r;
			dhdpcie_bus_wtcm16(bus, addr, (uint16) HTOL16(*(uint16 *)data));
			break;

		case D2H_MB_DATA:
			addr = bus->d2h_mb_data_ptr_addr;
			dhdpcie_bus_wtcm32(bus, addr, (uint32) HTOL32(*(uint32 *)data));
			break;

		case H2D_MB_DATA:
			addr = bus->h2d_mb_data_ptr_addr;
			dhdpcie_bus_wtcm32(bus, addr, (uint32) HTOL32(*(uint32 *)data));
			break;

		case HOST_API_VERSION:
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, host_cap);
			dhdpcie_bus_wtcm32(bus, addr, (uint32) HTOL32(*(uint32 *)data));
			break;

		case DNGL_TO_HOST_TRAP_ADDR:
			long_data = HTOL64(*(uint64 *)data);
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, host_trap_addr);
			dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *) &long_data, len);
			DHD_INFO(("Wrote trap addr:0x%x\n", (uint32) HTOL32(*(uint32 *)data)));
			break;

		default:
			break;
	}
} /* dhd_bus_cmn_writeshared */

/** A snippet of dongle memory is shared between host and dongle */
void
dhd_bus_cmn_readshared(dhd_bus_t *bus, void* data, uint8 type, uint16 ringid)
{
	ulong addr; /* dongle address */

	switch (type) {
		case RING_WR_UPD :
			addr = bus->ring_sh[ringid].ring_state_w;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, addr));
			break;

		case RING_RD_UPD :
			addr = bus->ring_sh[ringid].ring_state_r;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, addr));
			break;

		case TOTAL_LFRAG_PACKET_CNT :
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, total_lfrag_pkt_cnt);
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, addr));
			break;

		case H2D_MB_DATA:
			addr = bus->h2d_mb_data_ptr_addr;
			*(uint32*)data = LTOH32(dhdpcie_bus_rtcm32(bus, addr));
			break;

		case D2H_MB_DATA:
			addr = bus->d2h_mb_data_ptr_addr;
			*(uint32*)data = LTOH32(dhdpcie_bus_rtcm32(bus, addr));
			break;

		case MAX_HOST_RXBUFS :
			addr = DHD_PCIE_SHARED_MEMBER_ADDR(bus, max_host_rxbufs);
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, addr));
			break;

		default :
			break;
	}
}

uint32 dhd_bus_get_sharedflags(dhd_bus_t *bus)
{
	return ((pciedev_shared_t*)bus->pcie_sh)->flags;
}

void
dhd_bus_clearcounts(dhd_pub_t *dhdp)
{
}

/**
 * @param params    input buffer, NULL for 'set' operation.
 * @param plen      length of 'params' buffer, 0 for 'set' operation.
 * @param arg       output buffer
 */
int
dhd_bus_iovar_op(dhd_pub_t *dhdp, const char *name,
                 void *params, int plen, void *arg, int len, bool set)
{
	dhd_bus_t *bus = dhdp->bus;
	const bcm_iovar_t *vi = NULL;
	int bcmerror = BCME_UNSUPPORTED;
	int val_size;
	uint32 actionid;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(name);
	ASSERT(len >= 0);
	if (!name || len < 0)
		return BCME_BADARG;

	/* Get MUST have return space */
	ASSERT(set || (arg && len));
	if (!(set || (arg && len)))
		return BCME_BADARG;

	/* Set does NOT take qualifiers */
	ASSERT(!set || (!params && !plen));
	if (!(!set || (!params && !plen)))
		return BCME_BADARG;

	DHD_INFO(("%s: %s %s, len %d plen %d\n", __FUNCTION__,
	         name, (set ? "set" : "get"), len, plen));

	/* Look up var locally; if not found pass to host driver */
	if ((vi = bcm_iovar_lookup(dhdpcie_iovars, name)) == NULL) {
		goto exit;
	}

	/* set up 'params' pointer in case this is a set command so that
	 * the convenience int and bool code can be common to set and get
	 */
	if (params == NULL) {
		params = arg;
		plen = len;
	}

	if (vi->type == IOVT_VOID)
		val_size = 0;
	else if (vi->type == IOVT_BUFFER)
		val_size = len;
	else
		/* all other types are integer sized */
		val_size = sizeof(int);

	actionid = set ? IOV_SVAL(vi->varid) : IOV_GVAL(vi->varid);
	bcmerror = dhdpcie_bus_doiovar(bus, vi, actionid, name, params, plen, arg, len, val_size);

exit:
	return bcmerror;
} /* dhd_bus_iovar_op */

#ifdef BCM_BUZZZ
#include <bcm_buzzz.h>

int
dhd_buzzz_dump_cntrs(char *p, uint32 *core, uint32 *log,
	const int num_counters)
{
	int bytes = 0;
	uint32 ctr;
	uint32 curr[BCM_BUZZZ_COUNTERS_MAX], prev[BCM_BUZZZ_COUNTERS_MAX];
	uint32 delta[BCM_BUZZZ_COUNTERS_MAX];

	/* Compute elapsed counter values per counter event type */
	for (ctr = 0U; ctr < num_counters; ctr++) {
		prev[ctr] = core[ctr];
		curr[ctr] = *log++;
		core[ctr] = curr[ctr];  /* saved for next log */

		if (curr[ctr] < prev[ctr])
			delta[ctr] = curr[ctr] + (~0U - prev[ctr]);
		else
			delta[ctr] = (curr[ctr] - prev[ctr]);

		bytes += sprintf(p + bytes, "%12u ", delta[ctr]);
	}

	return bytes;
}

typedef union cm3_cnts { /* export this in bcm_buzzz.h */
	uint32 u32;
	uint8  u8[4];
	struct {
		uint8 cpicnt;
		uint8 exccnt;
		uint8 sleepcnt;
		uint8 lsucnt;
	};
} cm3_cnts_t;

int
dhd_bcm_buzzz_dump_cntrs6(char *p, uint32 *core, uint32 *log)
{
	int bytes = 0;

	uint32 cyccnt, instrcnt;
	cm3_cnts_t cm3_cnts;
	uint8 foldcnt;

	{   /* 32bit cyccnt */
		uint32 curr, prev, delta;
		prev = core[0]; curr = *log++; core[0] = curr;
		if (curr < prev)
			delta = curr + (~0U - prev);
		else
			delta = (curr - prev);

		bytes += sprintf(p + bytes, "%12u ", delta);
		cyccnt = delta;
	}

	{	/* Extract the 4 cnts: cpi, exc, sleep and lsu */
		int i;
		uint8 max8 = ~0;
		cm3_cnts_t curr, prev, delta;
		prev.u32 = core[1]; curr.u32 = * log++; core[1] = curr.u32;
		for (i = 0; i < 4; i++) {
			if (curr.u8[i] < prev.u8[i])
				delta.u8[i] = curr.u8[i] + (max8 - prev.u8[i]);
			else
				delta.u8[i] = (curr.u8[i] - prev.u8[i]);
			bytes += sprintf(p + bytes, "%4u ", delta.u8[i]);
		}
		cm3_cnts.u32 = delta.u32;
	}

	{   /* Extract the foldcnt from arg0 */
		uint8 curr, prev, delta, max8 = ~0;
		bcm_buzzz_arg0_t arg0; arg0.u32 = *log;
		prev = core[2]; curr = arg0.klog.cnt; core[2] = curr;
		if (curr < prev)
			delta = curr + (max8 - prev);
		else
			delta = (curr - prev);
		bytes += sprintf(p + bytes, "%4u ", delta);
		foldcnt = delta;
	}

	instrcnt = cyccnt - (cm3_cnts.u8[0] + cm3_cnts.u8[1] + cm3_cnts.u8[2]
		                 + cm3_cnts.u8[3]) + foldcnt;
	if (instrcnt > 0xFFFFFF00)
		bytes += sprintf(p + bytes, "[%10s] ", "~");
	else
		bytes += sprintf(p + bytes, "[%10u] ", instrcnt);
	return bytes;
}

int
dhd_buzzz_dump_log(char *p, uint32 *core, uint32 *log, bcm_buzzz_t *buzzz)
{
	int bytes = 0;
	bcm_buzzz_arg0_t arg0;
	static uint8 * fmt[] = BCM_BUZZZ_FMT_STRINGS;

	if (buzzz->counters == 6) {
		bytes += dhd_bcm_buzzz_dump_cntrs6(p, core, log);
		log += 2; /* 32bit cyccnt + (4 x 8bit) CM3 */
	} else {
		bytes += dhd_buzzz_dump_cntrs(p, core, log, buzzz->counters);
		log += buzzz->counters; /* (N x 32bit) CR4=3, CA7=4 */
	}

	/* Dump the logged arguments using the registered formats */
	arg0.u32 = *log++;

	switch (arg0.klog.args) {
		case 0:
			bytes += sprintf(p + bytes, fmt[arg0.klog.id]);
			break;
		case 1:
		{
			uint32 arg1 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1);
			break;
		}
		case 2:
		{
			uint32 arg1, arg2;
			arg1 = *log++; arg2 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2);
			break;
		}
		case 3:
		{
			uint32 arg1, arg2, arg3;
			arg1 = *log++; arg2 = *log++; arg3 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2, arg3);
			break;
		}
		case 4:
		{
			uint32 arg1, arg2, arg3, arg4;
			arg1 = *log++; arg2 = *log++;
			arg3 = *log++; arg4 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2, arg3, arg4);
			break;
		}
		default:
			printf("Maximum one argument supported\n");
			break;
	}

	bytes += sprintf(p + bytes, "\n");

	return bytes;
}

void dhd_buzzz_dump(bcm_buzzz_t *buzzz_p, void *buffer_p, char *p)
{
	int i;
	uint32 total, part1, part2, log_sz, core[BCM_BUZZZ_COUNTERS_MAX];
	void * log;

	for (i = 0; i < BCM_BUZZZ_COUNTERS_MAX; i++) {
		core[i] = 0;
	}

	log_sz = buzzz_p->log_sz;

	part1 = ((uint32)buzzz_p->cur - (uint32)buzzz_p->log) / log_sz;

	if (buzzz_p->wrap == TRUE) {
		part2 = ((uint32)buzzz_p->end - (uint32)buzzz_p->cur) / log_sz;
		total = (buzzz_p->buffer_sz - BCM_BUZZZ_LOGENTRY_MAXSZ) / log_sz;
	} else {
		part2 = 0U;
		total = buzzz_p->count;
	}

	if (total == 0U) {
		printf("bcm_buzzz_dump total<%u> done\n", total);
		return;
	} else {
		printf("bcm_buzzz_dump total<%u> : part2<%u> + part1<%u>\n",
		       total, part2, part1);
	}

	if (part2) {   /* with wrap */
		log = (void*)((size_t)buffer_p + (buzzz_p->cur - buzzz_p->log));
		while (part2--) {   /* from cur to end : part2 */
			p[0] = '\0';
			dhd_buzzz_dump_log(p, core, (uint32 *)log, buzzz_p);
			printf("%s", p);
			log = (void*)((size_t)log + buzzz_p->log_sz);
		}
	}

	log = (void*)buffer_p;
	while (part1--) {
		p[0] = '\0';
		dhd_buzzz_dump_log(p, core, (uint32 *)log, buzzz_p);
		printf("%s", p);
		log = (void*)((size_t)log + buzzz_p->log_sz);
	}

	printf("bcm_buzzz_dump done.\n");
}

int dhd_buzzz_dump_dngl(dhd_bus_t *bus)
{
	bcm_buzzz_t * buzzz_p = NULL;
	void * buffer_p = NULL;
	char * page_p = NULL;
	pciedev_shared_t *sh;
	int ret = 0;

	if (bus->dhd->busstate != DHD_BUS_DATA) {
		return BCME_UNSUPPORTED;
	}
	if ((page_p = (char *)MALLOC(bus->dhd->osh, 4096)) == NULL) {
		printf("Page memory allocation failure\n");
		goto done;
	}
	if ((buzzz_p = MALLOC(bus->dhd->osh, sizeof(bcm_buzzz_t))) == NULL) {
		printf("BCM BUZZZ memory allocation failure\n");
		goto done;
	}

	ret = dhdpcie_readshared(bus);
	if (ret < 0) {
		DHD_ERROR(("%s :Shared area read failed \n", __FUNCTION__));
		goto done;
	}

	sh = bus->pcie_sh;

	DHD_INFO(("%s buzzz:%08x\n", __FUNCTION__, sh->buzz_dbg_ptr));

	if (sh->buzz_dbg_ptr != 0U) {	/* Fetch and display dongle BUZZZ Trace */

		dhdpcie_bus_membytes(bus, FALSE, (ulong)sh->buzz_dbg_ptr,
		                     (uint8 *)buzzz_p, sizeof(bcm_buzzz_t));

		printf("BUZZZ[0x%08x]: log<0x%08x> cur<0x%08x> end<0x%08x> "
			"count<%u> status<%u> wrap<%u>\n"
			"cpu<0x%02X> counters<%u> group<%u> buffer_sz<%u> log_sz<%u>\n",
			(int)sh->buzz_dbg_ptr,
			(int)buzzz_p->log, (int)buzzz_p->cur, (int)buzzz_p->end,
			buzzz_p->count, buzzz_p->status, buzzz_p->wrap,
			buzzz_p->cpu_idcode, buzzz_p->counters, buzzz_p->group,
			buzzz_p->buffer_sz, buzzz_p->log_sz);

		if (buzzz_p->count == 0) {
			printf("Empty dongle BUZZZ trace\n\n");
			goto done;
		}

		/* Allocate memory for trace buffer and format strings */
		buffer_p = MALLOC(bus->dhd->osh, buzzz_p->buffer_sz);
		if (buffer_p == NULL) {
			printf("Buffer memory allocation failure\n");
			goto done;
		}

		/* Fetch the trace. format strings are exported via bcm_buzzz.h */
		dhdpcie_bus_membytes(bus, FALSE, (uint32)buzzz_p->log,   /* Trace */
		                     (uint8 *)buffer_p, buzzz_p->buffer_sz);

		/* Process and display the trace using formatted output */

		{
			int ctr;
			for (ctr = 0; ctr < buzzz_p->counters; ctr++) {
				printf("<Evt[%02X]> ", buzzz_p->eventid[ctr]);
			}
			printf("<code execution point>\n");
		}

		dhd_buzzz_dump(buzzz_p, buffer_p, page_p);

		printf("----- End of dongle BCM BUZZZ Trace -----\n\n");

		MFREE(bus->dhd->osh, buffer_p, buzzz_p->buffer_sz); buffer_p = NULL;
	}

done:

	if (page_p)   MFREE(bus->dhd->osh, page_p, 4096);
	if (buzzz_p)  MFREE(bus->dhd->osh, buzzz_p, sizeof(bcm_buzzz_t));
	if (buffer_p) MFREE(bus->dhd->osh, buffer_p, buzzz_p->buffer_sz);

	return BCME_OK;
}
#endif /* BCM_BUZZZ */

#define PCIE_GEN2(sih) ((BUSTYPE((sih)->bustype) == PCI_BUS) &&	\
	((sih)->buscoretype == PCIE2_CORE_ID))

#define PCIE_FLR_CAPAB_BIT		28
#define PCIE_FUNCTION_LEVEL_RESET_BIT	15
#define PCIE_SSRESET_STATUS_BIT		13
#define PCIE_SSRESET_DISABLE_BIT	14

/* Change delays for only QT HW, FPGA and silicon uses same delay */
#ifdef BCMQT_HW
#define DHD_FUNCTION_LEVEL_RESET_DELAY		300000u
#define DHD_SSRESET_STATUS_RETRY_DELAY	10000u
#else
#define DHD_FUNCTION_LEVEL_RESET_DELAY	55u	/* 55 msec delay */
#define DHD_SSRESET_STATUS_RETRY_DELAY	40u
#endif // endif
#define DHD_SSRESET_STATUS_RETRIES	50u

int
dhd_bus_perform_flr(dhd_bus_t *bus, bool force_fail)
{
	bool flr_capab;
	uint val;
	int retry = 0;

	DHD_ERROR(("******** Perform FLR ********\n"));

	/* Read PCIE_CFG_DEVICE_CAPABILITY bit 28 to check FLR capability */
	val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_DEVICE_CAPABILITY, sizeof(val));
	flr_capab =  val & (1 << PCIE_FLR_CAPAB_BIT);
	DHD_ERROR(("Read Device Capability: reg=0x%x read val=0x%x flr_capab=0x%x\n",
		PCIE_CFG_DEVICE_CAPABILITY, val, flr_capab));
	if (!flr_capab) {
	       DHD_ERROR(("Chip does not support FLR\n"));
	       return BCME_UNSUPPORTED;
	}

	/* Save pcie config space */
	DHD_ERROR(("Save Pcie Config Space\n"));
	DHD_PCIE_CONFIG_SAVE(bus);

	/* Set bit 15 of PCIE_CFG_DEVICE_CONTROL */
	DHD_ERROR(("Set PCIE_FUNCTION_LEVEL_RESET_BIT(%d) of PCIE_CFG_DEVICE_CONTROL(0x%x)\n",
		PCIE_FUNCTION_LEVEL_RESET_BIT, PCIE_CFG_DEVICE_CONTROL));
	val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_DEVICE_CONTROL, sizeof(val));
	DHD_ERROR(("read_config: reg=0x%x read val=0x%x\n", PCIE_CFG_DEVICE_CONTROL, val));
	val = val | (1 << PCIE_FUNCTION_LEVEL_RESET_BIT);
	DHD_ERROR(("write_config: reg=0x%x write val=0x%x\n", PCIE_CFG_DEVICE_CONTROL, val));
	OSL_PCI_WRITE_CONFIG(bus->osh, PCIE_CFG_DEVICE_CONTROL, sizeof(val), val);

	/* wait for DHD_FUNCTION_LEVEL_RESET_DELAY msec */
	DHD_ERROR(("Delay of %d msec\n", DHD_FUNCTION_LEVEL_RESET_DELAY));
	OSL_DELAY(DHD_FUNCTION_LEVEL_RESET_DELAY * 1000u);

	if (force_fail) {
		DHD_ERROR(("Set PCIE_SSRESET_DISABLE_BIT(%d) of PCIE_CFG_SUBSYSTEM_CONTROL(0x%x)\n",
			PCIE_SSRESET_DISABLE_BIT, PCIE_CFG_SUBSYSTEM_CONTROL));
		val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_SUBSYSTEM_CONTROL, sizeof(val));
		DHD_ERROR(("read_config: reg=0x%x read val=0x%x\n", PCIE_CFG_SUBSYSTEM_CONTROL,
			val));
		val = val | (1 << PCIE_SSRESET_DISABLE_BIT);
		DHD_ERROR(("write_config: reg=0x%x write val=0x%x\n", PCIE_CFG_SUBSYSTEM_CONTROL,
			val));
		OSL_PCI_WRITE_CONFIG(bus->osh, PCIE_CFG_SUBSYSTEM_CONTROL, sizeof(val), val);

		val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_SUBSYSTEM_CONTROL, sizeof(val));
		DHD_ERROR(("read_config: reg=0x%x read val=0x%x\n", PCIE_CFG_SUBSYSTEM_CONTROL,
			val));
	}

	/* Clear bit 15 of PCIE_CFG_DEVICE_CONTROL */
	DHD_ERROR(("Clear PCIE_FUNCTION_LEVEL_RESET_BIT(%d) of PCIE_CFG_DEVICE_CONTROL(0x%x)\n",
		PCIE_FUNCTION_LEVEL_RESET_BIT, PCIE_CFG_DEVICE_CONTROL));
	val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_DEVICE_CONTROL, sizeof(val));
	DHD_ERROR(("read_config: reg=0x%x read val=0x%x\n", PCIE_CFG_DEVICE_CONTROL, val));
	val = val & ~(1 << PCIE_FUNCTION_LEVEL_RESET_BIT);
	DHD_ERROR(("write_config: reg=0x%x write val=0x%x\n", PCIE_CFG_DEVICE_CONTROL, val));
	OSL_PCI_WRITE_CONFIG(bus->osh, PCIE_CFG_DEVICE_CONTROL, sizeof(val), val);

	/* Wait till bit 13 of PCIE_CFG_SUBSYSTEM_CONTROL is cleared */
	DHD_ERROR(("Wait till PCIE_SSRESET_STATUS_BIT(%d) of PCIE_CFG_SUBSYSTEM_CONTROL(0x%x)"
		"is cleared\n",	PCIE_SSRESET_STATUS_BIT, PCIE_CFG_SUBSYSTEM_CONTROL));
	do {
		val = OSL_PCI_READ_CONFIG(bus->osh, PCIE_CFG_SUBSYSTEM_CONTROL, sizeof(val));
		DHD_ERROR(("read_config: reg=0x%x read val=0x%x\n",
			PCIE_CFG_SUBSYSTEM_CONTROL, val));
		val = val & (1 << PCIE_SSRESET_STATUS_BIT);
		OSL_DELAY(DHD_SSRESET_STATUS_RETRY_DELAY);
	} while (val && (retry++ < DHD_SSRESET_STATUS_RETRIES));

	if (val) {
		DHD_ERROR(("ERROR: reg=0x%x bit %d is not cleared\n",
			PCIE_CFG_SUBSYSTEM_CONTROL, PCIE_SSRESET_STATUS_BIT));
		return BCME_ERROR;
	}

	/* Restore pcie config space */
	DHD_ERROR(("Restore Pcie Config Space\n"));
	DHD_PCIE_CONFIG_RESTORE(bus);

	DHD_ERROR(("******** FLR Succedeed ********\n"));

	return BCME_OK;
}

int
dhd_bus_devreset(dhd_pub_t *dhdp, uint8 flag)
{
	dhd_bus_t *bus = dhdp->bus;
	int bcmerror = 0;
	unsigned long flags;
	unsigned long flags_bus;
#ifdef CONFIG_ARCH_MSM
	int retry = POWERUP_MAX_RETRY;
#endif /* CONFIG_ARCH_MSM */

	if (flag == TRUE) { /* Turn off WLAN */
		/* Removing Power */
		DHD_ERROR(("%s: == Power OFF ==\n", __FUNCTION__));

		bus->dhd->up = FALSE;

		/* wait for other contexts to finish -- if required a call
		* to OSL_DELAY for 1s can be added to give other contexts
		* a chance to finish
		*/
		dhdpcie_advertise_bus_cleanup(bus->dhd);

		if (bus->dhd->busstate != DHD_BUS_DOWN) {
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
			atomic_set(&bus->dhd->block_bus, TRUE);
			dhd_flush_rx_tx_wq(bus->dhd);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

#ifdef BCMPCIE_OOB_HOST_WAKE
			/* Clean up any pending host wake IRQ */
			dhd_bus_oob_intr_set(bus->dhd, FALSE);
			dhd_bus_oob_intr_unregister(bus->dhd);
#endif /* BCMPCIE_OOB_HOST_WAKE */
			dhd_os_wd_timer(dhdp, 0);
			dhd_bus_stop(bus, TRUE);
			if (bus->intr) {
				DHD_BUS_LOCK(bus->bus_lock, flags_bus);
				dhdpcie_bus_intr_disable(bus);
				DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);
				dhdpcie_free_irq(bus);
			}
			dhd_deinit_bus_lock(bus);
			dhd_bus_release_dongle(bus);
			dhdpcie_bus_free_resource(bus);
			bcmerror = dhdpcie_bus_disable_device(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: dhdpcie_bus_disable_device: %d\n",
					__FUNCTION__, bcmerror));
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
				atomic_set(&bus->dhd->block_bus, FALSE);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */
			}
			/* Clean up protocol data after Bus Master Enable bit clear
			 * so that host can safely unmap DMA and remove the allocated buffers
			 * from the PKTID MAP. Some Applicantion Processors supported
			 * System MMU triggers Kernel panic when they detect to attempt to
			 * DMA-unmapped memory access from the devices which use the
			 * System MMU. Therefore, Kernel panic can be happened since it is
			 * possible that dongle can access to DMA-unmapped memory after
			 * calling the dhd_prot_reset().
			 * For this reason, the dhd_prot_reset() and dhd_clear() functions
			 * should be located after the dhdpcie_bus_disable_device().
			 */
			dhd_prot_reset(dhdp);
			dhd_clear(dhdp);
#ifdef CONFIG_ARCH_MSM
			bcmerror = dhdpcie_bus_clock_stop(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: host clock stop failed: %d\n",
					__FUNCTION__, bcmerror));
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
				atomic_set(&bus->dhd->block_bus, FALSE);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */
				goto done;
			}
#endif /* CONFIG_ARCH_MSM */
			DHD_GENERAL_LOCK(bus->dhd, flags);
			bus->dhd->busstate = DHD_BUS_DOWN;
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
			atomic_set(&bus->dhd->block_bus, FALSE);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */
		} else {
			if (bus->intr) {
				dhdpcie_free_irq(bus);
			}
#ifdef BCMPCIE_OOB_HOST_WAKE
			/* Clean up any pending host wake IRQ */
			dhd_bus_oob_intr_set(bus->dhd, FALSE);
			dhd_bus_oob_intr_unregister(bus->dhd);
#endif /* BCMPCIE_OOB_HOST_WAKE */
			dhd_dpc_kill(bus->dhd);
			if (!bus->no_bus_init) {
				dhd_bus_release_dongle(bus);
				dhdpcie_bus_free_resource(bus);
				bcmerror = dhdpcie_bus_disable_device(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_disable_device: %d\n",
						__FUNCTION__, bcmerror));
				}

				/* Clean up protocol data after Bus Master Enable bit clear
				 * so that host can safely unmap DMA and remove the allocated
				 * buffers from the PKTID MAP. Some Applicantion Processors
				 * supported System MMU triggers Kernel panic when they detect
				 * to attempt to DMA-unmapped memory access from the devices
				 * which use the System MMU.
				 * Therefore, Kernel panic can be happened since it is possible
				 * that dongle can access to DMA-unmapped memory after calling
				 * the dhd_prot_reset().
				 * For this reason, the dhd_prot_reset() and dhd_clear() functions
				 * should be located after the dhdpcie_bus_disable_device().
				 */
				dhd_prot_reset(dhdp);
				dhd_clear(dhdp);
			} else {
				bus->no_bus_init = FALSE;
			}
#ifdef CONFIG_ARCH_MSM
			bcmerror = dhdpcie_bus_clock_stop(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: host clock stop failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}
#endif  /* CONFIG_ARCH_MSM */
		}

		bus->dhd->dongle_reset = TRUE;
		DHD_ERROR(("%s:  WLAN OFF Done\n", __FUNCTION__));

	} else { /* Turn on WLAN */
		if (bus->dhd->busstate == DHD_BUS_DOWN) {
			/* Powering On */
			DHD_ERROR(("%s: == Power ON ==\n", __FUNCTION__));
#ifdef CONFIG_ARCH_MSM
			while (--retry) {
				bcmerror = dhdpcie_bus_clock_start(bus);
				if (!bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_clock_start OK\n",
						__FUNCTION__));
					break;
				} else {
					OSL_SLEEP(10);
				}
			}

			if (bcmerror && !retry) {
				DHD_ERROR(("%s: host pcie clock enable failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}
#endif /* CONFIG_ARCH_MSM */
			bus->is_linkdown = 0;
#ifdef SUPPORT_LINKDOWN_RECOVERY
			bus->read_shm_fail = FALSE;
#endif /* SUPPORT_LINKDOWN_RECOVERY */
			bcmerror = dhdpcie_bus_enable_device(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: host configuration restore failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}

			bcmerror = dhdpcie_bus_alloc_resource(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: dhdpcie_bus_resource_alloc failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}

			bcmerror = dhdpcie_bus_dongle_attach(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: dhdpcie_bus_dongle_attach failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}

			bcmerror = dhd_bus_request_irq(bus);
			if (bcmerror) {
				DHD_ERROR(("%s: dhd_bus_request_irq failed: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}

			bus->dhd->dongle_reset = FALSE;

			bcmerror = dhd_bus_start(dhdp);
			if (bcmerror) {
				DHD_ERROR(("%s: dhd_bus_start: %d\n",
					__FUNCTION__, bcmerror));
				goto done;
			}

			bus->dhd->up = TRUE;
			/* Renabling watchdog which is disabled in dhdpcie_advertise_bus_cleanup */
			if (bus->dhd->dhd_watchdog_ms_backup) {
				DHD_ERROR(("%s: Enabling wdtick after dhd init\n",
					__FUNCTION__));
				dhd_os_wd_timer(bus->dhd, bus->dhd->dhd_watchdog_ms_backup);
			}
			DHD_ERROR(("%s: WLAN Power On Done\n", __FUNCTION__));
		} else {
			DHD_ERROR(("%s: what should we do here\n", __FUNCTION__));
			goto done;
		}
	}

done:
	if (bcmerror) {
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->busstate = DHD_BUS_DOWN;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
	}
	return bcmerror;
}

/**
 * IOVAR handler of the DHD bus layer (in this case, the PCIe bus).
 *
 * @param actionid  e.g. IOV_SVAL(IOV_PCIEREG)
 * @param params    input buffer
 * @param plen      length in [bytes] of input buffer 'params'
 * @param arg       output buffer
 * @param len       length in [bytes] of output buffer 'arg'
 */
static int
dhdpcie_bus_doiovar(dhd_bus_t *bus, const bcm_iovar_t *vi, uint32 actionid, const char *name,
                void *params, int plen, void *arg, int len, int val_size)
{
	int bcmerror = 0;
	int32 int_val = 0;
	int32 int_val2 = 0;
	int32 int_val3 = 0;
	bool bool_val = 0;

	DHD_TRACE(("%s: Enter, action %d name %s params %p plen %d arg %p len %d val_size %d\n",
	           __FUNCTION__, actionid, name, params, plen, arg, len, val_size));

	if ((bcmerror = bcm_iovar_lencheck(vi, arg, len, IOV_ISSET(actionid))) != 0)
		goto exit;

	if (plen >= (int)sizeof(int_val))
		bcopy(params, &int_val, sizeof(int_val));

	if (plen >= (int)sizeof(int_val) * 2)
		bcopy((void*)((uintptr)params + sizeof(int_val)), &int_val2, sizeof(int_val2));

	if (plen >= (int)sizeof(int_val) * 3)
		bcopy((void*)((uintptr)params + 2 * sizeof(int_val)), &int_val3, sizeof(int_val3));

	bool_val = (int_val != 0) ? TRUE : FALSE;

	/* Check if dongle is in reset. If so, only allow DEVRESET iovars */
	if (bus->dhd->dongle_reset && !(actionid == IOV_SVAL(IOV_DEVRESET) ||
	                                actionid == IOV_GVAL(IOV_DEVRESET))) {
		bcmerror = BCME_NOTREADY;
		goto exit;
	}

	switch (actionid) {

	case IOV_SVAL(IOV_VARS):
		bcmerror = dhdpcie_downloadvars(bus, arg, len);
		break;
	case IOV_SVAL(IOV_PCIE_LPBK):
		bcmerror = dhdpcie_bus_lpback_req(bus, int_val);
		break;

	case IOV_SVAL(IOV_PCIE_DMAXFER): {
		int int_val4 = 0;
		int wait = 0;
		int core_num = 0;
		if (plen >= (int)sizeof(int_val) * 4) {
			bcopy((void*)((uintptr)params + 3 * sizeof(int_val)),
				&int_val4, sizeof(int_val4));
		}
		if (plen >= (int)sizeof(int_val) * 5) {
			bcopy((void*)((uintptr)params + 4 * sizeof(int_val)),
				&wait, sizeof(wait));
		}
		if (plen >= (int)sizeof(core_num) * 6) {
			bcopy((void*)((uintptr)params + 5 * sizeof(core_num)),
				&core_num, sizeof(core_num));
		}
		bcmerror = dhdpcie_bus_dmaxfer_req(bus, int_val, int_val2, int_val3,
				int_val4, core_num, wait);
		if (wait && bcmerror >= 0) {
			/* get the status of the dma transfer */
			int_val4 = dhdmsgbuf_dmaxfer_status(bus->dhd);
			bcopy(&int_val4, params, sizeof(int_val));
		}
		break;
	}

	case IOV_GVAL(IOV_PCIE_DMAXFER): {
		int dma_status = 0;
		dma_status = dhdmsgbuf_dmaxfer_status(bus->dhd);
		bcopy(&dma_status, arg, val_size);
		bcmerror = BCME_OK;
		break;
	}

	case IOV_GVAL(IOV_PCIE_SUSPEND):
		int_val = (bus->dhd->busstate == DHD_BUS_SUSPEND) ? 1 : 0;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_PCIE_SUSPEND):
		if (bool_val) { /* Suspend */
			int ret;
			unsigned long flags;

			/*
			 * If some other context is busy, wait until they are done,
			 * before starting suspend
			 */
			ret = dhd_os_busbusy_wait_condition(bus->dhd,
				&bus->dhd->dhd_bus_busy_state, DHD_BUS_BUSY_IN_DHD_IOVAR);
			if (ret == 0) {
				DHD_ERROR(("%s:Wait Timedout, dhd_bus_busy_state = 0x%x\n",
					__FUNCTION__, bus->dhd->dhd_bus_busy_state));
				return BCME_BUSY;
			}

			DHD_GENERAL_LOCK(bus->dhd, flags);
			DHD_BUS_BUSY_SET_SUSPEND_IN_PROGRESS(bus->dhd);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
			dhdpcie_bus_suspend(bus, TRUE, TRUE);
#else
			dhdpcie_bus_suspend(bus, TRUE);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

			DHD_GENERAL_LOCK(bus->dhd, flags);
			DHD_BUS_BUSY_CLEAR_SUSPEND_IN_PROGRESS(bus->dhd);
			dhd_os_busbusy_wake(bus->dhd);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
		} else { /* Resume */
			unsigned long flags;
			DHD_GENERAL_LOCK(bus->dhd, flags);
			DHD_BUS_BUSY_SET_RESUME_IN_PROGRESS(bus->dhd);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);

			dhdpcie_bus_suspend(bus, FALSE);

			DHD_GENERAL_LOCK(bus->dhd, flags);
			DHD_BUS_BUSY_CLEAR_RESUME_IN_PROGRESS(bus->dhd);
			dhd_os_busbusy_wake(bus->dhd);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
		}
		break;

	case IOV_GVAL(IOV_MEMSIZE):
		int_val = (int32)bus->ramsize;
		bcopy(&int_val, arg, val_size);
		break;

	/* Debug related. Dumps core registers or one of the dongle memory */
	case IOV_GVAL(IOV_DUMP_DONGLE):
	{
		dump_dongle_in_t ddi = *(dump_dongle_in_t*)params;
		dump_dongle_out_t *ddo = (dump_dongle_out_t*)arg;
		uint32 *p = ddo->val;
		const uint max_offset = 4096 - 1; /* one core contains max 4096/4 registers */

		if (plen < sizeof(ddi) || len < sizeof(ddo)) {
			bcmerror = BCME_BADARG;
			break;
		}

		switch (ddi.type) {
		case DUMP_DONGLE_COREREG:
			ddo->n_bytes = 0;

			if (si_setcoreidx(bus->sih, ddi.index) == NULL) {
				break; // beyond last core: core enumeration ended
			}

			ddo->address = si_addrspace(bus->sih, CORE_SLAVE_PORT_0, CORE_BASE_ADDR_0);
			ddo->address += ddi.offset; // BP address at which this dump starts

			ddo->id = si_coreid(bus->sih);
			ddo->rev = si_corerev(bus->sih);

			while (ddi.offset < max_offset &&
				sizeof(dump_dongle_out_t) + ddo->n_bytes < (uint)len) {
				*p++ = si_corereg(bus->sih, ddi.index, ddi.offset, 0, 0);
				ddi.offset += sizeof(uint32);
				ddo->n_bytes += sizeof(uint32);
			}
			break;
		default:
			// TODO: implement d11 SHM/TPL dumping
			bcmerror = BCME_BADARG;
			break;
		}
		break;
	}

	/* Debug related. Returns a string with dongle capabilities */
	case IOV_GVAL(IOV_DNGL_CAPS):
	{
		strncpy(arg, bus->dhd->fw_capabilities,
			MIN(strlen(bus->dhd->fw_capabilities), (size_t)len));
		((char*)arg)[len - 1] = '\0';
		break;
	}

#if defined(DEBUGGER) || defined(DHD_DSCOPE)
	case IOV_SVAL(IOV_GDB_SERVER):
		/* debugger_*() functions may sleep, so cannot hold spinlock */
		DHD_PERIM_UNLOCK(bus->dhd);
		if (int_val > 0) {
			debugger_init((void *) bus, &bus_ops, int_val, SI_ENUM_BASE(bus->sih));
		} else {
			debugger_close();
		}
		DHD_PERIM_LOCK(bus->dhd);
		break;
#endif /* DEBUGGER || DHD_DSCOPE */

#ifdef BCM_BUZZZ
	/* Dump dongle side buzzz trace to console */
	case IOV_GVAL(IOV_BUZZZ_DUMP):
		bcmerror = dhd_buzzz_dump_dngl(bus);
		break;
#endif /* BCM_BUZZZ */

	case IOV_SVAL(IOV_SET_DOWNLOAD_STATE):
		bcmerror = dhdpcie_bus_download_state(bus, bool_val);
		break;

	case IOV_GVAL(IOV_RAMSIZE):
		int_val = (int32)bus->ramsize;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RAMSIZE):
		bus->ramsize = int_val;
		bus->orig_ramsize = int_val;
		break;

	case IOV_GVAL(IOV_RAMSTART):
		int_val = (int32)bus->dongle_ram_base;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_CC_NVMSHADOW):
	{
		struct bcmstrbuf dump_b;

		bcm_binit(&dump_b, arg, len);
		bcmerror = dhdpcie_cc_nvmshadow(bus, &dump_b);
		break;
	}

	case IOV_GVAL(IOV_SLEEP_ALLOWED):
		bool_val = bus->sleep_allowed;
		bcopy(&bool_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SLEEP_ALLOWED):
		bus->sleep_allowed = bool_val;
		break;

	case IOV_GVAL(IOV_DONGLEISOLATION):
		int_val = bus->dhd->dongle_isolation;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DONGLEISOLATION):
		bus->dhd->dongle_isolation = bool_val;
		break;

	case IOV_GVAL(IOV_LTRSLEEPON_UNLOOAD):
		int_val = bus->ltrsleep_on_unload;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_LTRSLEEPON_UNLOOAD):
		bus->ltrsleep_on_unload = bool_val;
		break;

	case IOV_GVAL(IOV_DUMP_RINGUPD_BLOCK):
	{
		struct bcmstrbuf dump_b;
		bcm_binit(&dump_b, arg, len);
		bcmerror = dhd_prot_ringupd_dump(bus->dhd, &dump_b);
		break;
	}
	case IOV_GVAL(IOV_DMA_RINGINDICES):
	{	int h2d_support, d2h_support;

		d2h_support = bus->dhd->dma_d2h_ring_upd_support ? 1 : 0;
		h2d_support = bus->dhd->dma_h2d_ring_upd_support ? 1 : 0;
		int_val = d2h_support | (h2d_support << 1);
		bcopy(&int_val, arg, sizeof(int_val));
		break;
	}
	case IOV_SVAL(IOV_DMA_RINGINDICES):
		/* Can change it only during initialization/FW download */
		if (bus->dhd->busstate == DHD_BUS_DOWN) {
			if ((int_val > 3) || (int_val < 0)) {
				DHD_ERROR(("Bad argument. Possible values: 0, 1, 2 & 3\n"));
				bcmerror = BCME_BADARG;
			} else {
				bus->dhd->dma_d2h_ring_upd_support = (int_val & 1) ? TRUE : FALSE;
				bus->dhd->dma_h2d_ring_upd_support = (int_val & 2) ? TRUE : FALSE;
				bus->dhd->dma_ring_upd_overwrite = TRUE;
			}
		} else {
			DHD_ERROR(("%s: Can change only when bus down (before FW download)\n",
				__FUNCTION__));
			bcmerror = BCME_NOTDOWN;
		}
		break;

	case IOV_GVAL(IOV_METADATA_DBG):
		int_val = dhd_prot_metadata_dbg_get(bus->dhd);
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_METADATA_DBG):
		dhd_prot_metadata_dbg_set(bus->dhd, (int_val != 0));
		break;

	case IOV_GVAL(IOV_RX_METADATALEN):
		int_val = dhd_prot_metadatalen_get(bus->dhd, TRUE);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RX_METADATALEN):
		if (int_val > 64) {
			bcmerror = BCME_BUFTOOLONG;
			break;
		}
		dhd_prot_metadatalen_set(bus->dhd, int_val, TRUE);
		break;

	case IOV_SVAL(IOV_TXP_THRESHOLD):
		dhd_prot_txp_threshold(bus->dhd, TRUE, int_val);
		break;

	case IOV_GVAL(IOV_TXP_THRESHOLD):
		int_val = dhd_prot_txp_threshold(bus->dhd, FALSE, int_val);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DB1_FOR_MB):
		if (int_val)
			bus->db1_for_mb = TRUE;
		else
			bus->db1_for_mb = FALSE;
		break;

	case IOV_GVAL(IOV_DB1_FOR_MB):
		if (bus->db1_for_mb)
			int_val = 1;
		else
			int_val = 0;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_TX_METADATALEN):
		int_val = dhd_prot_metadatalen_get(bus->dhd, FALSE);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TX_METADATALEN):
		if (int_val > 64) {
			bcmerror = BCME_BUFTOOLONG;
			break;
		}
		dhd_prot_metadatalen_set(bus->dhd, int_val, FALSE);
		break;

	case IOV_SVAL(IOV_DEVRESET):
		switch (int_val) {
			case DHD_BUS_DEVRESET_ON:
			case DHD_BUS_DEVRESET_OFF:
				dhd_bus_devreset(bus->dhd, (uint8)int_val);
				break;
			case DHD_BUS_DEVRESET_FLR:
				dhd_bus_perform_flr(bus, FALSE);
				break;
			case DHD_BUS_DEVRESET_FLR_FORCE_FAIL:
				dhd_bus_perform_flr(bus, TRUE);
				break;
			default:
				DHD_ERROR(("%s: invalid argument for devreset\n", __FUNCTION__));
				break;
		}
		break;
	case IOV_SVAL(IOV_FORCE_FW_TRAP):
		if (bus->dhd->busstate == DHD_BUS_DATA)
			dhdpcie_fw_trap(bus);
		else {
			DHD_ERROR(("%s: Bus is NOT up\n", __FUNCTION__));
			bcmerror = BCME_NOTUP;
		}
		break;
	case IOV_GVAL(IOV_FLOW_PRIO_MAP):
		int_val = bus->dhd->flow_prio_map_type;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_FLOW_PRIO_MAP):
		int_val = (int32)dhd_update_flow_prio_map(bus->dhd, (uint8)int_val);
		bcopy(&int_val, arg, val_size);
		break;

#ifdef DHD_PCIE_RUNTIMEPM
	case IOV_GVAL(IOV_IDLETIME):
		int_val = bus->idletime;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_IDLETIME):
		if (int_val < 0) {
			bcmerror = BCME_BADARG;
		} else {
			bus->idletime = int_val;
			if (bus->idletime) {
				DHD_ENABLE_RUNTIME_PM(bus->dhd);
			} else {
				DHD_DISABLE_RUNTIME_PM(bus->dhd);
			}
		}
		break;
#endif /* DHD_PCIE_RUNTIMEPM */

	case IOV_GVAL(IOV_TXBOUND):
		int_val = (int32)dhd_txbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXBOUND):
		dhd_txbound = (uint)int_val;
		break;

	case IOV_SVAL(IOV_H2D_MAILBOXDATA):
		dhdpcie_send_mb_data(bus, (uint)int_val);
		break;

	case IOV_SVAL(IOV_INFORINGS):
		dhd_prot_init_info_rings(bus->dhd);
		break;

	case IOV_SVAL(IOV_H2D_PHASE):
		if (bus->dhd->busstate != DHD_BUS_DOWN) {
			DHD_ERROR(("%s: Can change only when bus down (before FW download)\n",
				__FUNCTION__));
			bcmerror = BCME_NOTDOWN;
			break;
		}
		if (int_val)
			bus->dhd->h2d_phase_supported = TRUE;
		else
			bus->dhd->h2d_phase_supported = FALSE;
		break;

	case IOV_GVAL(IOV_H2D_PHASE):
		int_val = (int32) bus->dhd->h2d_phase_supported;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_H2D_ENABLE_TRAP_BADPHASE):
		if (bus->dhd->busstate != DHD_BUS_DOWN) {
			DHD_ERROR(("%s: Can change only when bus down (before FW download)\n",
				__FUNCTION__));
			bcmerror = BCME_NOTDOWN;
			break;
		}
		if (int_val)
			bus->dhd->force_dongletrap_on_bad_h2d_phase = TRUE;
		else
			bus->dhd->force_dongletrap_on_bad_h2d_phase = FALSE;
		break;

	case IOV_GVAL(IOV_H2D_ENABLE_TRAP_BADPHASE):
		int_val = (int32) bus->dhd->force_dongletrap_on_bad_h2d_phase;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_H2D_TXPOST_MAX_ITEM):
		if (bus->dhd->busstate != DHD_BUS_DOWN) {
			DHD_ERROR(("%s: Can change only when bus down (before FW download)\n",
				__FUNCTION__));
			bcmerror = BCME_NOTDOWN;
			break;
		}
		dhd_prot_set_h2d_max_txpost(bus->dhd, (uint16)int_val);
		break;

	case IOV_GVAL(IOV_H2D_TXPOST_MAX_ITEM):
		int_val = dhd_prot_get_h2d_max_txpost(bus->dhd);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_RXBOUND):
		int_val = (int32)dhd_rxbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RXBOUND):
		dhd_rxbound = (uint)int_val;
		break;

	case IOV_GVAL(IOV_TRAPDATA):
	{
		struct bcmstrbuf dump_b;
		bcm_binit(&dump_b, arg, len);
		bcmerror = dhd_prot_dump_extended_trap(bus->dhd, &dump_b, FALSE);
		break;
	}

	case IOV_GVAL(IOV_TRAPDATA_RAW):
	{
		struct bcmstrbuf dump_b;
		bcm_binit(&dump_b, arg, len);
		bcmerror = dhd_prot_dump_extended_trap(bus->dhd, &dump_b, TRUE);
		break;
	}
	case IOV_SVAL(IOV_HANGREPORT):
		bus->dhd->hang_report = bool_val;
		DHD_ERROR(("%s: Set hang_report as %d\n",
			__FUNCTION__, bus->dhd->hang_report));
		break;

	case IOV_GVAL(IOV_HANGREPORT):
		int_val = (int32)bus->dhd->hang_report;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_CTO_PREVENTION):
		{
			uint32 pcie_lnkst;

			if (bus->sih->buscorerev < 19) {
				bcmerror = BCME_UNSUPPORTED;
				break;
			}
			si_corereg(bus->sih, bus->sih->buscoreidx,
					OFFSETOF(sbpcieregs_t, configaddr), ~0, PCI_LINK_STATUS);

			pcie_lnkst = si_corereg(bus->sih, bus->sih->buscoreidx,
				OFFSETOF(sbpcieregs_t, configdata), 0, 0);

			if ((bus->sih->buscorerev == 19) &&
				(((pcie_lnkst >> PCI_LINK_SPEED_SHIFT) &
					PCI_LINK_SPEED_MASK) == PCIE_LNK_SPEED_GEN1)) {
				bcmerror = BCME_UNSUPPORTED;
				break;
			}
			bus->dhd->cto_enable = bool_val;
			dhdpcie_cto_init(bus, bus->dhd->cto_enable);
			DHD_ERROR(("%s: set CTO prevention and recovery enable/disable %d\n",
				__FUNCTION__, bus->dhd->cto_enable));
		}
		break;

	case IOV_GVAL(IOV_CTO_PREVENTION):
		if (bus->sih->buscorerev < 19) {
			bcmerror = BCME_UNSUPPORTED;
			break;
		}
		int_val = (int32)bus->dhd->cto_enable;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_CTO_THRESHOLD):
		{
			if (bus->sih->buscorerev < 19) {
				bcmerror = BCME_UNSUPPORTED;
				break;
			}
			bus->dhd->cto_threshold = (uint32)int_val;
		}
		break;

	case IOV_GVAL(IOV_CTO_THRESHOLD):
		if (bus->sih->buscorerev < 19) {
			bcmerror = BCME_UNSUPPORTED;
			break;
		}
		if (bus->dhd->cto_threshold)
			int_val = (int32)bus->dhd->cto_threshold;
		else
			int_val = (int32)PCIE_CTO_TO_THRESH_DEFAULT;

		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_PCIE_WD_RESET):
		if (bool_val) {
			pcie_watchdog_reset(bus->osh, bus->sih,
				WD_ENABLE_MASK, WD_SSRESET_PCIE_F0_EN);
		}
		break;

	case IOV_GVAL(IOV_IDMA_ENABLE):
		int_val = bus->idma_enabled;
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_IDMA_ENABLE):
		bus->idma_enabled = (bool)int_val;
		break;
	case IOV_GVAL(IOV_IFRM_ENABLE):
		int_val = bus->ifrm_enabled;
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_IFRM_ENABLE):
		bus->ifrm_enabled = (bool)int_val;
		break;
	case IOV_GVAL(IOV_CLEAR_RING):
		bcopy(&int_val, arg, val_size);
		dhd_flow_rings_flush(bus->dhd, 0);
		break;
	case IOV_GVAL(IOV_DAR_ENABLE):
		int_val = bus->dar_enabled;
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_DAR_ENABLE):
		bus->dar_enabled = (bool)int_val;
		break;
	default:
		bcmerror = BCME_UNSUPPORTED;
		break;
	}

exit:
	return bcmerror;
} /* dhdpcie_bus_doiovar */

/** Transfers bytes from host to dongle using pio mode */
static int
dhdpcie_bus_lpback_req(struct  dhd_bus *bus, uint32 len)
{
	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return 0;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return 0;
	}
	if (bus->dhd->busstate != DHD_BUS_DATA) {
		DHD_ERROR(("not in a readystate to LPBK  is not inited\n"));
		return 0;
	}
	dhdmsgbuf_lpbk_req(bus->dhd, len);
	return 0;
}

/* Ring DoorBell1 to indicate Hostready i.e. D3 Exit */
void
dhd_bus_hostready(struct  dhd_bus *bus)
{
	if (!bus->dhd->d2h_hostrdy_supported) {
		return;
	}

	if (bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return;
	}

	DHD_INFO_HW4(("%s : Read PCICMD Reg: 0x%08X\n", __FUNCTION__,
		dhd_pcie_config_read(bus->osh, PCI_CFG_CMD, sizeof(uint32))));
	if (DAR_PWRREQ(bus)) {
		dhd_bus_pcie_pwr_req(bus);
	}
	si_corereg(bus->sih, bus->sih->buscoreidx, dhd_bus_db1_addr_get(bus), ~0, 0x12345678);
	bus->hostready_count ++;
	DHD_INFO_HW4(("%s: Ring Hostready:%d\n", __FUNCTION__, bus->hostready_count));
}

/* Clear INTSTATUS */
void
dhdpcie_bus_clear_intstatus(struct dhd_bus *bus)
{
	uint32 intstatus = 0;
	if ((bus->sih->buscorerev == 6) || (bus->sih->buscorerev == 4) ||
		(bus->sih->buscorerev == 2)) {
		intstatus = dhdpcie_bus_cfg_read_dword(bus, PCIIntstatus, 4);
		dhdpcie_bus_cfg_write_dword(bus, PCIIntstatus, 4, intstatus);
	} else {
		/* this is a PCIE core register..not a config register... */
		intstatus = si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_int, 0, 0);
		si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_int, bus->def_intmask,
			intstatus);
	}
}

int
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
dhdpcie_bus_suspend(struct dhd_bus *bus, bool state, bool byint)
#else
dhdpcie_bus_suspend(struct dhd_bus *bus, bool state)
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */
{
	int timeleft;
	int rc = 0;
	unsigned long flags, flags_bus;
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
	int d3_read_retry = 0;
	uint32 d2h_mb_data = 0;
	uint32 zero = 0;
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return BCME_ERROR;
	}

	if (dhd_query_bus_erros(bus->dhd)) {
		return BCME_ERROR;
	}

	DHD_GENERAL_LOCK(bus->dhd, flags);
	if (!(bus->dhd->busstate == DHD_BUS_DATA || bus->dhd->busstate == DHD_BUS_SUSPEND)) {
		DHD_ERROR(("not in a readystate\n"));
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		return BCME_ERROR;
	}
	DHD_GENERAL_UNLOCK(bus->dhd, flags);
	if (bus->dhd->dongle_reset) {
		DHD_ERROR(("Dongle is in reset state.\n"));
		return -EIO;
	}

	/* Check whether we are already in the requested state.
	 * state=TRUE means Suspend
	 * state=FALSE meanse Resume
	 */
	if (state == TRUE && bus->dhd->busstate == DHD_BUS_SUSPEND) {
		DHD_ERROR(("Bus is already in SUSPEND state.\n"));
		return BCME_OK;
	} else if (state == FALSE && bus->dhd->busstate == DHD_BUS_DATA) {
		DHD_ERROR(("Bus is already in RESUME state.\n"));
		return BCME_OK;
	}

	if (state) {
		int idle_retry = 0;
		int active;

		if (bus->is_linkdown) {
			DHD_ERROR(("%s: PCIe link was down, state=%d\n",
				__FUNCTION__, state));
			return BCME_ERROR;
		}

		/* Suspend */
		DHD_ERROR(("%s: Entering suspend state\n", __FUNCTION__));

		bus->dhd->dhd_watchdog_ms_backup = dhd_watchdog_ms;
		if (bus->dhd->dhd_watchdog_ms_backup) {
			DHD_ERROR(("%s: Disabling wdtick before going to suspend\n",
				__FUNCTION__));
			dhd_os_wd_timer(bus->dhd, 0);
		}

		DHD_GENERAL_LOCK(bus->dhd, flags);
		if (DHD_BUS_BUSY_CHECK_IN_TX(bus->dhd)) {
			DHD_ERROR(("Tx Request is not ended\n"));
			bus->dhd->busstate = DHD_BUS_DATA;
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
			return -EBUSY;
		}

		bus->last_suspend_start_time = OSL_SYSUPTIME_US();

		/* stop all interface network queue. */
		dhd_bus_stop_queue(bus);
		DHD_GENERAL_UNLOCK(bus->dhd, flags);

#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
		if (byint) {
			DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
			/* Clear wait_for_d3_ack before sending D3_INFORM */
			bus->wait_for_d3_ack = 0;
			dhdpcie_send_mb_data(bus, H2D_HOST_D3_INFORM);

			timeleft = dhd_os_d3ack_wait(bus->dhd, &bus->wait_for_d3_ack);
			DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
		} else {
			/* Clear wait_for_d3_ack before sending D3_INFORM */
			bus->wait_for_d3_ack = 0;
			dhdpcie_send_mb_data(bus, H2D_HOST_D3_INFORM | H2D_HOST_ACK_NOINT);
			while (!bus->wait_for_d3_ack && d3_read_retry < MAX_D3_ACK_TIMEOUT) {
				dhdpcie_handle_mb_data(bus);
				usleep_range(1000, 1500);
				d3_read_retry++;
			}
		}
#else
		DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);

		/* Clear wait_for_d3_ack before sending D3_INFORM */
		bus->wait_for_d3_ack = 0;
		/*
		 * Send H2D_HOST_D3_INFORM to dongle and mark bus->bus_low_power_state
		 * to DHD_BUS_D3_INFORM_SENT in dhd_prot_ring_write_complete_mbdata
		 * inside atomic context, so that no more DBs will be
		 * rung after sending D3_INFORM
		 */
		dhdpcie_send_mb_data(bus, H2D_HOST_D3_INFORM);

		/* Wait for D3 ACK for D3_ACK_RESP_TIMEOUT seconds */

		timeleft = dhd_os_d3ack_wait(bus->dhd, &bus->wait_for_d3_ack);

#ifdef DHD_RECOVER_TIMEOUT
		if (bus->wait_for_d3_ack == 0) {
			/* If wait_for_d3_ack was not updated because D2H MB was not received */
			uint32 intstatus = si_corereg(bus->sih, bus->sih->buscoreidx,
				bus->pcie_mailbox_int, 0, 0);
			int host_irq_disabled = dhdpcie_irq_disabled(bus);
			if ((intstatus) && (intstatus != (uint32)-1) &&
				(timeleft == 0) && (!dhd_query_bus_erros(bus->dhd))) {
				DHD_ERROR(("%s: D3 ACK trying again intstatus=%x"
					" host_irq_disabled=%d\n",
					__FUNCTION__, intstatus, host_irq_disabled));
				dhd_pcie_intr_count_dump(bus->dhd);
				dhd_print_tasklet_status(bus->dhd);
				dhd_prot_process_ctrlbuf(bus->dhd);
				timeleft = dhd_os_d3ack_wait(bus->dhd, &bus->wait_for_d3_ack);
				/* Clear Interrupts */
				dhdpcie_bus_clear_intstatus(bus);
			}
		} /* bus->wait_for_d3_ack was 0 */
#endif /* DHD_RECOVER_TIMEOUT */

		DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */

		/* To allow threads that got pre-empted to complete.
		 */
		while ((active = dhd_os_check_wakelock_all(bus->dhd)) &&
			(idle_retry < MAX_WKLK_IDLE_CHECK)) {
			OSL_SLEEP(1);
			idle_retry++;
		}

		if (bus->wait_for_d3_ack) {
			DHD_ERROR(("%s: Got D3 Ack \n", __FUNCTION__));

			/* Got D3 Ack. Suspend the bus */
			if (active) {
				DHD_ERROR(("%s():Suspend failed because of wakelock"
					"restoring Dongle to D0\n", __FUNCTION__));

				if (bus->dhd->dhd_watchdog_ms_backup) {
					DHD_ERROR(("%s: Enabling wdtick due to wakelock active\n",
						__FUNCTION__));
					dhd_os_wd_timer(bus->dhd,
						bus->dhd->dhd_watchdog_ms_backup);
				}

				/*
				 * Dongle still thinks that it has to be in D3 state until
				 * it gets a D0 Inform, but we are backing off from suspend.
				 * Ensure that Dongle is brought back to D0.
				 *
				 * Bringing back Dongle from D3 Ack state to D0 state is a
				 * 2 step process. Dongle would want to know that D0 Inform
				 * would be sent as a MB interrupt to bring it out of D3 Ack
				 * state to D0 state. So we have to send both this message.
				 */

				/* Clear wait_for_d3_ack to send D0_INFORM or host_ready */
				bus->wait_for_d3_ack = 0;

				DHD_BUS_LOCK(bus->bus_lock, flags_bus);
				bus->bus_low_power_state = DHD_BUS_NO_LOW_POWER_STATE;
				/* Enable back the intmask which was cleared in DPC
				 * after getting D3_ACK.
				 */
				bus->resume_intr_enable_count++;

				/* For Linux, Macos etc (otherthan NDIS) enable back the dongle
				 * interrupts using intmask and host interrupts
				 * which were disabled in the dhdpcie_bus_isr()->
				 * dhd_bus_handle_d3_ack().
				 */
				/* Enable back interrupt using Intmask!! */
				dhdpcie_bus_intr_enable(bus);
				/* Enable back interrupt from Host side!! */
				dhdpcie_enable_irq(bus);

				DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);

				if (bus->use_d0_inform) {
					DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
					dhdpcie_send_mb_data(bus,
						(H2D_HOST_D0_INFORM_IN_USE | H2D_HOST_D0_INFORM));
					DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
				}
				/* ring doorbell 1 (hostready) */
				dhd_bus_hostready(bus);

				DHD_GENERAL_LOCK(bus->dhd, flags);
				bus->dhd->busstate = DHD_BUS_DATA;
				/* resume all interface network queue. */
				dhd_bus_start_queue(bus);
				DHD_GENERAL_UNLOCK(bus->dhd, flags);
				rc = BCME_ERROR;
			} else {
				/* Actual Suspend after no wakelock */
				/* At this time bus->bus_low_power_state will be
				 * made to DHD_BUS_D3_ACK_RECIEVED after recieving D3_ACK
				 * in dhd_bus_handle_d3_ack()
				 */
				if (bus->use_d0_inform &&
					(bus->api.fw_rev < PCIE_SHARED_VERSION_6)) {
					DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
					dhdpcie_send_mb_data(bus, (H2D_HOST_D0_INFORM_IN_USE));
					DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
				}

#if defined(BCMPCIE_OOB_HOST_WAKE)
				dhdpcie_oob_intr_set(bus, TRUE);
#endif /* BCMPCIE_OOB_HOST_WAKE */

				DHD_GENERAL_LOCK(bus->dhd, flags);
				/* The Host cannot process interrupts now so disable the same.
				 * No need to disable the dongle INTR using intmask, as we are
				 * already calling disabling INTRs from DPC context after
				 * getting D3_ACK in dhd_bus_handle_d3_ack.
				 * Code may not look symmetric between Suspend and
				 * Resume paths but this is done to close down the timing window
				 * between DPC and suspend context and bus->bus_low_power_state
				 * will be set to DHD_BUS_D3_ACK_RECIEVED in DPC.
				 */
				bus->dhd->d3ackcnt_timeout = 0;
				bus->dhd->busstate = DHD_BUS_SUSPEND;
				DHD_GENERAL_UNLOCK(bus->dhd, flags);
				DHD_ERROR(("%s: BaseAddress0(0x%x)=0x%x, "
					"BaseAddress1(0x%x)=0x%x\n", __FUNCTION__,
					PCIECFGREG_BASEADDR0,
					dhd_pcie_config_read(bus->osh,
						PCIECFGREG_BASEADDR0, sizeof(uint32)),
					PCIECFGREG_BASEADDR1,
					dhd_pcie_config_read(bus->osh,
						PCIECFGREG_BASEADDR1, sizeof(uint32))));
				/* Handle Host Suspend */
				rc = dhdpcie_pci_suspend_resume(bus, state);
				if (!rc) {
					bus->last_suspend_end_time = OSL_SYSUPTIME_US();
				}
			}
		} else if (timeleft == 0) { /* D3 ACK Timeout */
			bus->dhd->d3ack_timeout_occured = TRUE;
			/* If the D3 Ack has timeout */
			bus->dhd->d3ackcnt_timeout++;
			DHD_ERROR(("%s: resumed on timeout for D3 ACK d3_inform_cnt %d \n",
				__FUNCTION__, bus->dhd->d3ackcnt_timeout));
			DHD_BUS_LOCK(bus->bus_lock, flags_bus);
			bus->bus_low_power_state = DHD_BUS_NO_LOW_POWER_STATE;
			DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);
			DHD_GENERAL_LOCK(bus->dhd, flags);
			bus->dhd->busstate = DHD_BUS_DATA;
			/* resume all interface network queue. */
			dhd_bus_start_queue(bus);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
			if (!bus->dhd->dongle_trap_occured) {
				uint32 intstatus = 0;

				/* Check if PCIe bus status is valid */
				intstatus = si_corereg(bus->sih, bus->sih->buscoreidx,
					bus->pcie_mailbox_int, 0, 0);
				if (intstatus == (uint32)-1) {
					/* Invalidate PCIe bus status */
					bus->is_linkdown = 1;
				}

				dhd_bus_dump_console_buffer(bus);
				dhd_prot_debug_info_print(bus->dhd);
#ifdef DHD_FW_COREDUMP
				if (bus->dhd->memdump_enabled) {
					/* write core dump to file */
					bus->dhd->memdump_type = DUMP_TYPE_D3_ACK_TIMEOUT;
					dhdpcie_mem_dump(bus);
				}
#endif /* DHD_FW_COREDUMP */

				DHD_ERROR(("%s: Event HANG send up due to D3_ACK timeout\n",
					__FUNCTION__));
#ifdef SUPPORT_LINKDOWN_RECOVERY
#ifdef CONFIG_ARCH_MSM
				bus->no_cfg_restore = 1;
#endif /* CONFIG_ARCH_MSM */
#endif /* SUPPORT_LINKDOWN_RECOVERY */
				dhd_os_check_hang(bus->dhd, 0, -ETIMEDOUT);
			}
			rc = -ETIMEDOUT;
		}
	} else {
		/* Resume */
		DHD_ERROR(("%s: Entering resume state\n", __FUNCTION__));
		bus->last_resume_start_time = OSL_SYSUPTIME_US();

		/**
		 * PCIE2_BAR0_CORE2_WIN gets reset after D3 cold.
		 * si_backplane_access(function to read/write backplane)
		 * updates the window(PCIE2_BAR0_CORE2_WIN) only if
		 * window being accessed is different form the window
		 * being pointed by second_bar0win.
		 * Since PCIE2_BAR0_CORE2_WIN is already reset because of D3 cold,
		 * invalidating second_bar0win after resume updates
		 * PCIE2_BAR0_CORE2_WIN with right window.
		 */
		si_invalidate_second_bar0win(bus->sih);
#if defined(BCMPCIE_OOB_HOST_WAKE)
		DHD_OS_OOB_IRQ_WAKE_UNLOCK(bus->dhd);
#endif /* BCMPCIE_OOB_HOST_WAKE */
		rc = dhdpcie_pci_suspend_resume(bus, state);
		DHD_ERROR(("%s: BaseAddress0(0x%x)=0x%x, BaseAddress1(0x%x)=0x%x\n",
			__FUNCTION__, PCIECFGREG_BASEADDR0,
			dhd_pcie_config_read(bus->osh, PCIECFGREG_BASEADDR0, sizeof(uint32)),
			PCIECFGREG_BASEADDR1,
			dhd_pcie_config_read(bus->osh, PCIECFGREG_BASEADDR1, sizeof(uint32))));

		DHD_BUS_LOCK(bus->bus_lock, flags_bus);
		/* set bus_low_power_state to DHD_BUS_NO_LOW_POWER_STATE */
		bus->bus_low_power_state = DHD_BUS_NO_LOW_POWER_STATE;
		DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);

		if (!rc && bus->dhd->busstate == DHD_BUS_SUSPEND) {
			if (bus->use_d0_inform) {
				DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
				dhdpcie_send_mb_data(bus, (H2D_HOST_D0_INFORM));
				DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
			}
			/* ring doorbell 1 (hostready) */
			dhd_bus_hostready(bus);
		}
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->busstate = DHD_BUS_DATA;
#ifdef DHD_PCIE_RUNTIMEPM
		if (DHD_BUS_BUSY_CHECK_RPM_SUSPEND_DONE(bus->dhd)) {
			bus->bus_wake = 1;
			OSL_SMP_WMB();
			wake_up_interruptible(&bus->rpm_queue);
		}
#endif /* DHD_PCIE_RUNTIMEPM */
		/* resume all interface network queue. */
		dhd_bus_start_queue(bus);

		/* TODO: for NDIS also we need to use enable_irq in future */
		bus->resume_intr_enable_count++;

		/* For Linux, Macos etc (otherthan NDIS) enable back the dongle interrupts
		 * using intmask and host interrupts
		 * which were disabled in the dhdpcie_bus_isr()->dhd_bus_handle_d3_ack().
		 */
		dhdpcie_bus_intr_enable(bus); /* Enable back interrupt using Intmask!! */
		dhdpcie_enable_irq(bus); /* Enable back interrupt from Host side!! */

		DHD_GENERAL_UNLOCK(bus->dhd, flags);

		if (bus->dhd->dhd_watchdog_ms_backup) {
			DHD_ERROR(("%s: Enabling wdtick after resume\n",
				__FUNCTION__));
			dhd_os_wd_timer(bus->dhd, bus->dhd->dhd_watchdog_ms_backup);
		}

		bus->last_resume_end_time = OSL_SYSUPTIME_US();

	}
	return rc;
}

uint32
dhdpcie_force_alp(struct dhd_bus *bus, bool enable)
{
	ASSERT(bus && bus->sih);
	if (enable) {
	si_corereg(bus->sih, bus->sih->buscoreidx,
		OFFSETOF(sbpcieregs_t, u.pcie2.clk_ctl_st), CCS_FORCEALP, CCS_FORCEALP);
	} else {
		si_corereg(bus->sih, bus->sih->buscoreidx,
			OFFSETOF(sbpcieregs_t, u.pcie2.clk_ctl_st), CCS_FORCEALP, 0);
	}
	return 0;
}

/* set pcie l1 entry time: dhd pciereg 0x1004[22:16] */
uint32
dhdpcie_set_l1_entry_time(struct dhd_bus *bus, int l1_entry_time)
{
	uint reg_val;

	ASSERT(bus && bus->sih);

	si_corereg(bus->sih, bus->sih->buscoreidx, OFFSETOF(sbpcieregs_t, configaddr), ~0,
		0x1004);
	reg_val = si_corereg(bus->sih, bus->sih->buscoreidx,
		OFFSETOF(sbpcieregs_t, configdata), 0, 0);
	reg_val = (reg_val & ~(0x7f << 16)) | ((l1_entry_time & 0x7f) << 16);
	si_corereg(bus->sih, bus->sih->buscoreidx, OFFSETOF(sbpcieregs_t, configdata), ~0,
		reg_val);

	return 0;
}

/** Transfers bytes from host to dongle and to host again using DMA */
static int
dhdpcie_bus_dmaxfer_req(struct  dhd_bus *bus,
		uint32 len, uint32 srcdelay, uint32 destdelay,
		uint32 d11_lpbk, uint32 core_num, uint32 wait)
{
	int ret = 0;

	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->busstate != DHD_BUS_DATA) {
		DHD_ERROR(("not in a readystate to LPBK  is not inited\n"));
		return BCME_ERROR;
	}

	if (len < 5 || len > 4194296) {
		DHD_ERROR(("len is too small or too large\n"));
		return BCME_ERROR;
	}

	bus->dmaxfer_complete = FALSE;
	ret = dhdmsgbuf_dmaxfer_req(bus->dhd, len, srcdelay, destdelay,
		d11_lpbk, core_num);
	if (ret != BCME_OK || !wait)
		return ret;

	ret = dhd_os_dmaxfer_wait(bus->dhd, &bus->dmaxfer_complete);
	if (ret < 0)
		ret = BCME_NOTREADY;

	return ret;

}

static int
dhdpcie_bus_download_state(dhd_bus_t *bus, bool enter)
{
	int bcmerror = 0;
	volatile uint32 *cr4_regs;

	if (!bus->sih) {
		DHD_ERROR(("%s: NULL sih!!\n", __FUNCTION__));
		return BCME_ERROR;
	}
	/* To enter download state, disable ARM and reset SOCRAM.
	 * To exit download state, simply reset ARM (default is RAM boot).
	 */
	if (enter) {
		/* Make sure BAR1 maps to backplane address 0 */
		dhdpcie_bus_cfg_write_dword(bus, PCI_BAR1_WIN, 4, 0x00000000);
		bus->alp_only = TRUE;

		/* some chips (e.g. 43602) have two ARM cores, the CR4 is receives the firmware. */
		cr4_regs = si_setcore(bus->sih, ARMCR4_CORE_ID, 0);

		if (cr4_regs == NULL && !(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
		    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0)) &&
		    !(si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
			DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
			bcmerror = BCME_ERROR;
			goto fail;
		}

		if (si_setcore(bus->sih, ARMCA7_CORE_ID, 0)) {
			/* Halt ARM & remove reset */
			si_core_reset(bus->sih, SICF_CPUHALT, SICF_CPUHALT);
			if (!(si_setcore(bus->sih, SYSMEM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SYSMEM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			si_core_reset(bus->sih, 0, 0);
			/* reset last 4 bytes of RAM address. to be used for shared area */
			dhdpcie_init_shared_addr(bus);
		} else if (cr4_regs == NULL) { /* no CR4 present on chip */
			si_core_disable(bus->sih, 0);

			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			si_core_reset(bus->sih, 0, 0);

			/* Clear the top bit of memory */
			if (bus->ramsize) {
				uint32 zeros = 0;
				if (dhdpcie_bus_membytes(bus, TRUE, bus->ramsize - 4,
				                     (uint8*)&zeros, 4) < 0) {
					bcmerror = BCME_ERROR;
					goto fail;
				}
			}
		} else {
			/* For CR4,
			 * Halt ARM
			 * Remove ARM reset
			 * Read RAM base address [0x18_0000]
			 * [next] Download firmware
			 * [done at else] Populate the reset vector
			 * [done at else] Remove ARM halt
			*/
			/* Halt ARM & remove reset */
			si_core_reset(bus->sih, SICF_CPUHALT, SICF_CPUHALT);
			if (BCM43602_CHIP(bus->sih->chip)) {
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKIDX, 5);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKPDA, 0);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKIDX, 7);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKPDA, 0);
			}
			/* reset last 4 bytes of RAM address. to be used for shared area */
			dhdpcie_init_shared_addr(bus);
		}
	} else {
		if (si_setcore(bus->sih, ARMCA7_CORE_ID, 0)) {
			/* write vars */
			if ((bcmerror = dhdpcie_bus_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}
			/* switch back to arm core again */
			if (!(si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM CA7 core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			/* write address 0 with reset instruction */
			bcmerror = dhdpcie_bus_membytes(bus, TRUE, 0,
				(uint8 *)&bus->resetinstr, sizeof(bus->resetinstr));
			/* now remove reset and halt and continue to run CA7 */
		} else if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if (!si_iscoreup(bus->sih)) {
				DHD_ERROR(("%s: SOCRAM core is down after reset?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			/* Enable remap before ARM reset but after vars.
			 * No backplane access in remap mode
			 */
			if (!si_setcore(bus->sih, PCMCIA_CORE_ID, 0) &&
			    !si_setcore(bus->sih, SDIOD_CORE_ID, 0)) {
				DHD_ERROR(("%s: Can't change back to SDIO core?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if (!(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
			    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
		} else {
			if (BCM43602_CHIP(bus->sih->chip)) {
				/* Firmware crashes on SOCSRAM access when core is in reset */
				if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
					DHD_ERROR(("%s: Failed to find SOCRAM core!\n",
						__FUNCTION__));
					bcmerror = BCME_ERROR;
					goto fail;
				}
				si_core_reset(bus->sih, 0, 0);
				si_setcore(bus->sih, ARMCR4_CORE_ID, 0);
			}

			/* write vars */
			if ((bcmerror = dhdpcie_bus_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}

			/* write a random number to TCM for the purpose of
			 * randomizing heap address space.
			 */
			dhdpcie_wrt_rnd(bus);

			/* switch back to arm core again */
			if (!(si_setcore(bus->sih, ARMCR4_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM CR4 core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			/* write address 0 with reset instruction */
			bcmerror = dhdpcie_bus_membytes(bus, TRUE, 0,
				(uint8 *)&bus->resetinstr, sizeof(bus->resetinstr));

			if (bcmerror == BCME_OK) {
				uint32 tmp;

				bcmerror = dhdpcie_bus_membytes(bus, FALSE, 0,
				                                (uint8 *)&tmp, sizeof(tmp));

				if (bcmerror == BCME_OK && tmp != bus->resetinstr) {
					DHD_ERROR(("%s: Failed to write 0x%08x to addr 0\n",
					          __FUNCTION__, bus->resetinstr));
					DHD_ERROR(("%s: contents of addr 0 is 0x%08x\n",
					          __FUNCTION__, tmp));
					bcmerror = BCME_ERROR;
					goto fail;
				}
			}

			/* now remove reset and halt and continue to run CR4 */
		}

		si_core_reset(bus->sih, 0, 0);

		/* Allow HT Clock now that the ARM is running. */
		bus->alp_only = FALSE;

		bus->dhd->busstate = DHD_BUS_LOAD;
	}

fail:
	/* Always return to PCIE core */
	si_setcore(bus->sih, PCIE2_CORE_ID, 0);

	return bcmerror;
} /* dhdpcie_bus_download_state */

static int
dhdpcie_bus_write_vars(dhd_bus_t *bus)
{
	int bcmerror = 0;
	uint32 varsize, phys_size;
	uint32 varaddr;
	uint8 *vbuffer;
	uint32 varsizew;
#ifdef DHD_DEBUG
	uint8 *nvram_ularray;
#endif /* DHD_DEBUG */

	/* Even if there are no vars are to be written, we still need to set the ramsize. */
	varsize = bus->varsz ? ROUNDUP(bus->varsz, 4) : 0;
	varaddr = (bus->ramsize - 4) - varsize;

	varaddr += bus->dongle_ram_base;

	if (bus->vars) {

		vbuffer = (uint8 *)MALLOC(bus->dhd->osh, varsize);
		if (!vbuffer)
			return BCME_NOMEM;

		bzero(vbuffer, varsize);
		bcopy(bus->vars, vbuffer, bus->varsz);
		/* Write the vars list */
		bcmerror = dhdpcie_bus_membytes(bus, TRUE, varaddr, vbuffer, varsize);

		/* Implement read back and verify later */
#ifdef DHD_DEBUG
		/* Verify NVRAM bytes */
		DHD_INFO(("Compare NVRAM dl & ul; varsize=%d\n", varsize));
		nvram_ularray = (uint8*)MALLOC(bus->dhd->osh, varsize);
		if (!nvram_ularray) {
			MFREE(bus->dhd->osh, vbuffer, varsize);
			return BCME_NOMEM;
		}

		/* Upload image to verify downloaded contents. */
		memset(nvram_ularray, 0xaa, varsize);

		/* Read the vars list to temp buffer for comparison */
		bcmerror = dhdpcie_bus_membytes(bus, FALSE, varaddr, nvram_ularray, varsize);
		if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d nvram bytes at 0x%08x\n",
					__FUNCTION__, bcmerror, varsize, varaddr));
		}

		/* Compare the org NVRAM with the one read from RAM */
		if (memcmp(vbuffer, nvram_ularray, varsize)) {
			DHD_ERROR(("%s: Downloaded NVRAM image is corrupted.\n", __FUNCTION__));
		} else
			DHD_ERROR(("%s: Download, Upload and compare of NVRAM succeeded.\n",
			__FUNCTION__));

		MFREE(bus->dhd->osh, nvram_ularray, varsize);
#endif /* DHD_DEBUG */

		MFREE(bus->dhd->osh, vbuffer, varsize);
	}

	phys_size = REMAP_ENAB(bus) ? bus->ramsize : bus->orig_ramsize;

	phys_size += bus->dongle_ram_base;

	/* adjust to the user specified RAM */
	DHD_INFO(("Physical memory size: %d, usable memory size: %d\n",
		phys_size, bus->ramsize));
	DHD_INFO(("Vars are at %d, orig varsize is %d\n",
		varaddr, varsize));
	varsize = ((phys_size - 4) - varaddr);

	/*
	 * Determine the length token:
	 * Varsize, converted to words, in lower 16-bits, checksum in upper 16-bits.
	 */
	if (bcmerror) {
		varsizew = 0;
		bus->nvram_csm = varsizew;
	} else {
		varsizew = varsize / 4;
		varsizew = (~varsizew << 16) | (varsizew & 0x0000FFFF);
		bus->nvram_csm = varsizew;
		varsizew = htol32(varsizew);
	}

	DHD_INFO(("New varsize is %d, length token=0x%08x\n", varsize, varsizew));

	/* Write the length token to the last word */
	bcmerror = dhdpcie_bus_membytes(bus, TRUE, (phys_size - 4),
		(uint8*)&varsizew, 4);

	return bcmerror;
} /* dhdpcie_bus_write_vars */

int
dhdpcie_downloadvars(dhd_bus_t *bus, void *arg, int len)
{
	int bcmerror = BCME_OK;
#ifdef KEEP_JP_REGREV
	char *tmpbuf;
	uint tmpidx;
#endif /* KEEP_JP_REGREV */

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	/* Basic sanity checks */
	if (bus->dhd->up) {
		bcmerror = BCME_NOTDOWN;
		goto err;
	}
	if (!len) {
		bcmerror = BCME_BUFTOOSHORT;
		goto err;
	}

	/* Free the old ones and replace with passed variables */
	if (bus->vars)
		MFREE(bus->dhd->osh, bus->vars, bus->varsz);

	bus->vars = MALLOC(bus->dhd->osh, len);
	bus->varsz = bus->vars ? len : 0;
	if (bus->vars == NULL) {
		bcmerror = BCME_NOMEM;
		goto err;
	}

	/* Copy the passed variables, which should include the terminating double-null */
	bcopy(arg, bus->vars, bus->varsz);

#ifdef DHD_USE_SINGLE_NVRAM_FILE
	if (dhd_bus_get_fw_mode(bus->dhd) == DHD_FLAG_MFG_MODE) {
		char *sp = NULL;
		char *ep = NULL;
		int i;
		char tag[2][8] = {"ccode=", "regrev="};

		/* Find ccode and regrev info */
		for (i = 0; i < 2; i++) {
			sp = strnstr(bus->vars, tag[i], bus->varsz);
			if (!sp) {
				DHD_ERROR(("%s: Could not find ccode info from the nvram %s\n",
					__FUNCTION__, bus->nv_path));
				bcmerror = BCME_ERROR;
				goto err;
			}
			sp = strchr(sp, '=');
			ep = strchr(sp, '\0');
			/* We assumed that string length of both ccode and
			 * regrev values should not exceed WLC_CNTRY_BUF_SZ
			 */
			if (ep && ((ep - sp) <= WLC_CNTRY_BUF_SZ)) {
				sp++;
				while (*sp != '\0') {
					DHD_INFO(("%s: parse '%s', current sp = '%c'\n",
						__FUNCTION__, tag[i], *sp));
					*sp++ = '0';
				}
			} else {
				DHD_ERROR(("%s: Invalid parameter format when parsing for %s\n",
					__FUNCTION__, tag[i]));
				bcmerror = BCME_ERROR;
				goto err;
			}
		}
	}
#endif /* DHD_USE_SINGLE_NVRAM_FILE */

#ifdef KEEP_JP_REGREV
#ifdef DHD_USE_SINGLE_NVRAM_FILE
	if (dhd_bus_get_fw_mode(bus->dhd) != DHD_FLAG_MFG_MODE)
#endif /* DHD_USE_SINGLE_NVRAM_FILE */
	{
		char *pos = NULL;
		tmpbuf = MALLOCZ(bus->dhd->osh, bus->varsz + 1);
		if (tmpbuf == NULL) {
			goto err;
		}
		memcpy(tmpbuf, bus->vars, bus->varsz);
		for (tmpidx = 0; tmpidx < bus->varsz; tmpidx++) {
			if (tmpbuf[tmpidx] == 0) {
				tmpbuf[tmpidx] = '\n';
			}
		}
		bus->dhd->vars_ccode[0] = 0;
		bus->dhd->vars_regrev = 0;
		if ((pos = strstr(tmpbuf, "ccode"))) {
			sscanf(pos, "ccode=%s\n", bus->dhd->vars_ccode);
		}
		if ((pos = strstr(tmpbuf, "regrev"))) {
			sscanf(pos, "regrev=%u\n", &(bus->dhd->vars_regrev));
		}
		MFREE(bus->dhd->osh, tmpbuf, bus->varsz + 1);
	}
#endif /* KEEP_JP_REGREV */

err:
	return bcmerror;
}

/* loop through the capability list and see if the pcie capabilty exists */
uint8
dhdpcie_find_pci_capability(osl_t *osh, uint8 req_cap_id)
{
	uint8 cap_id;
	uint8 cap_ptr = 0;
	uint8 byte_val;

	/* check for Header type 0 */
	byte_val = read_pci_cfg_byte(PCI_CFG_HDR);
	if ((byte_val & 0x7f) != PCI_HEADER_NORMAL) {
		DHD_ERROR(("%s : PCI config header not normal.\n", __FUNCTION__));
		goto end;
	}

	/* check if the capability pointer field exists */
	byte_val = read_pci_cfg_byte(PCI_CFG_STAT);
	if (!(byte_val & PCI_CAPPTR_PRESENT)) {
		DHD_ERROR(("%s : PCI CAP pointer not present.\n", __FUNCTION__));
		goto end;
	}

	cap_ptr = read_pci_cfg_byte(PCI_CFG_CAPPTR);
	/* check if the capability pointer is 0x00 */
	if (cap_ptr == 0x00) {
		DHD_ERROR(("%s : PCI CAP pointer is 0x00.\n", __FUNCTION__));
		goto end;
	}

	/* loop thr'u the capability list and see if the pcie capabilty exists */

	cap_id = read_pci_cfg_byte(cap_ptr);

	while (cap_id != req_cap_id) {
		cap_ptr = read_pci_cfg_byte((cap_ptr + 1));
		if (cap_ptr == 0x00) break;
		cap_id = read_pci_cfg_byte(cap_ptr);
	}

end:
	return cap_ptr;
}

void
dhdpcie_pme_active(osl_t *osh, bool enable)
{
	uint8 cap_ptr;
	uint32 pme_csr;

	cap_ptr = dhdpcie_find_pci_capability(osh, PCI_CAP_POWERMGMTCAP_ID);

	if (!cap_ptr) {
		DHD_ERROR(("%s : Power Management Capability not present\n", __FUNCTION__));
		return;
	}

	pme_csr = OSL_PCI_READ_CONFIG(osh, cap_ptr + PME_CSR_OFFSET, sizeof(uint32));
	DHD_ERROR(("%s : pme_sts_ctrl 0x%x\n", __FUNCTION__, pme_csr));

	pme_csr |= PME_CSR_PME_STAT;
	if (enable) {
		pme_csr |= PME_CSR_PME_EN;
	} else {
		pme_csr &= ~PME_CSR_PME_EN;
	}

	OSL_PCI_WRITE_CONFIG(osh, cap_ptr + PME_CSR_OFFSET, sizeof(uint32), pme_csr);
}

bool
dhdpcie_pme_cap(osl_t *osh)
{
	uint8 cap_ptr;
	uint32 pme_cap;

	cap_ptr = dhdpcie_find_pci_capability(osh, PCI_CAP_POWERMGMTCAP_ID);

	if (!cap_ptr) {
		DHD_ERROR(("%s : Power Management Capability not present\n", __FUNCTION__));
		return FALSE;
	}

	pme_cap = OSL_PCI_READ_CONFIG(osh, cap_ptr, sizeof(uint32));

	DHD_ERROR(("%s : pme_cap 0x%x\n", __FUNCTION__, pme_cap));

	return ((pme_cap & PME_CAP_PM_STATES) != 0);
}

uint32
dhdpcie_lcreg(osl_t *osh, uint32 mask, uint32 val)
{

	uint8	pcie_cap;
	uint8	lcreg_offset;	/* PCIE capability LCreg offset in the config space */
	uint32	reg_val;

	pcie_cap = dhdpcie_find_pci_capability(osh, PCI_CAP_PCIECAP_ID);

	if (!pcie_cap) {
		DHD_ERROR(("%s : PCIe Capability not present\n", __FUNCTION__));
		return 0;
	}

	lcreg_offset = pcie_cap + PCIE_CAP_LINKCTRL_OFFSET;

	/* set operation */
	if (mask) {
		/* read */
		reg_val = OSL_PCI_READ_CONFIG(osh, lcreg_offset, sizeof(uint32));

		/* modify */
		reg_val &= ~mask;
		reg_val |= (mask & val);

		/* write */
		OSL_PCI_WRITE_CONFIG(osh, lcreg_offset, sizeof(uint32), reg_val);
	}
	return OSL_PCI_READ_CONFIG(osh, lcreg_offset, sizeof(uint32));
}

uint8
dhdpcie_clkreq(osl_t *osh, uint32 mask, uint32 val)
{
	uint8	pcie_cap;
	uint32	reg_val;
	uint8	lcreg_offset;	/* PCIE capability LCreg offset in the config space */

	pcie_cap = dhdpcie_find_pci_capability(osh, PCI_CAP_PCIECAP_ID);

	if (!pcie_cap) {
		DHD_ERROR(("%s : PCIe Capability not present\n", __FUNCTION__));
		return 0;
	}

	lcreg_offset = pcie_cap + PCIE_CAP_LINKCTRL_OFFSET;

	reg_val = OSL_PCI_READ_CONFIG(osh, lcreg_offset, sizeof(uint32));
	/* set operation */
	if (mask) {
		if (val)
			reg_val |= PCIE_CLKREQ_ENAB;
		else
			reg_val &= ~PCIE_CLKREQ_ENAB;
		OSL_PCI_WRITE_CONFIG(osh, lcreg_offset, sizeof(uint32), reg_val);
		reg_val = OSL_PCI_READ_CONFIG(osh, lcreg_offset, sizeof(uint32));
	}
	if (reg_val & PCIE_CLKREQ_ENAB)
		return 1;
	else
		return 0;
}

void dhd_dump_intr_counters(dhd_pub_t *dhd, struct bcmstrbuf *strbuf)
{
	bcm_bprintf(strbuf, "\n ------- DUMPING INTR enable/disable counters-------\n");
	bcm_bprintf(strbuf, "resume_intr_enable_count=%lu dpc_intr_enable_count=%lu\n"
		"isr_intr_disable_count=%lu suspend_intr_disable_count=%lu\n"
		"dpc_return_busdown_count=%lu\n",
		dhd->bus->resume_intr_enable_count, dhd->bus->dpc_intr_enable_count,
		dhd->bus->isr_intr_disable_count, dhd->bus->suspend_intr_disable_count,
		dhd->bus->dpc_return_busdown_count);
#ifdef BCMPCIE_OOB_HOST_WAKE
	bcm_bprintf(strbuf, "oob_intr_count=%lu oob_intr_enable_count=%lu"
		" oob_intr_disable_count=%lu\n oob_irq_num=%d last_oob_irq_time=%llu\n",
		dhd->bus->oob_intr_count, dhd->bus->oob_intr_enable_count,
		dhd->bus->oob_intr_disable_count, dhdpcie_get_oob_irq_num(dhd->bus),
		dhd->bus->last_oob_irq_time);
#endif /* BCMPCIE_OOB_HOST_WAKE */
	bcm_bprintf(strbuf, "\ncurrent_time=%llu isr_entry_time=%llu isr_exit_time=%llu\n"
		"dpc_entry_time=%llu last_process_ctrlbuf_time=%llu "
		"last_process_flowring_time=%llu last_process_txcpl_time=%llu\n"
		"last_process_rxcpl_time=%llu last_process_infocpl_time=%llu "
		"dpc_exit_time=%llu resched_dpc_time=%llu\n",
		OSL_SYSUPTIME_US(), dhd->bus->isr_entry_time, dhd->bus->isr_exit_time,
		dhd->bus->dpc_entry_time, dhd->bus->last_process_ctrlbuf_time,
		dhd->bus->last_process_flowring_time, dhd->bus->last_process_txcpl_time,
		dhd->bus->last_process_rxcpl_time, dhd->bus->last_process_infocpl_time,
		dhd->bus->dpc_exit_time, dhd->bus->resched_dpc_time);
	bcm_bprintf(strbuf, "\nlast_suspend_start_time=%llu last_suspend_end_time=%llu"
		" last_resume_start_time=%llu last_resume_end_time=%llu\n",
		dhd->bus->last_suspend_start_time, dhd->bus->last_suspend_end_time,
		dhd->bus->last_resume_start_time, dhd->bus->last_resume_end_time);
}

void dhd_dump_intr_registers(dhd_pub_t *dhd, struct bcmstrbuf *strbuf)
{
	uint32 intstatus = 0;
	uint32 intmask = 0;
	uint32 mbintstatus = 0;
	uint32 d2h_mb_data = 0;

	intstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx,
		dhd->bus->pcie_mailbox_int, 0, 0);
	intmask = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx,
		dhd->bus->pcie_mailbox_mask, 0, 0);
	mbintstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, PCID2H_MailBox, 0, 0);
	dhd_bus_cmn_readshared(dhd->bus, &d2h_mb_data, D2H_MB_DATA, 0);

	bcm_bprintf(strbuf, "intstatus=0x%x intmask=0x%x mbintstatus=0x%x\n",
		intstatus, intmask, mbintstatus);
	bcm_bprintf(strbuf, "d2h_mb_data=0x%x def_intmask=0x%x\n",
		d2h_mb_data, dhd->bus->def_intmask);
}

/** Add bus dump output to a buffer */
void dhd_bus_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	uint16 flowid;
	int ix = 0;
	flow_ring_node_t *flow_ring_node;
	flow_info_t *flow_info;

	if (dhdp->busstate != DHD_BUS_DATA)
		return;

#ifdef DHD_WAKE_STATUS
	bcm_bprintf(strbuf, "wake %u rxwake %u readctrlwake %u\n",
		bcmpcie_get_total_wake(dhdp->bus), dhdp->bus->wake_counts.rxwake,
		dhdp->bus->wake_counts.rcwake);
#ifdef DHD_WAKE_RX_STATUS
	bcm_bprintf(strbuf, " unicast %u muticast %u broadcast %u arp %u\n",
		dhdp->bus->wake_counts.rx_ucast, dhdp->bus->wake_counts.rx_mcast,
		dhdp->bus->wake_counts.rx_bcast, dhdp->bus->wake_counts.rx_arp);
	bcm_bprintf(strbuf, " multi4 %u multi6 %u icmp6 %u multiother %u\n",
		dhdp->bus->wake_counts.rx_multi_ipv4, dhdp->bus->wake_counts.rx_multi_ipv6,
		dhdp->bus->wake_counts.rx_icmpv6, dhdp->bus->wake_counts.rx_multi_other);
	bcm_bprintf(strbuf, " icmp6_ra %u, icmp6_na %u, icmp6_ns %u\n",
		dhdp->bus->wake_counts.rx_icmpv6_ra, dhdp->bus->wake_counts.rx_icmpv6_na,
		dhdp->bus->wake_counts.rx_icmpv6_ns);
#endif /* DHD_WAKE_RX_STATUS */
#ifdef DHD_WAKE_EVENT_STATUS
	for (flowid = 0; flowid < WLC_E_LAST; flowid++)
		if (dhdp->bus->wake_counts.rc_event[flowid] != 0)
			bcm_bprintf(strbuf, " %s = %u\n", bcmevent_get_name(flowid),
				dhdp->bus->wake_counts.rc_event[flowid]);
	bcm_bprintf(strbuf, "\n");
#endif /* DHD_WAKE_EVENT_STATUS */
#endif /* DHD_WAKE_STATUS */

	dhd_prot_print_info(dhdp, strbuf);
	dhd_dump_intr_registers(dhdp, strbuf);
	dhd_dump_intr_counters(dhdp, strbuf);
	bcm_bprintf(strbuf, "h2d_mb_data_ptr_addr 0x%x, d2h_mb_data_ptr_addr 0x%x\n",
		dhdp->bus->h2d_mb_data_ptr_addr, dhdp->bus->d2h_mb_data_ptr_addr);
	bcm_bprintf(strbuf, "dhd cumm_ctr %d\n", DHD_CUMM_CTR_READ(&dhdp->cumm_ctr));
	bcm_bprintf(strbuf,
		"%s %4s %2s %4s %17s %4s %4s %6s %10s %4s %4s ",
		"Num:", "Flow", "If", "Prio", ":Dest_MacAddress:", "Qlen", "CLen", "L2CLen",
		"Overflows", "RD", "WR");
	bcm_bprintf(strbuf, "%5s %6s %5s \n", "Acked", "tossed", "noack");

	for (flowid = 0; flowid < dhdp->num_flow_rings; flowid++) {
		flow_ring_node = DHD_FLOW_RING(dhdp, flowid);
		if (!flow_ring_node->active)
			continue;

		flow_info = &flow_ring_node->flow_info;
		bcm_bprintf(strbuf,
			"%3d. %4d %2d %4d "MACDBG" %4d %4d %6d %10u ", ix++,
			flow_ring_node->flowid, flow_info->ifindex, flow_info->tid,
			MAC2STRDBG(flow_info->da),
			DHD_FLOW_QUEUE_LEN(&flow_ring_node->queue),
			DHD_CUMM_CTR_READ(DHD_FLOW_QUEUE_CLEN_PTR(&flow_ring_node->queue)),
			DHD_CUMM_CTR_READ(DHD_FLOW_QUEUE_L2CLEN_PTR(&flow_ring_node->queue)),
			DHD_FLOW_QUEUE_FAILURES(&flow_ring_node->queue));
		dhd_prot_print_flow_ring(dhdp, flow_ring_node->prot_info, strbuf,
			"%4d %4d ");
		bcm_bprintf(strbuf,
			"%5s %6s %5s\n", "NA", "NA", "NA");
	}
	bcm_bprintf(strbuf, "D3 inform cnt %d\n", dhdp->bus->d3_inform_cnt);
	bcm_bprintf(strbuf, "D0 inform cnt %d\n", dhdp->bus->d0_inform_cnt);
	bcm_bprintf(strbuf, "D0 inform in use cnt %d\n", dhdp->bus->d0_inform_in_use_cnt);
	if (dhdp->d2h_hostrdy_supported) {
		bcm_bprintf(strbuf, "hostready count:%d\n", dhdp->bus->hostready_count);
	}
	bcm_bprintf(strbuf, "d2h_intr_method -> %s\n",
		dhdp->bus->d2h_intr_method ? "PCIE_MSI" : "PCIE_INTX");
}

/**
 * Brings transmit packets on all flow rings closer to the dongle, by moving (a subset) from their
 * flow queue to their flow ring.
 */
static void
dhd_update_txflowrings(dhd_pub_t *dhd)
{
	unsigned long flags;
	dll_t *item, *next;
	flow_ring_node_t *flow_ring_node;
	struct dhd_bus *bus = dhd->bus;

	/* Hold flowring_list_lock to ensure no race condition while accessing the List */
	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	for (item = dll_head_p(&bus->flowring_active_list);
		(!dhd_is_device_removed(dhd) && !dll_end(&bus->flowring_active_list, item));
		item = next) {
		if (dhd->hang_was_sent) {
			break;
		}

		next = dll_next_p(item);
		flow_ring_node = dhd_constlist_to_flowring(item);

		/* Ensure that flow_ring_node in the list is Not Null */
		ASSERT(flow_ring_node != NULL);

		/* Ensure that the flowring node has valid contents */
		ASSERT(flow_ring_node->prot_info != NULL);

		dhd_prot_update_txflowring(dhd, flow_ring_node->flowid, flow_ring_node->prot_info);
	}
	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);
}

/** Mailbox ringbell Function */
static void
dhd_bus_gen_devmb_intr(struct dhd_bus *bus)
{
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		DHD_ERROR(("mailbox communication not supported\n"));
		return;
	}
	if (bus->db1_for_mb)  {
		/* this is a pcie core register, not the config register */
		DHD_INFO(("writing a mail box interrupt to the device, through doorbell 1\n"));
		if (DAR_PWRREQ(bus)) {
			dhd_bus_pcie_pwr_req(bus);
		}
		si_corereg(bus->sih, bus->sih->buscoreidx, dhd_bus_db1_addr_get(bus),
			~0, 0x12345678);
	} else {
		DHD_INFO(("writing a mail box interrupt to the device, through config space\n"));
		dhdpcie_bus_cfg_write_dword(bus, PCISBMbx, 4, (1 << 0));
		dhdpcie_bus_cfg_write_dword(bus, PCISBMbx, 4, (1 << 0));
	}
}

/* Upon receiving a mailbox interrupt,
 * if H2D_FW_TRAP bit is set in mailbox location
 * device traps
 */
static void
dhdpcie_fw_trap(dhd_bus_t *bus)
{
	/* Send the mailbox data and generate mailbox intr. */
	dhdpcie_send_mb_data(bus, H2D_FW_TRAP);
	/* For FWs that cannot interprete H2D_FW_TRAP */
	(void)dhd_wl_ioctl_set_intiovar(bus->dhd, "bus:disconnect", 99, WLC_SET_VAR, TRUE, 0);
}

/** mailbox doorbell ring function */
void
dhd_bus_ringbell(struct dhd_bus *bus, uint32 value)
{
	/* Skip after sending D3_INFORM */
	if (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE) {
		DHD_ERROR(("%s: trying to ring the doorbell after D3 inform %d\n",
			__FUNCTION__, bus->bus_low_power_state));
		return;
	}
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		si_corereg(bus->sih, bus->sih->buscoreidx, bus->pcie_mailbox_int,
			PCIE_INTB, PCIE_INTB);
	} else {
		/* this is a pcie core register, not the config regsiter */
		DHD_INFO(("writing a door bell to the device\n"));
		if (IDMA_ACTIVE(bus->dhd)) {
			if (DAR_PWRREQ(bus)) {
				dhd_bus_pcie_pwr_req(bus);
			}
			si_corereg(bus->sih, bus->sih->buscoreidx, dhd_bus_db0_addr_2_get(bus),
				~0, value);
		} else {
			if (DAR_PWRREQ(bus)) {
				dhd_bus_pcie_pwr_req(bus);
			}
			si_corereg(bus->sih, bus->sih->buscoreidx,
				dhd_bus_db0_addr_get(bus), ~0, 0x12345678);
		}
	}
}

/** mailbox doorbell ring function for IDMA/IFRM using dma channel2 */
void
dhd_bus_ringbell_2(struct dhd_bus *bus, uint32 value, bool devwake)
{
	/* this is a pcie core register, not the config regsiter */
	/* Skip after sending D3_INFORM */
	if (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE) {
		DHD_ERROR(("%s: trying to ring the doorbell after D3 inform %d\n",
			__FUNCTION__, bus->bus_low_power_state));
		return;
	}
	DHD_INFO(("writing a door bell 2 to the device\n"));
	if (DAR_PWRREQ(bus)) {
		dhd_bus_pcie_pwr_req(bus);
	}
	si_corereg(bus->sih, bus->sih->buscoreidx, dhd_bus_db0_addr_2_get(bus),
		~0, value);
}

void
dhdpcie_bus_ringbell_fast(struct dhd_bus *bus, uint32 value)
{
	/* Skip after sending D3_INFORM */
	if (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE) {
		DHD_ERROR(("%s: trying to ring the doorbell after D3 inform %d\n",
			__FUNCTION__, bus->bus_low_power_state));
		return;
	}

	if (DAR_PWRREQ(bus)) {
		dhd_bus_pcie_pwr_req(bus);
	}
	W_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr, value);
}

void
dhdpcie_bus_ringbell_2_fast(struct dhd_bus *bus, uint32 value, bool devwake)
{
	/* Skip after sending D3_INFORM */
	if (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE) {
		DHD_ERROR(("%s: trying to ring the doorbell after D3 inform %d\n",
			__FUNCTION__, bus->bus_low_power_state));
		return;
	}

	if (DAR_PWRREQ(bus)) {
		dhd_bus_pcie_pwr_req(bus);
	}
	W_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_2_addr, value);
}

static void
dhd_bus_ringbell_oldpcie(struct dhd_bus *bus, uint32 value)
{
	uint32 w;
	/* Skip after sending D3_INFORM */
	if (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE) {
		DHD_ERROR(("%s: trying to ring the doorbell after D3 inform %d\n",
			__FUNCTION__, bus->bus_low_power_state));
		return;
	}
	w = (R_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr) & ~PCIE_INTB) | PCIE_INTB;
	W_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr, w);
}

dhd_mb_ring_t
dhd_bus_get_mbintr_fn(struct dhd_bus *bus)
{
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		bus->pcie_mb_intr_addr = si_corereg_addr(bus->sih, bus->sih->buscoreidx,
			bus->pcie_mailbox_int);
		if (bus->pcie_mb_intr_addr) {
			bus->pcie_mb_intr_osh = si_osh(bus->sih);
			return dhd_bus_ringbell_oldpcie;
		}
	} else {
		bus->pcie_mb_intr_addr = si_corereg_addr(bus->sih, bus->sih->buscoreidx,
			dhd_bus_db0_addr_get(bus));
		if (bus->pcie_mb_intr_addr) {
			bus->pcie_mb_intr_osh = si_osh(bus->sih);
			return dhdpcie_bus_ringbell_fast;
		}
	}
	return dhd_bus_ringbell;
}

dhd_mb_ring_2_t
dhd_bus_get_mbintr_2_fn(struct dhd_bus *bus)
{
	bus->pcie_mb_intr_2_addr = si_corereg_addr(bus->sih, bus->sih->buscoreidx,
		dhd_bus_db0_addr_2_get(bus));
	if (bus->pcie_mb_intr_2_addr) {
		bus->pcie_mb_intr_osh = si_osh(bus->sih);
		return dhdpcie_bus_ringbell_2_fast;
	}
	return dhd_bus_ringbell_2;
}

bool BCMFASTPATH
dhd_bus_dpc(struct dhd_bus *bus)
{
	bool resched = FALSE;	  /* Flag indicating resched wanted */
	unsigned long flags;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	bus->dpc_entry_time = OSL_SYSUPTIME_US();

	DHD_GENERAL_LOCK(bus->dhd, flags);
	/* Check for only DHD_BUS_DOWN and not for DHD_BUS_DOWN_IN_PROGRESS
	 * to avoid IOCTL Resumed On timeout when ioctl is waiting for response
	 * and rmmod is fired in parallel, which will make DHD_BUS_DOWN_IN_PROGRESS
	 * and if we return from here, then IOCTL response will never be handled
	 */
	if (bus->dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: Bus down, ret\n", __FUNCTION__));
		bus->intstatus = 0;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		bus->dpc_return_busdown_count++;
		return 0;
	}
#ifdef DHD_PCIE_RUNTIMEPM
	bus->idlecount = 0;
#endif /* DHD_PCIE_RUNTIMEPM */
	DHD_BUS_BUSY_SET_IN_DPC(bus->dhd);
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	resched = dhdpcie_bus_process_mailbox_intr(bus, bus->intstatus);
	if (!resched) {
		bus->intstatus = 0;
		bus->dpc_intr_enable_count++;
		/* For Linux, Macos etc (otherthan NDIS) enable back the host interrupts
		 * which has been disabled in the dhdpcie_bus_isr()
		 */
		 dhdpcie_enable_irq(bus); /* Enable back interrupt!! */
		bus->dpc_exit_time = OSL_SYSUPTIME_US();
	} else {
		bus->resched_dpc_time = OSL_SYSUPTIME_US();
	}

	bus->dpc_sched = resched;

	DHD_GENERAL_LOCK(bus->dhd, flags);
	DHD_BUS_BUSY_CLEAR_IN_DPC(bus->dhd);
	dhd_os_busbusy_wake(bus->dhd);
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	return resched;

}

int
dhdpcie_send_mb_data(dhd_bus_t *bus, uint32 h2d_mb_data)
{
	uint32 cur_h2d_mb_data = 0;

	DHD_INFO_HW4(("%s: H2D_MB_DATA: 0x%08X\n", __FUNCTION__, h2d_mb_data));

	if (bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (bus->api.fw_rev >= PCIE_SHARED_VERSION_6 && !bus->use_mailbox) {
		DHD_INFO(("API rev is 6, sending mb data as H2D Ctrl message to dongle, 0x%04x\n",
			h2d_mb_data));
		/* Prevent asserting device_wake during doorbell ring for mb data to avoid loop. */
		{
			if (dhd_prot_h2d_mbdata_send_ctrlmsg(bus->dhd, h2d_mb_data)) {
				DHD_ERROR(("failure sending the H2D Mailbox message "
					"to firmware\n"));
				goto fail;
			}
		}
		goto done;
	}

	dhd_bus_cmn_readshared(bus, &cur_h2d_mb_data, H2D_MB_DATA, 0);

	if (cur_h2d_mb_data != 0) {
		uint32 i = 0;
		DHD_INFO(("GRRRRRRR: MB transaction is already pending 0x%04x\n", cur_h2d_mb_data));
		while ((i++ < 100) && cur_h2d_mb_data) {
			OSL_DELAY(10);
			dhd_bus_cmn_readshared(bus, &cur_h2d_mb_data, H2D_MB_DATA, 0);
		}
		if (i >= 100) {
			DHD_ERROR(("%s : waited 1ms for the dngl "
				"to ack the previous mb transaction\n", __FUNCTION__));
			DHD_ERROR(("%s : MB transaction is still pending 0x%04x\n",
				__FUNCTION__, cur_h2d_mb_data));
		}
	}

	dhd_bus_cmn_writeshared(bus, &h2d_mb_data, sizeof(uint32), H2D_MB_DATA, 0);
	dhd_bus_gen_devmb_intr(bus);

done:
	if (h2d_mb_data == H2D_HOST_D3_INFORM) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D3_INFORM to dongle\n", __FUNCTION__));
		bus->d3_inform_cnt++;
	}
	if (h2d_mb_data == H2D_HOST_D0_INFORM_IN_USE) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D0_INFORM_IN_USE to dongle\n", __FUNCTION__));
		bus->d0_inform_in_use_cnt++;
	}
	if (h2d_mb_data == H2D_HOST_D0_INFORM) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D0_INFORM to dongle\n", __FUNCTION__));
		bus->d0_inform_cnt++;
	}
	return BCME_OK;
fail:
	return BCME_ERROR;
}

static void
dhd_bus_handle_d3_ack(dhd_bus_t *bus)
{
	unsigned long flags_bus;
	DHD_BUS_LOCK(bus->bus_lock, flags_bus);
	bus->suspend_intr_disable_count++;
	/* Disable dongle Interrupts Immediately after D3 */

	/* For Linux, Macos etc (otherthan NDIS) along with disabling
	 * dongle interrupt by clearing the IntMask, disable directly
	 * interrupt from the host side as well. Also clear the intstatus
	 * if it is set to avoid unnecessary intrrupts after D3 ACK.
	 */
	dhdpcie_bus_intr_disable(bus); /* Disable interrupt using IntMask!! */
	dhdpcie_bus_clear_intstatus(bus);
	dhdpcie_disable_irq_nosync(bus); /* Disable host interrupt!! */

	/* Set bus_low_power_state to DHD_BUS_D3_ACK_RECIEVED */
	bus->bus_low_power_state = DHD_BUS_D3_ACK_RECIEVED;
	DHD_BUS_UNLOCK(bus->bus_lock, flags_bus);
	bus->wait_for_d3_ack = 1;
	dhd_os_d3ack_wake(bus->dhd);
}
void
dhd_bus_handle_mb_data(dhd_bus_t *bus, uint32 d2h_mb_data)
{

	DHD_INFO(("D2H_MB_DATA: 0x%04x\n", d2h_mb_data));

	if (d2h_mb_data & D2H_DEV_FWHALT)  {
		DHD_ERROR(("FW trap has happened\n"));
		dhdpcie_checkdied(bus, NULL, 0);
#ifdef SUPPORT_LINKDOWN_RECOVERY
#ifdef CONFIG_ARCH_MSM
		bus->no_cfg_restore = 1;
#endif /* CONFIG_ARCH_MSM */
#endif /* SUPPORT_LINKDOWN_RECOVERY */
		dhd_os_check_hang(bus->dhd, 0, -EREMOTEIO);
		return;
	}
	if (d2h_mb_data & D2H_DEV_DS_ENTER_REQ)  {
		bool ds_acked = FALSE;
		BCM_REFERENCE(ds_acked);
		if (bus->bus_low_power_state == DHD_BUS_D3_ACK_RECIEVED) {
			DHD_ERROR(("DS-ENTRY AFTER D3-ACK!!!!! QUITING\n"));
			bus->dhd->busstate = DHD_BUS_DOWN;
			return;
		}
		/* what should we do */
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP REQ\n"));
		{
			dhdpcie_send_mb_data(bus, H2D_HOST_DS_ACK);
			DHD_INFO(("D2H_MB_DATA: sent DEEP SLEEP ACK\n"));
		}
	}
	if (d2h_mb_data & D2H_DEV_DS_EXIT_NOTE)  {
		/* what should we do */
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP EXIT\n"));
	}
	if (d2h_mb_data & D2HMB_DS_HOST_SLEEP_EXIT_ACK)  {
		/* what should we do */
		DHD_INFO(("D2H_MB_DATA: D0 ACK\n"));
	}
	if (d2h_mb_data & D2H_DEV_D3_ACK)  {
		/* what should we do */
		DHD_INFO_HW4(("D2H_MB_DATA: D3 ACK\n"));
		if (!bus->wait_for_d3_ack) {
#if defined(DHD_HANG_SEND_UP_TEST)
			if (bus->dhd->req_hang_type == HANG_REASON_D3_ACK_TIMEOUT) {
				DHD_ERROR(("TEST HANG: Skip to process D3 ACK\n"));
			} else {
				dhd_bus_handle_d3_ack(bus);
			}
#else /* DHD_HANG_SEND_UP_TEST */
			dhd_bus_handle_d3_ack(bus);
#endif /* DHD_HANG_SEND_UP_TEST */
		}
	}
}

static void
dhdpcie_handle_mb_data(dhd_bus_t *bus)
{
	uint32 d2h_mb_data = 0;
	uint32 zero = 0;
	dhd_bus_cmn_readshared(bus, &d2h_mb_data, D2H_MB_DATA, 0);
	if (D2H_DEV_MB_INVALIDATED(d2h_mb_data)) {
		DHD_ERROR(("%s: Invalid D2H_MB_DATA: 0x%08x\n",
			__FUNCTION__, d2h_mb_data));
		return;
	}

	dhd_bus_cmn_writeshared(bus, &zero, sizeof(uint32), D2H_MB_DATA, 0);

	DHD_INFO_HW4(("D2H_MB_DATA: 0x%04x\n", d2h_mb_data));
	if (d2h_mb_data & D2H_DEV_FWHALT)  {
		DHD_ERROR(("FW trap has happened\n"));
		dhdpcie_checkdied(bus, NULL, 0);
		/* not ready yet dhd_os_ind_firmware_stall(bus->dhd); */
		return;
	}
	if (d2h_mb_data & D2H_DEV_DS_ENTER_REQ)  {
		/* what should we do */
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP REQ\n"));
		dhdpcie_send_mb_data(bus, H2D_HOST_DS_ACK);
		DHD_INFO(("D2H_MB_DATA: sent DEEP SLEEP ACK\n"));
	}
	if (d2h_mb_data & D2H_DEV_DS_EXIT_NOTE)  {
		/* what should we do */
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP EXIT\n"));
	}
	if (d2h_mb_data & D2H_DEV_D3_ACK)  {
		/* what should we do */
		DHD_INFO_HW4(("D2H_MB_DATA: D3 ACK\n"));
		if (!bus->wait_for_d3_ack) {
#if defined(DHD_HANG_SEND_UP_TEST)
			if (bus->dhd->req_hang_type == HANG_REASON_D3_ACK_TIMEOUT) {
				DHD_ERROR(("TEST HANG: Skip to process D3 ACK\n"));
			} else {
			dhd_bus_handle_d3_ack(bus);
			}
#else /* DHD_HANG_SEND_UP_TEST */
			dhd_bus_handle_d3_ack(bus);
#endif /* DHD_HANG_SEND_UP_TEST */
		}
	}
}

static void
dhdpcie_read_handle_mb_data(dhd_bus_t *bus)
{
	uint32 d2h_mb_data = 0;
	uint32 zero = 0;

	if (bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link is down\n", __FUNCTION__));
		return;
	}

	dhd_bus_cmn_readshared(bus, &d2h_mb_data, D2H_MB_DATA, 0);
	if (!d2h_mb_data)
		return;

	dhd_bus_cmn_writeshared(bus, &zero, sizeof(uint32), D2H_MB_DATA, 0);

	dhd_bus_handle_mb_data(bus, d2h_mb_data);
}

static bool
dhdpcie_bus_process_mailbox_intr(dhd_bus_t *bus, uint32 intstatus)
{
	bool resched = FALSE;

	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		/* Msg stream interrupt */
		if (intstatus & I_BIT1) {
			resched = dhdpci_bus_read_frames(bus);
		} else if (intstatus & I_BIT0) {
			/* do nothing for Now */
		}
	} else {
		if (intstatus & (PCIE_MB_TOPCIE_FN0_0 | PCIE_MB_TOPCIE_FN0_1))
			bus->api.handle_mb_data(bus);

		if ((bus->dhd->busstate == DHD_BUS_SUSPEND) || (bus->use_mailbox &&
			(bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE))) {
			DHD_ERROR(("%s: Bus is in power save state. "
				"Skip processing rest of ring buffers.\n", __FUNCTION__));
			goto exit;
		}

		/* Validate intstatus only for INTX case */
		if ((bus->d2h_intr_method == PCIE_MSI) ||
			((bus->d2h_intr_method == PCIE_INTX) && (intstatus & bus->d2h_mb_mask))) {
#ifdef DHD_PCIE_NATIVE_RUNTIMEPM
			if (pm_runtime_get(dhd_bus_to_dev(bus)) >= 0) {
				resched = dhdpci_bus_read_frames(bus);
				pm_runtime_mark_last_busy(dhd_bus_to_dev(bus));
				pm_runtime_put_autosuspend(dhd_bus_to_dev(bus));
			}
#else
			resched = dhdpci_bus_read_frames(bus);
#endif /* DHD_PCIE_NATIVE_RUNTIMEPM */
		}
	}

exit:
	return resched;
}

static bool
dhdpci_bus_read_frames(dhd_bus_t *bus)
{
	bool more = FALSE;

	/* First check if there a FW trap */
	if ((bus->api.fw_rev >= PCIE_SHARED_VERSION_6) &&
		(bus->dhd->dongle_trap_data = dhd_prot_process_trapbuf(bus->dhd))) {
		dhd_bus_handle_mb_data(bus, D2H_DEV_FWHALT);
		return FALSE;
	}

	/* There may be frames in both ctrl buf and data buf; check ctrl buf first */
	DHD_PERIM_LOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));

	dhd_prot_process_ctrlbuf(bus->dhd);
	bus->last_process_ctrlbuf_time = OSL_SYSUPTIME_US();
	/* Unlock to give chance for resp to be handled */
	DHD_PERIM_UNLOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));

	/* Do not process rest of ring buf once bus enters low power state */
	if (!bus->use_mailbox && (bus->bus_low_power_state != DHD_BUS_NO_LOW_POWER_STATE)) {
		DHD_ERROR(("%s: Bus is in power save state. "
			"Skip processing rest of ring buffers.\n", __FUNCTION__));
		return FALSE;
	}

	DHD_PERIM_LOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));
	/* update the flow ring cpls */
	dhd_update_txflowrings(bus->dhd);
	bus->last_process_flowring_time = OSL_SYSUPTIME_US();

	/* With heavy TX traffic, we could get a lot of TxStatus
	 * so add bound
	 */
	more |= dhd_prot_process_msgbuf_txcpl(bus->dhd, dhd_txbound);
	bus->last_process_txcpl_time = OSL_SYSUPTIME_US();

	/* With heavy RX traffic, this routine potentially could spend some time
	 * processing RX frames without RX bound
	 */
	more |= dhd_prot_process_msgbuf_rxcpl(bus->dhd, dhd_rxbound);
	bus->last_process_rxcpl_time = OSL_SYSUPTIME_US();

	/* Process info ring completion messages */
	more |= dhd_prot_process_msgbuf_infocpl(bus->dhd, DHD_INFORING_BOUND);
	bus->last_process_infocpl_time = OSL_SYSUPTIME_US();

#ifdef IDLE_TX_FLOW_MGMT
	if (bus->enable_idle_flowring_mgmt) {
		/* Look for idle flow rings */
		dhd_bus_check_idle_scan(bus);
	}
#endif /* IDLE_TX_FLOW_MGMT */

	/* don't talk to the dongle if fw is about to be reloaded */
	if (bus->dhd->hang_was_sent) {
		more = FALSE;
	}
	DHD_PERIM_UNLOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));

#ifdef SUPPORT_LINKDOWN_RECOVERY
	if (bus->read_shm_fail) {
		/* Read interrupt state once again to confirm linkdown */
		int intstatus = si_corereg(bus->sih, bus->sih->buscoreidx,
			bus->pcie_mailbox_int, 0, 0);
		if (intstatus != (uint32)-1) {
			DHD_ERROR(("%s: read SHM failed but intstatus is valid\n", __FUNCTION__));
#ifdef DHD_FW_COREDUMP
			if (bus->dhd->memdump_enabled) {
				DHD_OS_WAKE_LOCK(bus->dhd);
				bus->dhd->memdump_type = DUMP_TYPE_READ_SHM_FAIL;
				dhd_bus_mem_dump(bus->dhd);
				DHD_OS_WAKE_UNLOCK(bus->dhd);
			}
#endif /* DHD_FW_COREDUMP */
			bus->dhd->hang_reason = HANG_REASON_PCIE_LINK_DOWN;
			dhd_os_send_hang_message(bus->dhd);
		} else {
			DHD_ERROR(("%s: Link is Down.\n", __FUNCTION__));
#ifdef CONFIG_ARCH_MSM
			bus->no_cfg_restore = 1;
#endif /* CONFIG_ARCH_MSM */
			bus->is_linkdown = 1;
			bus->dhd->hang_reason = HANG_REASON_PCIE_LINK_DOWN;
			dhd_os_send_hang_message(bus->dhd);
		}
	}
#endif /* SUPPORT_LINKDOWN_RECOVERY */
	return more;
}

bool
dhdpcie_tcm_valid(dhd_bus_t *bus)
{
	uint32 addr = 0;
	int rv;
	uint32 shaddr = 0;
	pciedev_shared_t sh;

	shaddr = bus->dongle_ram_base + bus->ramsize - 4;

	/* Read last word in memory to determine address of pciedev_shared structure */
	addr = LTOH32(dhdpcie_bus_rtcm32(bus, shaddr));

	if ((addr == 0) || (addr == bus->nvram_csm) || (addr < bus->dongle_ram_base) ||
		(addr > shaddr)) {
		DHD_ERROR(("%s: address (0x%08x) of pciedev_shared invalid addr\n",
			__FUNCTION__, addr));
		return FALSE;
	}

	/* Read hndrte_shared structure */
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)&sh,
		sizeof(pciedev_shared_t))) < 0) {
		DHD_ERROR(("Failed to read PCIe shared struct with %d\n", rv));
		return FALSE;
	}

	/* Compare any field in pciedev_shared_t */
	if (sh.console_addr != bus->pcie_sh->console_addr) {
		DHD_ERROR(("Contents of pciedev_shared_t structure are not matching.\n"));
		return FALSE;
	}

	return TRUE;
}

static void
dhdpcie_update_bus_api_revisions(uint32 firmware_api_version, uint32 host_api_version)
{
	snprintf(bus_api_revision, BUS_API_REV_STR_LEN, "\nBus API revisions:(FW rev%d)(DHD rev%d)",
			firmware_api_version, host_api_version);
	return;
}

static bool
dhdpcie_check_firmware_compatible(uint32 firmware_api_version, uint32 host_api_version)
{
	bool retcode = FALSE;

	DHD_INFO(("firmware api revision %d, host api revision %d\n",
		firmware_api_version, host_api_version));

	switch (firmware_api_version) {
	case PCIE_SHARED_VERSION_7:
	case PCIE_SHARED_VERSION_6:
	case PCIE_SHARED_VERSION_5:
		retcode = TRUE;
		break;
	default:
		if (firmware_api_version <= host_api_version)
			retcode = TRUE;
	}
	return retcode;
}

static int
dhdpcie_readshared(dhd_bus_t *bus)
{
	uint32 addr = 0;
	int rv, dma_indx_wr_buf, dma_indx_rd_buf;
	uint32 shaddr = 0;
	pciedev_shared_t *sh = bus->pcie_sh;
	dhd_timeout_t tmo;
	bool idma_en = FALSE;

	shaddr = bus->dongle_ram_base + bus->ramsize - 4;
	/* start a timer for 5 seconds */
	dhd_timeout_start(&tmo, MAX_READ_TIMEOUT);

	while (((addr == 0) || (addr == bus->nvram_csm)) && !dhd_timeout_expired(&tmo)) {
		/* Read last word in memory to determine address of pciedev_shared structure */
		addr = LTOH32(dhdpcie_bus_rtcm32(bus, shaddr));
	}

	if ((addr == 0) || (addr == bus->nvram_csm) || (addr < bus->dongle_ram_base) ||
		(addr > shaddr)) {
		DHD_ERROR(("%s: address (0x%08x) of pciedev_shared invalid\n",
			__FUNCTION__, addr));
		DHD_ERROR(("Waited %u usec, dongle is not ready\n", tmo.elapsed));
#ifdef DEBUG_DNGL_INIT_FAIL
#ifdef CUSTOMER_HW4_DEBUG
		bus->dhd->memdump_enabled = DUMP_MEMFILE_BUGON;
#else
		dhd_get_memdump_info(bus->dhd);
#endif /* CUSTOMER_HW4_DEBUG */
		bus->dhd->memdump_type = DUMP_TYPE_DONGLE_INIT_FAILURE;
		dhdpcie_mem_dump(bus);
#endif /* DEBUG_DNGL_INIT_FAIL */
		return BCME_ERROR;
	} else {
		bus->shared_addr = (ulong)addr;
		DHD_ERROR(("PCIe shared addr (0x%08x) read took %u usec "
			"before dongle is ready\n", addr, tmo.elapsed));
	}

	/* Read hndrte_shared structure */
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)sh,
		sizeof(pciedev_shared_t))) < 0) {
		DHD_ERROR(("Failed to read PCIe shared struct with %d\n", rv));
		return rv;
	}

	/* Endianness */
	sh->flags = ltoh32(sh->flags);
	sh->trap_addr = ltoh32(sh->trap_addr);
	sh->assert_exp_addr = ltoh32(sh->assert_exp_addr);
	sh->assert_file_addr = ltoh32(sh->assert_file_addr);
	sh->assert_line = ltoh32(sh->assert_line);
	sh->console_addr = ltoh32(sh->console_addr);
	sh->msgtrace_addr = ltoh32(sh->msgtrace_addr);
	sh->dma_rxoffset = ltoh32(sh->dma_rxoffset);
	sh->rings_info_ptr = ltoh32(sh->rings_info_ptr);
	sh->flags2 = ltoh32(sh->flags2);

	/* load bus console address */
	bus->console_addr = sh->console_addr;

	/* Read the dma rx offset */
	bus->dma_rxoffset = bus->pcie_sh->dma_rxoffset;
	dhd_prot_rx_dataoffset(bus->dhd, bus->dma_rxoffset);

	DHD_INFO(("DMA RX offset from shared Area %d\n", bus->dma_rxoffset));

	bus->api.fw_rev = sh->flags & PCIE_SHARED_VERSION_MASK;
	if (!(dhdpcie_check_firmware_compatible(bus->api.fw_rev, PCIE_SHARED_VERSION)))
	{
		DHD_ERROR(("%s: pcie_shared version %d in dhd "
		           "is older than pciedev_shared version %d in dongle\n",
		           __FUNCTION__, PCIE_SHARED_VERSION,
		           bus->api.fw_rev));
		return BCME_ERROR;
	}
	dhdpcie_update_bus_api_revisions(bus->api.fw_rev, PCIE_SHARED_VERSION);

	bus->rw_index_sz = (sh->flags & PCIE_SHARED_2BYTE_INDICES) ?
		sizeof(uint16) : sizeof(uint32);
	DHD_INFO(("%s: Dongle advertizes %d size indices\n",
		__FUNCTION__, bus->rw_index_sz));

#ifdef IDLE_TX_FLOW_MGMT
	if (sh->flags & PCIE_SHARED_IDLE_FLOW_RING) {
		DHD_ERROR(("%s: FW Supports IdleFlow ring managment!\n",
			__FUNCTION__));
		bus->enable_idle_flowring_mgmt = TRUE;
	}
#endif /* IDLE_TX_FLOW_MGMT */

	if (IDMA_CAPABLE(bus)) {
		if (bus->sih->buscorerev == 23) {
		} else {
			idma_en = TRUE;
		}
	}

	if (idma_en) {
		bus->dhd->idma_enable = (sh->flags & PCIE_SHARED_IDMA) ? TRUE : FALSE;
		bus->dhd->ifrm_enable = (sh->flags & PCIE_SHARED_IFRM) ? TRUE : FALSE;
	}

	bus->dhd->d2h_sync_mode = sh->flags & PCIE_SHARED_D2H_SYNC_MODE_MASK;

	bus->dhd->dar_enable = (sh->flags & PCIE_SHARED_DAR) ? TRUE : FALSE;

	/* Does the FW support DMA'ing r/w indices */
	if (sh->flags & PCIE_SHARED_DMA_INDEX) {
		if (!bus->dhd->dma_ring_upd_overwrite) {
			{
				if (!IFRM_ENAB(bus->dhd)) {
					bus->dhd->dma_h2d_ring_upd_support = TRUE;
				}
				bus->dhd->dma_d2h_ring_upd_support = TRUE;
			}
		}

		if (bus->dhd->dma_d2h_ring_upd_support)
			bus->dhd->d2h_sync_mode = 0;

		DHD_INFO(("%s: Host support DMAing indices: H2D:%d - D2H:%d. FW supports it\n",
			__FUNCTION__,
			(bus->dhd->dma_h2d_ring_upd_support ? 1 : 0),
			(bus->dhd->dma_d2h_ring_upd_support ? 1 : 0)));
	} else if (!(sh->flags & PCIE_SHARED_D2H_SYNC_MODE_MASK)) {
		DHD_ERROR(("%s FW has to support either dma indices or d2h sync\n",
			__FUNCTION__));
		return BCME_UNSUPPORTED;
	} else {
		bus->dhd->dma_h2d_ring_upd_support = FALSE;
		bus->dhd->dma_d2h_ring_upd_support = FALSE;
	}

	/* Does the firmware support fast delete ring? */
	if (sh->flags2 & PCIE_SHARED2_FAST_DELETE_RING) {
		DHD_INFO(("%s: Firmware supports fast delete ring\n",
			__FUNCTION__));
		bus->dhd->fast_delete_ring_support = TRUE;
	} else {
		DHD_INFO(("%s: Firmware does not support fast delete ring\n",
			__FUNCTION__));
		bus->dhd->fast_delete_ring_support = FALSE;
	}

	/* get ring_info, ring_state and mb data ptrs and store the addresses in bus structure */
	{
		ring_info_t  ring_info;

		if ((rv = dhdpcie_bus_membytes(bus, FALSE, sh->rings_info_ptr,
			(uint8 *)&ring_info, sizeof(ring_info_t))) < 0)
			return rv;

		bus->h2d_mb_data_ptr_addr = ltoh32(sh->h2d_mb_data_ptr);
		bus->d2h_mb_data_ptr_addr = ltoh32(sh->d2h_mb_data_ptr);

		if (bus->api.fw_rev >= PCIE_SHARED_VERSION_6) {
			bus->max_tx_flowrings = ltoh16(ring_info.max_tx_flowrings);
			bus->max_submission_rings = ltoh16(ring_info.max_submission_queues);
			bus->max_completion_rings = ltoh16(ring_info.max_completion_rings);
			bus->max_cmn_rings = bus->max_submission_rings - bus->max_tx_flowrings;
			bus->api.handle_mb_data = dhdpcie_read_handle_mb_data;
			bus->use_mailbox = sh->flags & PCIE_SHARED_USE_MAILBOX;
		}
		else {
			bus->max_tx_flowrings = ltoh16(ring_info.max_tx_flowrings);
			bus->max_submission_rings = bus->max_tx_flowrings;
			bus->max_completion_rings = BCMPCIE_D2H_COMMON_MSGRINGS;
			bus->max_cmn_rings = BCMPCIE_H2D_COMMON_MSGRINGS;
			bus->api.handle_mb_data = dhdpcie_handle_mb_data;
			bus->use_mailbox = TRUE;
		}
		if (bus->max_completion_rings == 0) {
			DHD_ERROR(("dongle completion rings are invalid %d\n",
				bus->max_completion_rings));
			return BCME_ERROR;
		}
		if (bus->max_submission_rings == 0) {
			DHD_ERROR(("dongle submission rings are invalid %d\n",
				bus->max_submission_rings));
			return BCME_ERROR;
		}
		if (bus->max_tx_flowrings == 0) {
			DHD_ERROR(("dongle txflow rings are invalid %d\n", bus->max_tx_flowrings));
			return BCME_ERROR;
		}

		/* If both FW and Host support DMA'ing indices, allocate memory and notify FW
		 * The max_sub_queues is read from FW initialized ring_info
		 */
		if (bus->dhd->dma_h2d_ring_upd_support || IDMA_ENAB(bus->dhd)) {
			dma_indx_wr_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				H2D_DMA_INDX_WR_BUF, bus->max_submission_rings);
			dma_indx_rd_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				D2H_DMA_INDX_RD_BUF, bus->max_completion_rings);

			if ((dma_indx_wr_buf != BCME_OK) || (dma_indx_rd_buf != BCME_OK)) {
				DHD_ERROR(("%s: Failed to allocate memory for dma'ing h2d indices"
						"Host will use w/r indices in TCM\n",
						__FUNCTION__));
				bus->dhd->dma_h2d_ring_upd_support = FALSE;
				bus->dhd->idma_enable = FALSE;
			}
		}

		if (bus->dhd->dma_d2h_ring_upd_support) {
			dma_indx_wr_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				D2H_DMA_INDX_WR_BUF, bus->max_completion_rings);
			dma_indx_rd_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				H2D_DMA_INDX_RD_BUF, bus->max_submission_rings);

			if ((dma_indx_wr_buf != BCME_OK) || (dma_indx_rd_buf != BCME_OK)) {
				DHD_ERROR(("%s: Failed to allocate memory for dma'ing d2h indices"
						"Host will use w/r indices in TCM\n",
						__FUNCTION__));
				bus->dhd->dma_d2h_ring_upd_support = FALSE;
			}
		}

		if (IFRM_ENAB(bus->dhd)) {
			dma_indx_wr_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				H2D_IFRM_INDX_WR_BUF, bus->max_tx_flowrings);

			if (dma_indx_wr_buf != BCME_OK) {
				DHD_ERROR(("%s: Failed to alloc memory for Implicit DMA\n",
						__FUNCTION__));
				bus->dhd->ifrm_enable = FALSE;
			}
		}

		/* read ringmem and ringstate ptrs from shared area and store in host variables */
		dhd_fillup_ring_sharedptr_info(bus, &ring_info);
		if (dhd_msg_level & DHD_INFO_VAL) {
			bcm_print_bytes("ring_info_raw", (uchar *)&ring_info, sizeof(ring_info_t));
		}
		DHD_INFO(("ring_info\n"));

		DHD_ERROR(("%s: max H2D queues %d\n",
			__FUNCTION__, ltoh16(ring_info.max_tx_flowrings)));

		DHD_INFO(("mail box address\n"));
		DHD_INFO(("%s: h2d_mb_data_ptr_addr 0x%04x\n",
			__FUNCTION__, bus->h2d_mb_data_ptr_addr));
		DHD_INFO(("%s: d2h_mb_data_ptr_addr 0x%04x\n",
			__FUNCTION__, bus->d2h_mb_data_ptr_addr));
	}

	DHD_INFO(("%s: d2h_sync_mode 0x%08x\n",
		__FUNCTION__, bus->dhd->d2h_sync_mode));

	bus->dhd->d2h_hostrdy_supported =
		((sh->flags & PCIE_SHARED_HOSTRDY_SUPPORT) == PCIE_SHARED_HOSTRDY_SUPPORT);

	bus->dhd->ext_trap_data_supported =
		((sh->flags2 & PCIE_SHARED2_EXTENDED_TRAP_DATA) == PCIE_SHARED2_EXTENDED_TRAP_DATA);

	if ((sh->flags2 & PCIE_SHARED2_TXSTATUS_METADATA) == 0)
		bus->dhd->pcie_txs_metadata_enable = 0;

	return BCME_OK;
} /* dhdpcie_readshared */

/** Read ring mem and ring state ptr info from shared memory area in device memory */
static void
dhd_fillup_ring_sharedptr_info(dhd_bus_t *bus, ring_info_t *ring_info)
{
	uint16 i = 0;
	uint16 j = 0;
	uint32 tcm_memloc;
	uint32	d2h_w_idx_ptr, d2h_r_idx_ptr, h2d_w_idx_ptr, h2d_r_idx_ptr;
	uint16  max_tx_flowrings = bus->max_tx_flowrings;

	/* Ring mem ptr info */
	/* Alloated in the order
		H2D_MSGRING_CONTROL_SUBMIT              0
		H2D_MSGRING_RXPOST_SUBMIT               1
		D2H_MSGRING_CONTROL_COMPLETE            2
		D2H_MSGRING_TX_COMPLETE                 3
		D2H_MSGRING_RX_COMPLETE                 4
	*/

	{
		/* ringmemptr holds start of the mem block address space */
		tcm_memloc = ltoh32(ring_info->ringmem_ptr);

		/* Find out ringmem ptr for each ring common  ring */
		for (i = 0; i <= BCMPCIE_COMMON_MSGRING_MAX_ID; i++) {
			bus->ring_sh[i].ring_mem_addr = tcm_memloc;
			/* Update mem block */
			tcm_memloc = tcm_memloc + sizeof(ring_mem_t);
			DHD_INFO(("ring id %d ring mem addr 0x%04x \n",
				i, bus->ring_sh[i].ring_mem_addr));
		}
	}

	/* Ring state mem ptr info */
	{
		d2h_w_idx_ptr = ltoh32(ring_info->d2h_w_idx_ptr);
		d2h_r_idx_ptr = ltoh32(ring_info->d2h_r_idx_ptr);
		h2d_w_idx_ptr = ltoh32(ring_info->h2d_w_idx_ptr);
		h2d_r_idx_ptr = ltoh32(ring_info->h2d_r_idx_ptr);

		/* Store h2d common ring write/read pointers */
		for (i = 0; i < BCMPCIE_H2D_COMMON_MSGRINGS; i++) {
			bus->ring_sh[i].ring_state_w = h2d_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = h2d_r_idx_ptr;

			/* update mem block */
			h2d_w_idx_ptr = h2d_w_idx_ptr + bus->rw_index_sz;
			h2d_r_idx_ptr = h2d_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("h2d w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w, bus->ring_sh[i].ring_state_r));
		}

		/* Store d2h common ring write/read pointers */
		for (j = 0; j < BCMPCIE_D2H_COMMON_MSGRINGS; j++, i++) {
			bus->ring_sh[i].ring_state_w = d2h_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = d2h_r_idx_ptr;

			/* update mem block */
			d2h_w_idx_ptr = d2h_w_idx_ptr + bus->rw_index_sz;
			d2h_r_idx_ptr = d2h_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("d2h w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w, bus->ring_sh[i].ring_state_r));
		}

		/* Store txflow ring write/read pointers */
		if (bus->api.fw_rev < PCIE_SHARED_VERSION_6) {
			max_tx_flowrings -= BCMPCIE_H2D_COMMON_MSGRINGS;
		} else {
			/* Account for Debug info h2d ring located after the last tx flow ring */
			max_tx_flowrings = max_tx_flowrings + 1;
		}
		for (j = 0; j < max_tx_flowrings; i++, j++)
		{
			bus->ring_sh[i].ring_state_w = h2d_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = h2d_r_idx_ptr;

			/* update mem block */
			h2d_w_idx_ptr = h2d_w_idx_ptr + bus->rw_index_sz;
			h2d_r_idx_ptr = h2d_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("FLOW Rings h2d w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w,
				bus->ring_sh[i].ring_state_r));
		}
		/* store wr/rd pointers for  debug info completion ring */
		bus->ring_sh[i].ring_state_w = d2h_w_idx_ptr;
		bus->ring_sh[i].ring_state_r = d2h_r_idx_ptr;
		d2h_w_idx_ptr = d2h_w_idx_ptr + bus->rw_index_sz;
		d2h_r_idx_ptr = d2h_r_idx_ptr + bus->rw_index_sz;
		DHD_INFO(("d2h w/r : idx %d write %x read %x \n", i,
			bus->ring_sh[i].ring_state_w, bus->ring_sh[i].ring_state_r));
	}
} /* dhd_fillup_ring_sharedptr_info */

/**
 * Initialize bus module: prepare for communication with the dongle. Called after downloading
 * firmware into the dongle.
 */
int dhd_bus_init(dhd_pub_t *dhdp, bool enforce_mutex)
{
	dhd_bus_t *bus = dhdp->bus;
	int  ret = 0;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(bus->dhd);
	if (!bus->dhd)
		return 0;

	/* Make sure we're talking to the core. */
	bus->reg = si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	ASSERT(bus->reg != NULL);

	/* before opening up bus for data transfer, check if shared are is intact */
	ret = dhdpcie_readshared(bus);
	if (ret < 0) {
		DHD_ERROR(("%s :Shared area read failed \n", __FUNCTION__));
		return ret;
	}

	/* Make sure we're talking to the core. */
	bus->reg = si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	ASSERT(bus->reg != NULL);

	dhd_init_bus_lock(bus);

	/* Set bus state according to enable result */
	dhdp->busstate = DHD_BUS_DATA;
	bus->bus_low_power_state = DHD_BUS_NO_LOW_POWER_STATE;
	dhdp->dhd_bus_busy_state = 0;

	/* D11 status via PCIe completion header */
	if ((ret = dhdpcie_init_d11status(bus)) < 0) {
		return ret;
	}

	if (!dhd_download_fw_on_driverload)
		dhd_dpc_enable(bus->dhd);
	/* Enable the interrupt after device is up */
	dhdpcie_bus_intr_enable(bus);

	/* bcmsdh_intr_unmask(bus->sdh); */
#ifdef DHD_PCIE_RUNTIMEPM
	bus->idlecount = 0;
	bus->idletime = (int32)MAX_IDLE_COUNT;
	init_waitqueue_head(&bus->rpm_queue);
	mutex_init(&bus->pm_lock);
#else
	bus->idletime = 0;
#endif /* DHD_PCIE_RUNTIMEPM */

	/* Make use_d0_inform TRUE for Rev 5 for backward compatibility */
	if (bus->api.fw_rev < PCIE_SHARED_VERSION_6) {
		bus->use_d0_inform = TRUE;
	} else {
		bus->use_d0_inform = FALSE;
	}

	return ret;
}

static void
dhdpcie_init_shared_addr(dhd_bus_t *bus)
{
	uint32 addr = 0;
	uint32 val = 0;
	addr = bus->dongle_ram_base + bus->ramsize - 4;
#ifdef DHD_PCIE_RUNTIMEPM
	dhdpcie_runtime_bus_wake(bus->dhd, TRUE, __builtin_return_address(0));
#endif /* DHD_PCIE_RUNTIMEPM */
	dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val));
}

bool
dhdpcie_chipmatch(uint16 vendor, uint16 device)
{
	if (vendor != PCI_VENDOR_ID_BROADCOM) {
		DHD_ERROR(("%s: Unsupported vendor %x device %x\n", __FUNCTION__,
			vendor, device));
		return (-ENODEV);
	}

	if ((device == BCM4350_D11AC_ID) || (device == BCM4350_D11AC2G_ID) ||
		(device == BCM4350_D11AC5G_ID) || (device == BCM4350_CHIP_ID) ||
		(device == BCM43569_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4354_D11AC_ID) || (device == BCM4354_D11AC2G_ID) ||
		(device == BCM4354_D11AC5G_ID) || (device == BCM4354_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4356_D11AC_ID) || (device == BCM4356_D11AC2G_ID) ||
		(device == BCM4356_D11AC5G_ID) || (device == BCM4356_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4371_D11AC_ID) || (device == BCM4371_D11AC2G_ID) ||
		(device == BCM4371_D11AC5G_ID) || (device == BCM4371_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4345_D11AC_ID) || (device == BCM4345_D11AC2G_ID) ||
		(device == BCM4345_D11AC5G_ID) || BCM4345_CHIP(device)) {
		return 0;
	}

	if ((device == BCM43452_D11AC_ID) || (device == BCM43452_D11AC2G_ID) ||
		(device == BCM43452_D11AC5G_ID)) {
		return 0;
	}

	if ((device == BCM4335_D11AC_ID) || (device == BCM4335_D11AC2G_ID) ||
		(device == BCM4335_D11AC5G_ID) || (device == BCM4335_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM43602_D11AC_ID) || (device == BCM43602_D11AC2G_ID) ||
		(device == BCM43602_D11AC5G_ID) || (device == BCM43602_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM43569_D11AC_ID) || (device == BCM43569_D11AC2G_ID) ||
		(device == BCM43569_D11AC5G_ID) || (device == BCM43569_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4358_D11AC_ID) || (device == BCM4358_D11AC2G_ID) ||
		(device == BCM4358_D11AC5G_ID)) {
		return 0;
	}

	if ((device == BCM4349_D11AC_ID) || (device == BCM4349_D11AC2G_ID) ||
		(device == BCM4349_D11AC5G_ID) || (device == BCM4349_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4355_D11AC_ID) || (device == BCM4355_D11AC2G_ID) ||
		(device == BCM4355_D11AC5G_ID) || (device == BCM4355_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4359_D11AC_ID) || (device == BCM4359_D11AC2G_ID) ||
		(device == BCM4359_D11AC5G_ID)) {
		return 0;
	}

	if ((device == BCM43596_D11AC_ID) || (device == BCM43596_D11AC2G_ID) ||
		(device == BCM43596_D11AC5G_ID)) {
		return 0;
	}

	if ((device == BCM43597_D11AC_ID) || (device == BCM43597_D11AC2G_ID) ||
		(device == BCM43597_D11AC5G_ID)) {
		return 0;
	}

	if ((device == BCM4364_D11AC_ID) || (device == BCM4364_D11AC2G_ID) ||
		(device == BCM4364_D11AC5G_ID) || (device == BCM4364_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4361_D11AC_ID) || (device == BCM4361_D11AC2G_ID) ||
		(device == BCM4361_D11AC5G_ID) || (device == BCM4361_CHIP_ID)) {
		return 0;
	}
	if ((device == BCM4347_D11AC_ID) || (device == BCM4347_D11AC2G_ID) ||
		(device == BCM4347_D11AC5G_ID) || (device == BCM4347_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4365_D11AC_ID) || (device == BCM4365_D11AC2G_ID) ||
		(device == BCM4365_D11AC5G_ID) || (device == BCM4365_CHIP_ID)) {
		return 0;
	}

	if ((device == BCM4366_D11AC_ID) || (device == BCM4366_D11AC2G_ID) ||
		(device == BCM4366_D11AC5G_ID) || (device == BCM4366_CHIP_ID) ||
		(device == BCM43664_CHIP_ID) || (device == BCM43666_CHIP_ID)) {
		return 0;
	}
	DHD_ERROR(("%s: Unsupported vendor %x device %x\n", __FUNCTION__, vendor, device));
	return (-ENODEV);
} /* dhdpcie_chipmatch */

/**
 * Name:  dhdpcie_cc_nvmshadow
 *
 * Description:
 * A shadow of OTP/SPROM exists in ChipCommon Region
 * betw. 0x800 and 0xBFF (Backplane Addr. 0x1800_0800 and 0x1800_0BFF).
 * Strapping option (SPROM vs. OTP), presence of OTP/SPROM and its size
 * can also be read from ChipCommon Registers.
 */
static int
dhdpcie_cc_nvmshadow(dhd_bus_t *bus, struct bcmstrbuf *b)
{
	uint16 dump_offset = 0;
	uint32 dump_size = 0, otp_size = 0, sprom_size = 0;

	/* Table for 65nm OTP Size (in bits) */
	int  otp_size_65nm[8] = {0, 2048, 4096, 8192, 4096, 6144, 512, 1024};

	volatile uint16 *nvm_shadow;

	uint cur_coreid;
	uint chipc_corerev;
	chipcregs_t *chipcregs;

	/* Save the current core */
	cur_coreid = si_coreid(bus->sih);
	/* Switch to ChipC */
	chipcregs = (chipcregs_t *)si_setcore(bus->sih, CC_CORE_ID, 0);
	ASSERT(chipcregs != NULL);

	chipc_corerev = si_corerev(bus->sih);

	/* Check ChipcommonCore Rev */
	if (chipc_corerev < 44) {
		DHD_ERROR(("%s: ChipcommonCore Rev %d < 44\n", __FUNCTION__, chipc_corerev));
		return BCME_UNSUPPORTED;
	}

	/* Check ChipID */
	if (((uint16)bus->sih->chip != BCM4350_CHIP_ID) && !BCM4345_CHIP((uint16)bus->sih->chip) &&
	        ((uint16)bus->sih->chip != BCM4355_CHIP_ID) &&
	        ((uint16)bus->sih->chip != BCM4364_CHIP_ID)) {
		DHD_ERROR(("%s: cc_nvmdump cmd. supported for Olympic chips"
					"4350/4345/4355/4364 only\n", __FUNCTION__));
		return BCME_UNSUPPORTED;
	}

	/* Check if SRC_PRESENT in SpromCtrl(0x190 in ChipCommon Regs) is set */
	if (chipcregs->sromcontrol & SRC_PRESENT) {
		/* SPROM Size: 1Kbits (0x0), 4Kbits (0x1), 16Kbits(0x2) */
		sprom_size = (1 << (2 * ((chipcregs->sromcontrol & SRC_SIZE_MASK)
					>> SRC_SIZE_SHIFT))) * 1024;
		bcm_bprintf(b, "\nSPROM Present (Size %d bits)\n", sprom_size);
	}

	if (chipcregs->sromcontrol & SRC_OTPPRESENT) {
		bcm_bprintf(b, "\nOTP Present");

		if (((chipcregs->otplayout & OTPL_WRAP_TYPE_MASK) >> OTPL_WRAP_TYPE_SHIFT)
			== OTPL_WRAP_TYPE_40NM) {
			/* 40nm OTP: Size = (OtpSize + 1) * 1024 bits */
			/* Chipcommon rev51 is a variation on rev45 and does not support
			 * the latest OTP configuration.
			 */
			if (chipc_corerev != 51 && chipc_corerev >= 49) {
				otp_size = (((chipcregs->otplayout & OTPL_ROW_SIZE_MASK)
					>> OTPL_ROW_SIZE_SHIFT) + 1) * 1024;
				bcm_bprintf(b, "(Size %d bits)\n", otp_size);
			} else {
				otp_size =  (((chipcregs->capabilities & CC_CAP_OTPSIZE)
				        >> CC_CAP_OTPSIZE_SHIFT) + 1) * 1024;
				bcm_bprintf(b, "(Size %d bits)\n", otp_size);
			}
		} else {
			/* This part is untested since newer chips have 40nm OTP */
			/* Chipcommon rev51 is a variation on rev45 and does not support
			 * the latest OTP configuration.
			 */
			if (chipc_corerev != 51 && chipc_corerev >= 49) {
				otp_size = otp_size_65nm[(chipcregs->otplayout & OTPL_ROW_SIZE_MASK)
						>> OTPL_ROW_SIZE_SHIFT];
				bcm_bprintf(b, "(Size %d bits)\n", otp_size);
			} else {
				otp_size = otp_size_65nm[(chipcregs->capabilities & CC_CAP_OTPSIZE)
					        >> CC_CAP_OTPSIZE_SHIFT];
				bcm_bprintf(b, "(Size %d bits)\n", otp_size);
				DHD_INFO(("%s: 65nm/130nm OTP Size not tested. \n",
					__FUNCTION__));
			}
		}
	}

	/* Chipcommon rev51 is a variation on rev45 and does not support
	 * the latest OTP configuration.
	 */
	if (chipc_corerev != 51 && chipc_corerev >= 49) {
		if (((chipcregs->sromcontrol & SRC_PRESENT) == 0) &&
			((chipcregs->otplayout & OTPL_ROW_SIZE_MASK) == 0)) {
			DHD_ERROR(("%s: SPROM and OTP could not be found "
				"sromcontrol = %x, otplayout = %x \n",
				__FUNCTION__, chipcregs->sromcontrol, chipcregs->otplayout));
			return BCME_NOTFOUND;
		}
	} else {
		if (((chipcregs->sromcontrol & SRC_PRESENT) == 0) &&
			((chipcregs->capabilities & CC_CAP_OTPSIZE) == 0)) {
			DHD_ERROR(("%s: SPROM and OTP could not be found "
				"sromcontrol = %x, capablities = %x \n",
				__FUNCTION__, chipcregs->sromcontrol, chipcregs->capabilities));
			return BCME_NOTFOUND;
		}
	}

	/* Check the strapping option in SpromCtrl: Set = OTP otherwise SPROM */
	if ((!(chipcregs->sromcontrol & SRC_PRESENT) || (chipcregs->sromcontrol & SRC_OTPSEL)) &&
		(chipcregs->sromcontrol & SRC_OTPPRESENT)) {

		bcm_bprintf(b, "OTP Strap selected.\n"
		               "\nOTP Shadow in ChipCommon:\n");

		dump_size = otp_size / 16 ; /* 16bit words */

	} else if (((chipcregs->sromcontrol & SRC_OTPSEL) == 0) &&
		(chipcregs->sromcontrol & SRC_PRESENT)) {

		bcm_bprintf(b, "SPROM Strap selected\n"
				"\nSPROM Shadow in ChipCommon:\n");

		/* If SPROM > 8K only 8Kbits is mapped to ChipCommon (0x800 - 0xBFF) */
		/* dump_size in 16bit words */
		dump_size = sprom_size > 8 ? (8 * 1024) / 16 : sprom_size / 16;
	} else {
		DHD_ERROR(("%s: NVM Shadow does not exist in ChipCommon\n",
			__FUNCTION__));
		return BCME_NOTFOUND;
	}

	if (bus->regs == NULL) {
		DHD_ERROR(("ChipCommon Regs. not initialized\n"));
		return BCME_NOTREADY;
	} else {
		bcm_bprintf(b, "\n OffSet:");

		/* Chipcommon rev51 is a variation on rev45 and does not support
		 * the latest OTP configuration.
		 */
		if (chipc_corerev != 51 && chipc_corerev >= 49) {
			/* Chip common can read only 8kbits,
			* for ccrev >= 49 otp size is around 12 kbits so use GCI core
			*/
			nvm_shadow = (volatile uint16 *)si_setcore(bus->sih, GCI_CORE_ID, 0);
		} else {
			/* Point to the SPROM/OTP shadow in ChipCommon */
			nvm_shadow = chipcregs->sromotp;
		}

		if (nvm_shadow == NULL) {
			DHD_ERROR(("%s: NVM Shadow is not intialized\n", __FUNCTION__));
			return BCME_NOTFOUND;
		}

		/*
		* Read 16 bits / iteration.
		* dump_size & dump_offset in 16-bit words
		*/
		while (dump_offset < dump_size) {
			if (dump_offset % 2 == 0)
				/* Print the offset in the shadow space in Bytes */
				bcm_bprintf(b, "\n 0x%04x", dump_offset * 2);

			bcm_bprintf(b, "\t0x%04x", *(nvm_shadow + dump_offset));
			dump_offset += 0x1;
		}
	}

	/* Switch back to the original core */
	si_setcore(bus->sih, cur_coreid, 0);

	return BCME_OK;
} /* dhdpcie_cc_nvmshadow */

/** Flow rings are dynamically created and destroyed */
void dhd_bus_clean_flow_ring(dhd_bus_t *bus, void *node)
{
	void *pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node = (flow_ring_node_t *)node;
	unsigned long flags;

	queue = &flow_ring_node->queue;

#ifdef DHDTCPACK_SUPPRESS
	/* Clean tcp_ack_info_tbl in order to prevent access to flushed pkt,
	 * when there is a newly coming packet from network stack.
	 */
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif /* DHDTCPACK_SUPPRESS */

	/* clean up BUS level info */
	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

	/* Flush all pending packets in the queue, if any */
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	/* Reinitialise flowring's queue */
	dhd_flow_queue_reinit(bus->dhd, queue, FLOW_RING_QUEUE_THRESHOLD);
	flow_ring_node->status = FLOW_RING_STATUS_CLOSED;
	flow_ring_node->active = FALSE;

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	/* Hold flowring_list_lock to ensure no race condition while accessing the List */
	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	dll_delete(&flow_ring_node->list);
	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	/* Release the flowring object back into the pool */
	dhd_prot_flowrings_pool_release(bus->dhd,
		flow_ring_node->flowid, flow_ring_node->prot_info);

	/* Free the flowid back to the flowid allocator */
	dhd_flowid_free(bus->dhd, flow_ring_node->flow_info.ifindex,
	                flow_ring_node->flowid);
}

/**
 * Allocate a Flow ring buffer,
 * Init Ring buffer, send Msg to device about flow ring creation
*/
int
dhd_bus_flow_ring_create_request(dhd_bus_t *bus, void *arg)
{
	flow_ring_node_t *flow_ring_node = (flow_ring_node_t *)arg;

	DHD_INFO(("%s :Flow create\n", __FUNCTION__));

	/* Send Msg to device about flow ring creation */
	if (dhd_prot_flow_ring_create(bus->dhd, flow_ring_node) != BCME_OK)
		return BCME_NOMEM;

	return BCME_OK;
}

/** Handle response from dongle on a 'flow ring create' request */
void
dhd_bus_flow_ring_create_response(dhd_bus_t *bus, uint16 flowid, int32 status)
{
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Response %d \n", __FUNCTION__, flowid));

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow create Response failure error status = %d \n",
		     __FUNCTION__, status));
		/* Call Flow clean up */
		dhd_bus_clean_flow_ring(bus, flow_ring_node);
		return;
	}

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	flow_ring_node->status = FLOW_RING_STATUS_OPEN;
	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	/* Now add the Flow ring node into the active list
	 * Note that this code to add the newly created node to the active
	 * list was living in dhd_flowid_lookup. But note that after
	 * adding the node to the active list the contents of node is being
	 * filled in dhd_prot_flow_ring_create.
	 * If there is a D2H interrupt after the node gets added to the
	 * active list and before the node gets populated with values
	 * from the Bottom half dhd_update_txflowrings would be called.
	 * which will then try to walk through the active flow ring list,
	 * pickup the nodes and operate on them. Now note that since
	 * the function dhd_prot_flow_ring_create is not finished yet
	 * the contents of flow_ring_node can still be NULL leading to
	 * crashes. Hence the flow_ring_node should be added to the
	 * active list only after its truely created, which is after
	 * receiving the create response message from the Host.
	 */
	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	dll_prepend(&bus->flowring_active_list, &flow_ring_node->list);
	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	dhd_bus_schedule_queue(bus, flowid, FALSE); /* from queue to flowring */

	return;
}

int
dhd_bus_flow_ring_delete_request(dhd_bus_t *bus, void *arg)
{
	void * pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Delete\n", __FUNCTION__));

	flow_ring_node = (flow_ring_node_t *)arg;

#ifdef DHDTCPACK_SUPPRESS
	/* Clean tcp_ack_info_tbl in order to prevent access to flushed pkt,
	 * when there is a newly coming packet from network stack.
	 */
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif /* DHDTCPACK_SUPPRESS */
	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	if (flow_ring_node->status == FLOW_RING_STATUS_DELETE_PENDING) {
		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
		DHD_ERROR(("%s :Delete Pending flowid %u\n", __FUNCTION__, flow_ring_node->flowid));
		return BCME_ERROR;
	}
	flow_ring_node->status = FLOW_RING_STATUS_DELETE_PENDING;

	queue = &flow_ring_node->queue; /* queue associated with flow ring */

	/* Flush all pending packets in the queue, if any */
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	/* Send Msg to device about flow ring deletion */
	dhd_prot_flow_ring_delete(bus->dhd, flow_ring_node);

	return BCME_OK;
}

void
dhd_bus_flow_ring_delete_response(dhd_bus_t *bus, uint16 flowid, uint32 status)
{
	flow_ring_node_t *flow_ring_node;

	DHD_INFO(("%s :Flow Delete Response %d \n", __FUNCTION__, flowid));

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow Delete Response failure error status = %d \n",
		    __FUNCTION__, status));
		return;
	}
	/* Call Flow clean up */
	dhd_bus_clean_flow_ring(bus, flow_ring_node);

	return;

}

int dhd_bus_flow_ring_flush_request(dhd_bus_t *bus, void *arg)
{
	void *pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Flush\n", __FUNCTION__));

	flow_ring_node = (flow_ring_node_t *)arg;

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	queue = &flow_ring_node->queue; /* queue associated with flow ring */
	/* Flow ring status will be set back to FLOW_RING_STATUS_OPEN
	 * once flow ring flush response is received for this flowring node.
	 */
	flow_ring_node->status = FLOW_RING_STATUS_FLUSH_PENDING;

#ifdef DHDTCPACK_SUPPRESS
	/* Clean tcp_ack_info_tbl in order to prevent access to flushed pkt,
	 * when there is a newly coming packet from network stack.
	 */
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif /* DHDTCPACK_SUPPRESS */

	/* Flush all pending packets in the queue, if any */
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	/* Send Msg to device about flow ring flush */
	dhd_prot_flow_ring_flush(bus->dhd, flow_ring_node);

	return BCME_OK;
}

void
dhd_bus_flow_ring_flush_response(dhd_bus_t *bus, uint16 flowid, uint32 status)
{
	flow_ring_node_t *flow_ring_node;

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow flush Response failure error status = %d \n",
		    __FUNCTION__, status));
		return;
	}

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	flow_ring_node->status = FLOW_RING_STATUS_OPEN;
	return;
}

uint32
dhd_bus_max_h2d_queues(struct dhd_bus *bus)
{
	return bus->max_submission_rings;
}

/* To be symmetric with SDIO */
void
dhd_bus_pktq_flush(dhd_pub_t *dhdp)
{
	return;
}

void
dhd_bus_set_linkdown(dhd_pub_t *dhdp, bool val)
{
	dhdp->bus->is_linkdown = val;
}

#ifdef IDLE_TX_FLOW_MGMT
/* resume request */
int
dhd_bus_flow_ring_resume_request(dhd_bus_t *bus, void *arg)
{
	flow_ring_node_t *flow_ring_node = (flow_ring_node_t *)arg;

	DHD_ERROR(("%s :Flow Resume Request flow id %u\n", __FUNCTION__, flow_ring_node->flowid));

	flow_ring_node->status = FLOW_RING_STATUS_RESUME_PENDING;

	/* Send Msg to device about flow ring resume */
	dhd_prot_flow_ring_resume(bus->dhd, flow_ring_node);

	return BCME_OK;
}

/* add the node back to active flowring */
void
dhd_bus_flow_ring_resume_response(dhd_bus_t *bus, uint16 flowid, int32 status)
{

	flow_ring_node_t *flow_ring_node;

	DHD_TRACE(("%s :flowid %d \n", __FUNCTION__, flowid));

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	if (status != BCME_OK) {
		DHD_ERROR(("%s Error Status = %d \n",
			__FUNCTION__, status));
		return;
	}

	DHD_TRACE(("%s :Number of pkts queued in FlowId:%d is -> %u!!\n",
		__FUNCTION__, flow_ring_node->flowid,  flow_ring_node->queue.len));

	flow_ring_node->status = FLOW_RING_STATUS_OPEN;

	dhd_bus_schedule_queue(bus, flowid, FALSE);
	return;
}

/* scan the flow rings in active list for idle time out */
void
dhd_bus_check_idle_scan(dhd_bus_t *bus)
{
	uint64 time_stamp; /* in millisec */
	uint64 diff;

	time_stamp = OSL_SYSUPTIME();
	diff = time_stamp - bus->active_list_last_process_ts;

	if (diff > IDLE_FLOW_LIST_TIMEOUT) {
		dhd_bus_idle_scan(bus);
		bus->active_list_last_process_ts = OSL_SYSUPTIME();
	}

	return;
}

/* scan the nodes in active list till it finds a non idle node */
void
dhd_bus_idle_scan(dhd_bus_t *bus)
{
	dll_t *item, *prev;
	flow_ring_node_t *flow_ring_node;
	uint64 time_stamp, diff;
	unsigned long flags;
	uint16 ringid[MAX_SUSPEND_REQ];
	uint16 count = 0;

	time_stamp = OSL_SYSUPTIME();
	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);

	for (item = dll_tail_p(&bus->flowring_active_list);
	         !dll_end(&bus->flowring_active_list, item); item = prev) {
		prev = dll_prev_p(item);

		flow_ring_node = dhd_constlist_to_flowring(item);

		if (flow_ring_node->flowid == (bus->max_submission_rings - 1))
			continue;

		if (flow_ring_node->status != FLOW_RING_STATUS_OPEN) {
			/* Takes care of deleting zombie rings */
			/* delete from the active list */
			DHD_INFO(("deleting flow id %u from active list\n",
				flow_ring_node->flowid));
			__dhd_flow_ring_delete_from_active_list(bus, flow_ring_node);
			continue;
		}

		diff = time_stamp - flow_ring_node->last_active_ts;

		if ((diff > IDLE_FLOW_RING_TIMEOUT) && !(flow_ring_node->queue.len))  {
			DHD_ERROR(("\nSuspending flowid %d\n", flow_ring_node->flowid));
			/* delete from the active list */
			__dhd_flow_ring_delete_from_active_list(bus, flow_ring_node);
			flow_ring_node->status = FLOW_RING_STATUS_SUSPENDED;
			ringid[count] = flow_ring_node->flowid;
			count++;
			if (count == MAX_SUSPEND_REQ) {
				/* create a batch message now!! */
				dhd_prot_flow_ring_batch_suspend_request(bus->dhd, ringid, count);
				count = 0;
			}

		} else {

			/* No more scanning, break from here! */
			break;
		}
	}

	if (count) {
		dhd_prot_flow_ring_batch_suspend_request(bus->dhd, ringid, count);
	}

	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	return;
}

void dhd_flow_ring_move_to_active_list_head(struct dhd_bus *bus, flow_ring_node_t *flow_ring_node)
{
	unsigned long flags;
	dll_t* list;

	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	/* check if the node is already at head, otherwise delete it and prepend */
	list = dll_head_p(&bus->flowring_active_list);
	if (&flow_ring_node->list != list) {
		dll_delete(&flow_ring_node->list);
		dll_prepend(&bus->flowring_active_list, &flow_ring_node->list);
	}

	/* update flow ring timestamp */
	flow_ring_node->last_active_ts = OSL_SYSUPTIME();

	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	return;
}

void dhd_flow_ring_add_to_active_list(struct dhd_bus *bus, flow_ring_node_t *flow_ring_node)
{
	unsigned long flags;

	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);

	dll_prepend(&bus->flowring_active_list, &flow_ring_node->list);
	/* update flow ring timestamp */
	flow_ring_node->last_active_ts = OSL_SYSUPTIME();

	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	return;
}
void __dhd_flow_ring_delete_from_active_list(struct dhd_bus *bus, flow_ring_node_t *flow_ring_node)
{
	dll_delete(&flow_ring_node->list);
}

void dhd_flow_ring_delete_from_active_list(struct dhd_bus *bus, flow_ring_node_t *flow_ring_node)
{
	unsigned long flags;

	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);

	__dhd_flow_ring_delete_from_active_list(bus, flow_ring_node);

	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	return;
}
#endif /* IDLE_TX_FLOW_MGMT */

int
dhdpcie_bus_clock_start(struct dhd_bus *bus)
{
	return dhdpcie_start_host_pcieclock(bus);
}

int
dhdpcie_bus_clock_stop(struct dhd_bus *bus)
{
	return dhdpcie_stop_host_pcieclock(bus);
}

int
dhdpcie_bus_disable_device(struct dhd_bus *bus)
{
	return dhdpcie_disable_device(bus);
}

int
dhdpcie_bus_enable_device(struct dhd_bus *bus)
{
	return dhdpcie_enable_device(bus);
}

int
dhdpcie_bus_alloc_resource(struct dhd_bus *bus)
{
	return dhdpcie_alloc_resource(bus);
}

void
dhdpcie_bus_free_resource(struct dhd_bus *bus)
{
	dhdpcie_free_resource(bus);
}

int
dhd_bus_request_irq(struct dhd_bus *bus)
{
	return dhdpcie_bus_request_irq(bus);
}

bool
dhdpcie_bus_dongle_attach(struct dhd_bus *bus)
{
	return dhdpcie_dongle_attach(bus);
}

int
dhd_bus_release_dongle(struct dhd_bus *bus)
{
	bool dongle_isolation;
	osl_t *osh;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus) {
		osh = bus->osh;
		ASSERT(osh);

		if (bus->dhd) {
#if defined(DEBUGGER) || defined(DHD_DSCOPE)
			debugger_close();
#endif /* DEBUGGER || DHD_DSCOPE */

			dongle_isolation = bus->dhd->dongle_isolation;
			dhdpcie_bus_release_dongle(bus, osh, dongle_isolation, TRUE);
		}
	}

	return 0;
}

void
dhdpcie_cto_init(struct dhd_bus *bus, bool enable)
{
	uint32 val;

	if (enable) {
		dhdpcie_bus_cfg_write_dword(bus, PCI_INT_MASK, 4,
			PCI_CTO_INT_MASK | PCI_SBIM_MASK_SERR);
		val = dhdpcie_bus_cfg_read_dword(bus, PCI_SPROM_CONTROL, 4);
		dhdpcie_bus_cfg_write_dword(bus, PCI_SPROM_CONTROL, 4, val | SPROM_BACKPLANE_EN);
		if (bus->dhd->cto_threshold == 0) {
			bus->dhd->cto_threshold = PCIE_CTO_TO_THRESH_DEFAULT;
		}

		si_corereg(bus->sih, bus->sih->buscoreidx,
				OFFSETOF(sbpcieregs_t, ctoctrl), ~0,
				((bus->dhd->cto_threshold << PCIE_CTO_TO_THRESHOLD_SHIFT) &
				PCIE_CTO_TO_THRESHHOLD_MASK) |
				((PCIE_CTO_CLKCHKCNT_VAL << PCIE_CTO_CLKCHKCNT_SHIFT) &
				PCIE_CTO_CLKCHKCNT_MASK) |
				PCIE_CTO_ENAB_MASK);
	} else {
		dhdpcie_bus_cfg_write_dword(bus, PCI_INT_MASK, 4, 0);
		val = dhdpcie_bus_cfg_read_dword(bus, PCI_SPROM_CONTROL, 4);
		dhdpcie_bus_cfg_write_dword(bus, PCI_SPROM_CONTROL, 4, val & ~SPROM_BACKPLANE_EN);

		si_corereg(bus->sih, bus->sih->buscoreidx,
				OFFSETOF(sbpcieregs_t, ctoctrl), ~0, 0);
	}
}

static void
dhdpcie_cto_error_recovery(struct dhd_bus *bus)
{
	uint32 pci_intmask, err_status;
	uint8 i = 0;
	uint32 val;

	pci_intmask = dhdpcie_bus_cfg_read_dword(bus, PCI_INT_MASK, 4);
	dhdpcie_bus_cfg_write_dword(bus, PCI_INT_MASK, 4, pci_intmask & ~PCI_CTO_INT_MASK);

	DHD_OS_WAKE_LOCK(bus->dhd);

	/* reset backplane */
	val = dhdpcie_bus_cfg_read_dword(bus, PCI_SPROM_CONTROL, 4);
	dhdpcie_bus_cfg_write_dword(bus, PCI_SPROM_CONTROL, 4, val | SPROM_CFG_TO_SB_RST);

	/* clear timeout error */
	while (1) {
		err_status =  si_corereg(bus->sih, bus->sih->buscoreidx,
			DAR_ERRLOG(bus->sih->buscorerev),
			0, 0);
		if (err_status & PCIE_CTO_ERR_MASK) {
			si_corereg(bus->sih, bus->sih->buscoreidx,
					DAR_ERRLOG(bus->sih->buscorerev),
					~0, PCIE_CTO_ERR_MASK);
		} else {
			break;
		}
		OSL_DELAY(CTO_TO_CLEAR_WAIT_MS * 1000);
		i++;
		if (i > CTO_TO_CLEAR_WAIT_MAX_CNT) {
			DHD_ERROR(("cto recovery fail\n"));

			DHD_OS_WAKE_UNLOCK(bus->dhd);
			return;
		}
	}

	/* clear interrupt status */
	dhdpcie_bus_cfg_write_dword(bus, PCI_INT_STATUS, 4, PCI_CTO_INT_MASK);

	/* Halt ARM & remove reset */
	/* TBD : we can add ARM Halt here in case */

	DHD_ERROR(("cto recovery success\n"));

	DHD_OS_WAKE_UNLOCK(bus->dhd);
}

#if defined(DBG_PKT_MON) || defined(DHD_PKT_LOGGING)
static int
dhdpcie_init_d11status(struct dhd_bus *bus)
{
	uint32 addr;
	uint32 flags2;
	int ret = 0;

	if (bus->pcie_sh->flags2 & PCIE_SHARED2_D2H_D11_TX_STATUS) {
		flags2 = bus->pcie_sh->flags2;
		addr = bus->shared_addr + OFFSETOF(pciedev_shared_t, flags2);
		flags2 |= PCIE_SHARED2_H2D_D11_TX_STATUS;
		ret = dhdpcie_bus_membytes(bus, TRUE, addr,
			(uint8 *)&flags2, sizeof(flags2));
		if (ret < 0) {
			DHD_ERROR(("%s: update flag bit (H2D_D11_TX_STATUS) failed\n",
				__FUNCTION__));
			return ret;
		}
		bus->pcie_sh->flags2 = flags2;
		bus->dhd->d11_tx_status = TRUE;
	}
	return ret;
}

#else
static int
dhdpcie_init_d11status(struct dhd_bus *bus)
{
	return 0;
}
#endif /* DBG_PKT_MON || DHD_PKT_LOGGING */

#ifdef BCMPCIE_OOB_HOST_WAKE
int
dhd_bus_oob_intr_register(dhd_pub_t *dhdp)
{
	return dhdpcie_oob_intr_register(dhdp->bus);
}

void
dhd_bus_oob_intr_unregister(dhd_pub_t *dhdp)
{
	dhdpcie_oob_intr_unregister(dhdp->bus);
}

void
dhd_bus_oob_intr_set(dhd_pub_t *dhdp, bool enable)
{
	dhdpcie_oob_intr_set(dhdp->bus, enable);
}
#endif /* BCMPCIE_OOB_HOST_WAKE */

bool
dhdpcie_bus_get_pcie_hostready_supported(dhd_bus_t *bus)
{
	return bus->dhd->d2h_hostrdy_supported;
}

void
dhd_pcie_dump_core_regs(dhd_pub_t * pub, uint32 index, uint32 first_addr, uint32 last_addr)
{
	dhd_bus_t *bus = pub->bus;
	uint32	coreoffset = index << 12;
	uint32	core_addr = SI_ENUM_BASE(bus->sih) + coreoffset;
	uint32 value;

	while (first_addr <= last_addr) {
		core_addr = SI_ENUM_BASE(bus->sih) + coreoffset + first_addr;
		if (si_backplane_access(bus->sih, core_addr, 4, &value, TRUE) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
		}
		DHD_ERROR(("[0x%08x]: 0x%08x\n", core_addr, value));
		first_addr = first_addr + 4;
	}
}

bool
dhdpcie_bus_get_pcie_idma_supported(dhd_bus_t *bus)
{
	if (!bus->dhd)
		return FALSE;
	else if (bus->idma_enabled) {
		return bus->dhd->idma_enable;
	} else {
		return FALSE;
	}
}

bool
dhdpcie_bus_get_pcie_ifrm_supported(dhd_bus_t *bus)
{
	if (!bus->dhd)
		return FALSE;
	else if (bus->ifrm_enabled) {
		return bus->dhd->ifrm_enable;
	} else {
		return FALSE;
	}
}

bool
dhdpcie_bus_get_pcie_dar_supported(dhd_bus_t *bus)
{
	if (!bus->dhd) {
		return FALSE;
	} else if (bus->dar_enabled) {
		return bus->dhd->dar_enable;
	} else {
		return FALSE;
	}
}

void
dhdpcie_bus_enab_pcie_dw(dhd_bus_t *bus, uint8 dw_option)
{
	DHD_ERROR(("ENABLING DW:%d\n", dw_option));
	bus->dw_option = dw_option;
}

void
dhd_bus_dump_trap_info(dhd_bus_t *bus, struct bcmstrbuf *strbuf)
{
	trap_t *tr = &bus->dhd->last_trap_info;
	bcm_bprintf(strbuf,
		"\nTRAP type 0x%x @ epc 0x%x, cpsr 0x%x, spsr 0x%x, sp 0x%x,"
		" lp 0x%x, rpc 0x%x"
		"\nTrap offset 0x%x, r0 0x%x, r1 0x%x, r2 0x%x, r3 0x%x, "
		"r4 0x%x, r5 0x%x, r6 0x%x, r7 0x%x\n\n",
		ltoh32(tr->type), ltoh32(tr->epc), ltoh32(tr->cpsr), ltoh32(tr->spsr),
		ltoh32(tr->r13), ltoh32(tr->r14), ltoh32(tr->pc),
		ltoh32(bus->pcie_sh->trap_addr),
		ltoh32(tr->r0), ltoh32(tr->r1), ltoh32(tr->r2), ltoh32(tr->r3),
		ltoh32(tr->r4), ltoh32(tr->r5), ltoh32(tr->r6), ltoh32(tr->r7));
}

int
dhd_bus_readwrite_bp_addr(dhd_pub_t *dhdp, uint addr, uint size, uint* data, bool read)
{
	int bcmerror = 0;
	struct dhd_bus *bus = dhdp->bus;

	if (si_backplane_access(bus->sih, addr, size, data, read) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
			bcmerror = BCME_ERROR;
	}

	return bcmerror;
}

int
dhd_get_idletime(dhd_pub_t *dhd)
{
	return dhd->bus->idletime;
}

#ifdef DHD_SSSR_DUMP

static INLINE void
dhd_sbreg_op(dhd_pub_t *dhd, uint addr, uint *val, bool read)
{
	OSL_DELAY(1);
	si_backplane_access(dhd->bus->sih, addr, sizeof(uint), val, read);
	DHD_ERROR(("%s: addr:0x%x val:0x%x read:%d\n", __FUNCTION__, addr, *val, read));
	return;
}

static int
dhdpcie_get_sssr_fifo_dump(dhd_pub_t *dhd, uint *buf, uint fifo_size,
	uint addr_reg, uint data_reg)
{
	uint addr;
	uint val = 0;
	int i;

	DHD_ERROR(("%s\n", __FUNCTION__));

	if (!buf) {
		DHD_ERROR(("%s: buf is NULL\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (!fifo_size) {
		DHD_ERROR(("%s: fifo_size is 0\n", __FUNCTION__));
		return BCME_ERROR;
	}

	/* Set the base address offset to 0 */
	addr = addr_reg;
	val = 0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	addr = data_reg;
	/* Read 4 bytes at once and loop for fifo_size / 4 */
	for (i = 0; i < fifo_size / 4; i++) {
		si_backplane_access(dhd->bus->sih, addr, sizeof(uint), &val, TRUE);
		buf[i] = val;
		OSL_DELAY(1);
	}
	return BCME_OK;
}

static int
dhdpcie_get_sssr_dig_dump(dhd_pub_t *dhd, uint *buf, uint fifo_size,
	uint addr_reg)
{
	uint addr;
	uint val = 0;
	int i;
	si_t *sih = dhd->bus->sih;

	DHD_ERROR(("%s\n", __FUNCTION__));

	if (!buf) {
		DHD_ERROR(("%s: buf is NULL\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (!fifo_size) {
		DHD_ERROR(("%s: fifo_size is 0\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (addr_reg) {
		/* Check if vasip clk is disabled, if yes enable it */
		addr = dhd->sssr_reg_info.vasip_regs.wrapper_regs.ioctrl;
		dhd_sbreg_op(dhd, addr, &val, TRUE);
		if (!val) {
			val = 1;
			dhd_sbreg_op(dhd, addr, &val, FALSE);
		}

		addr = addr_reg;
		/* Read 4 bytes at once and loop for fifo_size / 4 */
		for (i = 0; i < fifo_size / 4; i++, addr += 4) {
			si_backplane_access(sih, addr, sizeof(uint), &val, TRUE);
			buf[i] = val;
			OSL_DELAY(1);
		}
	} else {
		uint cur_coreid;
		uint chipc_corerev;
		chipcregs_t *chipcregs;

		/* Save the current core */
		cur_coreid = si_coreid(sih);

		/* Switch to ChipC */
		chipcregs = (chipcregs_t *)si_setcore(sih, CC_CORE_ID, 0);

		chipc_corerev = si_corerev(sih);

		if (chipc_corerev == 64) {
			W_REG(si_osh(sih), &chipcregs->sr_memrw_addr, 0);

			/* Read 4 bytes at once and loop for fifo_size / 4 */
			for (i = 0; i < fifo_size / 4; i++) {
				buf[i] = R_REG(si_osh(sih), &chipcregs->sr_memrw_data);
				OSL_DELAY(1);
			}
		}

		/* Switch back to the original core */
		si_setcore(sih, cur_coreid, 0);
	}

	return BCME_OK;
}

#if defined(BCMPCIE) && defined(DHD_LOG_DUMP)
void
dhdpcie_get_etd_preserve_logs(dhd_pub_t *dhd,
		uint8 *ext_trap_data, void *event_decode_data)
{
	hnd_ext_trap_hdr_t *hdr = NULL;
	bcm_tlv_t *tlv;
	eventlog_trapdata_info_t *etd_evtlog = NULL;
	eventlog_trap_buf_info_t *evtlog_buf_arr = NULL;
	uint arr_size = 0;
	int i = 0;
	int err = 0;
	uint32 seqnum = 0;

	if (!ext_trap_data || !event_decode_data || !dhd)
		return;

	if (!dhd->concise_dbg_buf)
		return;

	/* First word is original trap_data, skip */
	ext_trap_data += sizeof(uint32);

	hdr = (hnd_ext_trap_hdr_t *)ext_trap_data;
	tlv = bcm_parse_tlvs(hdr->data, hdr->len, TAG_TRAP_LOG_DATA);
	if (tlv) {
		etd_evtlog = (eventlog_trapdata_info_t *)tlv->data;
		DHD_ERROR(("%s: etd_evtlog tlv found, num_elements=%x; "
			"seq_num=%x; log_arr_addr=%x\n", __FUNCTION__,
			(etd_evtlog->num_elements),
			ntoh32(etd_evtlog->seq_num), (etd_evtlog->log_arr_addr)));
		arr_size = (uint32)sizeof(*evtlog_buf_arr) * (etd_evtlog->num_elements);
		if (!arr_size) {
			DHD_ERROR(("%s: num event logs is zero! \n", __FUNCTION__));
			return;
		}
		evtlog_buf_arr = MALLOCZ(dhd->osh, arr_size);
		if (!evtlog_buf_arr) {
			DHD_ERROR(("%s: out of memory !\n",	__FUNCTION__));
			return;
		}
		/* read the eventlog_trap_buf_info_t array from dongle memory */
		err = dhdpcie_bus_membytes(dhd->bus, FALSE,
				(ulong)(etd_evtlog->log_arr_addr),
				(uint8 *)evtlog_buf_arr, arr_size);
		if (err != BCME_OK) {
			DHD_ERROR(("%s: Error reading event log array from dongle !\n",
				__FUNCTION__));
			goto err;
		}
		/* ntoh is required only for seq_num, because in the original
		* case of event logs from info ring, it is sent from dongle in that way
		* so for ETD also dongle follows same convention
		*/
		seqnum = ntoh32(etd_evtlog->seq_num);
		memset(dhd->concise_dbg_buf, 0, CONCISE_DUMP_BUFLEN);
		for (i = 0; i < (etd_evtlog->num_elements); ++i) {
			/* read each individual event log buf from dongle memory */
			err = dhdpcie_bus_membytes(dhd->bus, FALSE,
					((ulong)evtlog_buf_arr[i].buf_addr),
					dhd->concise_dbg_buf, (evtlog_buf_arr[i].len));
			if (err != BCME_OK) {
				DHD_ERROR(("%s: Error reading event log buffer from dongle !\n",
					__FUNCTION__));
				goto err;
			}
			dhd_dbg_msgtrace_log_parser(dhd, dhd->concise_dbg_buf,
				event_decode_data, (evtlog_buf_arr[i].len),
				FALSE, hton32(seqnum));
			++seqnum;
		}
err:
		MFREE(dhd->osh, evtlog_buf_arr, arr_size);
	} else {
		DHD_ERROR(("%s: Error getting trap log data in ETD !\n", __FUNCTION__));
	}
}
#endif /* BCMPCIE && DHD_LOG_DUMP */

static int
dhdpcie_resume_chipcommon_powerctrl(dhd_pub_t *dhd)
{
	uint addr;
	uint val;

	DHD_ERROR(("%s\n", __FUNCTION__));

	/* conditionally clear bits [11:8] of PowerCtrl */
	addr = dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl;
	dhd_sbreg_op(dhd, addr, &val, TRUE);
	if (!(val & dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl_mask)) {
		addr = dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl;
		val = dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl_mask;
		dhd_sbreg_op(dhd, addr, &val, FALSE);
	}
	return BCME_OK;
}

static int
dhdpcie_suspend_chipcommon_powerctrl(dhd_pub_t *dhd)
{
	uint addr;
	uint val;

	DHD_ERROR(("%s\n", __FUNCTION__));

	/* conditionally clear bits [11:8] of PowerCtrl */
	addr = dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl;
	dhd_sbreg_op(dhd, addr, &val, TRUE);
	if (val & dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl_mask) {
		addr = dhd->sssr_reg_info.chipcommon_regs.base_regs.powerctrl;
		val = 0;
		dhd_sbreg_op(dhd, addr, &val, FALSE);
	}
	return BCME_OK;
}

static int
dhdpcie_clear_intmask_and_timer(dhd_pub_t *dhd)
{
	uint addr;
	uint val;

	DHD_ERROR(("%s\n", __FUNCTION__));

	/* clear chipcommon intmask */
	addr = dhd->sssr_reg_info.chipcommon_regs.base_regs.intmask;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear PMUIntMask0 */
	addr = dhd->sssr_reg_info.pmu_regs.base_regs.pmuintmask0;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear PMUIntMask1 */
	addr = dhd->sssr_reg_info.pmu_regs.base_regs.pmuintmask1;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear res_req_timer */
	addr = dhd->sssr_reg_info.pmu_regs.base_regs.resreqtimer;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear macresreqtimer */
	addr = dhd->sssr_reg_info.pmu_regs.base_regs.macresreqtimer;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear macresreqtimer1 */
	addr = dhd->sssr_reg_info.pmu_regs.base_regs.macresreqtimer1;
	val = 0x0;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	/* clear VasipClkEn */
	if (dhd->sssr_reg_info.vasip_regs.vasip_sr_size) {
		addr = dhd->sssr_reg_info.vasip_regs.wrapper_regs.ioctrl;
		val = 0x0;
		dhd_sbreg_op(dhd, addr, &val, FALSE);
	}

	return BCME_OK;
}

static int
dhdpcie_d11_check_outofreset(dhd_pub_t *dhd)
{
	int i;
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	for (i = 0; i < MAX_NUM_D11CORES; i++) {
		/* Check if bit 0 of resetctrl is cleared */
		addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.resetctrl;
		if (!addr) {
			DHD_ERROR(("%s: skipping for core[%d] as 'addr' is NULL\n",
				__FUNCTION__, i));
			continue;
		}
		dhd_sbreg_op(dhd, addr, &val, TRUE);
		if (!(val & 1)) {
			dhd->sssr_d11_outofreset[i] = TRUE;
		} else {
			dhd->sssr_d11_outofreset[i] = FALSE;
		}
		DHD_ERROR(("%s: sssr_d11_outofreset[%d] : %d\n",
			__FUNCTION__, i, dhd->sssr_d11_outofreset[i]));
	}
	return BCME_OK;
}

static int
dhdpcie_d11_clear_clk_req(dhd_pub_t *dhd)
{
	int i;
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	for (i = 0; i < MAX_NUM_D11CORES; i++) {
		if (dhd->sssr_d11_outofreset[i]) {
			/* clear request clk only if itopoobb is non zero */
			addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.itopoobb;
			dhd_sbreg_op(dhd, addr, &val, TRUE);
			if (val != 0) {
				/* clear clockcontrolstatus */
				addr = dhd->sssr_reg_info.mac_regs[i].base_regs.clockcontrolstatus;
				val =
				dhd->sssr_reg_info.mac_regs[i].base_regs.clockcontrolstatus_val;
				dhd_sbreg_op(dhd, addr, &val, FALSE);
			}
		}
	}
	return BCME_OK;
}

static int
dhdpcie_arm_clear_clk_req(dhd_pub_t *dhd)
{
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	/* Check if bit 0 of resetctrl is cleared */
	addr = dhd->sssr_reg_info.arm_regs.wrapper_regs.resetctrl;
	dhd_sbreg_op(dhd, addr, &val, TRUE);
	if (!(val & 1)) {
		/* clear request clk only if itopoobb is non zero */
		addr = dhd->sssr_reg_info.arm_regs.wrapper_regs.itopoobb;
		dhd_sbreg_op(dhd, addr, &val, TRUE);
		if (val != 0) {
			/* clear clockcontrolstatus */
			addr = dhd->sssr_reg_info.arm_regs.base_regs.clockcontrolstatus;
			val = dhd->sssr_reg_info.arm_regs.base_regs.clockcontrolstatus_val;
			dhd_sbreg_op(dhd, addr, &val, FALSE);
		}
	}
	return BCME_OK;
}

static int
dhdpcie_pcie_clear_clk_req(dhd_pub_t *dhd)
{
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	/* clear request clk only if itopoobb is non zero */
	addr = dhd->sssr_reg_info.pcie_regs.wrapper_regs.itopoobb;
	dhd_sbreg_op(dhd, addr, &val, TRUE);
	if (val) {
		/* clear clockcontrolstatus */
		addr = dhd->sssr_reg_info.pcie_regs.base_regs.clockcontrolstatus;
		val = dhd->sssr_reg_info.pcie_regs.base_regs.clockcontrolstatus_val;
		dhd_sbreg_op(dhd, addr, &val, FALSE);
	}
	return BCME_OK;
}

static int
dhdpcie_pcie_send_ltrsleep(dhd_pub_t *dhd)
{
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	addr = dhd->sssr_reg_info.pcie_regs.base_regs.ltrstate;
	val = LTR_ACTIVE;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	val = LTR_SLEEP;
	dhd_sbreg_op(dhd, addr, &val, FALSE);

	return BCME_OK;
}

static int
dhdpcie_clear_clk_req(dhd_pub_t *dhd)
{
	DHD_ERROR(("%s\n", __FUNCTION__));

	dhdpcie_arm_clear_clk_req(dhd);

	dhdpcie_d11_clear_clk_req(dhd);

	dhdpcie_pcie_clear_clk_req(dhd);

	return BCME_OK;
}

static int
dhdpcie_bring_d11_outofreset(dhd_pub_t *dhd)
{
	int i;
	uint addr;
	uint val = 0;

	DHD_ERROR(("%s\n", __FUNCTION__));

	for (i = 0; i < MAX_NUM_D11CORES; i++) {
		if (dhd->sssr_d11_outofreset[i]) {
			/* disable core by setting bit 0 */
			addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.resetctrl;
			val = 1;
			dhd_sbreg_op(dhd, addr, &val, FALSE);
			OSL_DELAY(6000);

			addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.ioctrl;
			val = dhd->sssr_reg_info.mac_regs[0].wrapper_regs.ioctrl_resetseq_val[0];
			dhd_sbreg_op(dhd, addr, &val, FALSE);

			val = dhd->sssr_reg_info.mac_regs[0].wrapper_regs.ioctrl_resetseq_val[1];
			dhd_sbreg_op(dhd, addr, &val, FALSE);

			/* enable core by clearing bit 0 */
			addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.resetctrl;
			val = 0;
			dhd_sbreg_op(dhd, addr, &val, FALSE);

			addr = dhd->sssr_reg_info.mac_regs[i].wrapper_regs.ioctrl;
			val = dhd->sssr_reg_info.mac_regs[0].wrapper_regs.ioctrl_resetseq_val[2];
			dhd_sbreg_op(dhd, addr, &val, FALSE);

			val = dhd->sssr_reg_info.mac_regs[0].wrapper_regs.ioctrl_resetseq_val[3];
			dhd_sbreg_op(dhd, addr, &val, FALSE);

			val = dhd->sssr_reg_info.mac_regs[0].wrapper_regs.ioctrl_resetseq_val[4];
			dhd_sbreg_op(dhd, addr, &val, FALSE);
		}
	}
	return BCME_OK;
}

static int
dhdpcie_sssr_dump_get_before_sr(dhd_pub_t *dhd)
{
	int i;

	DHD_ERROR(("%s\n", __FUNCTION__));

	for (i = 0; i < MAX_NUM_D11CORES; i++) {
		if (dhd->sssr_d11_outofreset[i]) {
			dhdpcie_get_sssr_fifo_dump(dhd, dhd->sssr_d11_before[i],
				dhd->sssr_reg_info.mac_regs[i].sr_size,
				dhd->sssr_reg_info.mac_regs[i].base_regs.xmtaddress,
				dhd->sssr_reg_info.mac_regs[i].base_regs.xmtdata);
		}
	}

	if (dhd->sssr_reg_info.vasip_regs.vasip_sr_size) {
		dhdpcie_get_sssr_dig_dump(dhd, dhd->sssr_dig_buf_before,
			dhd->sssr_reg_info.vasip_regs.vasip_sr_size,
			dhd->sssr_reg_info.vasip_regs.vasip_sr_addr);
	}

	return BCME_OK;
}

static int
dhdpcie_sssr_dump_get_after_sr(dhd_pub_t *dhd)
{
	int i;

	DHD_ERROR(("%s\n", __FUNCTION__));

	for (i = 0; i < MAX_NUM_D11CORES; i++) {
		if (dhd->sssr_d11_outofreset[i]) {
			dhdpcie_get_sssr_fifo_dump(dhd, dhd->sssr_d11_after[i],
				dhd->sssr_reg_info.mac_regs[i].sr_size,
				dhd->sssr_reg_info.mac_regs[i].base_regs.xmtaddress,
				dhd->sssr_reg_info.mac_regs[i].base_regs.xmtdata);
		}
	}

	if (dhd->sssr_reg_info.vasip_regs.vasip_sr_size) {
		dhdpcie_get_sssr_dig_dump(dhd, dhd->sssr_dig_buf_after,
			dhd->sssr_reg_info.vasip_regs.vasip_sr_size,
			dhd->sssr_reg_info.vasip_regs.vasip_sr_addr);
	}

	return BCME_OK;
}

int
dhdpcie_sssr_dump(dhd_pub_t *dhd)
{
	if (!dhd->sssr_inited) {
		DHD_ERROR(("%s: SSSR not inited\n", __FUNCTION__));
		return BCME_ERROR;
	}

	if (dhd->bus->is_linkdown) {
		DHD_ERROR(("%s: PCIe link is down\n", __FUNCTION__));
		return BCME_ERROR;
	}

	dhdpcie_d11_check_outofreset(dhd);

	DHD_ERROR(("%s: Collecting Dump before SR\n", __FUNCTION__));
	if (dhdpcie_sssr_dump_get_before_sr(dhd) != BCME_OK) {
		DHD_ERROR(("%s: dhdpcie_sssr_dump_get_before_sr failed\n", __FUNCTION__));
		return BCME_ERROR;
	}

	dhdpcie_clear_intmask_and_timer(dhd);
	dhdpcie_suspend_chipcommon_powerctrl(dhd);
	dhdpcie_clear_clk_req(dhd);
	dhdpcie_pcie_send_ltrsleep(dhd);

	/* Wait for some time before Restore */
	OSL_DELAY(6000);

	dhdpcie_resume_chipcommon_powerctrl(dhd);
	dhdpcie_bring_d11_outofreset(dhd);

	DHD_ERROR(("%s: Collecting Dump after SR\n", __FUNCTION__));
	if (dhdpcie_sssr_dump_get_after_sr(dhd) != BCME_OK) {
		DHD_ERROR(("%s: dhdpcie_sssr_dump_get_after_sr failed\n", __FUNCTION__));
		return BCME_ERROR;
	}

	dhd_schedule_sssr_dump(dhd);

	return BCME_OK;
}
#endif /* DHD_SSSR_DUMP */

#ifdef DHD_WAKE_STATUS
wake_counts_t*
dhd_bus_get_wakecount(dhd_pub_t *dhd)
{
	return &dhd->bus->wake_counts;
}
int
dhd_bus_get_bus_wake(dhd_pub_t *dhd)
{
	return bcmpcie_set_get_wake(dhd->bus, 0);
}
#endif /* DHD_WAKE_STATUS */

#define OTP_ADDRESS (SI_ENUM_BASE_DEFAULT + CC_SROM_OTP)
#define OTP_USER_AREA_OFFSET 0x80
#define OTP_USER_AREA_ADDR (OTP_ADDRESS + OTP_USER_AREA_OFFSET)
#define OTP_VERSION_TUPLE_ID 0x15
#define OTP_VENDOR_TUPLE_ID 0x80
#define OTP_CIS_REGION_END_TUPLE_ID 0XFF
#define PMU_RES_STATE_REG_ADDR (SI_ENUM_BASE_DEFAULT + PMU_RES_STATE)
#define PMU_MINRESMASK_REG_ADDR (SI_ENUM_BASE_DEFAULT + MINRESMASKREG)
#define OTP_CTRL1_REG_ADDR (SI_ENUM_BASE_DEFAULT + 0xF4)
#define SPROM_CTRL_REG_ADDR (SI_ENUM_BASE_DEFAULT + CC_SROM_CTRL)
#define CHIP_COMMON_STATUS_REG_ADDR (SI_ENUM_BASE_DEFAULT + 0x2C)
#define PMU_OTP_PWR_ON_MASK 0xC47

int
dhdpcie_get_nvpath_otp(dhd_bus_t *bus, char* program, char *nv_path)
{
	uint32 val = 0;
	uint16 chip_id = 0;
	uint8 otp_data[2];
	char stepping[3];
	char module_name[5];
	char module_vendor = 0;
	char module_rev[4];
	uint8 tuple_id = 0;
	uint8 tuple_len = 0;
	uint32 cur_offset = 0;
	uint32 version_tuple_offset = 0;
	char module_info[64];
	char progname[32];
	bool srom_present = 0, otp_present = 0;
	uint32 sprom_ctrl = 0;
	uint32 otp_ctrl = 0, minres_mask = 0;
	int i = 0, j = 0, status = BCME_ERROR;

	if (!nv_path || !bus) {
		return BCME_ERROR;
	}

	/* read chip id first */
	if (si_backplane_access(bus->sih, SI_ENUM_BASE_DEFAULT, 4, &val, TRUE) != BCME_OK) {
		DHD_ERROR(("%s: bkplane access error ! \n", __FUNCTION__));
	}
	else {
		chip_id = val & 0xffff;
	}

	/* read SpromCtrl register */
	si_backplane_access(bus->sih, SPROM_CTRL_REG_ADDR, 4, &sprom_ctrl, TRUE);
	val = sprom_ctrl;

	/* proceed only if OTP is present - i.e, the 5th bit OtpPresent is set
	* and chip is 4355 or 4364
	*/
	if ((val & 0x20) && (chip_id == 0x4355 || chip_id == 0x4364)) {
		otp_present = 1;

		/* Check if the 4th bit (sprom_present) in CC Status REG is set */
		si_backplane_access(bus->sih, CHIP_COMMON_STATUS_REG_ADDR, 4, &val, TRUE);
		if (val & 0x10) {
			srom_present = 1;
		}

		/* OTP power up sequence */
		/* 1. cache otp ctrl and enable OTP clock through OTPCtrl1 register */
		si_backplane_access(bus->sih, OTP_CTRL1_REG_ADDR, 4, &otp_ctrl, TRUE);
		val = 0x1A0000;
		si_backplane_access(bus->sih, OTP_CTRL1_REG_ADDR, 4, &val, FALSE);

		/* 2. enable OTP power through min res mask register in PMU */
		si_backplane_access(bus->sih, PMU_MINRESMASK_REG_ADDR, 4, &minres_mask, TRUE);
		val = minres_mask | PMU_OTP_PWR_ON_MASK;
		si_backplane_access(bus->sih, PMU_MINRESMASK_REG_ADDR, 4, &val, FALSE);

		/* 3. if srom is present, need to set OtpSelect 4th bit
		* in SpromCtrl register to read otp
		*/
		if (srom_present) {

			val = sprom_ctrl | 0x10;
			si_backplane_access(bus->sih, SPROM_CTRL_REG_ADDR, 4, &val, FALSE);

		}
		/* Wait for PMU to power up. */
		OSL_DELAY(500);
		si_backplane_access(bus->sih, PMU_RES_STATE_REG_ADDR, 4, &val, TRUE);
		DHD_INFO(("%s: PMU_RES_STATE_REG_ADDR %x \n", __FUNCTION__, val));

		si_backplane_access(bus->sih, SI_ENUM_BASE_DEFAULT, 4, &val, TRUE);
		DHD_INFO(("%s: _SI_ENUM_BASE %x \n", __FUNCTION__, val));

		si_backplane_access(bus->sih, OTP_ADDRESS, 2, &val, TRUE);
		DHD_INFO(("%s: OTP_ADDRESS %x \n", __FUNCTION__, val));

		cur_offset = OTP_USER_AREA_ADDR + 0x40;
		/* read required data from otp to construct FW string name
		* data like - chip info, module info. This is present in the
		* form of a Vendor CIS Tuple whose format is provided by Olympic.
		* The data is in the form of ASCII character strings.
		* The Vendor tuple along with other CIS tuples are present
		* in the OTP user area. A CIS tuple is a TLV format.
		* (T = 1-byte, L = 1-byte, V = n-bytes)
		*/

		/* Find the version tuple */
		while (tuple_id != OTP_CIS_REGION_END_TUPLE_ID) {
			si_backplane_access(bus->sih, cur_offset,
				2, (uint *)otp_data, TRUE);

			tuple_id = otp_data[0];
			tuple_len = otp_data[1];
			if (tuple_id == OTP_VERSION_TUPLE_ID) {
				version_tuple_offset = cur_offset;
				break;
			}
			/* if its NULL tuple, skip */
			if (tuple_id == 0)
				cur_offset += 1;
			else
				cur_offset += tuple_len + 2;
		}

		/* skip the major, minor ver. numbers, manufacturer and product names */
		cur_offset = version_tuple_offset + 6;

		/* read the chip info */
		si_backplane_access(bus->sih, cur_offset,
			2, (uint *)otp_data, TRUE);
		if (otp_data[0] == 's' && otp_data[1] == '=') {
			/* read the stepping */
			cur_offset += 2;
			stepping[2] = 0;
			si_backplane_access(bus->sih, cur_offset,
				2, (uint *)stepping, TRUE);
			/* read module info */
			memset(module_info, 0, 64);
			cur_offset += 2;
			si_backplane_access(bus->sih, cur_offset,
				2, (uint *)otp_data, TRUE);
			while (otp_data[0] != OTP_CIS_REGION_END_TUPLE_ID &&
				otp_data[1] != OTP_CIS_REGION_END_TUPLE_ID) {
				memcpy(&module_info[i], otp_data, 2);
				i += 2;
				cur_offset += 2;
				si_backplane_access(bus->sih, cur_offset,
					2, (uint *)otp_data, TRUE);
			}
			/* replace any null characters found at the beginning
			* and middle of the string
			*/
			for (j = 0; j < i; ++j) {
				if (module_info[j] == 0)
					module_info[j] = ' ';
			}
			DHD_ERROR(("OTP chip_info: s=%c%c; module info: %s \n",
				stepping[0], stepping[1], module_info));
			/* extract the module name, revision and vendor
			* information from the module info string
			*/
			for (i = 0; module_info[i]; i++) {
				if (module_info[i] == 'M' && module_info[i + 1] == '=') {
					memcpy(module_name, &module_info[i + 2], 4);
					module_name[4] = 0;
					i += 5;
				}
				else if (module_info[i] == 'm' && module_info[i + 1] == '=') {
					memcpy(module_rev, &module_info[i + 2], 3);
					module_rev[3] = 0;
					i += 4;
				}
				else if (module_info[i] == 'V' && module_info[i + 1] == '=') {
					module_vendor = module_info[i + 2];
					i += 2;
				}
			}

			/* construct the complete file path to nvram as per
			* olympic conventions
			*/
			strncpy(progname, program, sizeof(progname));
			sprintf(nv_path, "P-%s_M-%s_V-%c__m-%s.txt", progname, module_name,
				module_vendor, module_rev);
			DHD_ERROR(("%s NVRAM path = %s\n", __FUNCTION__, nv_path));
			status = BCME_OK;
		}

		/* restore back the registers to their previous values */
		if (srom_present) {
			si_backplane_access(bus->sih, SPROM_CTRL_REG_ADDR, 4, &sprom_ctrl, FALSE);
		}

		if (otp_present) {
			si_backplane_access(bus->sih, PMU_MINRESMASK_REG_ADDR, 4,
				&minres_mask, FALSE);
			si_backplane_access(bus->sih, OTP_CTRL1_REG_ADDR, 4, &otp_ctrl, FALSE);
		}

	}
	return status;
}

/* Writes random number(s) to the TCM. FW upon initialization reads this register
 * to fetch the random number, and uses it to randomize heap address space layout.
 */
static void
dhdpcie_wrt_rnd(struct dhd_bus *bus)
{
	bcm_rand_metadata_t rnd_data;
	uint8 rand_buf[BCM_ENTROPY_HOST_NBYTES];
	uint32 count = BCM_ENTROPY_HOST_NBYTES;

	uint32 addr = bus->dongle_ram_base + (bus->ramsize - BCM_NVRAM_OFFSET_TCM) -
		((bus->nvram_csm & 0xffff)* BCM_NVRAM_IMG_COMPRS_FACTOR + sizeof(rnd_data));
	rnd_data.signature = htol32(BCM_NVRAM_RNG_SIGNATURE);
	rnd_data.count = htol32(count);
	/* write the metadata about random number */
	dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&rnd_data, sizeof(rnd_data));
	/* scale back by number of random number counts */
	addr -= count;
	/* Now write the random number(s) */
	dhd_get_random_bytes(rand_buf, count);
	dhdpcie_bus_membytes(bus, TRUE, addr, rand_buf, count);
}

void
dhd_pcie_intr_count_dump(dhd_pub_t *dhd)
{
	DHD_ERROR(("\n ------- DUMPING INTR enable/disable counters  ------- \r\n"));
	DHD_ERROR(("resume_intr_enable_count=%lu dpc_intr_enable_count=%lu\n",
		dhd->bus->resume_intr_enable_count, dhd->bus->dpc_intr_enable_count));
	DHD_ERROR(("isr_intr_disable_count=%lu suspend_intr_disable_count=%lu\n",
		dhd->bus->isr_intr_disable_count, dhd->bus->suspend_intr_disable_count));
#ifdef BCMPCIE_OOB_HOST_WAKE
	DHD_ERROR(("oob_intr_count=%lu oob_intr_enable_count=%lu oob_intr_disable_count=%lu\n",
		dhd->bus->oob_intr_count, dhd->bus->oob_intr_enable_count,
		dhd->bus->oob_intr_disable_count));
	DHD_ERROR(("oob_irq_num=%d last_oob_irq_time=%llu\n",
		dhdpcie_get_oob_irq_num(dhd->bus), dhd->bus->last_oob_irq_time));
#endif /* BCMPCIE_OOB_HOST_WAKE */
	DHD_ERROR(("dpc_return_busdown_count=%lu\n",
		dhd->bus->dpc_return_busdown_count));

	DHD_ERROR(("\ncurrent_time=%llu isr_entry_time=%llu isr_exit_time=%llu\n",
		OSL_SYSUPTIME_US(), dhd->bus->isr_entry_time, dhd->bus->isr_exit_time));
	DHD_ERROR(("dpc_entry_time=%llu last_process_ctrlbuf_time=%llu\n",
		dhd->bus->dpc_entry_time, dhd->bus->last_process_ctrlbuf_time));
	DHD_ERROR(("last_process_flowring_time=%llu last_process_txcpl_time=%llu\n",
		dhd->bus->last_process_flowring_time, dhd->bus->last_process_txcpl_time));
	DHD_ERROR(("last_process_rxcpl_time=%llu last_process_infocpl_time=%llu\n",
		dhd->bus->last_process_rxcpl_time, dhd->bus->last_process_infocpl_time));
	DHD_ERROR(("dpc_exit_time=%llu resched_dpc_time=%llu\n",
		dhd->bus->dpc_exit_time, dhd->bus->resched_dpc_time));

	DHD_ERROR(("\nlast_suspend_start_time=%llu last_suspend_end_time=%llu\n",
		dhd->bus->last_suspend_start_time, dhd->bus->last_suspend_end_time));
	DHD_ERROR(("last_resume_start_time=%llu last_resume_end_time=%llu\n",
		dhd->bus->last_resume_start_time, dhd->bus->last_resume_end_time));
}

void
dhd_bus_intr_count_dump(dhd_pub_t *dhd)
{
	dhd_pcie_intr_count_dump(dhd);
}

int
dhd_pcie_dma_info_dump(dhd_pub_t *dhd)
{
	if (dhd->bus->is_linkdown) {
		DHD_ERROR(("\n ------- SKIP DUMPING DMA Registers "
			"due to PCIe link down ------- \r\n"));
		return 0;
	}

	DHD_ERROR(("\n ------- DUMPING DMA Registers ------- \r\n"));

	//HostToDev
	DHD_ERROR(("HostToDev TX: XmtCtrl=0x%08x XmtPtr=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x200, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x204, 0, 0)));
	DHD_ERROR(("            : XmtAddrLow=0x%08x XmtAddrHigh=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x208, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x20C, 0, 0)));
	DHD_ERROR(("            : XmtStatus0=0x%08x XmtStatus1=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x210, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x214, 0, 0)));

	DHD_ERROR(("HostToDev RX: RcvCtrl=0x%08x RcvPtr=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x220, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x224, 0, 0)));
	DHD_ERROR(("            : RcvAddrLow=0x%08x RcvAddrHigh=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x228, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x22C, 0, 0)));
	DHD_ERROR(("            : RcvStatus0=0x%08x RcvStatus1=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x230, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x234, 0, 0)));

	//DevToHost
	DHD_ERROR(("DevToHost TX: XmtCtrl=0x%08x XmtPtr=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x240, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x244, 0, 0)));
	DHD_ERROR(("            : XmtAddrLow=0x%08x XmtAddrHigh=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x248, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x24C, 0, 0)));
	DHD_ERROR(("            : XmtStatus0=0x%08x XmtStatus1=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x250, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x254, 0, 0)));

	DHD_ERROR(("DevToHost RX: RcvCtrl=0x%08x RcvPtr=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x260, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x264, 0, 0)));
	DHD_ERROR(("            : RcvAddrLow=0x%08x RcvAddrHigh=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x268, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x26C, 0, 0)));
	DHD_ERROR(("            : RcvStatus0=0x%08x RcvStatus1=0x%08x\n",
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x270, 0, 0),
		si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, 0x274, 0, 0)));

	return 0;
}

int
dhd_pcie_debug_info_dump(dhd_pub_t *dhd)
{
	uint32 intstatus = 0;
	uint32 intmask = 0;
	uint32 mbintstatus = 0;
	uint32 d2h_mb_data = 0;
	int host_irq_disabled;

	DHD_ERROR(("bus->bus_low_power_state = %d\n", dhd->bus->bus_low_power_state));
	host_irq_disabled = dhdpcie_irq_disabled(dhd->bus);
	DHD_ERROR(("host pcie_irq disabled = %d\n", host_irq_disabled));
	dhd_print_tasklet_status(dhd);
	dhd_pcie_intr_count_dump(dhd);

	DHD_ERROR(("\n ------- DUMPING PCIE RC config space Registers ------- \r\n"));
	DHD_ERROR(("Pcie RC Error Status Val=0x%x\n",
		dhdpcie_rc_access_cap(dhd->bus, PCIE_EXTCAP_ID_ERR,
		PCIE_EXTCAP_AER_UCERR_OFFSET, TRUE, FALSE, 0)));

	DHD_ERROR(("RootPort PCIe linkcap=0x%08x\n",
		dhd_debug_get_rc_linkcap(dhd->bus)));

#ifdef CUSTOMER_HW4_DEBUG
	if (dhd->bus->is_linkdown) {
		DHD_ERROR(("Skip dumping the PCIe registers due to PCIe Link down\n"));
		return 0;
	}
#endif /* CUSTOMER_HW4_DEBUG */

	DHD_ERROR(("\n ------- DUMPING PCIE EP config space Registers ------- \r\n"));
	DHD_ERROR(("Status Command(0x%x)=0x%x, BaseAddress0(0x%x)=0x%x BaseAddress1(0x%x)=0x%x\n",
		PCIECFGREG_STATUS_CMD,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGREG_STATUS_CMD, sizeof(uint32)),
		PCIECFGREG_BASEADDR0,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGREG_BASEADDR0, sizeof(uint32)),
		PCIECFGREG_BASEADDR1,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGREG_BASEADDR1, sizeof(uint32))));
	DHD_ERROR(("LinkCtl(0x%x)=0x%x DeviceStatusControl2(0x%x)=0x%x "
		"L1SSControl(0x%x)=0x%x\n", PCIECFGREG_LINK_STATUS_CTRL,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGREG_LINK_STATUS_CTRL,
		sizeof(uint32)), PCIECFGGEN_DEV_STATUS_CTRL2,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGGEN_DEV_STATUS_CTRL2,
		sizeof(uint32)), PCIECFGREG_PML1_SUB_CTRL1,
		dhd_pcie_config_read(dhd->bus->osh, PCIECFGREG_PML1_SUB_CTRL1,
		sizeof(uint32))));

	intstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx,
		dhd->bus->pcie_mailbox_int, 0, 0);
	if (intstatus == (uint32)-1) {
		DHD_ERROR(("Skip dumping the PCIe Core registers due to invalid intstatus\n"));
		return 0;
	}

	intmask = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx,
		dhd->bus->pcie_mailbox_mask, 0, 0);
	mbintstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx,
		PCID2H_MailBox, 0, 0);
	DHD_ERROR(("\n ------- DUMPING INTR Status and Masks ------- \r\n"));
	DHD_ERROR(("intstatus=0x%x intmask=0x%x mbintstatus=0x%x\n",
		intstatus, intmask, mbintstatus));

	dhd_bus_cmn_readshared(dhd->bus, &d2h_mb_data, D2H_MB_DATA, 0);
	DHD_ERROR(("d2h_mb_data=0x%x def_intmask=0x%x \r\n", d2h_mb_data,
		dhd->bus->def_intmask));

	DHD_ERROR(("\n ------- DUMPING PCIE core Registers ------- \r\n"));
	DHD_ERROR(("ClkReq0(0x%x)=0x%x ClkReq1(0x%x)=0x%x ClkReq2(0x%x)=0x%x "
		"ClkReq3(0x%x)=0x%x\n", PCIECFGREG_PHY_DBG_CLKREQ0,
		dhd_pcie_corereg_read(dhd->bus->sih, PCIECFGREG_PHY_DBG_CLKREQ0),
		PCIECFGREG_PHY_DBG_CLKREQ1,
		dhd_pcie_corereg_read(dhd->bus->sih, PCIECFGREG_PHY_DBG_CLKREQ1),
		PCIECFGREG_PHY_DBG_CLKREQ2,
		dhd_pcie_corereg_read(dhd->bus->sih, PCIECFGREG_PHY_DBG_CLKREQ2),
		PCIECFGREG_PHY_DBG_CLKREQ3,
		dhd_pcie_corereg_read(dhd->bus->sih, PCIECFGREG_PHY_DBG_CLKREQ3)));

	dhd_pcie_dma_info_dump(dhd);

	return 0;
}
