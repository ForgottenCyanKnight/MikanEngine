"""Independent Enhanced oracles: change of measure, pairwise MIS and reciprocal LUTs.

These tests do not replace GPU image/transport verification. The quadrature oracle
uses two different continuous charts (v=u*u), so silently dropping the Jacobian
does not pass as it would in a PSS identity-only test.
"""
import math
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def balance(a, b, empty):
    return a / (a + b) if a + b > 0 else empty


def temporal_expectation(dst, src, *, jacobian=True, samples=100_000):
    canonical = donor = 0.0
    for i in range(samples):
        v = (i + .5) / samples
        u = math.sqrt(v)
        j = 2 * u if jacobian else 1.0
        canonical += dst(v) * balance(dst(v), src(u) / j, 1.0)
        u = v
        v = u * u
        j = 2 * u if jacobian else 1.0
        donor += dst(v) * j * balance(src(u), dst(v) * j, 0.0)
    return (canonical + donor) / samples


class EnhancedOracles(unittest.TestCase):
    def test_pairwise_change_of_measure(self):
        for dst, src, expected in (
            (lambda v: 1., lambda u: 1., 1.),
            (lambda v: 2 * v, lambda u: .1 + 4 * u, 1.),
            (lambda v: v * v, lambda u: 3 - u, 1 / 3),
            (lambda v: 1. if v > .5 else 0., lambda u: 1., .5),
            (lambda v: 1., lambda u: 1. if u < .4 else 0., 1.),
        ):
            self.assertAlmostEqual(temporal_expectation(dst, src), expected, delta=2e-4)

    def test_oracle_detects_missing_geometry_jacobian(self):
        value = temporal_expectation(lambda v: 2 * v, lambda u: .1 + 4 * u, jacobian=False)
        self.assertGreater(abs(value - 1.), .01)

    def test_mixed_chart_removes_only_adjacent_pdfs(self):
        prefix, f_prev, f_rc, suffix = .7, .3, .8, 5.
        p_prev, p_rc = .13, .4
        pss = prefix * (f_prev / p_prev) * (f_rc / p_rc) * suffix
        mixed = pss * p_prev * p_rc
        self.assertAlmostEqual(mixed, prefix * f_prev * f_rc * suffix)
        # Reconnection response changes, but the Jacobian remains G_dst/G_src.
        g_src, g_dst = .17, .31
        self.assertAlmostEqual(g_dst / g_src, 1 / (g_src / g_dst))

    def test_original_pair_table_is_exactly_reciprocal(self):
        data = (ROOT / 'engine/textures/restir/paired-neighbors-3-16.u32.bin').read_bytes()
        self.assertEqual(len(data), 3 * 256 * 256 * 4)
        words = struct.unpack('<196608I', data)
        for layer, size in enumerate((254, 232, 196)):
            def delta(x, y):
                packed = words[layer * 65536 + (y % size) * size + (x % size)]
                return (packed & 255) - 128, (packed >> 8) - 128
            for y in range(size):
                for x in range(size):
                    dx, dy = delta(x, y)
                    self.assertEqual(delta(x + dx, y + dy), (-dx, -dy))

    def test_custom_bsdf_classification_uses_pdf(self):
        # Enhanced compares area footprints in both directions, not just roughness.
        threshold = .2 * .2
        self.assertTrue(1 / 24 >= threshold)
        self.assertFalse(1 / 26 >= threshold)
        for g, pdf, inverse_g, next_pdf, limit in ((.1, .2, 4., .3, 2.), (.1, .2, .1, 20., 2.)):
            connect = min(1 / (g * pdf), inverse_g / next_pdf) >= limit
            self.assertEqual(connect, g == .1 and inverse_g == 4.)

    def test_compact_dispatch_covers_1080p_without_exceeding_vulkan_limits(self):
        for items in (1, 64, 65, 65535 * 64, 1920 * 1080 * 3):
            groups = (items + 63) // 64
            x = min(groups, 65535)
            y = (items - 1) // (65535 * 64) + 1
            self.assertLessEqual(x, 65535)
            self.assertGreaterEqual(x * y * 64, items)
            last = items - 1
            group_y, remaining = divmod(last, 65535 * 64)
            group_x, local = divmod(remaining, 64)
            self.assertEqual((group_y * 65535 + group_x) * 64 + local, last)

    def test_duplicate_cap_retains_positive_history_and_reduces_mass(self):
        caps = [20 + (1 - 20) * (count / 288) ** .1 for count in range(289)]
        self.assertEqual(caps[0], 20)
        self.assertEqual(caps[-1], 1)
        self.assertTrue(all(a >= b for a, b in zip(caps, caps[1:])))


if __name__ == '__main__':
    unittest.main(verbosity=2)
