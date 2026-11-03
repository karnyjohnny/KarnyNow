/* test_knsdp.c — golden battery for kn_sdp (B-track slice 5b).
 *
 * The parse/answer goldens are the REAL runB3 offer (karnynow-logs,
 * 2026-11-02, flight B3, seat 80-250-98-40: the console's
 * webrtc_offer_sdp event, 3122 B, 105 lines) and the answer that the
 * RTSP §65.4 rules derive from it, line by line. The nvstSdp goldens
 * pin the OpenNOW core_sdp.rs recipe (identity triplet, the 74-line
 * video table, the bitrate ladder, the ri.* defaults). Section-isolated,
 * never aborts (round-XVI suite rule). */
#include "kn/kn_sdp.h"
#include "test_port.h"

#include <string.h>

/* ------------------------------------------------- the runB3 offer */
static const char offer_runb3[] =
    "v=0\r\n"
    "o=- 4373647202393833435 2 IN IP4 127.0.0.1\r\n"
    "s=odrerir\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2 3\r\n"
    "a=ice-options:trickle\r\n"
    "a=ice-lite\r\n"
    "a=ice-ufrag:8474bea5\r\n"
    "a=ice-pwd:43dcc43c-f0fe-4af1-9a12-a797175ce1f6\r\n"
    "a=fingerprint:sha-256 68:3F:D2:80:C8:D6:77:24:DB:52:8A:18:3C:23:39:85:68:94:E9:A2:D6:8D:BB:9A:12:3C:A0:08:BF:01:D0:AD\r\n"
    "a=setup:actpass\r\n"
    "m=audio 47998 UDP/TLS/RTP/SAVPF 63 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:47998 IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
    "a=sendonly\r\n"
    "a=msid:second_stream_id audio\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:63 red/48000/2\r\n"
    "a=rtcp-fb:63 transport-cc\r\n"
    "a=fmtp:63 111/111\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtcp-fb:111 transport-cc\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=ptime:10\r\n"
    "a=ssrc:1 cname:odrerir\r\n"
    "m=video 47998 UDP/TLS/RTP/SAVPF 99 100 107 108 103 104 96 97 101 102 98\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:47998 IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
    "a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing\r\n"
    "a=extmap:12 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
    "a=sendonly\r\n"
    "a=msid:stream_id fbc-video-0\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtcp-fb:96 transport-cc\r\n"
    "a=rtcp-fb:96 ccm fir\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;sps-pps-idr-in-keyframe=1;profile-level-id=42001f\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96;rtx-time=125\r\n"
    "a=rtpmap:101 H264/90000\r\n"
    "a=rtcp-fb:101 transport-cc\r\n"
    "a=rtcp-fb:101 ccm fir\r\n"
    "a=rtcp-fb:101 nack\r\n"
    "a=rtcp-fb:101 nack pli\r\n"
    "a=fmtp:101 level-asymmetry-allowed=1;packetization-mode=1;sps-pps-idr-in-keyframe=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:102 rtx/90000\r\n"
    "a=fmtp:102 apt=101;rtx-time=125\r\n"
    "a=rtpmap:99 AV1/90000\r\n"
    "a=rtcp-fb:99 transport-cc\r\n"
    "a=rtcp-fb:99 ccm fir\r\n"
    "a=rtcp-fb:99 nack\r\n"
    "a=rtcp-fb:99 nack pli\r\n"
    "a=rtpmap:100 rtx/90000\r\n"
    "a=fmtp:100 apt=99;rtx-time=125\r\n"
    "a=rtpmap:107 H265/90000\r\n"
    "a=rtcp-fb:107 transport-cc\r\n"
    "a=rtcp-fb:107 ccm fir\r\n"
    "a=rtcp-fb:107 nack\r\n"
    "a=rtcp-fb:107 nack pli\r\n"
    "a=fmtp:107 level-id=153;profile-id=2;tier-flag=1\r\n"
    "a=rtpmap:108 rtx/90000\r\n"
    "a=fmtp:108 apt=107;rtx-time=125\r\n"
    "a=rtpmap:103 H265/90000\r\n"
    "a=rtcp-fb:103 transport-cc\r\n"
    "a=rtcp-fb:103 ccm fir\r\n"
    "a=rtcp-fb:103 nack\r\n"
    "a=rtcp-fb:103 nack pli\r\n"
    "a=fmtp:103 level-id=153;profile-id=1;tier-flag=1\r\n"
    "a=rtpmap:104 rtx/90000\r\n"
    "a=fmtp:104 apt=103;rtx-time=125\r\n"
    "a=ssrc:2 cname:fbc-video-02\r\n"
    "a=ssrc-group:FID 2 3\r\n"
    "a=ssrc:3 cname:fbc-video-02\r\n"
    "a=ssrc-group:FEC-FR 2 4\r\n"
    "a=ssrc:4 cname:fbc-video-02\r\n"
    "a=rtpmap:98 flexfec-03/90000\r\n"
    "a=rtcp-fb:98 transport-cc\r\n"
    "a=fmtp:98 repair-window=10000000\r\n"
    "m=application 47998 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n"
    "m=audio 47998 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:47998 IN IP4 0.0.0.0\r\n"
    "a=mid:3\r\n"
    "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
    "a=recvonly\r\n"
    "a=msid:third_stream_id mic\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtcp-fb:111 transport-cc\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=ptime:10\r\n"
    "a=ssrc:5 cname:odrerir\r\n"
    ;

