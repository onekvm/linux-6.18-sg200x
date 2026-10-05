#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import unittest

import nanokvm_ion_layout as layout


class NanoKvmIonLayoutTest(unittest.TestCase):
    def test_production_48_mib_layout_is_preserved(self) -> None:
        self.assertEqual(
            layout.select_layout(48).ion, layout.Region(0x8CE00000, 0x8FE00000)
        )

    def test_64_mib_uses_safe_expansion_arena(self) -> None:
        self.assertEqual(
            layout.select_layout(64).ion, layout.Region(0x85000000, 0x89000000)
        )

    def test_limits(self) -> None:
        self.assertEqual(layout.select_layout(22).ion.size, 22 * layout.MIB)
        self.assertEqual(layout.select_layout(112).ion.end, 0x8C000000)
        for invalid in (0, 21, 113):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                layout.select_layout(invalid)

    def test_rtos_ram_is_anchored_at_top_of_dram(self) -> None:
        selected = layout.select_layout(48, 4)
        self.assertEqual(selected.rtos, layout.Region(0x8FC00000, 0x90000000))
        self.assertEqual(selected.ion, layout.Region(0x85000000, 0x88000000))
        for invalid in (0, 17):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                layout.select_layout(48, invalid)

    def test_framebuffer_is_packed_without_overlap(self) -> None:
        legacy = layout.select_layout(48, 2, 8)
        self.assertEqual(legacy.framebuffer, layout.Region(0x85000000, 0x85800000))
        expanded = layout.select_layout(64, 2, 8)
        self.assertEqual(expanded.framebuffer, layout.Region(0x89000000, 0x89800000))
        self.assertFalse(layout.regions_overlap(expanded.ion, expanded.framebuffer))

    def test_framebuffer_overflow_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            layout.select_layout(112, 2, 1)
        with self.assertRaises(ValueError):
            layout.select_layout(48, 2, 33)

    def test_gpio_i2c_delay_is_rendered_and_validated(self) -> None:
        rendered = layout.render_dtsi(64, 2, 0, 1)
        self.assertIn("#define CVITEK_GPIO_I2C_DELAY_US 1", rendered)
        for invalid in (0, 101):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                layout.render_dtsi(64, 2, 0, invalid)

    def test_every_configurable_size_avoids_forbidden_regions(self) -> None:
        for size_mib in range(layout.MIN_SIZE_MIB, layout.MAX_SIZE_MIB + 1):
            for rtos_size_mib in range(
                layout.MIN_RTOS_RAM_SIZE_MIB, layout.MAX_RTOS_RAM_SIZE_MIB + 1
            ):
                selected = layout.select_layout(size_mib, rtos_size_mib)
                for forbidden in (*layout.STATIC_FORBIDDEN_REGIONS, selected.rtos):
                    self.assertFalse(layout.regions_overlap(selected.ion, forbidden))


if __name__ == "__main__":
    unittest.main()
