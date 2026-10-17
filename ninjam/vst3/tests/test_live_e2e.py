"""End-to-end test of Live mode in the VST3 plug-in, run by ctest (see CMakeLists.txt).

test_live_latency: plugin_e2e_host loads two instances of Jamsa.vst3 in one process and runs them
in step, as two tracks of a DAW (48 kHz, 270-sample blocks, in real time: with longer blocks
the plug-in raises the jitter buffer, see Engine::liveBufferFor(); and an interval, 96000
samples, ends inside a block, where NJClient switches between Live and intervals). Alice hosts the
server (the plug-in's own, with Live: UDP on the room's port) and plays short chirps at uneven
gaps; bob joins and plays nothing; an NJClient without Live ("old") joins too. The host finds
each chirp in what bob hears by cross-correlation:
  - before Live, through intervals (about an interval late), for bob and for old;
  - alice types !live on: bob hears her live (well under 100 ms late), on his output Player N
    (hers) and on no other, with the intervals' copy of her muted; old, which can't play Live,
    still hears her through intervals;
  - bob's jitter buffer goes from 20 to 40 ms: still live, and he hears her later by about as
    much, as his estimate of it says; then back to 20;
  - bob's DAW misses 32 ms once (as in an xrun): the glitch makes a dropout, so he goes back to
    intervals for a while and hears her there; he is live again after it, and then plays well
    (no dropouts, the delay steady);
  - alice turns Live off and on again within one of bob's intervals: he hears her intervals at
    once and on to that interval's end, without a gap, and live again only from an interval's
    start on;
  - alice turns Live off: bob hears her through intervals again at once.
The test prints the Live delay measured at the default 20 ms jitter buffer (median and 95th
percentile). Where the test machine was busy (its blocks late, dropouts, bob back on intervals
for a while, as with a DAW that is late), a phase isn't judged; the host measures a phase again
when too little of it is left, or when it had dropouts that the hosts didn't cause (up to three
times), and the test wants at least 60% of each, and no dropouts the hosts didn't cause.

Run by hand:
  NINJAM_VST3_PLUGIN=build/VST3/Release/Jamsa.vst3 \\
  NINJAM_VST3_E2E_HOST=build/bin/Release/plugin_e2e_host \\
  python3 -m unittest -v test_live_e2e
or the host alone: plugin_e2e_host build/VST3/Release/Jamsa.vst3 48000 270 live
(NINJAM_E2E_DEBUG=1 prints more, NINJAM_E2E_LIVE_BUFFER=N starts both at N ms).
"""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_plugin_e2e import HOST, PLUGIN, Run, fields  # noqa: E402

# the hosts' block: short enough for the default jitter buffer, and not a divisor of the interval
BLOCK = 270
# of the chirps alice plays, how many bob must find (and old, through intervals)
MIN_FOUND = 0.95
# of the chirps of a phase, how many must be where bob heard alice live without dropouts
MIN_CLEAN = 0.6
# how late bob may hear alice live at the default jitter buffer (ms): median, 95th percentile
MAX_LIVE_MEDIAN_MS, MAX_LIVE_P95_MS = 100.0, 150.0
# how much later than his estimate bob may hear alice (ms; the estimate leaves out where the
# host's blocks fall, up to a block later), where the test machine kept time
MIN_OVER_ESTIMATE_MS, MAX_OVER_ESTIMATE_MS = -5.0, 10.0
# how much the delay may move within 10 s, after bob's DAW missed 32 ms (ms)
MAX_AFTER_STALL_SPREAD_MS = 2.0


def found(f, key="found"):
    n, m = f[key].split("/")
    return int(n), int(m)


