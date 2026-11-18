# bfban validation lab

[English](validation-lab.md) | [Русский](../README.ru.md)

This document defines a reproducible test stand for `bfban`. It separates
compatibility testing from performance testing and avoids claiming that a
local laptop or an old enterprise kernel represents Rocky Linux 10.

## 1. Topology

Use a dedicated target VM or physical host and a separate traffic generator:

```text
operator ───── management network ───── bfban target
                                      Rocky 10
generator ─── isolated test network ─ Debian 13
  pktgen/                              Ubuntu 24.04
  trafgen/
```

Minimum target profile:

| Item | Compatibility target | Performance target |
|---|---|---|
| CPU | x86_64, 2 vCPU minimum | fixed CPU model, pinned vCPU or bare metal |
| RAM | 2 GiB minimum | 4+ GiB, fixed allocation |
| NIC | one working Ethernet interface | dedicated NIC/queue, no oversubscription |
| access | console or out-of-band SSH | console plus generator host |

For a VM smoke test, `veth` is acceptable. For PPS numbers, use a physical
NIC or a documented virtual NIC model and keep it identical across runs.
Never run a destructive DROP test over the only management interface.

## 2. Operating-system matrix

| Profile | Base kernel target | Package manager | Role |
|---|---:|---|---|
| Rocky Linux 10 | 6.12 | `dnf` | RHEL-family target |
| Debian 13 (trixie) | 6.12 | `apt` | Debian target |
| Ubuntu 24.04 LTS | 6.8 GA, newer HWE also valid | `apt` | Ubuntu target |

