/*
 * Copyright (c) 2024-2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_ewdg

#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

#include <hpm_ewdg_drv.h>
#include <hpm_sysctl_drv.h>
#include <hpm_clock_drv.h>

LOG_MODULE_REGISTER(wdt_hpm_ewdg, CONFIG_WDT_LOG_LEVEL);

/* Clock source selection */
#define WDT_HPM_EWDG_CLK_SRC_BUS    0
#define WDT_HPM_EWDG_CLK_SRC_OSC32K 1

/* OSC32K frequency constant */
#define EWDG_OSC32K_FREQ 32768U

#define EWDG_MAX_CLK_DIVIDER (1U << EWDG_SOC_CLK_DIV_VAL_MAX)

#if defined(EWDG_SOC_OVERTIME_REG_WIDTH) && (EWDG_SOC_OVERTIME_REG_WIDTH == 16)
#define EWDG_TIMEOUT_TICK_MAX 0xFFFFUL
#else
#define EWDG_TIMEOUT_TICK_MAX 0xFFFFFFFFUL
#endif

/**
 * @brief Get EWDG clock frequency at runtime using HPM SDK
 *
 * @param clk_source Clock source (bus or osc32k)
 * @return Clock frequency in Hz
 */
static uint32_t wdt_hpm_ewdg_get_clk_freq(uint8_t clk_source)
{
	if (clk_source == WDT_HPM_EWDG_CLK_SRC_BUS) {
		/* Query AHB clock frequency from HPM SDK */
		return clock_get_frequency(clock_ahb);
	} else {
		/* OSC32K is fixed at 32.768kHz */
		return EWDG_OSC32K_FREQ;
	}
}

struct wdt_hpm_ewdg_config {
	EWDG_Type *base;
	uint32_t default_timeout_ms;
	uint8_t clk_source; /* 0=bus, 1=osc32k */
	uint8_t instance;   /* 0=ewdg0, 1=ewdg1 */
#ifdef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct wdt_hpm_ewdg_data {
	bool started;
	bool timeout_installed;
	uint32_t timeout_ms;
	wdt_callback_t callback;
};

static int wdt_hpm_ewdg_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;
	EWDG_Type *base = cfg->base;
	ewdg_config_t ewdg_cfg;
	hpm_stat_t status;
	uint32_t clk_freq;

	if (data->started) {
		LOG_ERR("Watchdog already started");
		return -EBUSY;
	}

	if (!data->timeout_installed) {
		LOG_ERR("No timeout installed");
		return -EINVAL;
	}

	/* Get clock frequency at runtime from HPM SDK */
	clk_freq = wdt_hpm_ewdg_get_clk_freq(cfg->clk_source);

	/* Get default configuration */
	ewdg_get_default_config(base, &ewdg_cfg);

	/* Configure EWDG */
	ewdg_cfg.enable_watchdog = true;
	ewdg_cfg.int_rst_config.enable_timeout_reset = true;

	/* Select clock source based on devicetree configuration */
	if (cfg->clk_source == WDT_HPM_EWDG_CLK_SRC_BUS) {
		ewdg_cfg.ctrl_config.cnt_clk_sel = ewdg_cnt_clk_src_bus_clk;
		LOG_DBG("Using bus clock source, freq=%u Hz", clk_freq);
	} else {
		ewdg_cfg.ctrl_config.cnt_clk_sel = ewdg_cnt_clk_src_ext_osc_clk;
		LOG_DBG("Using OSC32K clock source, freq=%u Hz", clk_freq);
	}

	ewdg_cfg.ctrl_config.use_lowlevel_timeout = false;
	ewdg_cfg.ctrl_config.timeout_reset_us = data->timeout_ms * 1000U;
	ewdg_cfg.cnt_src_freq = clk_freq;

	/* Handle debug halt option */
	if (options & WDT_OPT_PAUSE_HALTED_BY_DBG) {
		ewdg_cfg.ctrl_config.keep_running_in_debug_mode = false;
	} else {
		ewdg_cfg.ctrl_config.keep_running_in_debug_mode = true;
	}

	/* Low power mode - keep running */
	ewdg_cfg.ctrl_config.low_power_mode = ewdg_low_power_mode_work_clock_normal;

