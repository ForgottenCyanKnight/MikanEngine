"""Exact finite-PSS oracle for the ported pairwise estimator (no GPU claim).
Enumerates independently constructed multi-candidate RIS reservoirs and resolves
both integrand channels in expectation. No third-party Python packages required.
"""
import itertools
import math
import unittest


def target(value):
    return sum(value)


def reservoirs(integrand, count):
    domain = len(integrand)
    outcomes = []
    for candidates in itertools.product(range(domain), repeat=count):
        weights = [target(integrand[u]) for u in candidates]
        total = sum(weights)
        if total == 0:
            outcomes.append((domain ** -count, candidates[0], 0.0, count))
            continue
        for u, weight in zip(candidates, weights):
            if weight:
                outcomes.append((domain ** -count * weight / total,
                                 u, total / (count * weight), count))
    return outcomes


def balance(a, b, zero=0.0):
    return a / (a + b) if a + b else zero


def expected_temporal(current, previous, mc, md):
    result = [0.0, 0.0]
    for probability, u, ucw, _ in reservoirs(current, mc):
        mis = balance(target(current[u]) * mc, target(previous[u]) * md, 1.0)
        for c in range(2):
            result[c] += probability * ucw * mis * current[u][c]
    for probability, u, ucw, _ in reservoirs(previous, md):
        mis = balance(target(previous[u]) * md, target(current[u]) * mc)
        for c in range(2):
            result[c] += probability * ucw * mis * current[u][c]
    return result


def expected_spatial(current, others, masses, mc):
    total = mc + sum(masses)
    other_mass = total - mc
    result = [0.0, 0.0]
    for probability, u, ucw, _ in reservoirs(current, mc):
        mis = mc / total
        for other, mass in zip(others, masses):
            mis += mass / total * balance(target(current[u]) * mc,
                                           target(other[u]) * other_mass, 1.0)
        for c in range(2):
            result[c] += probability * ucw * mis * current[u][c]
    for other, mass in zip(others, masses):
        for probability, u, ucw, _ in reservoirs(other, mass):
            mis = mass / total * balance(target(other[u]) * other_mass,
                                         target(current[u]) * mc)
            for c in range(2):
                result[c] += probability * ucw * mis * current[u][c]
    return result


class PairwiseTests(unittest.TestCase):
    def check_mean(self, actual, integrand):
        for c, value in enumerate(actual):
            self.assertTrue(math.isfinite(value))
            self.assertAlmostEqual(value, sum(v[c] for v in integrand) / len(integrand), places=11)

    def test_temporal_unequal_mass_and_disjoint_support(self):
        x = [(0, 0), (2, 8), (4, 1)]
        y = [(5, 2), (0, 0), (90, 3)]
        for mc, md in [(1, 1), (1, 4), (3, 2)]:
            self.check_mean(expected_temporal(x, y, mc, md), x)

    def test_spatial_three_different_receivers(self):
        x = [(0, 0), (2, 8), (4, 1)]
        others = [[(5, 2), (0, 0), (90, 3)],
                  [(1, 0), (8, 9), (0, 0)],
                  [(0, 0), (0, 0), (0, 0)]]
        self.check_mean(expected_spatial(x, others, [2, 3, 1], 2), x)

    def test_constant_integrand(self):
        x = [(2, 3)] * 3
        self.check_mean(expected_temporal(x, x, 1, 4), x)
        self.check_mean(expected_spatial(x, [x, x, x], [1, 2, 3], 2), x)

    def test_zero_integrand(self):
        zero = [(0, 0)] * 3
        bright = [(8, 2), (3, 9), (1, 5)]
        self.check_mean(expected_temporal(zero, bright, 1, 3), zero)
        self.check_mean(expected_spatial(zero, [bright], [2], 1), zero)


if __name__ == "__main__":
    unittest.main()
