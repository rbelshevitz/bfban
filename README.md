# bfban

[English](README.md) | [Русский](README.ru.md)

`bfban` is a small GPL-2.0-only dynamic ban-list manager for Linux
`bpfilter`. It is an enforcement backend for Fail2ban, CrowdSec, or another
security agent. It does not parse logs and does not decide whether an IP is
malicious.

```text
Fail2ban / CrowdSec / security agent
                 |
                 v
        bfband Unix-socket daemon
                 |
                 v
       bpfilter chain and BPF sets
                 |
                 v
             packet DROP
```

The current MVP provides:

- `bfban`: IPv4, IPv6, and CIDR management;
- `bfband`: Unix-socket daemon, state recovery, and optional TTLs;
- `bfbandctl`: the Fail2ban-facing client;
- `bfobserve`: a read-only AF_PACKET top-N traffic observer;
- systemd and Fail2ban integration files.

When `libbpfilter` is available through `pkg-config`, the daemon updates sets
through the direct C API. If it is unavailable, it falls back to the installed
`bfban`/`bfcli` tools. Select the behavior with `BFBAN_DIRECT=auto|yes|no`.

## Why this exists

Fail2ban is good at detection and ban lifetime management. A firewall backend
should keep a stable filtering program and update its data, instead of adding
one long rule chain per banned address. `bfban` uses four sets:

```text
banlist4      exact IPv4 addresses
banlist4net   IPv4 CIDR networks
banlist6      exact IPv6 addresses
banlist6net   IPv6 CIDR networks
```

This is a dynamic L3/L4 deny-list layer, not a replacement for stateful
nftables/iptables policy, NAT, conntrack, or upstream DDoS scrubbing.

## Step-by-step deployment

### 1. Check the host

Use the running kernel and actual interfaces, not only the distribution label:

```sh
uname -r
pkg-config --modversion libbpf
mountpoint /sys/fs/bpf
sudo bfobserve --list-interfaces
```

The current target is Linux 6.6+, libbpf 1.2+, and a bpfilter build whose
selected hooks work on that kernel. Ubuntu 24.04 (6.8), Debian 13 (6.12),
Fedora 40 (6.8), RHEL/Rocky/Alma 10 (6.12), and Ubuntu 22.04 with HWE 6.8
are examples of matching base kernels. RHEL/Rocky/Alma 9 (5.14) and Debian
12 (6.1) need a newer supported kernel or backport. Verify compatibility
before production use.

### 2. Build and install

```sh
cd ~/bfban
./configure --prefix=/usr/local
make
sudo make install
```

The installation expects `bfcli` at `/usr/local/sbin/bfcli`. Set
`BFBAN_BPFCLI` in `/etc/bfban/bfban.env` if it is elsewhere.

Run `./configure` after installing bpfilter so it can discover the
`libbpfilter` pkg-config file. Direct mode removes the per-ban `fork()`/`exec()`
path; it does not by itself prove a packet-processing throughput improvement.

