#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <hpm_common.h>
#include <hpm_soc.h>
#include "hpm_clock_drv.h"
#include "hpm_pllctlv2_drv.h"
#include "hpm_pcfg_drv.h"
#ifdef CONFIG_XIP
#include "hpm_bootheader.h"
#endif

#ifdef CONFIG_XIP
__attribute__((section(".nor_cfg_option"))) const uint32_t option[4] = {0xfcf90002, 0x00000005, 0x1000, 0x0};
#endif
__attribute__((weak)) void c_startup(void)
{
}

static void soc_init_clock(void)
{
    uint32_t cpu0_freq = clock_get_frequency(clock_cpu0);
    if (cpu0_freq == PLLCTL_SOC_PLL_REFCLK_FREQ)
    {
        /* Configure the External OSC ramp-up time: ~9ms */
        pllctlv2_xtal_set_rampup_time(HPM_PLLCTLV2, 32UL * 1000UL * 9U);

        /* Select clock setting preset1 */
        sysctl_clock_set_preset(HPM_SYSCTL, sysctl_preset_1);
    }
    /* Add most Clocks to group 0 */
    clock_add_to_group(clock_cpu0, 0);
    clock_add_to_group(clock_ahb, 0);
    clock_add_to_group(clock_lmm0, 0);
    clock_add_to_group(clock_mchtmr0, 0);
    clock_add_to_group(clock_rom, 0);
    clock_add_to_group(clock_mot0, 0); /* for trgm and synt peripheral */
    clock_add_to_group(clock_gpio, 0);
    clock_add_to_group(clock_hdma, 0);
    clock_add_to_group(clock_xpi0, 0);

#ifdef CONFIG_CAN
    /* Enable CAN clocks if CAN is configured */
    clock_add_to_group(clock_can0, 0);
    clock_add_to_group(clock_can1, 0);
    clock_add_to_group(clock_can2, 0);
    clock_add_to_group(clock_can3, 0);
#endif

    /* Connect Group0 to CPU0 */
    clock_connect_group_to_cpu(0, 0);

    /* Bump up DCDC voltage to 1175mv */
    pcfg_dcdc_set_voltage(HPM_PCFG, 1175);

    /* Configure mchtmr to 24MHz */
    clock_set_source_divider(clock_mchtmr0, clk_src_osc24m, 1);
}

static int hpmicro_soc_init(void)
{
    uint32_t key;

    key = irq_lock();
    soc_init_clock();
    irq_unlock(key);

    return 0;
}

SYS_INIT(hpmicro_soc_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
