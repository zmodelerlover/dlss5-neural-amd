"""The guide-switching rule, and the two frames that broke it.

SettleGuide picks which of the game's own buffers is depth and which is motion, from a bind tally.
Picking from one frame was the first bug: the GTA San Andreas log has the depth guide leave a
buffer bound 96440 times for one bound 9 times, on the frame the game changed resolution and drew
no scene pass. Nothing demotes a chosen guide, so every frame after that fed the network the wrong
depth.

Making a challenger win three presents *running* fixed that and broke the cold start. An engine
that rotates two or three depth targets never presents the same one three times in a row, so the
challenger changed every present, the streak reset every present, and the slot stayed empty for as
long as the game ran -- depth silently gone, in a game that has depth. Property 7 below is that
game, and it fails against the streak rule alone.

So there are two rules, for two different questions. With nothing chosen there is no incumbent to
protect and the only question is having seen enough: the tally is left standing for three presents
and the leader of the total is taken. With an incumbent the question is stability, and a challenger
still has to win three presents running.

This is that decision written out once more, in the smallest form that can be asserted against. If
the two disagree, this file is the one that is wrong.

    python tools/guide_switch_check.py
"""

HOLD = 3  # presents, for both rules -- the same constant SettleGuide uses


class Guide:
    def __init__(self):
        self.chosen = None
        self.challenger = None
        self.frames = 0
        self.cold_frames = 0

    def settle(self, tally):
        """tally: {resource: binds}, kept across presents unless cleared. Mirrors SettleGuide."""
        if not tally:
            tally.clear()
            return
        best = max(tally, key=lambda r: tally[r])
        if best == self.chosen:
            self.challenger, self.frames = None, 0
            tally.clear()
            return

        if self.chosen is None:
            self.cold_frames += 1
            if self.cold_frames < HOLD:
                return  # deliberately not cleared: leaving it standing is what accumulates
            self.cold_frames = 0
        elif best != self.challenger:
            self.challenger, self.frames = best, 1
            tally.clear()
            return
        else:
            self.frames += 1
            if self.frames < HOLD:
                tally.clear()
                return

        self.chosen, self.challenger, self.frames = best, None, 0
        tally.clear()


def run(frames, clear_each=False):
    """Each entry is one present's binds. The tally is the add-on's, and lives across presents."""
    g = Guide()
    tally = {}
    for present in frames:
        for res, binds in present.items():
            tally[res] = tally.get(res, 0) + binds
        g.settle(tally)
        if clear_each:
            tally.clear()
    return g.chosen


scene, odd, other = "scene", "odd", "other"

# 1. Nothing chosen yet: still three presents of evidence before anything is taken.
assert run([{scene: 96440}]) is None
assert run([{scene: 96440}] * 2) is None
assert run([{scene: 96440}] * 3) == scene

# 2. The first reported bug. One frame without the scene pass must not hand the guide away.
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

# 7. The second reported bug: an engine that rotates its depth targets. Nothing is ever presented
#    three times running, and under the streak rule alone the slot stayed empty forever.
assert run([{"depthA": 96440}, {"depthB": 96440}] * 50) in ("depthA", "depthB")
assert run([{"depthA": 9}, {"depthB": 9}, {"depthC": 9}] * 33) in ("depthA", "depthB", "depthC")

# 8. Rotation does not cost the scene pass its win: a shadow map bound in every present still
#    loses to the scene pass, which outbinds it by an order of magnitude.
assert run([{scene: 96440, "shadow": 900}, {scene: 96440, "shadow": 900}] * 5) == scene

# 9. And a cold start is still not decided by one odd present on its own.
assert run([{odd: 9}] + [{scene: 96440}] * 2) == scene

# 10. The tally has to stand across those three presents. The 32-bit bridge used to clear it at the
#     end of every present, which left the third present alone to decide the cold start.
assert run([{scene: 96440}] * 2 + [{odd: 9}]) == scene
assert run([{scene: 96440}] * 2 + [{odd: 9}], clear_each=True) == odd

print("guide switching: 10 properties hold")