/* --------------------------------- the answer the rules must build */
static const char answer_runb3[] =
    "v=0\r\n"
    "o=- 7 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2 3\r\n"
    "a=ice-ufrag:kn01\r\n"
    "a=ice-pwd:kn01kn01kn01kn01kn01kn\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:active\r\n"
    "m=audio 50000 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:50000 IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtcp-fb:111 transport-cc\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=ptime:10\r\n"
    "m=video 50000 UDP/TLS/RTP/SAVPF 96 97\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:50000 IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
    "a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing\r\n"
    "a=extmap:12 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtcp-fb:96 transport-cc\r\n"
    "a=rtcp-fb:96 ccm fir\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;sps-pps-idr-in-keyframe=1;profile-level-id=42001f\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96;rtx-time=125\r\n"
    "m=application 50000 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n"
    "m=audio 0 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:3\r\n"
    "a=inactive\r\n"
    ;

static kn_sdp_answer_cfg test_cfg(void)
{
    kn_sdp_answer_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_ip = "0.0.0.0";
    cfg.local_port = 50000;
    cfg.ufrag = "kn01";
    cfg.pwd = "kn01kn01kn01kn01kn01kn";
    cfg.fingerprint =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    cfg.sess_id = 7;
    cfg.width = 1280; cfg.height = 720; cfg.fps = 30;
    cfg.bitrate_kbps = 20000;
    return cfg;
}

static void test_parse_runb3(void)
{
    static kn_sdp_offer off;
    int rc = kn_sdp_offer_parse(offer_runb3, &off);
    CHECK(rc == KN_SDP_OK, "runb3 offer parses");
    CHECK(strcmp(off.ice_ufrag, "8474bea5") == 0, "ice-ufrag");
    CHECK(strcmp(off.ice_pwd,
                 "43dcc43c-f0fe-4af1-9a12-a797175ce1f6") == 0, "ice-pwd");
    CHECK(strncmp(off.fingerprint, "68:3F:D2:80:", 12) == 0,
          "fingerprint sha-256 value");
    CHECK(strcmp(off.setup, "actpass") == 0, "setup actpass");
    CHECK(off.ice_lite == 1, "ice-lite seen");
    CHECK(off.trickle == 1, "ice-options trickle seen");
    CHECK(off.n_media == 4, "four m-sections");
    /* mid 0: audio sendonly RED+opus */
    CHECK(strcmp(off.m[0].kind, "audio") == 0, "m0 audio");
    CHECK(off.m[0].port == 47998, "m0 port 47998");
    CHECK(strcmp(off.m[0].mid, "0") == 0, "m0 mid");
    CHECK(strcmp(off.m[0].dir, "sendonly") == 0, "m0 sendonly");
    CHECK(off.m[0].opus_pt == 111, "m0 opus pt 111");
    CHECK(off.m[0].ptime == 10, "m0 ptime 10");
    CHECK(strcmp(off.m[0].fmt, "63 111") == 0, "m0 fmt keeps RED 63");
    /* mid 1: video sendonly, H264 96 + RTX 97 */
    CHECK(strcmp(off.m[1].kind, "video") == 0, "m1 video");
    CHECK(strcmp(off.m[1].dir, "sendonly") == 0, "m1 sendonly");
    CHECK(off.m[1].h264_pt == 96, "m1 h264 pt 96");
    CHECK(off.m[1].h264_rtx_pt == 97, "m1 rtx pt 97 (apt=96)");
    /* mid 2: the datachannel */
    CHECK(strcmp(off.m[2].kind, "application") == 0, "m2 application");
    CHECK(off.m[2].sctp_port == 5000, "m2 sctp-port 5000");
    CHECK(off.m[2].max_message_size == 262144ULL, "m2 max-message-size");
    /* mid 3: the mic (recvonly — we reject it) */
    CHECK(strcmp(off.m[3].kind, "audio") == 0, "m3 audio");
    CHECK(strcmp(off.m[3].dir, "recvonly") == 0, "m3 recvonly (mic)");
    CHECK(off.m[3].opus_pt == 111, "m3 opus pt 111");
}