The bpfilter engine and C library are documented in the official
[`libbpfilter` documentation](https://bpfilter.io/developers/modules/libbpfilter/libbpfilter.html).
The upstream source is the [bpfilter GitHub repository](https://github.com/facebook/bpfilter).

### 3. Start the systemd daemon

`make install` installs `bfband.service` to `/etc/systemd/system`. The unit
creates `/run/bfban` for the socket and `/var/lib/bfban` for state, runs as
root because the current backend loads/updates BPF objects, and starts before
Fail2ban.

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now bfband.service
sudo systemctl status bfband.service
sudo journalctl -u bfband.service -b --no-pager
sudo bfbandctl ping
```

The socket is `/run/bfban/bfban.sock` with mode `0660`. The current unit uses
`root:root`. A future package should add a dedicated group and stricter
capability policy only after testing bpfilter's needs. Do not blindly enable
`ProtectSystem=strict` or drop `CAP_BPF`/`CAP_NET_ADMIN`; these may prevent
object loading or updates. See the official
[`systemd.exec` manual](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec).

### 4. Create and test the chain

```sh
sudo bfbandctl init
sudo bfban add 203.0.113.10
sudo bfban list
sudo bfcli chain get --name bfban_input
sudo bfban remove 203.0.113.10
```

Use documentation-only addresses such as `203.0.113.0/24` for a dry run.
On a real host, test a disposable source and keep an out-of-band console or
second SSH session. `bfban flush` removes the dynamic chain contents.

### 5. Connect Fail2ban

The installed action maps Fail2ban events to the daemon:

```text
actionstart -> bfbandctl init
actionban   -> bfbandctl add <ip>
actionunban -> bfbandctl remove <ip>
actionstop  -> bfbandctl flush
```

Create `/etc/fail2ban/jail.d/sshd-bfban.local`:

```ini
[sshd-bfban]
enabled = true
filter = sshd
backend = systemd
port = ssh
maxretry = 5
findtime = 10m
bantime = 1h
banaction = bfban
```

Then validate it:

```sh
sudo fail2ban-client reload
sudo fail2ban-client status sshd-bfban
sudo bfbandctl list
sudo journalctl -u fail2ban -u bfband -f
```

Fail2ban owns log filtering, thresholds, allowlists, and ban expiry. The MVP
has one global chain/state file, so multiple jails may interfere through
`actionstop`; use one shared jail/action until per-jail namespaces exist.

### 6. Investigate traffic before blocking

```sh
ip -s link
sar -n DEV 1
ethtool -S eno1
ss -s
conntrack -S
nstat -az
sudo bfobserve --list-interfaces
sudo bfobserve --interface eno1 --seconds 30 --top 20
sudo bfobserve --interface eno1 --seconds 30 --suggest --min-pps 1000
```

`bfobserve` is read-only, uses AF_PACKET, requires root or `CAP_NET_RAW`, and
reports source IP, packets, bytes, PPS, SYN/RST, UDP, and last destination
port. `--suggest` only prints `bfban add` commands. Check allowlists, ports,
protocols, NAT/CDN effects, and an independent feed before applying a ban.

If native XDP drops a packet before the normal stack, it may not appear in
`tcpdump`. The current observer is not an XDP telemetry map; that is future
work.

### 7. Apply CIDR or a threat feed carefully

```sh
sudo bfban add 203.0.113.0/24
./scripts/fetch-threat-feed.sh --output=/tmp/feodo.txt
less /tmp/feodo.txt
sudo ./scripts/fetch-threat-feed.sh --apply
```

Use botnet-specific indicators, not geography or an ASN as a proxy for malice.
Review allowlists, expiry, and rollback. References: [Feodo Tracker](https://feodotracker.abuse.ch/blocklist/)
and [Spamhaus DROP](https://www.spamhaus.org/faqs/do-not-route-or-peer-drop/).

### 8. Roll back and recover

```sh
sudo bfbandctl list
sudo bfban remove 203.0.113.10
sudo bfbandctl flush
sudo systemctl restart bfband
sudo journalctl -u bfband -b --no-pager
```

Keep console access. A local filter cannot fix a saturated upstream link; use
provider filtering or scrubbing for volumetric DDoS.

## Performance: evidence and benchmark method

The mechanism-level reason to test XDP/eBPF is established by primary docs:

1. bpfilter documents XDP as the first receive hook, before normal stack
   processing.
2. Linux documents eBPF JIT compilation to native machine code on supported
   architectures.
3. BPF hash maps provide key lookup/update operations; LPM-trie maps support
   prefix matching, with documented preallocation and LRU trade-offs.

This is a benchmark hypothesis, not proof that `bfban` beats nftables. A
Netfilter bpfilter chain, nftables set, legacy iptables chain, and native XDP
program use different hooks and must be compared with equal semantics.
This repository has no trustworthy end-to-end throughput result and makes no
percentage-performance claim.

Benchmark matrix:

```text
backend: iptables-legacy | nftables set | bpfilter Netfilter | bpfilter XDP
traffic: 64B, 128B, 512B, 1500B; IPv4/IPv6; accept/drop
load:    increase PPS until loss or one CPU saturates
measure: packets/s, CPU cycles/packet, drops, latency, update latency, memory
```

Record CPU, NIC/driver, kernel config, IRQ/RSS, offloads, map size, counters,
and logging. Candidate tools are `pktgen`, `trafgen`, `iperf3`, `perf stat`,
`ethtool -S`, and `bpftool`. Primary references:

- [Linux BPF/JIT documentation](https://docs.kernel.org/networking/filter.html)
- [Linux BPF map documentation](https://docs.kernel.org/bpf/map_hash.html)
- [Linux XDP live-frame documentation](https://docs.kernel.org/bpf/bpf_prog_run.html)
- [bpfilter hook placement](https://bpfilter.io/usage/explanation/hooks.html)
- [bpfilter rules and verdicts](https://bpfilter.io/usage/reference/rules.html)
- [nftables sets](https://wiki.netfilter.org/wiki-nftables/index.php/Sets)

## Memory and limitations

The current daemon reserves a fixed table for up to 65,536 entries, roughly
4.5 MiB before normal process overhead. Actual BPF-map memory depends on map
type, key/value size, capacity, allocator overhead, and IPv4/IPv6/LPM layout:

```sh
sudo bpftool map show
sudo bpftool map dump id MAP_ID
```

The MVP still needs per-jail namespaces, atomic CIDR-aware feed batches,
bounded daemon storage, measured benchmarks, stronger systemd hardening, and
eBPF per-source telemetry. See [TODO.md](TODO.md).

## License

GPL-2.0-only. See [COPYING](COPYING).
