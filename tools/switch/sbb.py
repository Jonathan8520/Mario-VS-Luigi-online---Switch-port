"""sbb.py -- talk to sys-botbase on the Switch (TCP 6000).

  python -I sbb.py getVersion            any command; prints the reply if it has one
  python -I sbb.py click A               buttons: A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE DUP DDOWN DLEFT DRIGHT LSTICK RSTICK
  python -I sbb.py seq "+R,W200,A,W500,-R"   clickSeq (W = wait ms, +X press, -X release)
  python -I sbb.py shot out.jpg          screenshot (pixelPeek) to a JPG
  python -I sbb.py ping                  1 if sys-botbase answers within 5 s (works during a freeze test)
"""
import socket, sys, time

HOST, PORT = "192.168.1.98", 6000
REPLY = {"getVersion", "getTitleID", "getTitleVersion", "getSystemLanguage", "getBuildID", "getHeapBase",
         "getMainNsoBase", "isProgramRunning", "game", "charge", "pixelPeek"}


def conn(timeout=5):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    return s


def cmd(s, line, reply=None):
    s.sendall((line + "\r\n").encode())
    if reply is None:
        reply = line.split()[0] in REPLY
    if not reply:
        return None
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(1 << 16)
        if not chunk:
            break
        buf += chunk
    return buf.strip().decode(errors="replace")


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "ping":
        t = time.time()
        try:
            with conn(5) as s:
                v = cmd(s, "getVersion")
            print(f"1 {v} {int((time.time() - t) * 1000)} ms")
        except OSError as e:
            print(f"0 {e}")
        return 0
    with conn(30 if argv[0] == "shot" else 10) as s:
        if argv[0] == "shot":
            hexjpg = cmd(s, "pixelPeek")
            data = bytes.fromhex(hexjpg)
            open(argv[1], "wb").write(data)
            print(f"{argv[1]}: {len(data)} bytes")
        elif argv[0] == "seq":
            cmd(s, "clickSeq " + argv[1], reply=False)
            time.sleep(0.2)
        else:
            r = cmd(s, " ".join(argv))
            if r is not None:
                print(r)
            else:
                time.sleep(0.2)   # let the module act before the socket closes
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
