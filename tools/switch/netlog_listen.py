"""netlog_listen.py [PORT] [OUT_FILE] -- receive the [debug] netlog datagrams
of mvlo_nx (nx_netbeat.c) and print them with the PC's time; append them to
OUT_FILE too. When the console freezes, the last lines say whether the game
was still alive and what it was waiting on; a gap means the console stopped.
usage: python -I netlog_listen.py 9999 netlog.txt
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
out = open(sys.argv[2], "a", encoding="utf-8") if len(sys.argv) > 2 else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
s.settimeout(5)
print(f"listening on UDP {port}", flush=True)
last = None
while True:
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        if last is not None:
            msg = f"{time.strftime('%H:%M:%S')} -- nothing for {time.time() - last:.0f} s"
            print(msg, flush=True)
            if out:
                out.write(msg + "\n"); out.flush()
        continue
    last = time.time()
    line = f"{time.strftime('%H:%M:%S')} {addr[0]} {data.decode('utf-8', 'replace')}"
    print(line, flush=True)
    if out:
        out.write(line + "\n"); out.flush()