#ifdef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
	if (data->callback != NULL) {
		ewdg_cfg.int_rst_config.enable_timeout_interrupt = true;
		/* Set interrupt to trigger slightly before reset */
		ewdg_cfg.ctrl_config.timeout_interrupt_us =
			(data->timeout_ms * 1000U) - (data->timeout_ms * 100U);
	}
#endif

	/* Initialize EWDG */
	status = ewdg_init(base, &ewdg_cfg);
	if (status != status_success) {
		LOG_ERR("EWDG init failed: %d", status);
		return -EIO;
	}

#ifdef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
	if (data->callback != NULL && cfg->irq_config_func != NULL) {
		cfg->irq_config_func(dev);
	}
#endif

	data->started = true;
	LOG_INF("Watchdog started, timeout=%u ms", data->timeout_ms);

	return 0;
}

static int wdt_hpm_ewdg_disable(const struct device *dev)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;

	if (!data->started) {
		return -EFAULT;
	}

	ewdg_disable(cfg->base);
	data->started = false;
	data->timeout_installed = false;

	LOG_INF("Watchdog disabled");

	return 0;
}

static int wdt_hpm_ewdg_install_timeout(const struct device *dev,
					const struct wdt_timeout_cfg *timeout_cfg)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;

	if (data->started) {
		LOG_ERR("Cannot install timeout while watchdog is running");
		return -EBUSY;
	}

	if (timeout_cfg == NULL) {
		LOG_ERR("Timeout configuration is NULL");
		return -EINVAL;
	}

	/* Window mode (min != 0) is not supported in basic implementation */
	if (timeout_cfg->window.min != 0U) {
		LOG_ERR("Window mode not supported (min must be 0)");
		return -ENOTSUP;
	}

	if (timeout_cfg->window.max == 0U) {
		LOG_ERR("Timeout must be greater than 0");
		return -EINVAL;
	}

	switch (timeout_cfg->flags) {
	case WDT_FLAG_RESET_SOC:
	case WDT_FLAG_RESET_NONE:
		break;
	case WDT_FLAG_RESET_CPU_CORE:
	default:
		LOG_ERR("Unsupported reset flag: %d", timeout_cfg->flags);
		return -ENOTSUP;
	}

#ifndef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
	/* If interrupt support is disabled, callback cannot be used */
	if (timeout_cfg->callback != NULL) {
		LOG_ERR("Callbacks require CONFIG_WDT_HPMICRO_EWDG_INTERRUPT");
		return -ENOTSUP;
	}
#endif

	/* Get clock frequency at runtime */
	uint32_t clk_freq = wdt_hpm_ewdg_get_clk_freq(cfg->clk_source);

	uint32_t max_timeout_ms = ((uint64_t)EWDG_TIMEOUT_TICK_MAX *
				   EWDG_MAX_CLK_DIVIDER * 1000ULL) / clk_freq;

	if (timeout_cfg->window.max > max_timeout_ms) {
		LOG_ERR("Timeout %u ms exceeds maximum %u ms",
			timeout_cfg->window.max, max_timeout_ms);
		return -EINVAL;
	}

	data->timeout_ms = timeout_cfg->window.max;
	data->callback = timeout_cfg->callback;
	data->timeout_installed = true;

	LOG_DBG("Timeout installed: %u ms (max supported: %u ms)",
		data->timeout_ms, max_timeout_ms);

	return 0; /* Return channel 0 (single channel) */
}

static int wdt_hpm_ewdg_feed(const struct device *dev, int channel_id)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;
	hpm_stat_t status;

	ARG_UNUSED(channel_id);

	if (!data->started) {
		return -EINVAL;
	}

	status = ewdg_refresh(cfg->base);
	if (status != status_success) {
		LOG_ERR("Failed to refresh watchdog: %d", status);
		return -EIO;
	}

	return 0;
}

