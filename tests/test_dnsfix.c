/*
 * test_dnsfix.c - DNS 修正 / ICMP 診断の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象:
 *   src/net/vm_netdiag.c   ホスト環境の実測 (resolv.conf / ICMP ソケット)
 *   src/net/vm_nat.c       vm_nat_pick_guest_dns() の判定
 *
 * ===========================================================================
 * 何を直したのか / なぜこのテストが必要か
 * ===========================================================================
 * 症状:「PPP は確立し IP も配られる。NLA の HTTP チェックも通る。
 *        しかしブラウザは ERR_NAME_NOT_RESOLVED、ping 8.8.8.8 も無応答」
 *
 * 原因 (A) 名前解決:
 *   libslirp の DNS 代理は、ゲストのクエリの宛先を
 *   /etc/resolv.conf の最初の nameserver に書き換えて転送する
 *   (libslirp master src/socket.c sotranslate_out4 -> get_dns_addr)。
 *
 *       if (!s->disable_dns &&
 *           so->so_faddr.s_addr == s->vnameserver_addr.s_addr) {
 *           return (so->so_fport == htons(53) &&
 *                   get_dns_addr(&sin->sin_addr, &sin->sin_port) >= 0);
 *       }
 *
 *   systemd-resolved が動く環境 (Armbian/Debian/Ubuntu の既定) では
 *   その nameserver は 127.0.0.53 になる。ところが本実装は
 *   SlirpConfig.outbound_addr を設定しており、libslirp は全ての
 *   外向きソケットを実 NIC の IP に bind する。結果
 *
 *       送信元 192.168.x.y  ->  宛先 127.0.0.53:53
 *
 *   という UDP になり、stub listener は非ローカル送信元のクエリに
 *   応答しない。つまり代理は完全に無応答になる。
 *
 *   修正: ホストの resolver がループバックなら代理を使わず、
 *         config.ini の実 DNS (8.8.8.8 等) をゲストに配る。
 *
 * 原因 (B) ping:
 *   libslirp の icmp_send() は SOCK_DGRAM+IPPROTO_ICMP →
 *   SOCK_RAW+IPPROTO_ICMP の順に試し、両方失敗すると -1 を返して
 *   ゲストの ICMP Echo を **ログも出さずに捨てる**。
 *   ping_group_range の Debian 既定 "1 0" は lo > hi なので空集合。
 *
 *   修正: 起動時に実測して警告し、sysctl.d で恒久化する。
 *
 * ===========================================================================
 * このテストの方針
 * ===========================================================================
 * 「環境依存の判定ロジック」を環境に依存せずに検証する必要がある。
 * そのため vm_netdiag.c は resolv.conf / ping_group_range のパスを
 * 環境変数 (VM_RESOLV_CONF / VM_PING_GROUP_RANGE) で差し替えられる
 * ようにしてある。ここでは一時ファイルに様々な resolv.conf を書いて
 * 判定が期待通りになる事を確認する。
 *
 * 実環境に依存する項目 (実際に ICMP ソケットが開けるか、libslirp が
 * リンクされているか) は FAIL ではなく SKIP にする。
 * CI やコンテナでは権限が無いのが普通で、それは実装の不具合ではない。
 * ===========================================================================
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "vmodem/vm_netdiag.h"
#include "vmodem/vm_nat.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

/* ==========================================================================
 * 共通ヘルパ (test_linux_step6.c と同じ書式に揃える)
 * ========================================================================== */
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

static void check(int ok, const char *fmt, ...)
{
    char    buf[240];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (ok) { g_pass++; printf("  [ OK ] %s\n", buf); }
    else    { g_fail++; printf("  [FAIL] %s\n", buf); }
}

static void skip(const char *fmt, ...)
{
    char    buf[240];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_skip++;
    printf("  [SKIP] %s\n", buf);
}

static void note(const char *fmt, ...)
{
    char    buf[240];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("         %s\n", buf);
}

static void head(const char *title)
{
    printf("\n--- %s ---\n", title);
}

/* --------------------------------------------------------------------------
 * 一時ファイルに内容を書き、VM_RESOLV_CONF で指させる
 * -------------------------------------------------------------------------- */
static char g_tmp_resolv[256];

static bool write_resolv(const char *content)
{
    FILE *f;

    snprintf(g_tmp_resolv, sizeof(g_tmp_resolv),
             "/tmp/vm_test_resolv_%ld.conf", (long)getpid());

    f = fopen(g_tmp_resolv, "w");
    if (f == NULL)
        return false;

    fputs(content, f);
    fclose(f);

    setenv("VM_RESOLV_CONF", g_tmp_resolv, 1);
    return true;
}

