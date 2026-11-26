#!/usr/bin/env python3
"""Debugging reloaded code from VS Code: drives VS Code's debug adapters over the Debug
Adapter Protocol as VS Code does (launch, breakpoints by full path, stop, stack, variables)
on tests/reload/cases/debug_frames. Run by run_debugger_tests.sh with the host it built.

    vscode_debug.py HOST ZC CASE RUN_DIR

Each adapter found among the installed VS Code extensions (~/.vscode/extensions, or
VSCODE_EXTENSIONS) is tried: CodeLLDB (lldb) and the C/C++ extension (cppdbg, with gdb).
Prints a [PASS] or [FAIL] line a check and "vscode: N passed, M failed"; exits 1 on a failure.
An adapter that isn't installed is skipped.
"""
import glob, json, os, queue, shutil, socket, subprocess, sys, tempfile, threading, time

host, zc, case, run = sys.argv[1:5]
WINDOWS = os.name == "nt"
EXT = os.environ.get("VSCODE_EXTENSIONS") or os.path.expanduser("~/.vscode/extensions")
LINE = next(i + 1 for i, l in enumerate(open(os.path.join(case, "step1.zc"))) if "return doubled;" in l)


def newest(pattern):
    found = sorted(glob.glob(os.path.join(EXT, pattern)))
    return found[-1] if found else None


class Adapter:
    def __init__(self, kind):
        self.kind = kind
        self.seq = 0
        self.events = queue.Queue()
        self.responses = {}
        self.cond = threading.Condition()
        self.proc = None

    def start(self):
        exe = ".exe" if WINDOWS else ""
        if self.kind == "lldb":
            base = newest("vadimcn.vscode-lldb-*")
            if not base:
                return "CodeLLDB isn't installed"
            lib = os.path.join(base, "lldb", "bin" if WINDOWS else "lib", "liblldb." + ("dll" if WINDOWS else "so"))
            s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
            self.proc = subprocess.Popen([os.path.join(base, "adapter", "codelldb" + exe), "--liblldb", lib,
                                          "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(200):
                try:
                    sock = socket.create_connection(("127.0.0.1", port))
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                return "CodeLLDB's adapter didn't start"
            self.rf, self.wf = sock.makefile("rb"), sock.makefile("wb")
            self.launch = {"type": "lldb", "request": "launch", "program": host, "args": [zc, case, run], "cwd": run}
        else:
            base = newest("ms-vscode.cpptools-*-" + ("win32" if WINDOWS else "linux") + "-*")
            gdb = shutil.which("gdb")
            if not base:
                return "the C/C++ extension isn't installed"
            if not gdb:
                return "no gdb"
            self.proc = subprocess.Popen([os.path.join(base, "debugAdapters", "bin", "OpenDebugAD7" + exe)],
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            self.rf, self.wf = self.proc.stdout, self.proc.stdin
            self.launch = {"type": "cppdbg", "request": "launch", "program": host, "args": [zc, case, run],
                           "cwd": run, "MIMode": "gdb", "miDebuggerPath": gdb, "externalConsole": False}
        threading.Thread(target=self.read, daemon=True).start()
        return None

    def read(self):
        while True:
            h = b""
            while not h.endswith(b"\r\n\r\n"):
                c = self.rf.read(1)
                if not c:
                    return
                h += c
            n = int([l for l in h.decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
            m = json.loads(self.rf.read(n))
            if m["type"] == "event":
                self.events.put(m)
            elif m["type"] == "response":
                with self.cond:
                    self.responses[m["request_seq"]] = m
                    self.cond.notify_all()

    def req(self, cmd, args=None, t=30):
        self.seq += 1
        s = self.seq
        b = json.dumps({"seq": s, "type": "request", "command": cmd, "arguments": args or {}}).encode()
        self.wf.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
        self.wf.flush()
        with self.cond:
            self.cond.wait_for(lambda: s in self.responses, timeout=t)
            return self.responses.get(s) or {}

    def wait(self, name, t):
        end = time.time() + t
        while time.time() < end:
            try:
                e = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if e["event"] == name:
                return e
        return None

    def stop(self):
        try:
            self.req("disconnect", {"terminateDebuggee": True}, t=5)
        except Exception:
            pass
        if self.proc:
            self.proc.kill()


passed = failed = 0


def check(what, ok):
    global passed, failed
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}")
    if ok:
        passed += 1
    else:
        failed += 1


def same_file(a, b):
    return a and os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


for kind in ("lldb", "cppdbg"):
    name = {"lldb": "CodeLLDB", "cppdbg": "C/C++ (gdb)"}[kind]
    a = Adapter(kind)
    why = a.start()
    if why:
        print(f"  [SKIP] {name}: {why}")
        continue
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    try:
        a.req("initialize", {"adapterID": kind, "clientID": "vscode", "linesStartAt1": True,
                             "columnsStartAt1": True, "pathFormat": "path"})
        a.req("launch", a.launch)
        a.wait("initialized", 30)
        for step in ("step1.zc", "step2.zc"):
            a.req("setBreakpoints", {"source": {"path": os.path.join(case, step)}, "breakpoints": [{"line": LINE}]})
        a.req("configurationDone")
        for n, step in ((1, "step1.zc"), (2, "step2.zc")):
            e = a.wait("stopped", 120)
            if not e:
                check(f"{name}: a breakpoint by full path stops in version {n}", False)
                break
            tid = e["body"].get("threadId")
            frames = a.req("stackTrace", {"threadId": tid, "levels": 4}).get("body", {}).get("stackFrames", [])
            top = frames[0] if frames else {}
            check(f"{name}: a breakpoint by full path stops in version {n} ({step}:{LINE})",
                  same_file(top.get("source", {}).get("path"), os.path.join(case, step)) and top.get("line") == LINE)
            names = [f["name"].split("(")[0] for f in frames]
            check(f"{name}: its stack goes inner, big, reload_step, zr_call", names[:4] == ["inner", "big", "reload_step", "zr_call"])
            scopes = a.req("scopes", {"frameId": top.get("id", 0)}).get("body", {}).get("scopes", [])
            vs = a.req("variables", {"variablesReference": scopes[0]["variablesReference"]}).get("body", {}).get("variables", []) if scopes else []
            vals = {v["name"]: v["value"] for v in vs}
            check(f"{name}: an argument and a local read right", vals.get("x") == "10" and vals.get("doubled") == "20")
            a.req("continue", {"threadId": tid})
        check(f"{name}: the program runs to its end", a.wait("terminated", 120) is not None)
    finally:
        a.stop()

print(f"vscode: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
