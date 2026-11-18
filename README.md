# bfban

[English](README.md) | [Русский](README.ru.md)

🛡️ `bfban` is a Linux service for dynamic IP blocklists. Fail2ban, CrowdSec, an
operator, or another security agent sends it IP/CIDR decisions; `bfban` keeps
the list, TTLs, and state, then applies them through bpfilter/eBPF.

🎯 In short: the detector decides, `bfban` enforces. It does not read logs or
replace the host firewall.

The project is licensed under GPL-2.0-only.

The names are deliberately separate: `bfban` is the direct CLI backend,
`bfband` is the long-running daemon, and `bfbandctl` is the Unix-socket client.
The supplied Fail2ban action runs `bfbandctl`; `banaction = bfban` refers to
the action file name, not to a direct invocation of the `bfban` binary.

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

📦 The current MVP provides:

- `bfban`: IPv4, IPv6, and CIDR management;
- `bfband`: Unix-socket daemon, state recovery, and optional TTLs;
- `bfbandctl`: the Fail2ban-facing client;
- `bfobserve`: a read-only AF_PACKET top-N traffic observer;
- systemd and Fail2ban integration files.

⚙️ When `libbpfilter` is available through `pkg-config`, the daemon updates sets
through the direct C API. If it is unavailable, it falls back to the installed
`bfban`/`bfcli` tools. Select the behavior with `BFBAN_DIRECT=auto|yes|no`.

## Roles: Fail2ban, nftables, bpfilter, and bfban

🧩 These components have different roles:

| Component | Role |
|---|---|
| Fail2ban/CrowdSec | detect activity and decide that an address should be blocked |
| `nftables`/`iptables` | general-purpose firewall engines; they can also hold dynamic sets |
| `bpfilter` | packet-filtering engine that loads chains, rules, and sets as eBPF programs |
| `bfband`/`bfbandctl` | daemon and client that manage dynamic bans, TTLs, state, and bpfilter sets |
| `NF_LOCAL_IN`/XDP | kernel hooks where the filtering chain is attached |

There are two valid designs:

```text
Fail2ban → nftables set → nftables hook
Fail2ban → bfband → bpfilter set/chain → NF_LOCAL_IN or XDP
```

🔀 `bfban` is therefore not a second firewall that must be placed on top of
`nftables`. It is an alternative enforcement backend for the dynamic blocklist.
The host can still use `nftables` for its ordinary firewall policy while
`bfban` handles a separate, frequently changing list.

### Where eBPF is used

🧠 `bfband` is the control path, not the packet path. When a ban is added, the
daemon updates a bpfilter set. bpfilter generates and loads a small eBPF
program and its kernel maps (internal tables). After that, each packet is
checked at the selected hook; the packet does not go through `bfband` or start
a user-space process. `bfbandctl list` shows the control-plane state, while
`bfcli ruleset get` shows the loaded filtering chain and its sets.

✅ The practical reason to use `bfban` is its management path: one daemon API,
TTL handling, persistent state, restart reconciliation, exact IP/CIDR sets, and the
option to attach the chain at an XDP hook. If a direct Fail2ban-to-nftables
set already provides everything required, there is no need to add `bfban`.

### What this proof of concept claims

🔬 This project does not claim that `bfban` is a drop-in replacement for
`nftables`. The proof of concept validates a narrower claim: bpfilter can
enforce a dynamic IP/port blocklist (L3/L4) on a real host, while `bfban`
manages updates, TTLs, state recovery, and CIDR sets. A full firewall
replacement would also
need comparable stateful filtering, NAT, routing, logging, rich match support,
and measured feature and performance parity. Those are outside v1.

### Why use this backend?

📌 There are two different kinds of benefit: how packets are filtered and how the
changing blocklist is operated.

#### Packet-filtering benefits

| Benefit | What it means in practice | Limitation |
|---|---|---|
| One stable chain, mutable sets | Adding or removing an address updates set data instead of building one rule per ban | nftables sets provide a similar model |
| CIDR prefix matching | A network such as `203.0.113.0/24` is matched as one prefix, not expanded into 256 addresses | This is for IP/port blocking, not arbitrary firewall policy |
| Early ingress option | With XDP, a supported interface can drop an incoming packet before the normal network stack and Netfilter | XDP is ingress-only, driver-dependent, and not automatically faster |
| eBPF execution | bpfilter translates the chain into a kernel BPF program and maps; the kernel executes the decision on the packet path | This project has not established a performance win over nftables |

