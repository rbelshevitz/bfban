# TCP queues, windows, and memory during an incident

`bfban` is an L3/L4 deny-list. It does not change TCP window advertisements,
socket buffers, congestion control, or application accept loops. Those are
separate kernel/application controls and must be measured before changing.

| Area | Kernel control | Meaning | Operational rule |
|---|---|---|---|
| Receive autotuning | `net.ipv4.tcp_moderate_rcvbuf`, `tcp_rmem` | TCP receive-buffer growth | Keep autotuning enabled unless a controlled test proves otherwise |
| Send buffers | `tcp_wmem` | Automatically selected send-buffer limits | Do not raise globally just because traffic is high |
| Half-open handshakes | `tcp_max_syn_backlog` | Per-listener SYN_RECV queue | Tune together with backlog and memory |
| Completed accept queue | `net.core.somaxconn` | Upper bound for `listen(2)` backlog | Verify the application passes a larger backlog |
| SYN flood fallback | `tcp_syncookies` | Cookies when the SYN queue overflows | `1` is the normal fallback; do not force `2` in production |
| Global TCP memory | `tcp_mem` | Page limits and memory pressure | Record values and RAM size before changing |

Linux documents that receive autotuning grows up to `tcp_rmem[2]`, while a
SYN_RECV request consumes approximately 304 bytes. It also warns that
syncookies are a fallback and can disable TCP extensions or harm valid traffic
when used as a substitute for capacity tuning. See the kernel
[`ip-sysctl` documentation](https://docs.kernel.org/networking/ip-sysctl.html).

Record current values first:

```sh
sysctl net.core.somaxconn net.core.netdev_max_backlog \
  net.ipv4.tcp_max_syn_backlog net.ipv4.tcp_syncookies \
  net.ipv4.tcp_moderate_rcvbuf net.ipv4.tcp_rmem \
  net.ipv4.tcp_wmem net.ipv4.tcp_mem
ss -s
ss -ant state syn-recv
nstat -az | grep -E 'TcpExt|Listen|Syncookies|TCP' || true
cat /proc/meminfo
```

If the source is abusive, apply an exact IP or CIDR ban. If distributed
traffic saturates the uplink, host-level TCP tuning cannot recover the link;
use upstream filtering, a provider ACL, or a scrubbing service.

Change one control at a time and re-test valid clients, including window
scaling, SACK, timestamps, and long-lived connections. Do not use
`tcp_syncookies=2` as a permanent setting, and do not make TCP buffers or
backlogs large by default: each queue has a memory cost.
