# bfban

[English](README.md) | [Русский](README.ru.md)

`bfban` is a Linux enforcement backend for dynamic IP blocklists. It uses
`bpfilter`/eBPF sets and exposes an integration path for Fail2ban, CrowdSec,
and other security agents. It does not parse logs and does not decide whether
an IP is malicious.

The project is licensed under GPL-2.0-only.

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

## What is implemented

| Feature | Status |
|---|---|
| Fail2ban action through a Unix-socket daemon | implemented |
| Exact IPv4/IPv6 and CIDR blocklists | implemented |
| Separate exact and prefix/LPM sets | implemented |
| Direct `libbpfilter` update path with CLI fallback | implemented when `bpfilter.pc` is available |
| systemd state recovery and TTL handling | implemented |
| `bfobserve` interface listing, top-N, JSON, and operator suggestions | implemented using AF_PACKET |
| Open-addressing daemon index for fast ADD/DEL/TTL lookup | implemented |
| Per-source eBPF/XDP telemetry, automatic detection, and per-jail namespaces | out of scope for v1 |

The practical “wow” features are deliberately operational rather than
marketing claims: a CIDR prefix is stored as one LPM entry, Fail2ban talks to
one daemon socket, `bfobserve` shows where traffic is coming from before an
operator bans it, and state recovery lets the dynamic list survive a daemon
restart. Automatic detection and upstream DDoS mitigation are outside the
current scope.

The finite delivery checklist is [`docs/roadmap.md`](docs/roadmap.md).

## Scope of use

Use `bfban` when a Linux edge host needs a dynamic exact-IP or CIDR deny list
fed by Fail2ban, CrowdSec, an operator, or a threat-intelligence feed. The
daemon centralizes updates, keeps state and TTLs, and applies the decision to
bpfilter without replacing the host's main firewall.

Do not use it as a log detector, a complete stateful firewall, a NAT/conntrack
replacement, an automatic botnet classifier, or a cure for an already
saturated uplink. Those cases require the existing firewall, application
controls, upstream ACLs, or DDoS scrubbing.

## Anti-DDoS role

`bfban` is the enforcement stage of a layered anti-DDoS workflow:

```text
traffic/logs/feed
       |
       v
detection and decision: Fail2ban, CrowdSec, operator, threat feed
       |
       v
bfban: exact IP/CIDR set update
       |
       v
bpfilter: DROP at the selected kernel hook
```

| Situation | What bfban does | What it cannot do |
|---|---|---|
| Known malicious IP or CIDR | applies a L3/L4 deny entry | discover the IOC by itself |
| SSH/web brute force | receives a Fail2ban decision and enforces it | parse logs or choose thresholds |
| High-PPS source visible on the host | `bfobserve` shows top sources; operator can apply a ban | guarantee every packet is visible or auto-classify an attack |
| Distributed/spoofed flood | can drop known sources at the chosen hook | stop spoofed sources or restore a saturated uplink |
| Saturated provider link | provides no local remedy | replace provider filtering/scrubbing |

The default hook is `NF_LOCAL_IN`, so the default profile protects services
delivered to this host. It is not a router-wide DDoS filter. Here, “early
ingress” means the receive path of an interface: XDP runs before the regular
Linux network stack creates and processes the packet as an `skb`, so a DROP can
happen before Netfilter and conntrack. For earlier ingress drop on a supported
interface, configure XDP explicitly:

```sh
export BFBAN_HOOK='BF_HOOK_XDP{ifindex=2}'
sudo bfban init
```