#### Management benefits

| Problem | What `bfban` provides |
|---|---|
| Several producers need to submit bans | One Unix-socket API for Fail2ban, CrowdSec, feeds, and operators |
| Bans are short-lived and change often | Central TTL handling, state persistence, and restart reconciliation |
| The list contains both addresses and networks | Separate exact and CIDR/LPM sets without expanding networks |
| Updates must not grow a rule chain per address | One stable bpfilter chain with mutable set contents |
| The enforcement point matters | The same list can be attached at `NF_LOCAL_IN` or explicitly selected XDP |
| A failed update must be recoverable | Bounded requests, state durability, rollback, and kernel/state reconciliation |

`nftables` can already provide some of these pieces, including sets and element
timeouts. `bfban` is useful when the daemon-managed lifecycle, one API for
multiple producers, bpfilter integration, or XDP placement is the reason to
choose it. It is not useful merely because the host needs any firewall at all.

Choose the tool by the job:

| Need | Use |
|---|---|
| Stateful firewall policy, NAT, port/service rules | `nftables` (or existing `iptables` rules) |
| Compatibility with an existing iptables-based setup | `iptables` |
| Frequent exact-IP/CIDR updates from Fail2ban, CrowdSec, or a feed | `bfban` |
| A single stable kernel program with mutable blocklist data | `bfban` with bpfilter/eBPF |
| Filtering before the regular network stack on a supported interface | `bfban` with an explicitly configured XDP hook |

## What is XDP?

XDP (eXpress Data Path) is an early Linux hook for incoming packets. It runs
before the normal network stack and can return `DROP` before Netfilter and
conntrack. This can reduce host CPU work for known unwanted sources.

It is an ingress hook, not an outbound filter or a cure for a full provider
link. Performance depends on the network driver and on native vs generic XDP.
`bfban` uses it only when explicitly configured:

```sh
sudo env BFBAN_HOOK='BF_HOOK_XDP{ifindex=2}' /usr/sbin/bfban init
```

Use the interface index from `bfobserve --list-interfaces`; keep a separate
management path while testing.

### XDP compatibility check

🔌 `bfban` supports both XDP modes. Native XDP requires support from the kernel
driver. Generic XDP is available on a much wider range of interfaces and runs
later in the receive path. The kernel, driver, and attachment path determine
which mode is used; a driver name alone is not enough to decide that.

| Mode | Where it runs | Practical effect |
|---|---|---|
| Native XDP (`xdpdrv`) | In the NIC driver's receive path, before the packet becomes an `skb` | Earliest drop and usually the lowest host overhead; requires driver support |
| Generic XDP (`xdpgeneric`) | In the regular Linux receive path, after the packet has been turned into an `skb` | Broad compatibility and useful for testing; it loses part of the early-ingress benefit and usually has lower PPS |
| Hardware offload | In supported NIC hardware | Not selected by `bfban`; requires a specific NIC, driver, and offload support |

The filtering rule is the same, but the execution point is different. Native
XDP can avoid work that generic XDP has already incurred, such as allocating
and passing an `skb` through part of the network stack. Generic XDP is still
real XDP and can be useful when native support is unavailable, but it should
not be presented as equivalent performance.

Other compatibility details matter too: native support can depend on the
kernel's driver version, MTU and XDP-fragment support, existing XDP programs,
and driver-specific restrictions. Hardware offload is a separate feature and
is not the normal `bfban` path.

First inspect the actual interface and driver:

```sh
IFACE=eno1
IFINDEX=$(cat "/sys/class/net/$IFACE/ifindex")
ip -details link show dev "$IFACE"
ethtool -i "$IFACE"
sudo bpftool feature probe dev "$IFACE"
printf 'ifindex=%s\n' "$IFINDEX"
```

The repository also provides a read-only preflight helper:

```sh
sudo ./scripts/check-xdp.sh eno1
```

It prints the interface index, link and driver details, kernel/device eBPF
probes, and any current BPF network attachment. It does not attach a program
or change the firewall.

