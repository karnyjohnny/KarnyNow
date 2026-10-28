/* sandbox_session_e2e.c — sandbox-only E2E of the stage-2 signaling
 * chain against tools/fake_rig.py over REAL loopback sockets (round XXI;
 * NOT a gate, like the other sandbox_* harnesses — the target-side proof
 * of the production TLS path is T10/T12, and the first real rig is the
 * true test per R32).
 *
 * Chain proven here:
 *   plain TCP -> kn_http/kn_cm  (POST v2/session + poll -> ready session,
 *                                rtsps endpoint derivation, bundle peer)
 *   plain TCP -> kn_ws          (RFC 6455 handshake with x-nv-sessionid)
 *   kn_ws     -> kn_rtsp        (OPTIONS -> DESCRIBE -> SETUP sweep ->
 *                                ANNOUNCE(negotiated) -> PLAY)
 *   handoff   -> SRTP material  (salt rule, profile default, session init
 *                                or honest NO_BACKEND in bare builds)
 *   handoff   -> UDP peer       (kn_sock bind + connect to the SETUP peer)
 *
 * build+run (sandbox):
 *   gcc -std=c11 -O1 -g -Wall -Wextra -Istreamer/include -Istreamer/src \
 *       -Ithird_party/{tiny-aes-c,crypto-algorithms-sha1} \
 *       tools/sandbox_session_e2e.c streamer/src/{kn_ws,kn_rtsp,kn_json, \
 *       kn_http,kn_cm,kn_sock,kn_sha1,knfec,knsrtp,kn_aes,kn_rx,kn_gs, \
 *       kn_bw}.c \
 *       third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
 *       -pthread -o build/sandbox_session_e2e
 *   ./build/sandbox_session_e2e
 */
#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_cm.h"
#include "kn/kn_http.h"
#include "kn/kn_rtsp.h"
#include "kn/kn_rx.h"
#include "kn/kn_sock.h"
#include "kn/kn_ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_failures;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (name));        \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------ TCP seam */

typedef struct { int fd; } tcp_ctx;

static int tcp_send(void *c, const uint8_t *b, size_t n)
{
    tcp_ctx *t = (tcp_ctx *)c;
    size_t off = 0;
    while (off < n) {
        ssize_t k = send(t->fd, b + off, n - off, 0);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)k;
    }
    return 0;
}

static int tcp_recv(void *c, uint8_t *b, size_t cap, uint32_t timeout_ms)
{
    tcp_ctx *t = (tcp_ctx *)c;
    struct pollfd p;
    ssize_t k;
    int pr;
    p.fd = t->fd;
    p.events = POLLIN;
    pr = poll(&p, 1, (int)timeout_ms);
    if (pr == 0) return 0;
    if (pr < 0) return errno == EINTR ? 0 : -1;
    k = recv(t->fd, b, cap, 0);
    if (k > 0) return (int)k;
    if (k == 0) return -2;                 /* EOF */
    return errno == EINTR ? 0 : -1;
}

