/* Every driverlib module the ROM API tables name. DRIVERLIB_NOROM is set by
 * the build, so each X below resolves to the flash-linked NOROM_X.
 *
 * The CC13x2/CC26x2 and CC26x0 driverlibs differ in which modules they ship;
 * __has_include guards make this one file work for both.  The CC26x0 driverlib
 * has no sha2/pka, no aon_pmctl/aux_sysif, and no aes (its crypto is in
 * crypto.h + rom_crypto.c).  The CC13x2x7/CC26x2x7 driverlib has no crypto.h. */
#ifndef ROM_INCLUDES_H
#define ROM_INCLUDES_H
#include <stdint.h>
#include <stdbool.h>
#include "driverlib/adi.h"
#if __has_include("driverlib/aes.h")
#include "driverlib/aes.h"
#endif
#include "driverlib/aon_batmon.h"
#include "driverlib/aon_event.h"
#include "driverlib/aon_ioc.h"
#if __has_include("driverlib/aon_pmctl.h")
#include "driverlib/aon_pmctl.h"
#endif
#include "driverlib/aon_rtc.h"
#if __has_include("driverlib/aon_wuc.h")
#include "driverlib/aon_wuc.h"
#endif
#include "driverlib/aux_adc.h"
#include "driverlib/aux_smph.h"
#if __has_include("driverlib/aux_sysif.h")
#include "driverlib/aux_sysif.h"
#endif
#include "driverlib/aux_tdc.h"
#if __has_include("driverlib/aux_timer.h")
#include "driverlib/aux_timer.h"
#endif
#if __has_include("driverlib/aux_wuc.h")
#include "driverlib/aux_wuc.h"
#endif
#include "driverlib/ccfgread.h"
#include "driverlib/chipinfo.h"
#include "driverlib/cpu.h"
#if __has_include("driverlib/crypto.h")
#include "driverlib/crypto.h"
#endif
#include "driverlib/ddi.h"
#include "driverlib/event.h"
#include "driverlib/flash.h"
#include "driverlib/gpio.h"
#include "driverlib/i2c.h"
#include "driverlib/i2s.h"
#include "driverlib/interrupt.h"
#include "driverlib/ioc.h"
#include "driverlib/osc.h"
#if __has_include("driverlib/pka.h")
#include "driverlib/pka.h"
#endif
#include "driverlib/prcm.h"
#include "driverlib/pwr_ctrl.h"
#include "driverlib/rfc.h"
#include "driverlib/setup.h"
#include "driverlib/setup_rom.h"
#if __has_include("driverlib/sha2.h")
#include "driverlib/sha2.h"
#endif
#include "driverlib/smph.h"
#include "driverlib/ssi.h"
#include "driverlib/sys_ctrl.h"
#include "driverlib/systick.h"
#include "driverlib/timer.h"
#include "driverlib/trng.h"
#include "driverlib/uart.h"
#include "driverlib/udma.h"
#include "driverlib/vims.h"
#include "driverlib/watchdog.h"

/* ROM_VERSION (ROM_API_TABLE[0]); no firmware path reads it. */
#define CSIM_ROM_VERSION 0x00000001u
#endif