`bpftool feature probe dev` reports eBPF-related device features; it is a
preflight check, not proof that this exact bpfilter chain will attach. The
decisive test is to attach the real chain on a disposable or test interface.
Set `BFBAN_HOOK` in `/etc/bfban/bfban.env` to the selected interface, then:

```ini
BFBAN_HOOK=BF_HOOK_XDP{ifindex=2}
```

Restart the daemon and initialize the chain:

```sh
sudo systemctl restart bfband
sudo /usr/sbin/bfbandctl init
sudo bpftool net show dev "$IFACE"
sudo ip -details link show dev "$IFACE"
```

The output should show an XDP program attached to the interface. If attach
fails, inspect `journalctl -u bfband` and return to `NF_LOCAL_IN`. Do not test
this on the only management interface: a bad XDP program or an incorrect
blocklist can cut off administration. `bpftool net` or `ip -details link` may
show whether the attachment is native (`xdpdrv`) or generic (`xdpgeneric`);
record the actual mode and test both modes separately when measuring
performance. `bfban` supports both native and generic attachment, but does not
expose a separate mode switch. Verify which mode the kernel and driver actually use.

For a real functional test, add the source address of a second machine, not
the server's own address, and verify both the packet behavior and the chain
counters. A successful attachment alone does not prove that the intended
traffic is being dropped.

#### Common drivers with native XDP support

Native support is a property of the Linux driver, not just the NIC model. The
following drivers are common examples with upstream native-XDP support on
recent kernels:

| Vendor or platform | Drivers |
|---|---|
| Intel | `igb`, `igc`, `i40e`, `ixgbe`, `ixgbevf`, `ice` |
| Mellanox/NVIDIA | `mlx4`, `mlx5` |
| Broadcom | `bnxt_en` |
| Amazon EC2 | `ena` |
| Google Cloud | `gve` |
| QLogic | `qede` |
| Netronome | `nfp` |
| VMware and virtual machines | `vmxnet3`, `virtio_net`, `veth`, `tun` |