static void test_answer_runb3(void)
{
    static kn_sdp_offer off;
    char ans[4096];
    kn_sdp_answer_cfg cfg = test_cfg();
    int rc;
    CHECK(kn_sdp_offer_parse(offer_runb3, &off) == KN_SDP_OK,
          "answer: offer parses");
    rc = kn_sdp_answer_build(&off, &cfg, ans, sizeof ans);
    CHECK(rc == KN_SDP_OK, "answer builds");
    if (rc == KN_SDP_OK)
        CHECK(strcmp(ans, answer_runb3) == 0, "answer == golden (runB3)");
    if (rc == KN_SDP_OK && strcmp(ans, answer_runb3) != 0) {
        /* surface the first divergent byte for the operator log */
        size_t i = 0;
        while (ans[i] && ans[i] == answer_runb3[i]) ++i;
        printf("  first diff at %zu: got %.40s want %.40s\n", i,
               ans + i, answer_runb3 + i);
    }
    /* the answer ends CRLF and never carries a bare LF */
    CHECK(rc == KN_SDP_OK && strstr(ans, "\n") != NULL, "answer is CRLF");
    {   /* too-small buffer is an honest ERR_BIG, never a partial */
        char tiny[64];
        CHECK(kn_sdp_answer_build(&off, &cfg, tiny, sizeof tiny)
                  == KN_SDP_ERR_BIG, "answer ERR_BIG on tiny buffer");
    }
    CHECK(kn_sdp_answer_build(&off, NULL, ans, sizeof ans)
              == KN_SDP_ERR_ARGS, "answer ERR_ARGS without cfg");
    CHECK(kn_sdp_answer_build(NULL, &cfg, ans, sizeof ans)
              == KN_SDP_ERR_ARGS, "answer ERR_ARGS without offer");
    {   /* missing identity fields must be refused */
        kn_sdp_answer_cfg bad = test_cfg();
        bad.ufrag = "";
        CHECK(kn_sdp_answer_build(&off, &bad, ans, sizeof ans)
                  == KN_SDP_ERR_ARGS, "answer ERR_ARGS on empty ufrag");
    }
}

/* The offer/answer rules on synthetic shapes: setup mirroring, codec
 * rejection, direction mapping. */
static void test_answer_rules(void)
{
    static kn_sdp_offer off;
    char ans[4096];
    kn_sdp_answer_cfg cfg = test_cfg();
    static const char synth[] =
        "v=0\r\n"
        "o=- 1 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1 2\r\n"
        "a=setup:active\r\n"            /* offerer is the DTLS client */
        "m=video 47998 UDP/TLS/RTP/SAVPF 99 100\r\n"   /* AV1 only */
        "a=mid:0\r\n"
        "a=sendonly\r\n"
        "a=rtpmap:99 AV1/90000\r\n"
        "a=rtpmap:100 rtx/90000\r\n"
        "a=fmtp:100 apt=99\r\n"
        "m=audio 47998 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "m=application 47998 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "a=mid:2\r\n";
    CHECK(kn_sdp_offer_parse(synth, &off) == KN_SDP_OK, "synth parses");
    CHECK(off.m[0].h264_pt == 0, "synth video has no H264");
    CHECK(kn_sdp_answer_build(&off, &cfg, ans, sizeof ans) == KN_SDP_OK,
          "synth answer builds");
    CHECK(strstr(ans, "a=setup:passive") != NULL, "offer active => passive");
    CHECK(strstr(ans, "m=video 0 UDP/TLS/RTP/SAVPF 99 100") != NULL, "no-H264 video rejected, formats kept");
    CHECK(strstr(ans, "m=audio 50000 UDP/TLS/RTP/SAVPF 111") != NULL, "sendrecv audio answered recvonly section");
    CHECK(strstr(ans, "a=sendrecv") == NULL, "no sendrecv echoed");
    CHECK(strstr(ans, "m=application 50000 UDP/DTLS/SCTP "
                            "webrtc-datachannel") != NULL, "datachannel mirrored");
}

