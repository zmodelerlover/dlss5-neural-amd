r"""Attach ngx_param_trace2.js to the game and write the trace to a file, non-interactively.

    python run_trace.py --out out\ngx_params.log --cmd out\cmd.txt

The frida CLI is a REPL: with no live stdin it reads EOF and exits, so it cannot be driven
from a scripted session. This does the same job through the frida Python API and adds the
one thing the protocol needs -- a way to stamp the log at each step. Append a line to the
command file and it is passed to the script's mark() export, which writes a banner and a
full snapshot of every DLSSNR.* parameter at that instant:

    Add-Content out\cmd.txt 'S1 Model A -> Model B'
    Add-Content out\cmd.txt 'STOP'
"""
import argparse
import os
import sys
import time

import frida

LEVELS = {"info": " ", "warning": "W", "error": "E", "debug": "D"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", default="eurotrucks2.exe")
    ap.add_argument("--script", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "ngx_param_trace2.js"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--timeout", type=float, default=7200.0, help="give up after this many seconds")
    a = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    log = open(a.out, "a", encoding="utf-8", buffering=1, newline="\n")

    def w(line):
        log.write(line + "\n")
        log.flush()

    w("")
    w("=" * 78)
    w("# run_trace.py  start %s  frida %s  process %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), frida.__version__, a.process))
    w("=" * 78)

    try:
        session = frida.attach(a.process)
    except Exception as e:
        w("!! attach failed: %r" % (e,))
        print("ATTACH-FAILED: %r" % (e,), file=sys.stderr)
        return 2

    detached = {"why": None}
    session.on("detached", lambda reason, *rest: detached.update(why=reason))

    with open(a.script, "r", encoding="utf-8") as f:
        source = f.read()
    script = session.create_script(source)

    def on_log(level, text):
        w(("%s| %s" % (LEVELS.get(level, "?"), text)) if level != "info" else text)

    def on_message(message, data):
        if message.get("type") == "error":
            w("!! script error: %s" % message.get("description"))
            st = message.get("stack")
            if st:
                for ln in st.splitlines():
                    w("!!   " + ln)
        elif message.get("type") == "send":
            w("-> %r" % (message.get("payload"),))

    script.set_log_handler(on_log)
    script.on("message", on_message)
    script.load()
    api = script.exports_sync

    print("ATTACHED pid-ok; writing %s" % a.out)
    open(a.cmd, "a", encoding="utf-8").close()

    consumed = 0
    t0 = time.time()
    rc = 0
    try:
        while True:
            if detached["why"]:
                w("!! detached: %s" % detached["why"])
                rc = 3
                break
            if time.time() - t0 > a.timeout:
                w("!! timeout after %.0fs" % (time.time() - t0))
                rc = 4
                break
            try:
                with open(a.cmd, "r", encoding="utf-8") as f:
                    lines = f.read().splitlines()
            except OSError:
                lines = []
            while consumed < len(lines):
                line = lines[consumed].strip()
                consumed += 1
                if not line:
                    continue
                if line.upper() == "STOP":
                    w("# STOP requested at %s" % time.strftime("%H:%M:%S"))
                    raise KeyboardInterrupt
                if line.upper() == "SNAP":
                    api.snap()
                    continue
                if line.upper() == "STATUS":
                    w("# status: %r" % (api.status(),))
                    continue
                api.mark(line)
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            w("# final status: %r" % (api.status(),))
        except Exception:
            pass
        w("# run_trace.py  end %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
        try:
            script.unload()
        except Exception:
            pass
        try:
            session.detach()
        except Exception:
            pass
        log.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
