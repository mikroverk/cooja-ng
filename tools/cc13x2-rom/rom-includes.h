/* Every driverlib module the ROM API tables name. DRIVERLIB_NOROM is set by
 * the build, so each X below resolves to the flash-linked NOROM_X. */
#ifndef ROM_INCLUDES_H
#define ROM_INCLUDES_H
#include <stdint.h>
#include <stdbool.h>
#include "driverlib/adi.h"
#include "driverlib/aes.h"
#include "driverlib/aon_batmon.h"
#include "driverlib/aon_event.h"
#include "driverlib/aon_ioc.h"
#include "driverlib/aon_pmctl.h"
#include "driverlib/aon_rtc.h"
#include "driverlib/aux_adc.h"
#include "driverlib/aux_smph.h"
#include "driverlib/aux_sysif.h"
#include "driverlib/aux_tdc.h"
#include "driverlib/ccfgread.h"
#include "driverlib/chipinfo.h"
#include "driverlib/cpu.h"
#if __has_include("driverlib/crypto.h")   /* not in the CC13x2x7/CC26x2x7 driverlib */
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
#include "driverlib/pka.h"
#include "driverlib/prcm.h"
#include "driverlib/pwr_ctrl.h"
#include "driverlib/rfc.h"
#include "driverlib/setup.h"
#include "driverlib/setup_rom.h"
#include "driverlib/sha2.h"
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
