/*
 * vm_netdiag.c - ホスト側ネットワーク環境の実測
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計背景・なぜ必要かは include/vmodem/vm_netdiag.h の冒頭に全て書いた。
 * ここでは実装上の注意点だけを述べる。
 */
/*
 * strtoul / snprintf しか使わないので機能テストマクロは要らないが、
 * getgid(2) のために unistd.h を POSIX で通す必要がある。
 * ★どの #include より先に定義しないと効かない★
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "vmodem/vm_netdiag.h"
#include "vmodem/vm_log.h"

#ifdef _WIN32
#  include "vmodem/vm_winsock.h"
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

/* ==========================================================================
 * 小物
 * ========================================================================== */

/*
 * "192.168.99.3" -> ホストバイトオーダの uint32。
 *
 * ★ inet_addr / inet_pton を使わない理由 ★
 * このファイルは Windows でもコンパイルされる。inet_pton は Windows では
 * ws2tcpip.h と Vista 以降が必要で、リンク順の都合も出る。
 * 一方 4 オクテットの十進パースは 20 行で書けて、依存が 0 になる。
 * また inet_addr は "1.2.3" のような短縮形を受けてしまうが、
 * resolv.conf の判定でそれを通すと誤検知の元になるので、
 * ここでは「4 オクテット厳密」だけを受ける。
 */
static bool parse_ipv4_strict(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int      oct;

    if (s == NULL || out == NULL)
        return false;

    for (oct = 0; oct < 4; oct++) {
        unsigned long n;
        int           digits = 0;

        if (oct > 0) {
            if (*s != '.')
                return false;
            s++;
        }

        if (*s < '0' || *s > '9')
            return false;

        n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10u + (unsigned long)(*s - '0');
            s++;
            if (++digits > 3)
                return false;      /* "1234" は不正 */
            if (n > 255ul)
                return false;
        }

        v = (v << 8) | (uint32_t)n;
    }

    /* 末尾に余計な文字があれば不正 */
    if (*s != '\0')
        return false;

    *out = v;
    return true;
}

static const char *resolv_conf_path(void)
{
    /*
     * テストから差し替えられるようにする。
     * 本番では常に NULL なので /etc/resolv.conf が使われる。
     */
    const char *p = getenv("VM_RESOLV_CONF");

    if (p != NULL && *p != '\0')
        return p;

    return "/etc/resolv.conf";
}

static const char *ping_group_range_path(void)
{
    const char *p = getenv("VM_PING_GROUP_RANGE");

    if (p != NULL && *p != '\0')
        return p;

    return "/proc/sys/net/ipv4/ping_group_range";
}

/* ==========================================================================
 * /etc/resolv.conf
 * ========================================================================== */
bool vm_netdiag_host_resolver(vm_netdiag_resolver_t *out)
{
    FILE *f;
    char  line[512];

    if (out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    f = fopen(resolv_conf_path(), "r");
    if (f == NULL)
        return false;

    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char    *p = line;
        char    *tok;
        char    *pct;
        uint32_t ip;

        /* 行頭の空白を飛ばす */
        while (*p == ' ' || *p == '\t')
            p++;

        /* コメント行 (# ; ) は無視 */
        if (*p == '#' || *p == ';')
            continue;

        if (strncmp(p, "nameserver", 10) != 0)
            continue;

        p += 10;

        /* "nameserver" の直後は空白でなければならない ("nameserverX" 対策) */
        if (*p != ' ' && *p != '\t')
            continue;

        while (*p == ' ' || *p == '\t')
            p++;

        /* アドレス部を切り出す (空白 / 改行まで) */
        tok = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n')
            p++;
        *p = '\0';

        if (*tok == '\0')
            continue;

        /*
         * libslirp と同じく "%eth0" のインタフェース指定を落とす。
         * (src/slirp.c get_dns_addr_resolv_conf: char *c = strchr(buff2, '%'))
         */
        pct = strchr(tok, '%');
        if (pct != NULL)
            *pct = '\0';

        /*
         * IPv6 (":" を含む) は数えるだけで採用しない。
         * 我々が気にするのは IPv4 の代理先だけである。
         */
        if (strchr(tok, ':') != NULL)
            continue;

        if (!parse_ipv4_strict(tok, &ip))
            continue;

        out->count++;

        /* ★最初の 1 つ★ だけを採用する。libslirp もそうしている。 */
        if (out->count == 1) {
            out->available = true;
            out->first     = ip;
            out->loopback  = ((ip & 0xFF000000u) == 0x7F000000u);
        }
    }

    fclose(f);

    /*
     * ★ nameserver が 1 行も無い場合 ★
     * libslirp は 127.0.0.1 と ::1 にフォールバックする
     * (src/slirp.c: try_and_setdns_server(..., "127.0.0.1", ...))。
     * つまり結果的にループバック宛になるので、
     * 「代理先がループバック」と同じ扱いにしておく必要がある。
     * ここを available:false のままにすると、
     * 最も壊れやすい環境で警告が出なくなってしまう。
     */
    if (!out->available) {
        out->available = true;
        out->first     = 0x7F000001u;   /* 127.0.0.1 */
        out->loopback  = true;
        out->count     = 0;
    }

    return out->available;
}

/* ==========================================================================
 * ping_group_range
 * ========================================================================== */