static const struct wdt_driver_api wdt_hpm_ewdg_api = {
	.setup = wdt_hpm_ewdg_setup,
	.disable = wdt_hpm_ewdg_disable,
	.install_timeout = wdt_hpm_ewdg_install_timeout,
	.feed = wdt_hpm_ewdg_feed,
};

#ifdef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
static void wdt_hpm_ewdg_isr(const struct device *dev)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;
	uint32_t status;

	status = ewdg_get_status_flags(cfg->base);

	if ((status & EWDG_EVENT_TIMEOUT_INTERRUPT) != 0) {
		if (data->callback != NULL) {
			data->callback(dev, 0);
		}
	}

	/* Clear status flags (except timeout which clears on refresh) */
	ewdg_clear_status_flags(cfg->base, status & ~EWDG_EVENT_TIMEOUT_INTERRUPT);
}
#endif

static int wdt_hpm_ewdg_init(const struct device *dev)
{
	const struct wdt_hpm_ewdg_config *cfg = dev->config;
	struct wdt_hpm_ewdg_data *data = dev->data;

	data->started = false;
	data->timeout_installed = false;
	data->timeout_ms = cfg->default_timeout_ms;
	data->callback = NULL;

#ifdef CONFIG_WDT_DISABLE_AT_BOOT
	/* Disable watchdog at boot if requested */
	ewdg_disable(cfg->base);
	LOG_DBG("EWDG disabled at boot");
#endif

	LOG_DBG("EWDG initialized at %p, default timeout %u ms",
		cfg->base, cfg->default_timeout_ms);

	return 0;
}

#ifdef CONFIG_WDT_HPMICRO_EWDG_INTERRUPT
#define WDT_HPM_EWDG_IRQ_CONFIG(n)					\
	static void wdt_hpm_ewdg_irq_config_##n(const struct device *dev) \
	{								\
		IRQ_CONNECT(DT_INST_IRQN(n),				\
			    DT_INST_IRQ(n, priority),			\
			    wdt_hpm_ewdg_isr,				\
			    DEVICE_DT_INST_GET(n),			\
			    0);						\
		irq_enable(DT_INST_IRQN(n));				\
	}
#define WDT_HPM_EWDG_IRQ_CONFIG_INIT(n) .irq_config_func = wdt_hpm_ewdg_irq_config_##n,
#else
#define WDT_HPM_EWDG_IRQ_CONFIG(n)
#define WDT_HPM_EWDG_IRQ_CONFIG_INIT(n)
#endif

/* Helper to get clock source from DT string property */
#define WDT_HPM_EWDG_CLK_SRC(n) \
	(DT_INST_ENUM_IDX_OR(n, clk_source, WDT_HPM_EWDG_CLK_SRC_BUS))

/* Helper to get instance number from register address */
/* EWDG0: 0xf00b0000, EWDG1: 0xf00b4000 */
#define WDT_HPM_EWDG_INSTANCE(n) \
	(((DT_INST_REG_ADDR(n) & 0xFFFF) == 0x0000) ? 0 : 1)

#define WDT_HPM_EWDG_INIT(n)						\
	WDT_HPM_EWDG_IRQ_CONFIG(n)					\
									\
	static const struct wdt_hpm_ewdg_config wdt_hpm_ewdg_cfg_##n = { \
		.base = (EWDG_Type *)DT_INST_REG_ADDR(n),		\
		.default_timeout_ms = DT_INST_PROP(n, timeout_ms),	\
		.clk_source = WDT_HPM_EWDG_CLK_SRC(n),			\
		.instance = WDT_HPM_EWDG_INSTANCE(n),			\
		WDT_HPM_EWDG_IRQ_CONFIG_INIT(n)				\
	};								\
									\
	static struct wdt_hpm_ewdg_data wdt_hpm_ewdg_data_##n;		\
									\
	DEVICE_DT_INST_DEFINE(n,					\
			      wdt_hpm_ewdg_init,			\
			      NULL,					\
			      &wdt_hpm_ewdg_data_##n,			\
			      &wdt_hpm_ewdg_cfg_##n,			\
			      PRE_KERNEL_1,				\
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &wdt_hpm_ewdg_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_HPM_EWDG_INIT)
