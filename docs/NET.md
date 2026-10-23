# Phase-C network probes (`kn_net`) — implementation notes

Status: **IMPLEMENTED, sandbox-verified** (native 8 suites PASS incl.
test_knselftest 97; cross `-Werror` x64/i686 clean; kn_selftest +
knvstd_main clean under `-Wformat-truncation=2` at -O0/-O2/-O3; ctest
10/10; T10 dry-run ALL PASS 38 on POSIX with all four probes honestly
skipped). **The probes have NEVER EXECUTED** — they need winsock +
schannel + a real network: **T10 round 11 on the target is the first
run**, and its output is a stage-2 planning input, not just a gate.

With this delta the §7.9 registry has **NO stubs left: 12/12 self-tests
implemented.**

## Why these four, and why schannel

§7.9 defines phase C as the network diagnostics that separate *"our
protocol code is broken"* from *"this machine cannot even talk to the
endpoints"*. They gate stage-2 debugging. The TLS stack is **schannel** —
Win7-native, zero dependencies, and exactly what the stage-2 C client
will use, so the probe diagnoses the REAL future stack (a green probe
with OpenSSL would prove nothing about Win7 schannel + the stale cert
store).

Working agreements honored: verification is NEVER disabled (SNI target
name passed to InitializeSecurityContext, `dwFlags = 0` = schannel auto
credential validation); a failed handshake is REPORTED with its raw
SECURITY_STATUS — the measurement, not something to work around.

## The endpoints (from the research index, BOOTSTRAP §5)

| Layer | NVIDIA hosts | Baseline (non-NVIDIA) |
|---|---|---|
| DNS | `login.nvidia.com`, `prod.cloudmatchbeta.nvidiagrid.net` | `stun.l.google.com` |
| TCP/443 + TLS | same two | `www.cloudflare.com` |
| chain dump | `login.nvidia.com` (§7.9's exact wording) | — |

The baseline per layer makes the verdicts separable: baseline dead →
machine/network problem; baseline alive + NVIDIA dead → endpoint/blocking
problem (a stage-2 blocker finding). (The media/Mjolnir and bundle hosts
are per-session dynamic — they get probed in stage 2 from the real
CloudMatch response, not hard-coded here. STUN hosts are UDP — out of
scope for the 443/TLS probes.)

## Design

- `kn_net_dns_lookup`: getaddrinfo (AF_UNSPEC), counts A/AAAA, first of
  each via inet_ntop, timed.
- `kn_net_tcp_connect443`: first A record, non-blocking connect + select
  (5 s), SO_ERROR verdict, then blocking with 10 s RCV/SND timeouts.
  rc distinguishes dns(-1)/refused(-2)/timeout(-3)/socket(-4).
- `kn_net_tls_probe` / `kn_net_chain_dump`: one shared session core —
  connect → `AcquireCredentialsHandle(UNISP_NAME, SCHANNEL_CRED{version 4,
  grbitEnabledProtocols=0, dwFlags=0})` → the documented
  InitializeSecurityContext loop (TOKEN/EMPTY input pair, EXTRA_DATA
  carry-over via memmove, ALLOCATE_MEMORY output + FreeContextBuffer,
  send-on-failure so TLS alerts go out, 64-iteration cap, SEC_E_OK /
  SEC_I_CONTINUE_NEEDED / SEC_E_INCOMPLETE_MESSAGE handling) →
  SECPKG_ATTR_CONNECTION_INFO (protocol + cipher/hash/exch strengths) →
  SECPKG_ATTR_REMOTE_CERT_CONTEXT (leaf subject/issuer via CertNameToStrA,
  JSON-sanitized) → optional CertGetCertificateChain + walk (depth cap 6,
  per-element subject/issuer/notBefore/notAfter/trust) +
  CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL). Full chain
  details also go to diag.log at INFO (one line per element — the bundle
  stays self-explanatory); the evidence JSON carries the summary.
- `grbitEnabledProtocols = 0` is deliberate: the probe measures the
  machine's STOCK schannel defaults (the Win7 TLS-1.2-client question —
  KB3140245/DefaultSecureProtocols reality — is exactly what stage 2 must
  know). `protocol_name` decodes the SP_PROT bits; mingw's schannel.h
  predates TLS 1.1+, so the 1.1/1.2/1.3 client bits are defined locally
  (stable ABI values, decode-only use).

## Verdict semantics (and T10's gating policy)

| Test | ok | degraded | failed |
|---|---|---|---|
| net.dns | all resolve | — | baseline dead (no DNS at all) OR an NVIDIA host dead (stage-2 blocker) |
| net.tcp443 | all connect | — | baseline dead (no egress) OR NVIDIA refused/timeout |
| net.tls | all verify, all ≥ TLS 1.2 | all verify but some host negotiated < TLS 1.2 | baseline TLS dead (schannel/egress) OR NVIDIA handshake failed (sec_status reported) |
| tls.chain | chain clean (trust 0, policy 0) | chain BUILT but flagged (the §7.9 stale-store finding) | handshake/chain build failed |

T10 gates **net.dns + net.tcp443** on ok/degraded (a connected machine
must pass — FAILED turns T10 red on purpose: it is a blocker finding).
**net.tls + tls.chain are REPORTED** (executed + status + reason logged
prominently, no pass/fail gate): a Win7 TLS failure is a research finding
to analyse (TLS version? cert store? SNI?), not automatically a client
bug — auto-failing the build gate on it would bury the analysis under a
red verdict. The skrot shows all four statuses either way.

## Landmines recorded this round

1. `sspi.h` REQUIRES `#define SECURITY_WIN32` before inclusion (hard
   `#error` otherwise).
2. `"????-??-??"` inside a string literal contains `??-` — a TRIGRAPH
   (GCC converts it to `~` and -Werror=trigraphs fires under -std=c11).
   Placeholder text must avoid `??` sequences.
3. `winsock2.h` before `windows.h` (classic) — kept at the top.
4. mingw maps the SSPI entry points via `__MINGW_NAME_AW`; explicit `A`
   suffixes are used (AcquireCredentialsHandleA,
   InitializeSecurityContextA, QueryContextAttributesA).
5. New PE imports: ws2_32.dll, secur32.dll, crypt32.dll — all
   Win7-built-in (objdump-verified); F1 `-static` unaffected (import
   archives, like d3d9/winmm/ole32).

## Stage-2 follow-ups from here

- The round-11 probe RESULTS drive the WSS+RTSP client design: negotiated
  TLS version (Win7 schannel ceiling), cert-store gaps (roots to install
  via Windows Update vs. a bundled-store decision — never a bypass),
  endpoint reachability (CDN vs regional blocking).
- Reuse: `kn_tls_session` core becomes the transport's TLS layer (the
  WSS client grows a send/recv over the established context —
  EncryptMessage/DecryptMessage with the SECPKG_ATTR_STREAM_SIZES).
- Mjolnir/bundle hosts get probed per-session once CloudMatch answers.