static void drop_resolv(void)
{
    if (g_tmp_resolv[0] != '\0') {
        unlink(g_tmp_resolv);
        g_tmp_resolv[0] = '\0';
    }
    unsetenv("VM_RESOLV_CONF");
}

static uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8)  | (uint32_t)d;
}

/* ==========================================================================
 * 1. resolv.conf のパース
 * ==========================================================================
 * ★ libslirp と同じ規則である事が絶対条件 ★
 * ここがずれると「代理が使えるか」の判定が無意味になる。
 * 参照: libslirp master src/slirp.c get_dns_addr_resolv_conf()
 * ========================================================================== */
static void test_resolv_parse(void)
{
    vm_netdiag_resolver_t rs;

    head("1. /etc/resolv.conf のパース (libslirp と同じ規則)");

    /* ---- 1-1: systemd-resolved の典型 ---- */
    if (write_resolv("nameserver 127.0.0.53\noptions edns0 trust-ad\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.available && rs.first == ip4(127,0,0,53) &&
              rs.loopback && rs.count == 1,
              "127.0.0.53 を loopback と判定 (available=%d first=%08x "
              "loopback=%d count=%d)",
              (int)rs.available, (unsigned)rs.first,
              (int)rs.loopback, rs.count);
        note("これが今回の症状を起こす環境。代理は使えない");
    } else {
        skip("一時ファイルを作れない");
    }

    /* ---- 1-2: ルータを指す普通の環境 ---- */
    if (write_resolv("nameserver 192.168.1.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.available && rs.first == ip4(192,168,1,1) && !rs.loopback,
              "192.168.1.1 は loopback ではない → 代理が使える");
    }

    /* ---- 1-3: 複数ある場合は「最初の 1 つ」 ---- */
    /*
     * libslirp の get_dns_addr() は最初の nameserver だけを代理先に
     * 使う (found == 0 の時だけ pdns_addr にコピーする)。
     * 我々も最初の 1 つで判定しなければ、
     * 「2 番目が実アドレスだから大丈夫」という誤判定をする。
     */
    if (write_resolv("nameserver 127.0.0.53\nnameserver 8.8.8.8\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.first == ip4(127,0,0,53) && rs.loopback && rs.count == 2,
              "複数あっても最初の 1 つで判定する (count=%d first=%08x)",
              rs.count, (unsigned)rs.first);
        note("libslirp も最初の 1 つしか代理先に使わないため");
    }

    /* ---- 1-4: "%eth0" のインタフェース指定を落とす ---- */
    if (write_resolv("nameserver 192.168.1.1%eth0\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.available && rs.first == ip4(192,168,1,1) && !rs.loopback,
              "\"192.168.1.1%%eth0\" から %% 以降を落とす");
    }

    /* ---- 1-5: IPv6 は無視して次の IPv4 を採る ---- */
    if (write_resolv("nameserver fe80::1\nnameserver 10.0.0.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.available && rs.first == ip4(10,0,0,1) && !rs.loopback,
              "IPv6 の nameserver は飛ばして IPv4 を採る");
        note("我々が気にするのは IPv4 の代理先だけ");
    }

    /* ---- 1-6: nameserver が 1 行も無い ---- */
    /*
     * ★ここを間違えると最も壊れやすい環境で警告が出なくなる★
     * libslirp は nameserver が無いと 127.0.0.1 にフォールバックする
     * (src/slirp.c: try_and_setdns_server(..., "127.0.0.1", ...))。
     * 結果的にループバック宛になるので、同じ扱いにしなければならない。
     */
    if (write_resolv("# 何も無い\noptions edns0\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.available && rs.loopback && rs.count == 0,
              "nameserver 無し → libslirp の 127.0.0.1 "
              "フォールバックに合わせて loopback 扱い");
    }

    /* ---- 1-7: コメント行を無視する ---- */
    if (write_resolv("#nameserver 127.0.0.53\nnameserver 1.1.1.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.first == ip4(1,1,1,1) && !rs.loopback && rs.count == 1,
              "コメントアウトされた nameserver を拾わない");
    }

    /* ---- 1-8: "nameserverX" に釣られない ---- */
    if (write_resolv("nameserverfoo 127.0.0.53\nnameserver 9.9.9.9\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.first == ip4(9,9,9,9) && rs.count == 1,
              "\"nameserverfoo\" は nameserver 行ではない");
    }

    /* ---- 1-9: 不正なアドレスを弾く ---- */
    /*
     * inet_addr(3) は "1.2.3" や "300.1.1.1" を通してしまう物がある。
     * 4 オクテット厳密にしないと、壊れた resolv.conf で誤判定する。
     */
    if (write_resolv("nameserver 1.2.3\nnameserver 300.1.1.1\n"
                     "nameserver 172.16.0.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.first == ip4(172,16,0,1) && rs.count == 1,
              "\"1.2.3\" と \"300.1.1.1\" を弾いて 172.16.0.1 を採る");
    }

    /* ---- 1-10: ループバックは 127.0.0.0/8 全体 ---- */
    /*
     * systemd-resolved は 127.0.0.53 と 127.0.0.54 を使い、
     * dnsmasq は 127.0.0.1 を使う。/8 全体を見なければ取り逃す。
     */
    if (write_resolv("nameserver 127.0.0.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.loopback, "127.0.0.1 (dnsmasq 等) も loopback");
    }
    if (write_resolv("nameserver 127.0.0.54\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.loopback, "127.0.0.54 (systemd-resolved の 2 本目) も loopback");
    }

    /* ---- 1-11: タブ区切りも受ける ---- */
    if (write_resolv("nameserver\t\t203.0.113.1\n")) {
        memset(&rs, 0, sizeof(rs));
        vm_netdiag_host_resolver(&rs);
        check(rs.first == ip4(203,0,113,1),
              "タブ区切りの nameserver を読める");
    }

    /* ---- 1-12: ファイルが無い ---- */
    setenv("VM_RESOLV_CONF", "/nonexistent/vmodem/resolv.conf", 1);
    memset(&rs, 0xAA, sizeof(rs));
    {
        bool r = vm_netdiag_host_resolver(&rs);
        check(!r && !rs.available,
              "読めないファイルでは available=false で返す (構造体は初期化済み)");
    }

    drop_resolv();
}

/* ==========================================================================
 * 2. vm_nat_pick_guest_dns() の判定
 * ==========================================================================
 * 「ゲストに何を配るか」の決定。これが今回の修正の中核。
 * ========================================================================== */
static void test_pick_guest_dns(void)
{
    const uint32_t fb1 = ip4(8,8,8,8);
    const uint32_t fb2 = ip4(8,8,4,4);
    uint32_t       d1, d2;
    bool           used_proxy;

    head("2. vm_nat_pick_guest_dns() の判定");

    /* ---- 2-1: n == NULL なら fallback をそのまま ---- */
    d1 = d2 = 0;
    used_proxy = vm_nat_pick_guest_dns(NULL, fb1, fb2, &d1, &d2);
    check(!used_proxy && d1 == fb1 && d2 == fb2,
          "n=NULL → config の値をそのまま配る");

    /* ---- 2-2: out が NULL でも落ちない ---- */
    used_proxy = vm_nat_pick_guest_dns(NULL, fb1, fb2, NULL, NULL);
    check(!used_proxy, "out=NULL で false を返す (クラッシュしない)");

    /*
     * ---- 2-3 / 2-4: slirp バックエンドでの分岐 ----
     *
     * ここは vm_nat_create が成功しないと試せない。
     * libslirp が無い環境 (CI / コンテナ) では SKIP にする。
     */
    if (!vm_nat_backend_available(VM_NAT_SLIRP)) {
        skip("libslirp が無いため slirp バックエンドの判定は試せない");
        note("代わりに loopback バックエンドで "
             "「代理が無い時は fallback」を確認する");

        {
            vm_nat_cfg_t cfg;
            vm_nat_t    *n = NULL;

            vm_nat_cfg_defaults(&cfg);
            cfg.backend = VM_NAT_LOOPBACK;

            if (vm_nat_create(&n, &cfg, NULL, NULL) == VM_OK && n != NULL) {
                d1 = d2 = 0;
                used_proxy = vm_nat_pick_guest_dns(n, fb1, fb2, &d1, &d2);
                check(!used_proxy && d1 == fb1 && d2 == fb2,
                      "loopback バックエンドでは代理を使わず config の値");
                vm_nat_destroy(n);
            } else {
                skip("loopback バックエンドも作れなかった");
            }
        }
        return;
    }

    {
        vm_nat_cfg_t cfg;
        vm_nat_t    *n = NULL;

        vm_nat_cfg_defaults(&cfg);
        cfg.backend = VM_NAT_SLIRP;

        /*
         * ★ resolv.conf を差し替えてから create する必要はない ★
         * 判定は vm_nat_pick_guest_dns() を呼んだ時点で行われるので、
         * create 後に環境を変えても効く。実機で resolv.conf が
         * 書き換わった場合 (DHCP の更新等) にも追従する設計。
         */
        if (vm_nat_create(&n, &cfg, NULL, NULL) != VM_OK || n == NULL) {
            skip("slirp バックエンドを作れなかった");
            return;
        }

        /* ---- 2-3: ホストがループバック → 代理を使わない ---- */
        if (write_resolv("nameserver 127.0.0.53\n")) {
            d1 = d2 = 0;
            used_proxy = vm_nat_pick_guest_dns(n, fb1, fb2, &d1, &d2);
            check(!used_proxy && d1 == fb1 && d2 == fb2,
                  "ホスト DNS = 127.0.0.53 → 代理を使わず 8.8.8.8/8.8.4.4 を配る");
            note("★これが ERR_NAME_NOT_RESOLVED の修正★");
            note("代理を配ると stub listener に届かず無応答になる");
        }

        /* ---- 2-4: ホストが実アドレス → 代理を使う ---- */
        if (write_resolv("nameserver 192.168.1.1\n")) {
            d1 = d2 = 0;
            used_proxy = vm_nat_pick_guest_dns(n, fb1, fb2, &d1, &d2);
            check(used_proxy &&
                  d1 == vm_nat_dns_ip(n) && d2 == vm_nat_dns_ip(n),
                  "ホスト DNS = 192.168.1.1 → 代理 (%08x) を配る",
                  (unsigned)vm_nat_dns_ip(n));
            note("この場合は代理が正しく動くので、VPN や社内 DNS に追従できる");
        }

        /* ---- 2-5: ループバック かつ config が空 → 公開 DNS を補う ---- */
        /*
         * dns1/dns2 が両方 0 だと vm_ppp は DNS オプションを
         * Config-Reject する。RAS / pppd は Reject を受けると設定を
         * 作り直してもう 1 往復するので、33.6kbps では接続完了が
         * 目に見えて遅くなる。最後の逃げ道として公開 DNS を入れる。
         */
        if (write_resolv("nameserver 127.0.0.53\n")) {
            d1 = d2 = 0;
            used_proxy = vm_nat_pick_guest_dns(n, 0u, 0u, &d1, &d2);
            check(!used_proxy && d1 != 0u && d2 != 0u,
                  "config が空でも 0 は配らない (d1=%08x d2=%08x)",
                  (unsigned)d1, (unsigned)d2);
            note("0 を配ると IPCP が Config-Reject され 1 往復増える");
        }

        drop_resolv();
        vm_nat_destroy(n);
    }
}

/* ==========================================================================
 * 3. ICMP ソケットの実測
 * ==========================================================================
 * 「ping 8.8.8.8 が通らない」の切り分け。
 * ここは権限に依存するので、値そのものは assert しない。
 * ========================================================================== */
static void test_icmp_probe(void)
{
    vm_netdiag_icmp_t ic;
    bool              ok;

    head("3. ICMP ソケットの可用性 (ping が通るか)");

    memset(&ic, 0, sizeof(ic));
    ok = vm_netdiag_icmp_probe(&ic);

    /*
     * ★ 戻り値と内訳の整合性 ★
     * これは環境に依存しない性質なので必ず成り立つべき。
     */
    check(ok == (ic.dgram_ok || ic.raw_ok),
          "戻り値は dgram_ok || raw_ok と一致する (ok=%d dgram=%d raw=%d)",
          (int)ok, (int)ic.dgram_ok, (int)ic.raw_ok);

    if (ok) {
        note("この環境ではゲストの ping が通る");
    } else {
        /*
         * FAIL にしない。コンテナや非 root では開けないのが普通で、
         * それは実装の不具合ではない。
         */
        skip("ICMP ソケットが開けない (dgram errno=%d raw errno=%d)",
             ic.dgram_err, ic.raw_err);
        note("libslirp の icmp_send() も同じ順序で試して失敗し、");
        note("ゲストの ICMP Echo は黙って捨てられる (TCP/UDP は正常)");
        note("恒久対策: /etc/sysctl.d/99-vmodem.conf");
    }

    if (ic.range_known) {
        note("ping_group_range = \"%u %u\" gid_in_range=%d",
             (unsigned)ic.range_lo, (unsigned)ic.range_hi,
             (int)ic.gid_in_range);
    }

    /* ---- out=NULL でも落ちない ---- */
    (void)vm_netdiag_icmp_probe(NULL);
    check(1, "out=NULL で呼んでもクラッシュしない");
}

/* ==========================================================================
 * 4. ping_group_range の "lo > hi = 空集合" の扱い
 * ==========================================================================
 * ★ここが最も誤りやすい★
 * Debian 系の既定値は "1 0"。これは「gid 1 以上 0 以下」= 空集合。
 * 「1 から 0 まで」という範囲ではない。
 * lo <= gid だけを見ると「gid 1000 は 1 以上だから OK」と誤判定する。
 * ========================================================================== */
static void test_ping_group_range(void)
{
    char  path[256];
    FILE *f;

    head("4. ping_group_range の解釈 (\"1 0\" は空集合)");

    snprintf(path, sizeof(path), "/tmp/vm_test_pgr_%ld", (long)getpid());

    /* ---- 4-1: Debian 既定 "1 0" は空集合 ---- */
    f = fopen(path, "w");
    if (f == NULL) {
        skip("一時ファイルを作れない");
        return;
    }
    fputs("1\t0\n", f);
    fclose(f);
    setenv("VM_PING_GROUP_RANGE", path, 1);

    {
        vm_netdiag_icmp_t ic;
        memset(&ic, 0, sizeof(ic));
        (void)vm_netdiag_icmp_probe(&ic);

        check(ic.range_known && ic.range_lo == 1u && ic.range_hi == 0u &&
              !ic.gid_in_range,
              "\"1 0\" は lo>hi なので gid_in_range=false "
              "(lo=%u hi=%u in=%d)",
              (unsigned)ic.range_lo, (unsigned)ic.range_hi,
              (int)ic.gid_in_range);
        note("★ lo <= gid だけを見ると誤判定する ★");
    }

    /* ---- 4-2: 全開放 "0 2147483647" なら含まれる ---- */
    f = fopen(path, "w");
    if (f != NULL) {
        fputs("0\t2147483647\n", f);
        fclose(f);

        {
            vm_netdiag_icmp_t ic;
            memset(&ic, 0, sizeof(ic));
            (void)vm_netdiag_icmp_probe(&ic);

            check(ic.range_known && ic.gid_in_range,
                  "\"0 2147483647\" なら現在の gid が含まれる "
                  "(= 99-vmodem.conf の設定値)");
        }
    }

    /* ---- 4-3: 読めない場合は range_known=false ---- */
    setenv("VM_PING_GROUP_RANGE", "/nonexistent/vmodem/pgr", 1);
    {
        vm_netdiag_icmp_t ic;
        memset(&ic, 0, sizeof(ic));
        (void)vm_netdiag_icmp_probe(&ic);
        check(!ic.range_known,
              "読めない時は range_known=false (推測しない)");
    }

    unsetenv("VM_PING_GROUP_RANGE");
    unlink(path);
}

/* ==========================================================================
 * 5. 診断レポートが落ちない事
 * ========================================================================== */
static void test_report(void)
{
    head("5. vm_netdiag_report() (起動時のログ出力)");

    /*
     * 出力内容は人間向けなので assert しない。
     * 「どんな resolv.conf でもクラッシュせず、
     *   自己参照も検出できる」事だけを確認する。
     */
    if (write_resolv("nameserver 127.0.0.53\n")) {
        vm_netdiag_report(ip4(192,168,99,0), 0xFFFFFF00u);
        check(1, "ループバック環境で警告を出して正常復帰");
    }

    if (write_resolv("nameserver 192.168.1.1\n")) {
        vm_netdiag_report(ip4(192,168,99,0), 0xFFFFFF00u);
        check(1, "通常環境で情報を出して正常復帰");
    }

    /* 自己参照: resolver が我々の仮想ネットワークを指している */
    if (write_resolv("nameserver 192.168.99.3\n")) {
        vm_netdiag_report(ip4(192,168,99,0), 0xFFFFFF00u);
        check(1, "自己参照 (192.168.99.3) を検出して正常復帰");
        note("代理先が我々自身になる設定ミス。症状は同じ「無応答」");
    }

    drop_resolv();
}

/* ========================================================================== */
int main(void)
{
    printf("=== test_dnsfix: DNS 修正 / ICMP 診断の検証 ===\n");

    /*
     * ログを黙らせない。
     * vm_netdiag_report() が何を出すかは実機の切り分けで重要なので、
     * テスト出力にも見えている方が良い。
     */
    vm_log_init(VM_LOG_INFO, NULL);

    test_resolv_parse();
    test_pick_guest_dns();
    test_icmp_probe();
    test_ping_group_range();
    test_report();

    printf("\n===========================================\n");
    printf("  PASS %d / FAIL %d / SKIP %d\n", g_pass, g_fail, g_skip);
    printf("===========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
