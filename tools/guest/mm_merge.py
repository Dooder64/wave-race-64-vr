#!/usr/bin/env python3
"""Joining Majora's Mask scenes into one guest course, and tidying what a jet
ski meets that Link never did.

Neighbouring scenes each draw a rough copy of the other's land near their
edge (Great Bay Coast of Zora Cape's, and the other way round). Where both
have scenery, the scene with more of it (more triangles in the cells round
about: the detailed one there) owns that ground, and the other's triangles
and collision there are left out (Owners).

Link is kept in by walls nobody sees: collision standing in the water with
nothing drawn on it (Visible). A jet ski is stopped dead by them in open
water, so a steep one through the waterline is left out.
"""

import random
from collections import defaultdict

import numpy as np


def triangles(scenery):
    """mm_scene.Scenery's triangles as an (n, 3, 3) array of positions."""
    return np.array([[v[:3] for v in vs] for vs, _tex in scenery.triangles], float).reshape(-1, 3, 3)


def flat_water(T, tol=2.0):
    """The water surface's own triangles: flat at the waterline."""
    return np.all(np.abs(T[:, :, 1]) < tol, axis=1)


class Owners:
    CELL = 500.0

    def __init__(self, scenes):
        """`scenes`: one (n, 3, 3) array of scenery positions per scene."""
        allp = np.concatenate([T.reshape(-1, 3) for T in scenes])
        self.x0, self.z0 = allp[:, 0].min() - self.CELL, allp[:, 2].min() - self.CELL
        self.w = int((allp[:, 0].max() - self.x0) / self.CELL) + 2
        self.h = int((allp[:, 2].max() - self.z0) / self.CELL) + 2
        self.density = []
        for T in scenes:
            D = np.zeros((self.h, self.w))
            for c in T[~flat_water(T)].mean(1):
                D[self.cell(c)] += 1
            P = np.pad(D, 1)
            self.density.append(sum(P[1 + a:1 + a + self.h, 1 + b:1 + b + self.w] for a in (-1, 0, 1) for b in (-1, 0, 1)))

    def cell(self, p):
        return int((p[2] - self.z0) / self.CELL), int((p[0] - self.x0) / self.CELL)

    def keeps(self, scene, tri):
        """Whether scene `scene` keeps a triangle (positions) of its own."""
        if np.all(np.abs(np.asarray(tri)[:, 1]) < 2.0):
            return True            # the water's surface: every scene's
        j, i = self.cell(np.mean(tri, axis=0))
        if not (0 <= j < self.h and 0 <= i < self.w):
            return True
        mine = self.density[scene][j, i]
        return all(d[j, i] <= mine for k, d in enumerate(self.density) if k != scene)


class Visible:
    """How much of a triangle has scenery drawn within `reach` of it."""

    def __init__(self, T, reach=60.0):
        rng = random.Random(1)
        keep = ~flat_water(T)
        pts = []
        for t in T[keep]:
            area = np.linalg.norm(np.cross(t[1] - t[0], t[2] - t[0])) / 2
            for _ in range(int(min(400, 4 + area / 2000))):
                pts.append(self._sample(t, rng))
            pts.extend(t)
        self.P = np.array(pts)
        self.reach = reach
        self.grid = defaultdict(list)
        for k, p in enumerate(self.P):
            self.grid[tuple((p // 100.0).astype(int))].append(k)
        self.rng = rng

    @staticmethod
    def _sample(t, rng):
        a, b = rng.random(), rng.random()
        if a + b > 1:
            a, b = 1 - a, 1 - b
        return t[0] + (t[1] - t[0]) * a + (t[2] - t[0]) * b

    def fraction(self, t, n=40):
        hits = 0
        for _ in range(n):
            q = self._sample(t, self.rng)
            k = (q // 100.0).astype(int)
            best = 1e9
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        ids = self.grid.get((k[0] + dx, k[1] + dy, k[2] + dz))
                        if ids:
                            best = min(best, float(np.linalg.norm(self.P[ids] - q, axis=1).min()))
            hits += best < self.reach
        return hits / n


def steep(tri, limit=0.7):
    t = np.asarray(tri, float)
    n = np.cross(t[1] - t[0], t[2] - t[0])
    ln = np.linalg.norm(n)
    return ln > 1e-6 and abs(n[1] / ln) < limit


def through_waterline(tri, water=0.0):
    ys = [p[1] for p in tri]
    return min(ys) < water + 10 and max(ys) > water - 10