@unittest.skipUnless(PLUGIN and HOST, "set NINJAM_VST3_PLUGIN and NINJAM_VST3_E2E_HOST")
class LiveEndToEndTest(unittest.TestCase):
    def assertFound(self, f, key, log, what):
        n, m = found(f, key)
        self.assertGreater(m, 0, what + ": no chirps to look for\n" + log)
        self.assertGreaterEqual(n, MIN_FOUND * m, "%s: found %d of %d chirps\n%s" % (what, n, m, log))

    def test_live_latency(self):
        run = Run(["live"], block=BLOCK)
        lines = {}  # "ready", "intervals", "on", "live", "buffer40", "stall", "recovered", "afterstall", "toggle", "off" -> fields

        def on_line(line):
            m = re.match(r"LIVE (ready|intervals|on|recovered|toggle|off) (.*)$", line)
            if m:
                lines[m.group(1)] = fields(m.group(2))
            m = re.match(r"LIVE phase (\w+) (.*)$", line)
            if m:
                lines[m.group(1)] = fields(m.group(2))
            m = re.match(r"live status \(bob\): (.*)$", line)
            if m:
                lines["bob_status"] = fields(m.group(1))
        try:
            run.run_to_end(on_line, timeout=480)
        finally:
            run.close()

        log = run.log()
        self.assertEqual(run.proc.returncode, 0, log)
        self.assertIn("DONE", run.output, log)
        ready, iv, on = lines.get("ready", {}), lines.get("intervals", {}), lines.get("on", {})
        live, b40, off = lines.get("live", {}), lines.get("buffer40", {}), lines.get("off", {})

        # the hosted room offers Live, and starts with it off
        self.assertEqual((ready.get("b_offered"), ready.get("b_room")), ("1", "off"), log)
        self.assertEqual(ready.get("b_peers"), "1", "bob sees alice as a Live player (not old)\n" + log)

        # before Live: intervals, for both
        self.assertFound(iv, "found", log, "bob through intervals")
        self.assertFound(iv, "old_found", log, "old through intervals")
        iv_lag = float(iv["lag_ms"])
        self.assertGreater(iv_lag, 500.0, log)

        # !live on: bob hears alice live, and his users show it
        self.assertEqual(on.get("b_live"), "1", log)
        self.assertEqual(on.get("marker"), "1", "\" \xb7 live\" on alice's channel in bob's users\n" + log)

        for name, f in (("live", live), ("buffer40", b40)):
            self.assertTrue(f, "no phase %s\n%s" % (name, log))
            what = "phase " + name
            # live at the end, unless the hosts were late in that attempt: the machine busy makes both
            # of them late, bob's jitter buffer runs dry, and he may rightly be back on intervals then
            if f.get("late_blocks") == "0":
                self.assertEqual(f.get("b_live"), "1", what + "\n" + log)
            # dropouts that the hosts didn't cause (they were on time): in each attempt, not just
            # in the quarters that the clean chirps skip
            self.assertEqual(f.get("ontime"), "0", what + ": dropouts while the hosts were on time\n" + log)
            n, m = found(f, "clean")
            self.assertGreaterEqual(n, MIN_CLEAN * m, "%s: only %d of %d chirps where bob heard alice live without "
                                    "dropouts\n%s" % (what, n, m, log))
            self.assertFound(f, "found", log, what + ": bob live")
            self.assertLess(float(f["median_ms"]), MAX_LIVE_MEDIAN_MS, what + "\n" + log)
            self.assertLess(float(f["p95_ms"]), MAX_LIVE_P95_MS, what + "\n" + log)
            self.assertGreater(float(f["mean_corr"]), 0.8, what + "\n" + log)
            # the intervals' copy of alice: muted while she plays live
            heard, checked = (int(x) for x in f["interval_copy"].split("/"))
            self.assertGreater(checked, 5, what + "\n" + log)
            self.assertEqual(heard, 0, what + ": bob also hears alice's intervals\n" + log)
            self.assertLess(float(f["outside"]), 0.01, what + ": bob hears more than her live copy\n" + log)
            # her output Player N plays her live, the other player outputs nothing
            self.assertGreaterEqual(int(f["player"]), 1, what + "\n" + log)
            self.assertFound(f, "player_found", log, what + ": bob's output Player " + f["player"])
            self.assertLess(float(f["other_players_peak"]), 0.001, what + "\n" + log)
            # old has no Live: intervals, as before
            self.assertFound(f, "old_found", log, what + ": old through intervals")
            self.assertGreater(float(f["old_lag_ms"]), 500.0, what + "\n" + log)

        # bob's DAW missed 32 ms: back on intervals for a while (the glitch makes a dropout: this is
        # where the test sees a player go back to intervals; if Live ever rides out the glitch,
        # that needs another cause), where he hears alice's intervals; live again after it
        rec, stall, aft = lines.get("recovered", {}), lines.get("stall", {}), lines.get("afterstall", {})
        self.assertTrue(stall, "no phase stall\n" + log)
        self.assertEqual(rec.get("fell_back"), "1", "bob did not go back to intervals after his DAW missed 32 ms\n" + log)
        self.assertFound(stall, "fallback_found", log, "back on intervals after the stall: bob through intervals")
        self.assertEqual(rec.get("b_live"), "1", "bob is not live again after his DAW missed 32 ms\n" + log)
        # ... and then he plays well: no dropouts, and the delay doesn't drift (judged where the
        # test machine kept time)
        self.assertTrue(aft, "no phase afterstall\n" + log)
        if aft.get("late_blocks") == "0":
            what = "10 s after bob's DAW missed 32 ms"
            self.assertEqual((aft.get("dropouts"), aft.get("stayed_live")), ("0", "1"), what + "\n" + log)
            n, m = found(aft, "clean")
            self.assertGreaterEqual(n, MIN_CLEAN * m, what + "\n" + log)
            self.assertFound(aft, "found", log, what)
            self.assertLess(float(aft["median_ms"]), MAX_LIVE_MEDIAN_MS, what + "\n" + log)
            spread = float(aft["max_ms"]) - float(aft["min_ms"])
            self.assertLess(spread, MAX_AFTER_STALL_SPREAD_MS, what + ": the delay moved by %.1f ms\n%s" % (spread, log))
        else:
            print("after the stall: the test machine was late, not judged", flush=True)

        # the default jitter buffer is the one in use (with these short blocks)
        st = lines.get("bob_status", {})
        self.assertEqual((st.get("buffer"), st.get("inuse"), st.get("hostblock")), ("20", "20", str(BLOCK)), log)

        # a 40 ms jitter buffer: bob's estimate goes up by about 20 ms ...
        rise = float(b40["estimate_ms"]) - float(live["estimate_ms"])
        self.assertTrue(12.0 < rise < 28.0, "estimate %s -> %s ms\n%s" % (live["estimate_ms"], b40["estimate_ms"], log))
        # ... and so does what he hears, as late as the estimate says, give or take a block. Judged
        # where the test machine kept time from the start (after a late block, AOO's clock filter
        # moves the delay for a while)
        if all(f.get("late_blocks") == "0" and f.get("late_before", "").startswith("0/") for f in (live, b40)):
            heard = float(b40["median_ms"]) - float(live["median_ms"])
            self.assertTrue(12.0 < heard < 28.0, "heard %s -> %s ms\n%s" % (live["median_ms"], b40["median_ms"], log))
            for name, f in (("live", live), ("buffer40", b40)):
                over = float(f["median_ms"]) - float(f["estimate_ms"])
                self.assertTrue(MIN_OVER_ESTIMATE_MS < over < MAX_OVER_ESTIMATE_MS, "phase %s: heard %s ms, estimate %s ms\n%s"
                                % (name, f["median_ms"], f["estimate_ms"], log))
        else:
            print("the delay heard against the estimate: not judged (the test machine was late)", flush=True)

        # Live off and on within one interval: intervals at once, all of them to the interval's end
        # (no gap), and live only from an interval's start on
        tg = lines.get("toggle", {})
        self.assertTrue(tg, "no toggle\n" + log)
        self.assertGreaterEqual(float(tg["off_room_s"]), 0.0, "bob's room didn't go off\n" + log)
        self.assertEqual((tg.get("live_while_off"), tg.get("marker_off")), ("0", "0"),
                         "bob shows alice live while the room's Live is off\n" + log)
        self.assertEqual(tg.get("early_live"), "0", "bob live again before his interval ended\n" + log)
        n, m = found(tg, "interval_found")
        self.assertGreaterEqual(m, 2, "toggle: no interval copies to look for\n" + log)
        self.assertGreaterEqual(n, MIN_FOUND * m, "toggle: bob found %d of alice's %d interval copies\n%s" % (n, m, log))
        self.assertEqual(tg.get("live_again"), "1", "bob not live again after Live came back on\n" + log)

        # Live off: intervals again at once, at the lag from before
        self.assertEqual((off.get("b_live"), off.get("b_room")), ("0", "off"), log)
        self.assertFound(off, "found", log, "Live off: bob through intervals")

        print("Live delay at the default 20 ms jitter buffer: median %s ms, p95 %s ms (bob's estimate %s ms); "
              "at 40 ms: median %s ms, p95 %s ms" % (live["median_ms"], live["p95_ms"], live["estimate_ms"],
                                                    b40["median_ms"], b40["p95_ms"]), flush=True)


if __name__ == "__main__":
    unittest.main()
