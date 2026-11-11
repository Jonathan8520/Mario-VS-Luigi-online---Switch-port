<?php
/* udp_echo.php [PORT] [OUT_FILE] -- a tiny UDP echo server for the console's
 * [debug] udp_soak test (nx_net.c). The .nro sends ~20 small datagrams a
 * second to this PC and polls for the reply; this bounces each datagram back
 * to its sender and (optionally) logs a one-line summary. In PHP so a Windows
 * firewall that blocks python.exe inbound but allows php-cgi.exe still works,
 * exactly like netlog_listen.php:
 *   php-cgi -q -d max_execution_time=0 -f udp_echo.php 9998 udp_soak.txt
 * or, with a plain php CLI:
 *   php udp_echo.php 9998 udp_soak.txt
 * Point the console at this PC with, in mvlo.ini:
 *   [debug]
 *   udp_soak = <this PC's LAN IP>:9998
 * Then watch: it should print a steady ~20 packets/s. If the console freezes,
 * the packets stop; the PC's clock on the last line dates the freeze. */
set_time_limit(0);
$port = isset($argv[1]) ? (int)$argv[1] : 9998;
$out  = isset($argv[2]) ? fopen($argv[2], "a") : null;

$sock = stream_socket_server("udp://0.0.0.0:$port", $errno, $errstr, STREAM_SERVER_BIND);
if (!$sock) { fwrite(STDERR, "bind failed: $errstr\n"); exit(1); }
echo "udp echo listening on UDP $port\n"; flush();

$count = 0;      /* datagrams since the last summary line */
$bytes = 0;
$tick  = time();
$last  = null;   /* time of the last datagram received */

while (true) {
  $r = [$sock]; $w = null; $e = null;
  /* Wake at least once a second to print the rate and to notice silence. The
   * summary must not wait for a quiet second: under steady traffic select
   * never times out (02/11: no line for 24 min while echoing 20 pkt/s). */
  $ready = stream_select($r, $w, $e, 1);
  $now = time();
  if ($now - $tick >= 1) {
    $line = date("H:i:s") . " -- $count pkt/s, $bytes B"
          . ($last !== null && $now - $last >= 2 ? "  (SILENT " . ($now - $last) . " s)" : "") . "\n";
    echo $line; flush();
    if ($out) { fwrite($out, $line); fflush($out); }
    $count = 0; $bytes = 0; $tick = $now;
  }
  if (!$ready) continue;
  $data = stream_socket_recvfrom($sock, 2048, 0, $peer);
  if ($data === false || $data === "") continue;
  /* Echo it straight back to the sender. */
  stream_socket_sendto($sock, $data, 0, $peer);
  $count++;
  $bytes += strlen($data);
  $last = time();
}
