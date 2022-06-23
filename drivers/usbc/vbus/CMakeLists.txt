# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_VBUS_VOLTAGE_DIVIDER_ADC vbus_voltage_divider_adc.c)
