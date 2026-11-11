<?php
/* netlog_listen.php PORT OUT_FILE -- same as netlog_listen.py, for PCs whose
 * firewall blocks python.exe inbound but allows php-cgi.exe:
 *   php-cgi -q -d max_execution_time=0 -f netlog_listen.php 9999 netlog.txt
 * Receives the [debug] netlog datagrams of mvlo_nx (nx_netbeat.c), prints them
 * with the PC's time and appends them to OUT_FILE; says so when they stop. */
set_time_limit(0);
$port = isset($argv[1]) ? (int)$argv[1] : 9999;
$out = isset($argv[2]) ? fopen($argv[2], "a") : null;
$sock = stream_socket_server("udp://0.0.0.0:$port", $errno, $errstr, STREAM_SERVER_BIND);
if (!$sock) { fwrite(STDERR, "bind failed: $errstr\n"); exit(1); }
echo "listening on UDP $port\n"; flush();
$last = null;
while (true) {
  $r = [$sock]; $w = null; $e = null;
  if (stream_select($r, $w, $e, 5) === 0) {
    if ($last !== null) {
      $msg = date("H:i:s") . " -- nothing for " . (time() - $last) . " s\n";
      echo $msg; flush();
      if ($out) { fwrite($out, $msg); fflush($out); }
    }
    continue;
  }
  $data = stream_socket_recvfrom($sock, 2048, 0, $peer);
  $last = time();
  $line = date("H:i:s") . " $peer $data\n";
  echo $line; flush();
  if ($out) { fwrite($out, $line); fflush($out); }
}