static void test_nvst(void)
{
    char nv[16384];
    kn_sdp_answer_cfg cfg = test_cfg();
    CHECK(kn_sdp_nvst_build(&cfg, nv, sizeof nv) == KN_SDP_OK,
          "nvst builds");
    CHECK(strchr(nv, '\r') == NULL, "nvst is LF-only (OpenNOW join)");
    CHECK(nv[strlen(nv) - 1] == '\n', "nvst ends with a newline");
    CHECK(strstr(nv, "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1") != NULL, "nvst o= line verbatim (OpenNOW)");
    CHECK(strstr(nv, "a=general.icePassword:kn01kn01kn01kn01kn01kn") != NULL, "nvst identity: icePassword");
    CHECK(strstr(nv, "a=general.iceUserNameFragment:kn01") != NULL, "nvst identity: iceUserNameFragment");
    CHECK(strstr(nv, "a=general.dtlsFingerprint:AA:BB:CC") != NULL, "nvst identity: dtlsFingerprint");
    CHECK(strstr(nv, "a=msid:fbc-video-0") != NULL, "video table present");
    CHECK(strstr(nv, "a=video.packetSize:1140") != NULL, "video table tail");
    CHECK(strstr(nv, "a=video.framePacing.pid.minTargetFrameTimeUs:31666") != NULL, "frame pacing 950000/30");
    CHECK(strstr(nv, "a=packetPacing.numGroups:5") != NULL, "numGroups 5 at 30 fps");
    CHECK(strstr(nv, "a=video.clientViewportWd:1280") != NULL, "viewport wd");
    CHECK(strstr(nv, "a=video.clientViewportHt:720") != NULL, "viewport ht");
    CHECK(strstr(nv, "a=video.maxFPS:30") != NULL, "maxFPS");
    CHECK(strstr(nv, "a=video.bitDepth:8") != NULL, "bitDepth 8");
    CHECK(strstr(nv, "a=video.initialBitrateKbps:5000") != NULL, "initial = max(min, max/4)");
    CHECK(strstr(nv, "a=vqos.bw.maximumBitrateKbps:20000") != NULL, "bw max");
    CHECK(strstr(nv, "a=vqos.bw.minimumBitrateKbps:5000") != NULL, "bw min");
    CHECK(strstr(nv, "a=vqos.grc.maximumBitrateKbps:20000") != NULL, "grc max");
    CHECK(strstr(nv, "m=audio 0 RTP/AVP\na=msid:audio\n"
                           "m=application 0 RTP/AVP\na=msid:input_1\n"
                           "a=ri.partialReliableThresholdMs:30\n"
                           "a=ri.hidDeviceMask:4294967295\n"
                           "a=ri.enablePartiallyReliableTransferGamepad:15\n"
                           "a=ri.enablePartiallyReliableTransferHid:"
                           "4294967295\n") != NULL, "nvst tail golden");
    {   /* OpenNOW joins with a trailing "" element: the document ends
         * with exactly ONE newline (no blank-line double). */
        size_t l = strlen(nv);
        CHECK(l >= 2 && nv[l - 1] == '\n' && nv[l - 2] != '\n',
              "single trailing newline (join tail)");
    }
    /* the fps>60 branch + the 120 fps pacing groups */
    cfg.fps = 120;
    CHECK(kn_sdp_nvst_build(&cfg, nv, sizeof nv) == KN_SDP_OK,
          "nvst builds at 120 fps");
    CHECK(strstr(nv, "a=packetPacing.numGroups:3") != NULL, "numGroups 3 @120");
    CHECK(strstr(nv, "a=vqos.maxStreamFpsEstimate:120") != NULL, "fps estimate");
    CHECK(strstr(nv, "a=video.fbcDynamicFpsGrabTimeoutMs:6") != NULL, "grab 6");
    CHECK(strstr(nv, "a=vqos.resControl.cpmRtc.decodeTimeThresholdMs:9") != NULL, "decode 9");
    CHECK(strstr(nv, "a=video.framePacing.pid.minTargetFrameTimeUs:7916") != NULL, "pace @120");
    /* bitrate clamps (core_sdp.rs: clamp(1000,200000), min<=5000) */
    cfg.fps = 30; cfg.bitrate_kbps = 500;
    CHECK(kn_sdp_nvst_build(&cfg, nv, sizeof nv) == KN_SDP_OK, "nvst @500k");
    CHECK(strstr(nv, "a=vqos.bw.maximumBitrateKbps:1000") != NULL, "clamp low");
    CHECK(strstr(nv, "a=vqos.bw.minimumBitrateKbps:1000") != NULL, "min=max");
    CHECK(strstr(nv, "a=video.initialBitrateKbps:1000") != NULL, "init=min");
    cfg.bitrate_kbps = 400000;
    CHECK(kn_sdp_nvst_build(&cfg, nv, sizeof nv) == KN_SDP_OK, "nvst @400M");
    CHECK(strstr(nv, "a=vqos.bw.maximumBitrateKbps:200000") != NULL, "clamp high");
    CHECK(strstr(nv, "a=video.initialBitrateKbps:50000") != NULL, "init=max/4");
    /* identity is mandatory */
    {
        kn_sdp_answer_cfg bad = test_cfg();
        bad.fingerprint = "";
        CHECK(kn_sdp_nvst_build(&bad, nv, sizeof nv) == KN_SDP_ERR_ARGS,
              "nvst ERR_ARGS without fingerprint");
    }
}