static void read_ping_group_range(vm_netdiag_icmp_t *ic)
{
#ifdef _WIN32
    (void)ic;
#else
    FILE *f;
    char  buf[128];

    f = fopen(ping_group_range_path(), "r");
    if (f == NULL)
        return;

    if (fgets(buf, (int)sizeof(buf), f) != NULL) {
        char *end = NULL;
        unsigned long lo, hi;

        lo = strtoul(buf, &end, 10);
        if (end != NULL && *end != '\0') {
            hi = strtoul(end, NULL, 10);

            ic->range_known = true;
            ic->range_lo    = (uint32_t)lo;
            ic->range_hi    = (uint32_t)hi;

            /*
             * ★ lo > hi は「空集合」である ★
             * Debian 系の既定値は `1 0` で、これは
             * 「gid 1 以上 0 以下」= 誰も該当しない、という意味。
             * うっかり lo <= gid だけを見ると
             * 「gid 1000 は 1 以上だから OK」と誤判定する。
             */
            {
                unsigned long gid = (unsigned long)getgid();

                ic->gid_in_range = (lo <= hi) &&
                                   (gid >= lo) && (gid <= hi);
            }
        }
    }

    fclose(f);
#endif
}

/* ==========================================================================
 * ICMP ソケットのプローブ
 * ========================================================================== */
bool vm_netdiag_icmp_probe(vm_netdiag_icmp_t *out)
{
    vm_netdiag_icmp_t local;

    if (out == NULL)
        out = &local;

    memset(out, 0, sizeof(*out));

    read_ping_group_range(out);

#ifdef _WIN32
    /*
     * Windows の libslirp は IcmpSendEcho2 系を使い、
     * 管理者権限も raw socket も要らない。常に可用とみなす。
     */
    out->dgram_ok = true;
    return true;
#else
    /*
     * libslirp の icmp_send() と **同じ順序・同じ型** で試す。
     * ここを揃えないと「我々は開けたが libslirp は開けない」
     * (あるいはその逆) という無意味な診断になる。
     */
    {
        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);

        if (s >= 0) {
            out->dgram_ok = true;
            close(s);
        } else {
            out->dgram_err = errno;
        }
    }

    {
        int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

        if (s >= 0) {
            out->raw_ok = true;
            close(s);
        } else {
            out->raw_err = errno;
        }
    }

    return out->dgram_ok || out->raw_ok;
#endif
}

/* ==========================================================================
 * まとめて報告
 * ========================================================================== */
static void fmt_ip(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)(ip & 0xFFu));
}

void vm_netdiag_report(uint32_t nat_network, uint32_t nat_netmask)
{
    vm_netdiag_resolver_t rs;
    vm_netdiag_icmp_t     ic;
    char                  abuf[16];

    /* ---- (A) リゾルバ ---- */
    if (vm_netdiag_host_resolver(&rs) && rs.available) {
        fmt_ip(rs.first, abuf, sizeof(abuf));

        if (rs.count == 0) {
            VM_LOGW("netdiag: %s に nameserver 行が無い。"
                    "libslirp は 127.0.0.1 にフォールバックする",
                    resolv_conf_path());
        } else {
            VM_LOGI("netdiag: ホストの DNS = %s (nameserver %d 件)",
                    abuf, rs.count);
        }

        if (rs.loopback) {
            /*
             * ここが今回の本命。詳細な理屈はヘッダに書いた。
             * 「何が起きるか」「なぜか」「どう直したか」を全て出す。
             * 運用者がログだけで納得できる事を狙っている。
             */
            VM_LOGW("netdiag: ホストの DNS がループバック (%s) を指している。"
                    "systemd-resolved 等の stub listener は、実 NIC の "
                    "アドレスから来たクエリに応答しないため、libslirp の "
                    "DNS 代理 (outbound_addr で bind 済み) は無応答になる。"
                    "→ ゲストには代理ではなく実在の外部 DNS を配る",
                    abuf);
        }

        /*
         * 自己参照の検出。
         * resolv.conf が 192.168.99.x を指していると、
         * 代理先が我々自身になって無限ループになる。
         * 起こりにくいが、起きた時の症状が「無応答」で同じなので測る。
         */
        if ((rs.first & nat_netmask) == (nat_network & nat_netmask)) {
            VM_LOGE("netdiag: ホストの DNS (%s) が VModem の仮想 "
                    "ネットワーク内を指している。設定を見直す事", abuf);
        }
    } else {
        VM_LOGW("netdiag: %s を読めなかった", resolv_conf_path());
    }

    /* ---- (B) ICMP ---- */
    if (vm_netdiag_icmp_probe(&ic)) {
        VM_LOGI("netdiag: ICMP ソケット可 (dgram=%d raw=%d) → ping は通る",
                (int)ic.dgram_ok, (int)ic.raw_ok);
    } else {
        VM_LOGW("netdiag: ICMP ソケットが開けない "
                "(dgram errno=%d, raw errno=%d)。"
                "ゲストからの ping は無応答になる (libslirp の "
                "icmp_send() が -1 を返して黙って捨てる)。"
                "TCP/UDP は影響を受けないので Web は見られる",
                ic.dgram_err, ic.raw_err);

        if (ic.range_known && !ic.gid_in_range) {
            VM_LOGW("netdiag: ping_group_range = \"%u %u\" に gid %u が"
                    "含まれていない (lo > hi は空集合)。恒久的に直すには "
                    "/etc/sysctl.d/99-vmodem.conf に "
                    "net.ipv4.ping_group_range = 0 2147483647 を書く "
                    "(scripts/setup-gadget-linux.sh が自動で作る)",
                    (unsigned)ic.range_lo, (unsigned)ic.range_hi,
#ifdef _WIN32
                    0u
#else
                    (unsigned)getgid()
#endif
                    );
        }
    }
}
