# bfban roadmap and delivery status

This document is intentionally finite. It records what is already delivered
for v1 and the small set of release gates that still need validation. It is
not an open-ended feature wishlist.

## v1 scope

`bfban` v1 is a Linux dynamic L3/L4 enforcement backend for bpfilter. The
daemon is `bfband`; Fail2ban and other agents may make decisions, while
`bfband` applies them to the bpfilter chain. v1 is not a full replacement for
`nftables`: stateful firewall policy, NAT, routing, and general-purpose rule
parity are outside its scope.

## Delivered

- [x] GPL-2.0-only project and source headers.
- [x] `bfban` CLI for `init`, `add`, `add-many`, `remove`, `list`, and `flush`.
- [x] IPv4 and IPv6 exact-address blocklists.
- [x] IPv4 and IPv6 CIDR support using separate prefix/LPM sets.
- [x] CIDR canonicalization and duplicate elimination.
- [x] Batch feed parsing, validation, grouping, and bounded update chunks.
- [x] Atomic state-file replacement and state-file locking for CLI mutations.
- [x] `bfband` Unix-socket daemon with TTLs and restart recovery.
- [x] Bounded daemon storage and an open-addressing lookup index.
- [x] Expired-entry cleanup with index rebuild.
- [x] Kernel/state reconciliation during daemon initialization.
- [x] Deferred retry for failed TTL removals instead of a busy loop.
- [x] Bounded two-second request reads for sequential Unix-socket clients.
- [x] Preflight capacity checks and best-effort kernel/state rollback.
- [x] State writes with file `fsync` and attempted directory `fsync`.
- [x] Symmetric direct/CLI rollback paths when `libbpfilter` is compiled in but
  runtime direct mode is disabled.
- [x] `FLUSH` clears both the entry table and the daemon hash index.
- [x] Partial batch rollback failures are reported and block further mutations
  until an explicit `INIT` reconciliation succeeds.
- [x] Failed persistence of expired-entry cleanup prevents an unsafe daemon
  startup.
- [x] Post-`rename()` directory-open and `fsync` failures are reported as
  durability warnings, not treated as pre-commit save failures.
- [x] CLI fallback batch rollback removes every address submitted to the kernel,
  including entries after the first failed command.
- [x] Failed restoration of the previous state marks reconciliation as needed.
- [x] Direct `libbpfilter` update path when the development package is present.
- [x] CLI fallback through `bfban` and `bfcli` when direct mode is unavailable.
- [x] Direct batch updates grouped by address family and exact/CIDR set.
- [x] Root-only Unix-socket peer validation and `SIGPIPE` handling.
- [x] `bfbandctl` client with single-address and batch operations.
- [x] Fail2ban action template with custom-prefix support.
- [x] systemd unit and generated runtime configuration.
- [x] `configure --prefix=...` and out-of-tree `build/` output.
- [x] `bfobserve` interface listing, top-N reporting, JSON, and suggestions.
- [x] Ingress-default observation with explicit egress/both modes.
- [x] VLAN handling, bounded IPv6 extension-header parsing, and safe packet reads.
- [x] Rocky Linux 10, Debian 13, and Ubuntu 24.04 validation procedure.
- [x] TCP backlog, buffer, SYN-cookie, and memory guidance.
- [x] English and Russian deployment, Fail2ban, rollback, and troubleshooting docs.
- [x] Test-only fault injection for backend calls and state-save boundaries,
  with a stateful fake kernel-set, restart reconciliation checks, direct-mode
  isolation, and explicit root-only `PASS`/`SKIP` reporting in `make test`.

## Validated baseline

The following end-to-end checks have passed on a real Rocky Linux 10 host with
Linux 6.12 and bpfilter 0.8.0:

- [x] Build with `libbpfilter` and run `bfband` under systemd.
- [x] Create the `BF_HOOK_NF_LOCAL_IN` chain with an `ACCEPT` policy.
- [x] Add an IPv4 address to `banlist4` and observe a real ICMP DROP.
- [x] Remove the address automatically when its TTL expires and observe ICMP
  recovery.
- [x] Restart `bfband` while a ban is active and preserve both the ban and its
  original expiry time.
- [x] Apply an IPv4 `/32` prefix through the separate prefix/LPM path.

This validates the real kernel datapath and state recovery on this host. It is
not a performance comparison with nftables/iptables and does not yet cover the
Debian 13 or Ubuntu 24.04 profiles.

## Release gates

These are validation gates, not new product features:

- [ ] Run the direct profile on a target host with packaged or source-installed
  `bpfilter` and record the exact kernel, bpfilter, and libbpf versions.
- [ ] Execute the full functional lab on Rocky Linux 10, Debian 13, and Ubuntu
  24.04, including exact IPv4/IPv6, CIDR, restart, rollback, and Fail2ban tests.
- [ ] Compare equivalent policies against nftables/iptables and record raw
  benchmark output before making a performance claim.
- [ ] Verify the installed systemd and Fail2ban files on a clean host with both
  `/usr/local` and a non-default prefix.

## Execution roadmap

This is the remaining v1 work. It is intentionally limited to integration
validation; no new firewall features are required for these steps.

### 1. Prepare the direct-profile test host

- [ ] Select one disposable host for each target profile: Rocky Linux 10,
  Debian 13, and Ubuntu 24.04 LTS.
- [ ] Install or build `bpfilter`, `libbpf`, `bpftool`, `pkg-config`, and the
  compiler development packages.
- [ ] Confirm that `bpffs` is mounted and that the service account has the
  capabilities required by the installed bpfilter integration.
- [ ] Build with `configure` and verify that the generated configuration
  really points to the advertised headers and library.
- [ ] Run `BFBAN_DIRECT=yes` so a missing or unusable direct backend is a hard
  failure instead of an unnoticed CLI fallback.
- [ ] Record distribution, kernel, bpfilter, libbpf, compiler, and NIC/driver
  versions in the test result.

### 2. Validate real kernel-set behavior

- [ ] Start `bfband` under systemd and verify `PING` and `INIT`.
- [ ] Add and remove exact IPv4 and IPv6 addresses.
- [ ] Add and remove IPv4 and IPv6 CIDR prefixes, including canonicalization
  and duplicate requests.
- [ ] Verify the four expected bpfilter sets and DROP rules with the native
  bpfilter inspection tool.
- [ ] Restart the daemon and confirm that state is reapplied to the real
  backend.
- [ ] Exercise TTL expiry, `FLUSH`, `ADD_MANY`, and a 65,536-entry-scale
  bounded feed on a disposable host.

### 3. Validate integrations and failure recovery

- [ ] Run a disposable Fail2ban jail through `bfbandctl`; verify ban, unban,
  TTL, daemon restart, and journal diagnostics.
- [ ] Stop or restart the daemon during a controlled update and verify that
  the state file and real kernel sets recover through `INIT`.
- [ ] Repeat the functional checks after a clean reboot where the test host
  permits it.
- [ ] Keep the fault-injection suite as a pre-integration regression gate;
  it validates compensation logic, while the real-host suite validates the
  bpfilter API and kernel behavior.

### 4. Measure before making performance claims

- [ ] Compare equivalent exact-address and CIDR policies against nftables sets
  and the supported iptables path.
- [ ] Measure packets per second, CPU cycles per packet, drop latency, update
  latency, memory, and behavior at several set sizes.
- [ ] Keep packet size, protocol, hook, NIC, offloads, IRQ/RSS, and CPU
  allocation identical between runs.
- [ ] Store raw commands and outputs under a dated validation directory; do
  not publish a percentage without the host metadata and raw measurements.

### 5. Release decision

- [ ] All three distribution profiles pass the functional and integration
  checks, or an exception is documented with its exact missing prerequisite.
- [ ] Direct mode is demonstrated on at least one supported host; a staged
  library or successful compile alone is not sufficient.
- [ ] systemd, Fail2ban, restart, rollback, CIDR, and TTL checks pass.
- [ ] Performance results are reported as measurements with scope and limits,
  not as a general claim that bpfilter is faster than nftables.

The current development workstation has a Rocky-family 5.14 kernel and a
staged libbpfilter library, but the direct runtime probe cannot access
`/sys/fs/bpf`. That result is a test-environment limitation and does not count
as a direct-profile pass.

State changes use a compensating rollback because the kernel update and the
state-file rename cannot be one physical transaction. A failed rollback is
reported and the next daemon initialization reconciles the state with the
kernel chain.

## Explicitly outside v1

- Per-jail bpfilter namespaces.
- Automatic attack classification or automatic banning.
- Per-source XDP/eBPF telemetry maps.
- TPACKET_V3 or AF_XDP collector implementation.
- Upstream DDoS scrubbing or provider coordination.
- Stateful firewall, NAT, conntrack, or rate-limiting replacement.

These may be considered for a separate v2 design after v1 measurements; they
are not prerequisites for the daemon/Fail2ban release.