static void test_candidate_and_errors(void)
{
    char c[128];
    CHECK(kn_sdp_candidate_host("192.168.1.10", 50000, c, sizeof c)
              == KN_SDP_OK, "candidate builds");
    CHECK(strcmp(c, "candidate:1 1 udp 2122260223 192.168.1.10 50000 "
                    "typ host") == 0, "candidate golden (F37 form)");
    CHECK(kn_sdp_candidate_host("", 1, c, sizeof c) == KN_SDP_ERR_ARGS,
          "candidate ERR_ARGS empty ip");
    CHECK(kn_sdp_candidate_host("1.2.3.4", 0, c, sizeof c)
              == KN_SDP_ERR_ARGS, "candidate ERR_ARGS port 0");
    CHECK(kn_sdp_offer_parse(NULL, NULL) == KN_SDP_ERR_ARGS,
          "parse ERR_ARGS");
    {
        static kn_sdp_offer off;
        CHECK(kn_sdp_offer_parse("hello world", &off) == KN_SDP_ERR_PARSE,
              "parse rejects garbage");
        CHECK(kn_sdp_offer_parse("m=audio 1 UDP/TLS/RTP/SAVPF 111\r\n",
                                 &off) == KN_SDP_ERR_PARSE,
              "parse needs v=0");
        CHECK(kn_sdp_offer_parse("v=0\r\no=- 1 1 IN IP4 0\r\n",
                                 &off) == KN_SDP_ERR_PARSE,
              "parse needs an m-line");
    }
}

/* Slice 5d: the ICE candidate parser — the runB2a trickle line (the
 * seat's literal "raddr invalid rport 0" tolerance), Chrome shapes,
 * tcp/IPv6 edges and the reject battery. */
