"""The guide-switching rule, and the frame that broke it.

SettleGuide picks which of the game's own buffers is depth and which is motion, from a bind tally
that is cleared every present. Picking from one frame was the bug: the GTA San Andreas log has the
depth guide leave a buffer bound 96440 times for one bound 9 times, on the frame the game changed
resolution and drew no scene pass. Nothing demotes a chosen guide, so every frame after that fed
the network the wrong depth.

This is the same decision written out once more, in the smallest form that can be asserted
against. If the two disagree, this file is the one that is wrong.

    python tools/guide_switch_check.py
"""

HOLD = 3  # presents a challenger has to win running, same constant as SettleGuide


class Guide:
    def __init__(self):
        self.chosen = None
        self.challenger = None
        self.frames = 0

    def settle(self, tally):
        """tally: {resource: binds} for one present. Mirrors SettleGuide."""
        if not tally:
            return
        best = max(tally, key=lambda r: tally[r])
        if best == self.chosen:
            self.challenger, self.frames = None, 0
            return
        if best != self.challenger:
            self.challenger, self.frames = best, 1
            return
        self.frames += 1
        if self.frames < HOLD:
            return
        self.chosen, self.challenger, self.frames = best, None, 0


def run(frames):
    g = Guide()
    for t in frames:
        g.settle(t)
    return g.chosen


scene, odd, other = "scene", "odd", "other"

# 1. Nothing chosen yet: the first buffer still has to win three frames, but it wins them.
assert run([{scene: 96440}]) is None
assert run([{scene: 96440}] * 3) == scene

# 2. The reported bug. One frame without the scene pass must not hand the guide away.
steady = [{scene: 96440}] * 10
assert run(steady + [{odd: 9}] + steady) == scene

# 3. Two odd frames are still not enough, and they do not accumulate across an interruption.
assert run(steady + [{odd: 9}, {odd: 9}] + steady) == scene
assert run(steady + [{odd: 9}, {scene: 96440}, {odd: 9}] + steady) == scene

# 4. A real switch still happens -- the game moved to a new buffer and keeps using it.
assert run(steady + [{other: 5000}] * 3) == other

# 5. Two challengers taking turns cancel each other out rather than either one winning.
assert run(steady + [{odd: 9}, {other: 9}] * 6) == scene

# 6. The incumbent winning the frame clears a challenger's streak, even at two.
assert run(steady + [{odd: 9}, {odd: 9}, {scene: 96440}, {odd: 9}, {odd: 9}]) == scene

print("guide switching: 6 properties hold")