Use the actual interface index from `bfobserve --list-interfaces`, keep a
management path outside the test interface, and benchmark native and generic
XDP separately. XDP is an ingress hook on the selected interface, not an
outbound socket filter. It can reduce host-side work; it cannot increase the
capacity of an already saturated network link. See [bpfilter hook placement](https://bpfilter.io/usage/explanation/hooks.html)
and the [XDP research paper](https://doi.org/10.1145/3281411.3281443).

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

## CIDR blocklists

CIDR is a first-class input format, not just a documentation example. The
daemon accepts exact addresses and prefixes for both address families:

```sh
sudo bfban add 203.0.113.10       # one IPv4 address
sudo bfban add 203.0.113.0/24     # IPv4 prefix
sudo bfban add 2001:db8::1        # one IPv6 address
sudo bfban add 2001:db8::/32      # IPv6 prefix
sudo bfban list
sudo bfban add-many /path/to/addresses.txt  # one batch update
```

Internally, `bfban init` creates four independent sets. Exact addresses use
hash-style sets; `ip4.snet` and `ip6.snet` use bpfilter's prefix/LPM set form:

| Input | bpfilter set | Matcher |
|---|---|---|
| IPv4 address | `banlist4` | `ip4.saddr` |
| IPv4 CIDR | `banlist4net` | `ip4.snet` |
| IPv6 address | `banlist6` | `ip6.saddr` |
| IPv6 CIDR | `banlist6net` | `ip6.snet` |

The network is not expanded into thousands of individual addresses. CIDR
entries are persisted in the state file and restored by `bfband` after restart.
`add-many` validates and deduplicates an input file, groups entries by set type,
and performs one `chain update-set` per group (in chunks if needed). This is
cheaper than invoking `bfban add` once per feed line. Each group update is
atomic; a multi-group file is not one global transaction.
CIDRs are canonicalized before storage (`192.0.2.17/24` becomes
`192.0.2.0/24`), so equivalent prefixes do not create duplicate entries.

## Step-by-step deployment

### 1. Check the host

Use the running kernel and actual interfaces, not only the distribution label:

```sh
uname -r
pkg-config --modversion libbpf
mountpoint /sys/fs/bpf
sudo bfobserve --list-interfaces
```

The current validation target is Linux 6.6+, libbpf 1.2+, and a bpfilter build
whose selected hooks work on that kernel. This is a tested target baseline,
not a universal minimum asserted by `bfban` alone. Ubuntu 24.04 (6.8), Debian
13 (6.12),
Fedora 40 (6.8), RHEL/Rocky/Alma 10 (6.12), and Ubuntu 22.04 with HWE 6.8
are examples of matching base kernels. RHEL/Rocky/Alma 9 (5.14) and Debian
12 (6.1) need a newer supported kernel or backport. Verify compatibility
before production use.

### 2. Build and install

Dependency split:

| Use case | Required components |
|---|---|
| `bfban` fallback build | C compiler, libc development headers, `make` |
| Direct `bfband` build | `bpfilter`, `bpfilter-devel`, `pkg-config` |
| bpfilter source build | CMake, GCC, Clang, `libbpf-devel`, Bison, Flex, `sed`, `xxd` |
| Runtime enforcement | `bfcli`/`bpfilter`, mounted BPF filesystem, root privileges |
| Fail2ban integration | `fail2ban` and systemd, if using the supplied action |
| Validation/diagnostics | `bpftool`, `iproute`, `ethtool`, `perf`, `sar`/`sysstat` |

On Fedora or an EPEL-enabled RHEL-family host, use the packaged bpfilter when
available:

```sh
sudo dnf install -y epel-release
sudo dnf install -y bpfilter bpfilter-devel
command -v bfcli
pkg-config --modversion bpfilter
pkg-config --cflags --libs bpfilter
```

`bpfilter` provides `bfcli` and the runtime library; `bpfilter-devel` provides
the headers and development metadata required for the direct `bfband` build.
If the package is unavailable for the selected distribution, build and install
bpfilter from its [official source-build instructions](https://bpfilter.io/developers/build.html).

The short source-build form is:

```sh
cmake -S /path/to/bpfilter -B /tmp/bpfilter-build \
  -DNO_DOCS=ON -DNO_TESTS=ON -DNO_CHECKS=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /tmp/bpfilter-build -j"$(nproc)"
sudo cmake --install /tmp/bpfilter-build
sudo ldconfig
```

```sh
cd ~/bfban
./configure --prefix=/usr/local
make
sudo make install
```

`make` uses a separate `build/` directory. The selected prefix is also applied
to the generated systemd unit and `/etc/bfban/bfban.env`; a non-default prefix
therefore does not leave `ExecStart` pointing at `/usr/local`.

The generated environment file points `BFBAN_BPFCLI` at the selected install
prefix. Override `BFBAN_BPFCLI` in `/etc/bfban/bfban.env` if `bfcli` is
installed elsewhere.

Run `./configure` after installing bpfilter so it can discover the
`libbpfilter` pkg-config file. Direct mode removes the per-ban `fork()`/`exec()`
and CLI parsing path. It does not by itself prove a packet-processing
throughput improvement.

If bpfilter was built from source but not installed into its configured
prefix, set `PKG_CONFIG_PATH` to the directory containing `bpfilter.pc` and
make sure that file's `prefix`, `includedir`, and `libdir` point to the same
installation tree. A build-directory `.pc` file that still says
`prefix=/usr/local` may be detected successfully but then make compilation
fail.

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

The daemon protocol also accepts a bounded batch request:

```text
ADD_MANY 203.0.113.10 198.51.100.0/24
```

`bfbandctl add-many FILE` uses this path. In direct mode, exact and CIDR
entries are grouped by bpfilter set before the kernel update; the fallback
remains compatible with the CLI backend.

The daemon `fsync`s the state file and then attempts to `fsync` its directory.
Kernel changes and the state file are not one physical transaction,
so a failed state write triggers a best-effort kernel rollback and startup
reconciliation can reapply the recorded state. If a compensating rollback fails,
the daemon reports the condition and requires an explicit successful `INIT`
before accepting further mutations.

The Fail2ban `actioncheck` command checks that the daemon socket is alive. It
is not a packet-path benchmark and does not claim that every upstream hook is
healthy; `bfbandctl init`/daemon reconciliation is the recovery operation.

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
port. It defaults to ingress accounting and ignores `PACKET_OUTGOING`; use
`--direction egress` or `--direction both` when auditing host-generated
traffic. `--suggest` only prints `bfban add` commands. Check allowlists, ports,
protocols, NAT/CDN effects, and an independent feed before applying a ban.

If native XDP drops a packet before the normal stack, it may not appear in
`tcpdump`. The current observer is not an XDP telemetry map; that is future
work.

TCP queue, window, SYN-cookie, and memory guidance is in
[`docs/tcp-ddos-tuning.md`](docs/tcp-ddos-tuning.md). `bfban` does not replace
TCP backlog tuning or upstream mitigation when the uplink itself is saturated.

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

## Kernel-specific technical basis

The design relies on specific kernel mechanisms, not on a general claim that
all eBPF code is faster:

- XDP is attached at the receive path before the normal Linux network stack;
  native driver mode can avoid work that would otherwise happen later. Generic
  XDP/SKB mode has different costs and must be measured separately.
- Linux can JIT eBPF instructions to native machine code on supported
  architectures. JIT state and kernel configuration must be recorded in a
  benchmark; it is not safe to assume identical code generation everywhere.
- Exact address sets use hash-style storage. Prefix sets use longest-prefix
  matching (LPM); they represent a CIDR network without expanding it into all
  member addresses.
- bpfilter's `chain update-set` is an atomic delta operation that preserves
  counters, but bpfilter documents that it regenerates the chain's BPF program
  with the changed set. The direct API removes userspace process/CLI overhead;
  it is not a promise of zero-cost kernel updates.

The academic basis for the XDP placement and performance trade-offs is:

> Toke Høiland-Jørgensen, Jesper Dangaard Brouer, Daniel Borkmann, John
> Fastabend, Tom Herbert, David Ahern, and David Miller. “The eXpress Data
> Path: Fast Programmable Packet Processing in the Operating System Kernel.”
> ACM CoNEXT 2018. DOI: [10.1145/3281411.3281443](https://doi.org/10.1145/3281411.3281443).

Kernel references are [BPF/JIT](https://docs.kernel.org/networking/filter.html),
[LPM trie maps](https://docs.kernel.org/bpf/map_lpm_trie.html), and
[BPF hash maps](https://docs.kernel.org/bpf/map_hash.html).

## Performance: evidence and benchmark method

The mechanism-level reason to test XDP/eBPF is established by primary docs:

1. bpfilter documents XDP as the first receive hook, before normal stack
   processing.
2. Linux documents eBPF JIT compilation to native machine code on supported
   architectures.
3. BPF hash maps provide key lookup/update operations; LPM-trie maps support
   longest-prefix matching, with documented allocation trade-offs.

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

## Validation lab

The reproducible cross-distribution test plan is in
[`docs/validation-lab.md`](docs/validation-lab.md). Use Rocky Linux 10 as the
RHEL-family target, Debian 13 as the Debian target, and Ubuntu 24.04 LTS as
the Ubuntu target. Rocky Linux 9 is useful for fallback/build checks, but its
5.14 kernel is not the primary compatibility target.

For a local regression run:

```sh
make test
```

The test build enables private fault-injection hooks. As root, it exercises
backend failure on a selected operation, failed state persistence before and
after `rename()`, batch rollback, reconciliation blocking, CIDR handling, and
the daemon request timeout. Without root, the basic CLI checks still run and
the root-only daemon cases are reported as `SKIP`, not `PASS`.

## Memory and limitations

The current daemon reserves a fixed table for up to 65,536 entries and uses a
bounded open-addressing index for ADD/DEL/TTL lookup. The entry table is
roughly 4.5 MiB before normal process overhead; the index adds bounded memory.
Actual BPF-map memory depends on map
type, key/value size, capacity, allocator overhead, and IPv4/IPv6/LPM layout:

```sh
sudo bpftool map show
sudo bpftool map dump id MAP_ID
```

Version 1 deliberately stops here. Per-jail namespaces, per-source
eBPF/XDP telemetry, and automatic detection are separate future products, not
requirements for the daemon and Fail2ban integration described above.

## License

GPL-2.0-only. See [COPYING](COPYING).