static void test_candidate_parse(void)
{
    kn_sdp_candidate c;
    char buf[128];

    /* the REAL runB2a server candidate (karnynow-logs runB2a/1,
     * +67 ms after our answer) — the authoritative media destination */
    CHECK(kn_sdp_candidate_parse(
              "candidate:1467310487 1 udp 1677721855 80.250.98.36 5004 "
              "typ srflx raddr invalid rport 0", &c) == KN_SDP_OK,
          "runB2a srflx candidate parses");
    CHECK(strcmp(c.foundation, "1467310487") == 0 && c.component == 1 &&
          strcmp(c.transport, "udp") == 0 && c.priority == 1677721855ull &&
          strcmp(c.ip, "80.250.98.36") == 0 &&
          c.ip_be == ((80u << 24) | (250u << 16) | (98u << 8) | 36u) &&
          c.port == 5004u && strcmp(c.typ, "srflx") == 0,
          "runB2a candidate fields exact (ip_be network-order)");
    CHECK(strcmp(c.raddr, "invalid") == 0 && c.rport == 0,
          "the seat's literal \"raddr invalid rport 0\" is tolerated");

    /* our own candidate_host output round-trips */
    CHECK(kn_sdp_candidate_host("192.168.100.11", 50400, buf, sizeof buf)
              == KN_SDP_OK &&
          kn_sdp_candidate_parse(buf, &c) == KN_SDP_OK &&
          c.component == 1 && strcmp(c.transport, "udp") == 0 &&
          c.priority == 2122260223ull && c.port == 50400u &&
          strcmp(c.typ, "host") == 0 && c.raddr[0] == '\0',
          "candidate_host output round-trips through the parser");

    /* the SDP-attribute form + a real raddr/rport pair */
    CHECK(kn_sdp_candidate_parse(
              "a=candidate:2 1 udp 100 1.2.3.4 5000 typ srflx "
              "raddr 192.168.1.5 rport 5000", &c) == KN_SDP_OK &&
          strcmp(c.foundation, "2") == 0 &&
          strcmp(c.raddr, "192.168.1.5") == 0 && c.rport == 5000u,
          "a= prefix and a numeric raddr/rport");

    /* Chrome's tcp shape (component 9) + ignored extensions */
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 9 tcp 1518214911 192.168.1.5 9 typ host "
              "tcptype active generation 0 network-id 1", &c) == KN_SDP_OK &&
          c.component == 9 && strcmp(c.transport, "tcp") == 0 &&
          c.priority == 1518214911ull && strcmp(c.typ, "host") == 0 &&
          c.raddr[0] == '\0',
          "tcp candidate (component 9) + unknown extensions ignored");

    /* IPv6 parses but stays ip_be == 0 (kn_sock is IPv4-only) */
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 1 udp 2122260223 2001:db8::1 5004 typ host",
              &c) == KN_SDP_OK &&
          strcmp(c.ip, "2001:db8::1") == 0 && c.ip_be == 0u &&
          c.port == 5004u,
          "IPv6 host: fields parse, ip_be stays 0");

    /* the reject battery */
    CHECK(kn_sdp_candidate_parse(NULL, &c) == KN_SDP_ERR_ARGS &&
          kn_sdp_candidate_parse("candidate:1 1 udp 1 1.2.3.4 5 typ host",
                                 NULL) == KN_SDP_ERR_ARGS,
          "ERR_ARGS on NULL");
    CHECK(kn_sdp_candidate_parse("1 1 udp 100 1.2.3.4 5000 typ host", &c)
              == KN_SDP_ERR_PARSE,
          "missing candidate: prefix");
    CHECK(kn_sdp_candidate_parse("candidate:", &c) == KN_SDP_ERR_PARSE,
          "empty candidate");
    CHECK(kn_sdp_candidate_parse("candidate:1 1 udp", &c) == KN_SDP_ERR_PARSE,
          "too few tokens");
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 1 udp 100 1.2.3.4 5000 host", &c)
              == KN_SDP_ERR_PARSE,
          "missing the typ keyword");
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 1 udp -5 1.2.3.4 5000 typ host", &c)
              == KN_SDP_ERR_PARSE,
          "negative priority rejected");
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 1 udp 100 1.2.3.4 0 typ host", &c)
              == KN_SDP_ERR_PARSE,
          "port 0 rejected");
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 1 udp 100 1.2.3.4 70000 typ host", &c)
              == KN_SDP_ERR_PARSE,
          "port 70000 rejected");
    CHECK(kn_sdp_candidate_parse(
              "candidate:1 0 udp 100 1.2.3.4 5000 typ host", &c)
              == KN_SDP_ERR_PARSE,
          "component 0 rejected");
}

int main(void)
{
    test_parse_runb3();
    test_answer_runb3();
    test_answer_rules();
    test_nvst();
    test_candidate_and_errors();
    test_candidate_parse();
    return kn_test_summary("test_knsdp");
}