Record the exact point release and running kernel. Rocky's official release
matrix lists Rocky 10.3 with a 6.12 kernel; Debian 13 ships the 6.12 series;
Ubuntu 24.04 ships the 6.8 GA kernel. Sources: [Rocky releases](https://docs.rockylinux.org/releases/),
[Debian 13 release notes](https://www.debian.org/releases/trixie/release-notes/whats-new.en.html),
and [Ubuntu 24.04 release notes](https://documentation.ubuntu.com/release-notes/24.04/).

Rocky Linux 9 is not a substitute for Rocky 10 in this matrix: its base
kernel is 5.14. It can verify the fallback CLI path or a build, but not certify
the current kernel target.

## 3. Install test dependencies

The `bfban` fallback build itself has no external library dependency. The
direct profile additionally needs the installed `bpfilter` runtime and
development package; the bpfilter source build has its own compiler/toolchain
requirements. The packages below are for the complete validation lab, not all
of them are required on a production host.

### Rocky Linux 10

```sh
sudo dnf upgrade -y
sudo dnf install -y git gcc gcc-c++ make cmake ninja-build \
  pkgconf-pkg-config clang llvm libbpf libbpf-devel bpftool \
  kernel-devel kernel-headers iproute ethtool perf sysstat fail2ban
sudo dnf install -y epel-release
sudo dnf install -y bpfilter bpfilter-devel
```

### Debian 13

```sh
sudo apt update
sudo apt install -y git build-essential cmake ninja-build pkg-config \
  clang llvm libbpf-dev bpftool linux-headers-amd64 \
  iproute2 ethtool linux-perf sysstat fail2ban
```

### Ubuntu 24.04

```sh
sudo apt update
sudo apt install -y git build-essential cmake ninja-build pkg-config \
  clang llvm libbpf-dev bpftool linux-headers-$(uname -r) \
  iproute2 ethtool linux-tools-common linux-tools-$(uname -r) \
  linux-perf sysstat fail2ban
```

Package names can differ in cloud/minimal images. Record the image and
repository state instead of silently substituting a different kernel/toolchain.

## 4. Build bpfilter and bfban

Build and install bpfilter according to its upstream instructions. The
validation requirement is that both the CLI and library are visible:

For a source build, the minimal reproducible form is:

```sh
cmake -S /path/to/bpfilter -B /tmp/bpfilter-build \
  -DNO_DOCS=ON -DNO_TESTS=ON -DNO_CHECKS=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /tmp/bpfilter-build -j"$(nproc)"
sudo cmake --install /tmp/bpfilter-build
sudo ldconfig
```

```sh
command -v bfcli
pkg-config --modversion libbpf
pkg-config --exists bpfilter
pkg-config --cflags --libs bpfilter
```

Then build bfban from the SSH checkout:

```sh
git clone git@github.com:rbelshevitz/bfban.git
cd bfban
./configure --prefix=/usr
make clean all
sudo make install
```

For a non-default prefix, verify that generated runtime paths follow it:

```sh
./configure --prefix=/opt/bfban
make
make DESTDIR=/tmp/bfban-root install
grep -E 'ExecStart|BFBAN_BIN|BFBAN_BPFCLI' \
  /tmp/bfban-root/etc/systemd/system/bfband.service \
  /tmp/bfban-root/etc/bfban/bfban.env
```

The configure output must say `libbpfilter: enabled` for the primary Rocky
10/Debian 13/Ubuntu 24.04 test. Use `BFBAN_DIRECT=yes` to fail rather than
silently fall back when direct integration is required.

When bpfilter is only staged in a build directory, inspect
`pkg-config --variable=prefix bpfilter` and ensure the `.pc` file points to the
headers and library that it advertises. Otherwise `configure` can report
`libbpfilter: enabled` while the compiler still searches `/usr/local/include`.

## 5. Check kernel and BPF capabilities

```sh
mkdir -p /tmp/bfban-validation
{
  date -Is
  cat /etc/os-release
  uname -a
  lscpu
  ip -br link
  pkg-config --modversion libbpf
  pkg-config --modversion bpfilter
  mountpoint /sys/fs/bpf
  bpftool feature probe kernel
} 2>&1 | tee /tmp/bfban-validation/host.txt
```

If policy permits a manual mount:

```sh
sudo mount -t bpf bpf /sys/fs/bpf
mountpoint /sys/fs/bpf
```

Record JIT status without changing security-sensitive sysctls:

```sh
sysctl net.core.bpf_jit_enable 2>/dev/null || true
```

## 6. Functional and CIDR smoke test

Use documentation addresses or traffic from the isolated generator. Do not
ban the management address.

```sh
sudo systemctl enable --now bfband.service
sudo bfbandctl ping
sudo bfbandctl init
sudo bfban add 198.51.100.10
sudo bfban add 198.51.100.0/24
sudo bfban add 2001:db8:100::1
sudo bfban add 2001:db8:100::/48
sudo bfban list
sudo bfcli chain get --name bfban_input
```

The chain must contain four sets and four DROP rules: `banlist4`/`ip4.saddr`,
`banlist4net`/`ip4.snet`, `banlist6`/`ip6.saddr`, and `banlist6net`/`ip6.snet`.

Test persistence and removal:

```sh
sudo systemctl restart bfband.service
sudo bfbandctl list
sudo bfban remove 198.51.100.10
sudo bfban remove 198.51.100.0/24
sudo bfban remove 2001:db8:100::1
sudo bfban remove 2001:db8:100::/48
sudo bfbandctl flush
```

Expected result: restart restores entries, remove deletes only its own entry,
and `flush` leaves no dynamic entries. Capture failures with:

```sh
sudo systemctl status bfband --no-pager
sudo journalctl -u bfband -b --no-pager
```

## 7. Fail2ban smoke test

Enable one disposable SSH jail, then verify ban and unban in
`bfbandctl list`. The current MVP has one global chain, so this is a single-jail
test, not a multi-jail isolation test:

```sh
sudo fail2ban-client reload
sudo fail2ban-client status sshd-bfban
sudo bfbandctl list
sudo journalctl -u fail2ban -u bfband -f
```

## 8. Performance test

Measure these backends separately and do not compare different hooks as if they
were equivalent:

```text
A. iptables-legacy at Netfilter
B. nftables set at Netfilter
C. bpfilter chain at Netfilter
D. bpfilter chain at XDP
```

Keep source, destination, packet size, protocol, accept/drop ratio, entry
count, CPU allocation, IRQ/RSS, counters, and logging identical. Record:

```sh
uname -a
ethtool -i eno1
ethtool -k eno1
ethtool -l eno1
cat /proc/cmdline
```

Start with 64-byte and 1500-byte packets and increase PPS until loss or one
CPU saturates. Measure packets/s, CPU cycles/packet, loss, latency, update
latency, memory, and whether packets reached the intended hook. Candidate
tools are `pktgen`, `trafgen`, `iperf3`, `perf stat`, `sar`, `ethtool -S`, and
`bpftool`.

Keep raw outputs under `/tmp/bfban-validation/<os>-<backend>-<date>/`. Do not
report a percentage without the raw command line and host metadata.

The kernel documents that plain AF_PACKET needs one system call per packet,
while PACKET_MMAP/TPACKET_V3 uses a mapped ring and block-level polling.
Therefore `bfobserve` is not the high-PPS benchmark path; TPACKET_V3 is the
next optimization, with AF_XDP reserved for a specialized collector. See
[Packet MMAP](https://docs.kernel.org/networking/packet_mmap.html) and
[AF_XDP](https://docs.kernel.org/networking/af_xdp.html).

For the current observer, explicitly verify direction filtering before using
its suggestions during an incident:

```sh
sudo bfobserve --interface eno1 --seconds 10 --direction ingress --top 20
sudo bfobserve --interface eno1 --seconds 10 --direction egress --top 20
sudo bfobserve --interface eno1 --seconds 10 --direction both --json
```

The first command is the anti-DDoS view. The second is useful for detecting a
compromised host or an unexpected local generator; it must not be mixed into
an ingress source ranking.

## 9. Acceptance criteria

All three OS profiles must pass:

- direct `libbpfilter` mode starts;
- exact IPv4/IPv6 and CIDR smoke tests pass;
- daemon restart restores state;
- Fail2ban ban/unban works;
- rollback/flush works without management lockout;
- benchmark results include host metadata and raw outputs;
- no performance claim is made before controlled comparison.
