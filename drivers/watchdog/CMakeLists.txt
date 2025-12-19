# Copyright (c) 2024-2025
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()

zephyr_library_sources_ifdef(CONFIG_WDT_HPMICRO_EWDG wdt_hpmicro_ewdg.c)
