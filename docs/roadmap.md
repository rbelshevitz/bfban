# bfban roadmap and delivery status

This document is intentionally finite. It records what is already delivered for
the 0.1 release and the real-host baseline that has been validated. It is not an
open-ended feature wishlist.

## 0.1 scope

`bfban` 0.1 release is a Linux dynamic L3/L4 enforcement backend for bpfilter.
The daemon is `bfband`; Fail2ban and other agents may make decisions, while
`bfband` applies them to the bpfilter chain. The 0.1 release is not a full
replacement for `nftables`: stateful firewall policy, NAT, routing, and
general-purpose rule parity are outside its scope.

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

State changes use a compensating rollback because the kernel update and the
state-file rename cannot be one physical transaction. A failed rollback is
reported and the next daemon initialization reconciles the state with the
kernel chain.

## Explicitly outside 0.1

- Per-jail bpfilter namespaces.
- Automatic attack classification or automatic banning.
- Per-source XDP/eBPF telemetry maps.
- TPACKET_V3 or AF_XDP collector implementation.
- Upstream DDoS scrubbing or provider coordination.
- Stateful firewall, NAT, conntrack, or rate-limiting replacement.

These may be considered for a separate 0.2 design after 0.1 measurements; they
are not prerequisites for the daemon/Fail2ban release.