This is a guide, not a compatibility promise: kernel version, vendor
backports, driver configuration, MTU, and an existing BPF attachment can
change the result. Compare the `driver:` line from `ethtool -i IFACE` with the
driver list, then perform the real attach test. The maintained driver table is
in the [eBPF XDP documentation](https://docs.ebpf.io/linux/program-type/BPF_PROG_TYPE_XDP/).

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

The release checklist is [`docs/roadmap.md`](docs/roadmap.md).

## Practical use

`bfban` needs bpfilter and a running `bfband` service. Fail2ban is optional:
install it only if Fail2ban will be the source of ban decisions.

Start the service and test one temporary ban:

```sh
sudo systemctl enable --now bfband.service
sudo /usr/sbin/bfbandctl init
sudo /usr/sbin/bfbandctl add 203.0.113.10 60
sudo /usr/sbin/bfbandctl list
sudo bfcli ruleset get
```

There are three common ways to provide decisions:

1. **Manual or custom agent.** Use `bfbandctl add IP [TTL]` and
   `bfbandctl remove IP`. Use `bfban add` for a persistent CLI-managed entry
   when no daemon TTL is needed.
2. **Fail2ban.** Install Fail2ban, enable the supplied `bfban` action, and set
   `banaction = bfban` in the jail. Fail2ban reads logs and chooses thresholds;
   `bfband` only applies the resulting ban and unban commands.
3. **Threat feed.** Run `scripts/fetch-threat-feed.sh` without `--apply`,
   review the validated file and allowlist, then use `--apply` to submit it.

The service can be inspected with `bfbandctl list` and `bfcli ruleset get`.
Remove one entry with `bfbandctl remove IP`, or clear the dynamic chain with
`bfbandctl flush`.

## Scope of use

Use `bfban` when a Linux edge host needs a dynamic exact-IP or CIDR blocklist
fed by Fail2ban, CrowdSec, an operator, or a threat-intelligence feed. The
daemon centralizes updates, keeps state and TTLs, and applies the decision to
the kernel through bpfilter without replacing the host's main firewall.

Fail2ban is optional. It is required only for the supplied Fail2ban action;
`bfban` can also be driven directly by an operator, a feed, CrowdSec, or a
custom security agent.

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
| Known malicious IP or CIDR | applies an IP/port blocking entry | discover an IOC (indicator of compromise) by itself |
| SSH/web brute force | receives a Fail2ban decision and enforces it | parse logs or choose thresholds |
| High-PPS source visible on the host | `bfobserve` shows top sources; operator can apply a ban | guarantee every packet is visible or auto-classify an attack |
| Distributed/spoofed flood | can drop known sources at the chosen hook | stop spoofed sources or restore a saturated uplink |
| Saturated provider link | provides no local remedy | replace provider filtering/scrubbing |

The default hook is `NF_LOCAL_IN`, so the default profile protects services
delivered to this host. It is not a router-wide DDoS filter. For an earlier
drop on a supported interface, configure XDP as described above:

```sh
sudo env BFBAN_HOOK='BF_HOOK_XDP{ifindex=2}' /usr/sbin/bfban init
```

Use the interface index from `bfobserve --list-interfaces`, keep a management
path outside the test interface, and benchmark native and generic XDP
separately. See [bpfilter hook placement](https://bpfilter.io/usage/explanation/hooks.html)
and the [XDP research paper](https://doi.org/10.1145/3281411.3281443).

## How the dynamic blocklist is stored

Fail2ban is good at detection and ban lifetime management. A firewall backend
should keep a stable filtering program and update its data, instead of adding
one long rule chain per banned address. `bfban` uses four sets:

```text
banlist4      exact IPv4 addresses
banlist4net   IPv4 CIDR networks
banlist6      exact IPv6 addresses
banlist6net   IPv6 CIDR networks
```

This is a dynamic IP/port blocklist layer, not a replacement for stateful
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
./configure --prefix=/usr
make
sudo make install
```

`make` uses a separate `build/` directory. The selected prefix is also applied
to the generated systemd unit and `/etc/bfban/bfban.env`; a custom prefix
therefore updates `ExecStart` and the tool paths consistently.

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
an incorrect prefix may be detected successfully but then make compilation
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

The ban path is:

```text
sshd/nginx log → Fail2ban filter and jail
    → actionban: bfbandctl add <ip>
    → bfband Unix socket → bpfilter/eBPF set → DROP
```

When `bantime` expires, Fail2ban runs `actionunban` and sends
`bfbandctl remove <ip>`. The action does not pass a TTL; Fail2ban owns the ban
lifetime.

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

A threat feed is not a database of all bots. It is a curated list of network
indicators, usually IP addresses or CIDR networks associated with a specific
threat: for example, confirmed botnet command-and-control infrastructure or
known malicious hosting. Feeds are optional inputs; `bfban` does not discover
them, score them, or verify that an address is still malicious.

The repository's example uses the Feodo Tracker recommended IPv4 list. It is a
high-confidence, botnet-specific example, not a universal blocklist. Spamhaus
DROP is a separate CIDR-oriented source that requires its own importer and
review of the provider's usage terms. Do not turn a country list, ASN list, or
unreviewed reputation dump into a deny rule: shared hosting, NAT, VPNs, and
CDNs can create false positives.

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
- Linux can JIT-compile eBPF instructions to native machine code on supported
  architectures. JIT state and kernel configuration must be recorded in a
  benchmark; it is not safe to assume identical code generation everywhere.
- Exact address sets use hash-style storage. Prefix sets use longest-prefix
  matching (LPM); they represent a CIDR network without expanding it into all
  member addresses.
- bpfilter's `chain update-set` is an atomic delta operation that preserves
  counters, but bpfilter documents that it regenerates the chain's BPF program
  with the changed set. The direct API removes user-space process/CLI overhead;
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

The test build enables private fault-injection hooks and a stateful fake
kernel-set. As root, it exercises backend failure on a selected operation,
failed state persistence before and after `rename()`, batch rollback,
reconciliation blocking, CIDR handling, daemon request timeout, and restart
reconciliation. It compares the fake kernel-set with both `LIST` output and
the saved state file. Direct libbpfilter mode is explicitly disabled for this
test; the production direct path is covered by the separate libbpfilter build
and the validation lab. Without root, the basic CLI checks still run and the
root-only daemon cases are reported as `SKIP`, not `PASS`.

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