static int tcp_connect(uint16_t port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(0x7F000001u);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* --------------------------------------------------- ws -> rtsp seam */

static int ws_send_text(void *c, const char *t, size_t n)
{
    return kn_ws_send_text((kn_ws_client *)c, t, n);
}

static int ws_recv_text(void *c, char *buf, size_t cap, uint32_t t,
                        size_t *out)
{
    kn_ws_client *w = (kn_ws_client *)c;
    int op = 0;
    size_t n = 0;
    int rc = kn_ws_recv_msg(w, t, &op, &n);
    if (rc == KN_WS_ERR_TIMEOUT) return KN_RTSP_ERR_TIMEOUT;
    if (rc != KN_WS_OK) return KN_RTSP_ERR_IO;
    /* the reference feeds TEXT **and BINARY** frames into the RTSP
     * buffer (nvst_rtsp.rs:620-623); the rig exercises the binary path
     * on DESCRIBE (round XXIV) */
    if ((op != KN_WS_OP_TEXT && op != KN_WS_OP_BINARY) || n + 1 > cap)
        return KN_RTSP_ERR_IO;
    memcpy(buf, w->msg, n);
    buf[n] = '\0';
    *out = n;
    return KN_RTSP_OK;
}

static uint64_t real_now(void *c) { (void)c; return kn_sock_now_ms(); }
static void real_sleep(uint32_t ms, void *c)
{
    struct timespec ts;
    (void)c;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* --------------------------------------------------------- rig spawn */

static pid_t spawn_rig(int *out_fd)
{
    int fds[2];
    pid_t pid;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execlp("python3", "python3", "tools/fake_rig.py",
               "--exit-after-play", "1", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    *out_fd = fds[0];
    return pid;
}

static int read_ready_line(int fd, char *line, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t k = read(fd, line + n, 1);
        if (k <= 0) return -1;
        if (line[n] == '\n') { line[n] = '\0'; return 0; }
        n++;
    }
    return -1;
}

static long json_long(const char *line, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : -1;
}

/* ------------------------------------------------------------ main */

int main(void)
{
    char ready[512], done_line[512];
    long http_port, ws_port, media_port;
    pid_t rig;
    int rigfd, fd, status;
    static uint8_t rxb[64 * 1024];
    static char jsonbuf[128 * 1024];
    static char rtsp_buf[KN_RTSP_MAX_RESPONSE + 4096];
    static uint8_t ws_msg[KN_RTSP_MAX_RESPONSE + 8192];
    kn_cm_settings st;
    kn_cm_session sess;
    tcp_ctx tcp;
    kn_http_io hio;
    int http_status = 0, rc;

    printf("== sandbox session E2E (fake rig over real loopback sockets) ==\n");

    rig = spawn_rig(&rigfd);
    if (rig < 0) { printf("cannot spawn fake_rig.py\n"); return 2; }
    if (read_ready_line(rigfd, ready, sizeof ready) != 0) {
        printf("rig produced no ready line\n");
        kill(rig, SIGTERM);
        return 2;
    }
    printf("rig: %s\n", ready);
    http_port  = json_long(ready, "http_port");
    ws_port    = json_long(ready, "ws_port");
    media_port = json_long(ready, "media_port");
    CHECK(http_port > 0 && ws_port > 0 && media_port > 0, "rig ports parsed");

    /* ---- CloudMatch create + poll over plain HTTP ---- */
    memset(&st, 0, sizeof st);
    st.token = "FAKE-TOKEN";
    st.device_id = "fake-device-hash";
    st.app_id = 1234567;
    st.title = "Fake Game";
    st.width = 1280; st.height = 720; st.fps = 30;
    st.timezone_offset_ms = 7200000;

    fd = tcp_connect((uint16_t)http_port);
    CHECK(fd >= 0, "HTTP connect to the rig");
    tcp.fd = fd;
    hio.send = tcp_send; hio.recv = tcp_recv; hio.ctx = &tcp;
    rc = kn_cm_create(hio, "127.0.0.1", "/", &st, 5000, real_now, NULL,
                      rxb, sizeof rxb, jsonbuf, sizeof jsonbuf,
                      &http_status, &sess);
    CHECK(rc == KN_HTTP_OK && http_status == 200, "POST v2/session 200");
    CHECK(strcmp(sess.session_id, "FAKE-SESS-0001") == 0 && sess.ready,
          "session ready (status 2 + rtsps endpoint)");
    CHECK(sess.have_rtsps, "rtsps endpoint derived from usage-16 conn");
    {
        char want[64];
        snprintf(want, sizeof want, "rtsps://127.0.0.1:%ld", ws_port);
        CHECK(strcmp(sess.rtsps_endpoint, want) == 0,
              "endpoint points at the rig WS port");
        snprintf(want, sizeof want, "wss://127.0.0.1:%ld", ws_port);
        CHECK(strcmp(sess.signaling_url, want) == 0,
              "signalingUrl = rtsps->wss transform");
    }
    CHECK(sess.have_media && sess.media_port == (uint16_t)media_port &&
          strcmp(sess.media_ip, "127.0.0.1") == 0,
          "bundle/media peer (usage 2) extracted");
    close(fd);

    /* poll round */
    fd = tcp_connect((uint16_t)http_port);
    tcp.fd = fd;
    rc = kn_cm_poll(hio, "127.0.0.1", "/", &st, sess.session_id, 5000,
                    real_now, NULL, rxb, sizeof rxb, jsonbuf,
                    sizeof jsonbuf, &http_status, &sess);
    CHECK(rc == KN_HTTP_OK && http_status == 200 && sess.ready,
          "GET v2/session/{id} poll ok");
    close(fd);

    /* ---- WebSocket + the full RTSP sequence ---- */
    fd = tcp_connect((uint16_t)ws_port);
    CHECK(fd >= 0, "WS connect to the rig");
    tcp.fd = fd;
    {
        kn_ws_client wsc;
        kn_ws_io wio;
        kn_rtsp_client rtc;
        kn_rtsp_msg_io rio;
        kn_rtsp_header common[3];
        kn_rtsp_response resp;
        kn_rtsp_describe desc;
        kn_rtsp_setup_result setup;
        char err[160];

        wio.send = tcp_send; wio.recv = tcp_recv; wio.ctx = &tcp;
        rc = kn_ws_client_open(&wsc, wio, "127.0.0.1", (uint16_t)ws_port,
                               "/rtsp",
                               "x-nv-sessionid: FAKE-SESS-0001\r\n"
                               "content-length: 0\r\n",
                               5000, ws_msg, sizeof ws_msg,
                               err, sizeof err);
        CHECK(rc == KN_WS_OK, "WS handshake with x-nv-sessionid");
        if (rc != KN_WS_OK) {
            printf("ws open error: %s\n", err);
            goto done;
        }

        rio.send_text = ws_send_text;
        rio.recv_text = ws_recv_text;
        rio.ctx = &wsc;
        CHECK(kn_rtsp_client_init(&rtc, rio, rtsp_buf, sizeof rtsp_buf,
                                  10000, real_now, NULL, real_sleep, NULL)
                  == KN_RTSP_OK, "rtsp client init");

        common[0].name = "X-GS-Version"; common[0].value = "14.2";
        {   /* Host = the authority WITH the port (nvst_rtsp.rs:1070-76
             * strips only the scheme; mirrored with knvstd round XXIV) */
            static char authbuf[64];
            const char *a = sess.rtsps_endpoint + 8;  /* "rtsps://" */
            size_t al = strcspn(a, "/?#");
            if (al == 0 || al >= sizeof authbuf) {
                printf("bad rtsps endpoint authority\n");
                goto done;
            }
            memcpy(authbuf, a, al);
            authbuf[al] = '\0';
            common[1].name = "Host";     common[1].value = authbuf;
        }
        common[2].name = "x-nv-sessionid";
        common[2].value = "FAKE-SESS-0001";

        rc = kn_rtsp_request(&rtc, "OPTIONS", sess.rtsps_endpoint,
                             common, 3, NULL, 0, 0, &resp);
        CHECK(rc == KN_RTSP_OK && resp.status == 200, "OPTIONS 200");

        {
            kn_rtsp_header dh[5];
            dh[0] = common[0]; dh[1] = common[1]; dh[2] = common[2];
            dh[3].name = "Accept"; dh[3].value = "application/sdp";
            dh[4].name = "x-nv-abtesting"; dh[4].value = "2";
            rc = kn_rtsp_request(&rtc, "DESCRIBE", sess.rtsps_endpoint,
                                 dh, 5, NULL, 0, 0, &resp);
        }
        CHECK(rc == KN_RTSP_OK && resp.status == 200, "DESCRIBE 200");
        CHECK(kn_rtsp_extract_describe(&resp, &desc) == KN_RTSP_OK,
              "DESCRIBE extracts");
        CHECK(desc.have_runtime_key && desc.key_id == 1194884,
              "runtime key + id from the rig SDP");
        CHECK(desc.native_bundle == 1, "nativeRtcOnBundlePort required flag");
        CHECK(strcmp(desc.remote_ufrag, "RIGUFRAG") == 0,
              "remote ICE ufrag (V2)");
        CHECK(strcmp(desc.video_control, "streamid=video/0") == 0,
              "video control");

        {   /* local UDP socket: the bundle port for SETUP + the media sink */
            kn_sock_udp udp;
            char uerr[128];
            uint32_t lo = 0x0100007Fu;
            CHECK(kn_sock_udp_open(&udp, "127.0.0.1", 0, 0, uerr,
                                   sizeof uerr) == KN_SOCK_OK,
                  "media UDP socket opens");
            rc = kn_rtsp_setup_video(&rtc, desc.video_control,
                                     sess.rtsps_endpoint, desc.session,
                                     desc.ping_version,
                                     kn_sock_udp_port(&udp), common, 3,
                                     sess.have_media ? sess.media_ip : NULL,
                                     sess.have_media ? sess.media_port : 0,
                                     &setup);
            CHECK(rc == KN_RTSP_OK && setup.status == 200, "SETUP 200");
            CHECK(setup.peer_port == (uint16_t)media_port &&
                  strcmp(setup.peer_ip, "127.0.0.1") == 0,
                  "SETUP peer = the rig media port");
            CHECK(setup.bundle_fallback == 0, "primary Transport peer used");
            CHECK(setup.ping_version == 6 && setup.have_ping_payload &&
                  strcmp(setup.ping_payload, "a1b24800") == 0,
                  "ping version + payload from SETUP");
            CHECK(kn_sock_udp_connect(&udp, lo, setup.peer_port)
                      == KN_SOCK_OK,
                  "media socket connected to the SETUP peer");

            /* ---- ANNOUNCE with the negotiated SDP ---- */
            {
                kn_rtsp_announce_params ap;
                static char ann[16384], neg[16384], salt[25];
                size_t alen = 0, nlen = 0;
                kn_rtsp_header ah[5];
                const char *profile;
                memset(&ap, 0, sizeof ap);
                ap.width = 1280; ap.height = 720; ap.fps = 30;
                ap.bitrate_kbps = 5000;
                ap.bitstream_format = 0;
                ap.bit_depth = 0; ap.chroma_format = 1;
                snprintf(ap.key_hex, sizeof ap.key_hex, "%s",
                         desc.runtime_key);
                ap.key_id = desc.key_id;
                ap.bundle_port = kn_sock_udp_port(&udp);
                snprintf(ap.local_address, sizeof ap.local_address,
                         "127.0.0.1");
                snprintf(ap.ufrag, sizeof ap.ufrag, "LOCALUFRAG");
                snprintf(ap.password, sizeof ap.password, "LOCALPWD");
                snprintf(ap.fingerprint, sizeof ap.fingerprint, "LO:CA:LF");
                ap.video_port = setup.peer_port;
                ap.video_packet_size = 1280;
                ap.rtcp_on_sctp = desc.rtcp_on_sctp;
                ap.unified_socket = desc.unified_socket;
                CHECK(kn_rtsp_build_announce(&ap, ann, sizeof ann, &alen)
                          == KN_RTSP_OK, "announce built");
                CHECK(kn_rtsp_negotiate_announce(ann, resp.body, neg,
                                                 sizeof neg, &nlen, NULL)
                          == KN_RTSP_OK, "announce negotiated vs DESCRIBE");
                ah[0] = common[0]; ah[1] = common[1]; ah[2] = common[2];
                ah[3].name = "Session"; ah[3].value = desc.session;
                ah[4].name = "Content-Type";
                ah[4].value = "application/sdp";
                rc = kn_rtsp_request(&rtc, "ANNOUNCE", sess.rtsps_endpoint,
                                     ah, 5, neg, nlen, 0, &resp);
                CHECK(rc == KN_RTSP_OK && resp.status == 200,
                      "ANNOUNCE 200");
                /* ---- round XXV: the pre-PLAY ICE/NATT burst ----
                 * The rig now mirrors the R7 run-6 state machine: the
                 * first PLAY is answered 455 unconditionally and later
                 * ones only after it validated a hole-punch request on
                 * the media socket. The ladder must resolve the remote
                 * ufrag to increment_hex("a1b24800") = "a1b24801"
                 * (nvst_rtsp.rs:2316 — no serverBundlePort in the rig
                 * SDP, so the bundle username leg stays empty and the
                 * NATT payload falls back to "PING", nvst.rs:7398). */
                {
                    char rem_ufrag[64], uname[160];
                    uint8_t tid[12], tx[KN_SOCK_STUN_MAX_PKT], rxb[2048];
                    size_t plen, ulen;
                    uint16_t mtype, xport;
                    uint32_t xip;
                    int i, burst, sent = 0, ok_in = 0, n;
                    CHECK(kn_rtsp_resolve_remote_ufrag(
                              setup.ping_payload, &desc, NULL,
                              setup.ping_version, rem_ufrag,
                              sizeof rem_ufrag) == 1 &&
                          strcmp(rem_ufrag, "a1b24801") == 0,
                          "ICE ladder: the hex SETUP payload increments");
                    for (burst = 0; burst < 3; ++burst) {
                        snprintf(uname, sizeof uname, "%s:%s", rem_ufrag,
                                 "LOCALUFRAG");
                        for (i = 0; i < 3; ++i) {
                            memset(tid, (uint8_t)(burst * 16 + i + 1),
                                   sizeof tid);
                            if (kn_sock_stun_build(
                                    tx, sizeof tx, &plen,
                                    KN_SOCK_STUN_BINDING_REQUEST, tid,
                                    (const uint8_t *)"RIGPASSWORD", 11,
                                    KN_SOCK_STUN_ATTR_USERNAME,
                                    (const uint8_t *)uname,
                                    strlen(uname)) == KN_SOCK_OK &&
                                kn_sock_udp_sendto(&udp, lo,
                                                   setup.peer_port, tx,
                                                   plen) == KN_SOCK_OK)
                                sent++;
                        }
                        snprintf(uname, sizeof uname, "PING:%s",
                                 "LOCALUFRAG");
                        memset(tid, (uint8_t)(0x5A + burst), sizeof tid);
                        if (kn_sock_stun_build(
                                tx, sizeof tx, &plen,
                                KN_SOCK_STUN_BINDING_REQUEST, tid,
                                (const uint8_t *)"RIGPASSWORD", 11,
                                KN_SOCK_STUN_ATTR_USERNAME,
                                (const uint8_t *)uname,
                                strlen(uname)) == KN_SOCK_OK &&
                            kn_sock_udp_sendto(&udp, lo, setup.peer_port,
                                               tx, plen) == KN_SOCK_OK)
                            sent++;
                        for (i = 0; i < 16; ++i) {
                            size_t rlen = 0;
                            ulen = 0; mtype = 0; xport = 0; xip = 0;
                            n = kn_sock_udp_recv(&udp, rxb, sizeof rxb,
                                                 25, &rlen, NULL);
                            if (n != 1) break;
                            if (!kn_sock_stun_inspect(rxb, rlen, &mtype,
                                                      tid, uname,
                                                      sizeof uname,
                                                      &ulen, &xip,
                                                      &xport))
                                continue;
                            if (mtype == KN_SOCK_STUN_BINDING_SUCCESS &&
                                kn_sock_stun_check_integrity(
                                    rxb, rlen,
                                    (const uint8_t *)"RIGPASSWORD", 11))
                                ok_in++;
                        }
                    }
                    CHECK(sent == 12, "the burst sent 3x(3 ICE + 1 NATT)");
                    CHECK(ok_in >= 1,
                          "the rig answered a binding request and the "
                          "success passed the integrity check");
                }
                rc = kn_rtsp_request(&rtc, "PLAY", sess.rtsps_endpoint,
                                     ah, 4, NULL, 0, 0, &resp);
                CHECK(rc == KN_RTSP_OK && resp.status == 455,
                      "the rig gates the first PLAY with 455 (run-6)");
                rc = kn_rtsp_request(&rtc, "PLAY", sess.rtsps_endpoint,
                                     ah, 4, NULL, 0, 0, &resp);
                CHECK(rc == KN_RTSP_OK && resp.status == 200,
                      "PLAY 200 after the hole-punch burst");
                CHECK(desc.disable_play == 0, "PLAY was required");

                /* ---- the handoff: SRTP material ---- */
                profile = kn_rtsp_srtp_profile(NULL, resp.body);
                CHECK(strcmp(profile, "AEAD_AES_256_GCM_8") == 0,
                      "profile defaults to GCM256-tag8 (:1283)");
                kn_rtsp_salt_from_key_id(desc.key_id, salt);
                CHECK(strcmp(salt, "000000000000000000123B84") == 0,
                      "salt = {key_id:024X} of 1194884 = 0x123B84");
                {   /* hex -> bytes for the kn_rx session */
                    uint8_t key[32], sltb[14];
                    size_t i;
                    int ok = 1;
                    for (i = 0; i < 32; ++i) {
                        unsigned v;
                        if (sscanf(ap.key_hex + 2 * i, "%2x", &v) != 1)
                            ok = 0;
                        key[i] = (uint8_t)v;
                    }
                    for (i = 0; i < 12; ++i) {
                        unsigned v;
                        if (sscanf(salt + 2 * i, "%2x", &v) != 1) ok = 0;
                        sltb[i] = (uint8_t)v;
                    }
                    CHECK(ok && key[0] == 0x00 && key[31] == 0x0F &&
                          sltb[9] == 0x12 && sltb[10] == 0x3B &&
                          sltb[11] == 0x84,
                          "key/salt bytes transcribe (00..0F, 12 3B 84)");
                    {
                        kn_rx_srtp_session srtp;
                        int src = kn_rx_srtp_session_init(
                            &srtp, KN_RX_SRTP_GCM256, key, 32, sltb, 12, 8);
                        if (src == KN_RX_OK)
                            CHECK(1, "GCM256-tag8 session derives "
                                     "(mbedTLS build)");
                        else
                            CHECK(src == KN_RX_ERR_NO_CIPHER,
                                  "bare build refuses GCM256 HONESTLY "
                                  "(NO_BACKEND)");
                    }
                }
            }
            kn_sock_udp_close(&udp);
        }
        kn_ws_send_close(&wsc, 1000);
    }
done:
    close(fd);

    /* ---- rig evidence ---- */
    if (read_ready_line(rigfd, done_line, sizeof done_line) == 0) {
        printf("rig done: %s\n", done_line);
        CHECK(json_long(done_line, "plays") == 1, "rig saw exactly 1 PLAY");
        CHECK(json_long(done_line, "ice_req") >= 1,
              "rig validated >=1 hole-punch request (independent STUN)");
        CHECK(json_long(done_line, "ice_resp") >= 1,
              "rig answered >=1 binding request");
        CHECK(json_long(done_line, "play_455") == 1,
              "rig gated exactly one PLAY with 455");
        CHECK(json_long(done_line, "announce_has_key") == 1,
              "rig saw the runtime key echoed in ANNOUNCE");
    } else {
        CHECK(0, "rig done line readable");
    }
    close(rigfd);
    {
        int i;
        for (i = 0; i < 50; ++i) {
            if (waitpid(rig, &status, WNOHANG) == rig) break;
            real_sleep(100, NULL);
        }
        if (i == 50) { kill(rig, SIGTERM); (void)waitpid(rig, &status, 0); }
    }

    printf("sandbox_session_e2e: %d checks, %d failures -> %s\n",
           g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
