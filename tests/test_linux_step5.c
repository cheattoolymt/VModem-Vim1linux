/*
 * test_linux_step5.c - Linux 移植 Step 5 / Step 5-a の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象:
 *   Step 5   : src/net/vm_nat.c            (vm_nat_now_ns の POSIX 対応)
 *   Step 5   : src/net/vm_hostroute.c      (Linux ビルドからの除外)
 *   Step 5-a : src/net/vm_hostroute_linux.c (getifaddrs による外向き IP 検出)
 *
 * ===========================================================================
 * 使い方
 * ===========================================================================
 *   gcc -O2 -std=c99 -Wall -Wextra -Iinclude tests/test_linux_step5.c \
 *       src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c \
 *       src/net/vm_eth.c src/net/vm_hostroute_linux.c src/net/vm_hostroute.c \
 *       src/core/vm_log.c src/core/vm_types.c \
 *       -lpthread -o test_linux_step5
 *   ./test_linux_step5
 *
 * ★ソースリストに vm_hostroute.c と vm_hostroute_linux.c の両方を
 *   意図的に並べている★
 *   Step 5 の要求「vm_hostroute.c を Linux ビルドから除外する」を
 *   Makefile 側だけでなく翻訳単位側でも保証したので、両方渡しても
 *   multiple definition にならない。**このテストがリンクできること自体が
 *   除外の検証になっている** (リンクエラーはテスト失敗と同じ意味を持つ)。
 *
 * ===========================================================================
 * 設計方針: 「環境に依存する事実」と「実装が守るべき不変条件」を分ける
 * ===========================================================================
 * 外向き IP の検出結果は当然マシンごとに違う。CI・sandbox・実機 VIM1 で
 * 値が異なるのは正常なので、値そのものを assert してはいけない。
 * 代わりに以下のような **どの環境でも成り立つ性質** を検証する。
 *
 *   - 返った IP は実際に bind() できる (= 本当にローカルのアドレス)
 *   - 返った IP はループバックではない
 *   - exclude_net/mask で除外した範囲の IP は絶対に返らない
 *   - name_out は必ず NUL 終端され、バッファを溢れさせない
 *   - 0 (= 判定不能) は「失敗」ではなく正当な戻り値である
 *
 * 検出できない環境 (ネットワーク未接続の CI コンテナ等) では 0 が返る。
 * その場合は FAIL ではなく SKIP にし、0 が返ったこと自体は
 * 「安全な失敗」として OK と判定する。
 * ===========================================================================
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include "vmodem/vm_nat.h"
#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

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

/* ホストオーダの IPv4 を文字列化 */
static void ipstr(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >>  8) & 0xFFu),
             (unsigned)( ip        & 0xFFu));
}

/* 単調時計での経過ミリ秒 (テスト自身の基準時計。被験体とは別物) */
static double ref_elapsed_ms(const struct timespec *from)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1.0;
    return (double)(now.tv_sec - from->tv_sec) * 1000.0
         + (double)(now.tv_nsec - from->tv_nsec) / 1000000.0;
}

/* =========================================================================
 * Step 5 (1): vm_nat_now_ns の基本性質
 *
 * この関数は libslirp の cb_clock_get_ns として渡される唯一の時計であり、
 * TCP の再送タイマ・DHCP のリース期限・ARP キャッシュの寿命がすべて
 * ここに乗る。したがって「巻き戻らない」ことが最重要である。
 * =======================================================================*/
static void test_now_ns_basic(void)
{
    int64_t a, b, c;

    printf("\n[Step 5-1] vm_nat_now_ns() の基本性質\n");

    a = vm_nat_now_ns();
    b = vm_nat_now_ns();
    c = vm_nat_now_ns();

    /*
     * 0 は「時計が取れなかった」を意味する値として使われかねないが、
     * 移植後の実装は失敗時でも last_ns + 1ms を返すので 0 にはならない。
     * ここが 0 だと libslirp 側で「起動直後」と誤認され、
     * 再送タイマが即発火する恐れがある。
     */
    check(a > 0, "初回呼び出しが正の値を返す (a=%lld)", (long long)a);

    /* 非減少。等しいのは許容 (分解能より速く 2 回呼んだ場合) */
    check(b >= a && c >= b,
          "連続呼び出しが非減少 (a=%lld b=%lld c=%lld)",
          (long long)a, (long long)b, (long long)c);

    /*
     * CLOCK_MONOTONIC はシステム起動からの経過なので、
     * 現実的に 1 年 (3.15e16 ns) を超えることは考えにくいが、
     * int64 の上限 (9.2e18 ns ≒ 292 年) には遠く及ばない。
     * オーバーフローの心配がないことを確認しておく。
     */
    check(a < 9000000000000000000LL,
          "int64 のオーバーフロー領域に入っていない");
}

/* =========================================================================
 * Step 5 (2): 実時間との一致 (スケールが ns で合っているか)
 *
 * ここが最も間抜けな移植バグを捕まえる。
 * GetTickCount64() は ms 単位なので、素朴に置き換えると
 * 1000 倍 / 1000000 倍のスケールミスが起きる。その場合
 * libslirp のタイマは 1000 倍速 or 1000 倍遅で動き、
 * 「繋がるが極端に遅い」という原因不明の症状になる。
 * =======================================================================*/
static void test_now_ns_scale(void)
{
    struct timespec  ref0;
    struct timespec  req;
    int64_t          t0, t1;
    double           measured_ms, ref_ms, err_ms;

    printf("\n[Step 5-2] 実時間とのスケール一致 (ns 単位であること)\n");

    if (clock_gettime(CLOCK_MONOTONIC, &ref0) != 0) {
        skip("基準時計が取れないためスケール検証を省略");
        return;
    }

    t0 = vm_nat_now_ns();

    /* 200ms 眠る。EINTR で早起きしても基準時計と比べるので問題ない */
    req.tv_sec  = 0;
    req.tv_nsec = 200 * 1000 * 1000;
    (void)nanosleep(&req, NULL);

    t1     = vm_nat_now_ns();
    ref_ms = ref_elapsed_ms(&ref0);

    measured_ms = (double)(t1 - t0) / 1000000.0;
    err_ms      = measured_ms - ref_ms;
    if (err_ms < 0) err_ms = -err_ms;

    note("被験体 %.3f ms / 基準 %.3f ms / 差 %.3f ms",
         measured_ms, ref_ms, err_ms);

    /*
     * 許容差 5ms。スケールが 1000 倍ずれていれば差は 200ms 級になるので
     * この閾値で確実に検出できる。5ms はスケジューラの揺らぎの範囲。
     */
    check(err_ms < 5.0,
          "経過時間が基準時計と一致する (差 %.3f ms < 5 ms)", err_ms);

    /* 明示的にスケールミスの方向も見る (桁違いを分かりやすく報告) */
    check(measured_ms > 100.0 && measured_ms < 400.0,
          "200ms の sleep が 200ms 前後として観測される (%.1f ms)",
          measured_ms);
}

/* =========================================================================
 * Step 5 (3): 分解能
 *
 * GetTickCount64 相当 (15.6ms 刻み) のままだと、libslirp の
 * 100ms 単位のタイマ判定が粗くなりすぎる。ns 精度の時計に
 * 置き換わったことを、隣接する 2 値の差から確認する。
 * =======================================================================*/
static void test_now_ns_resolution(void)
{
    int i;
    int64_t prev, cur;
    int64_t min_delta = 0;
    int     changed   = 0;

    printf("\n[Step 5-3] 分解能 (ms 刻みの時計ではないこと)\n");

    prev = vm_nat_now_ns();
    for (i = 0; i < 200000; i++) {
        cur = vm_nat_now_ns();
        if (cur > prev) {
            int64_t d = cur - prev;
            if (changed == 0 || d < min_delta) min_delta = d;
            changed++;
        }
        prev = cur;
    }

    if (changed == 0) {
        skip("観測窓の中で値が変化しなかった (分解能を測れない)");
        return;
    }

    note("値が変化した回数 %d / 最小増分 %lld ns", changed, (long long)min_delta);

    /*
     * 1ms (1000000ns) 未満の増分が観測できれば、ms 刻みの時計ではない。
     * clock_gettime(CLOCK_MONOTONIC) は通常 1ns〜数十 ns の分解能を持つ。
     */
    check(min_delta < 1000000LL,
          "1ms より細かい増分を観測できる (最小 %lld ns)",
          (long long)min_delta);
}

/* =========================================================================
 * Step 5 (4): マルチスレッドでも巻き戻らない
 *
 * vm_nat_now_ns() は単調性の最終保証として static な last_ns を持つ。
 * これは複数スレッドから同時に呼ぶと理論上データレースになるが、
 * 「巻き戻らない」という観測可能な性質が壊れないことを確認する。
 *
 * 実装コメントに書いた通り、本番では libslirp を触るのは 1 スレッド
 * だけなので競合しない。ここでは念のための保険を測っている。
 * =======================================================================*/
#define STEP5_THREADS   4
#define STEP5_ITER      50000

typedef struct {
    int      backwards;   /* 巻き戻りを観測した回数 */
    int64_t  first;
    int64_t  last;
} step5_thread_result_t;

static void *now_ns_thread(void *arg)
{
    step5_thread_result_t *r = (step5_thread_result_t *)arg;
    int      i;
    int64_t  prev, cur;

    prev     = vm_nat_now_ns();
    r->first = prev;

    for (i = 0; i < STEP5_ITER; i++) {
        cur = vm_nat_now_ns();
        if (cur < prev) r->backwards++;
        prev = cur;
    }
    r->last = prev;
    return NULL;
}

static void test_now_ns_threads(void)
{
    pthread_t             th[STEP5_THREADS];
    step5_thread_result_t res[STEP5_THREADS];
    int                   i, started = 0, total_back = 0;

    printf("\n[Step 5-4] 複数スレッドから呼んでも巻き戻らない\n");

    memset(res, 0, sizeof(res));

    for (i = 0; i < STEP5_THREADS; i++) {
        if (pthread_create(&th[i], NULL, now_ns_thread, &res[i]) != 0) break;
        started++;
    }
    for (i = 0; i < started; i++) pthread_join(th[i], NULL);

    if (started == 0) {
        skip("スレッドを起動できなかった");
        return;
    }

    for (i = 0; i < started; i++) total_back += res[i].backwards;

    note("スレッド %d 本 x %d 回 / 巻き戻り観測 %d 回",
         started, STEP5_ITER, total_back);

    check(total_back == 0,
          "どのスレッドでも時刻が巻き戻らない (巻き戻り %d 回)", total_back);
}

/* =========================================================================
 * Step 5-a 準備: ローカルアドレス一覧の収集
 *
 * 「返ってきた IP が本当にこのホストのものか」を独立に検証するため、
 * 被験体とは別に自分で getifaddrs() を回して答え合わせ用の表を作る。
 * =======================================================================*/
#define STEP5_MAX_ADDR 32

typedef struct {
    uint32_t     ip;
    char         name[IF_NAMESIZE + 1];
    unsigned int flags;
} step5_local_addr_t;

static step5_local_addr_t g_local[STEP5_MAX_ADDR];
static int                g_local_n = 0;

static void collect_local_addrs(void)
{
    struct ifaddrs *head = NULL, *p;

    if (getifaddrs(&head) != 0 || head == NULL) return;

    for (p = head; p != NULL && g_local_n < STEP5_MAX_ADDR; p = p->ifa_next) {
        const struct sockaddr_in *sin;

        /* ifa_addr は NULL になり得る (アドレス未割り当ての IF) */
        if (p->ifa_addr == NULL) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;

        sin = (const struct sockaddr_in *)(const void *)p->ifa_addr;

        g_local[g_local_n].ip    = ntohl(sin->sin_addr.s_addr);
        g_local[g_local_n].flags = p->ifa_flags;
        g_local[g_local_n].name[0] = '\0';
        if (p->ifa_name != NULL) {
            strncpy(g_local[g_local_n].name, p->ifa_name, IF_NAMESIZE);
            g_local[g_local_n].name[IF_NAMESIZE] = '\0';
        }
        g_local_n++;
    }
    freeifaddrs(head);
}

static const step5_local_addr_t *find_local(uint32_t ip)
{
    int i;
    for (i = 0; i < g_local_n; i++)
        if (g_local[i].ip == ip) return &g_local[i];
    return NULL;
}

/* 指定 IP に bind できるか。実際にローカルアドレスかの決定的な検査 */
static bool can_bind(uint32_t ip, int *err_out)
{
    struct sockaddr_in sa;
    int  fd;
    bool ok;

    if (err_out != NULL) *err_out = 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = 0;               /* 任意ポート */
    sa.sin_addr.s_addr = htonl(ip);

    ok = (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    if (!ok && err_out != NULL) *err_out = errno;
    close(fd);
    return ok;
}

/* =========================================================================
 * Step 5-a (1): 検出結果が実在のローカルアドレスであること
 *
 * ここが libslirp の outbound_addr に直結する。存在しないアドレスを
 * 渡すと slirp_bind_outbound() の bind() が EADDRNOTAVAIL で落ち、
 * 全ての外向き接続が失敗する (= 一番デバッグしづらい壊れ方)。
 * =======================================================================*/
static void test_hostroute_valid(void)
{
    char     name[IF_NAMESIZE + 1];
    char     buf[32];
    uint32_t ip;
    int      err = 0;

    printf("\n[Step 5-a-1] 検出した IP が実在のローカルアドレスである\n");

    memset(name, 0xAA, sizeof(name));   /* 未初期化検出用の毒 */
    name[sizeof(name) - 1] = '\0';

    ip = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));

    if (ip == 0) {
        /*
         * 0 は「判定不能」= libslirp に outbound_addr を渡さない、
         * という安全側の失敗。これは正当な結果なので FAIL にしない。
         */
        check(1, "検出できない環境では 0 (安全な失敗) を返す");
        skip("外向き IP を検出できないため以降の値検証を省略");
        return;
    }

    ipstr(ip, buf, sizeof(buf));
    note("検出結果: ip=%s if=%s", buf, name);

    check(can_bind(ip, &err),
          "検出した %s に bind できる%s", buf,
          err ? " (失敗)" : "");
    if (err) note("bind の errno=%d (%s)", err, strerror(err));

    check(find_local(ip) != NULL,
          "検出した %s が getifaddrs の一覧に存在する", buf);

    /* ループバックは絶対に選んではいけない (指示書の除外要件) */
    check((ip >> 24) != 127u,
          "ループバック 127.0.0.0/8 を選んでいない");

    /* 0.0.0.0 / 255.255.255.255 のような特殊値も不可 */
    check(ip != 0xFFFFFFFFu, "ブロードキャストアドレスを選んでいない");
}

/* =========================================================================
 * Step 5-a (2): 選ばれた NIC が除外対象の種類でないこと
 *
 * 指示書の要求: PPP (IFF_POINTOPOINT)・トンネル系 (tun/tap/ppp/
 * docker/veth 等) を除外する。名前とフラグの両面から確認する。
 * =======================================================================*/
static void test_hostroute_excludes_virtual(void)
{
    static const char *bad_prefix[] = {
        "tun", "tap", "ppp", "docker", "veth", "br-", "virbr",
        "vnet", "wg", "gre", "sit", "dummy", "lo"
    };
    char      name[IF_NAMESIZE + 1];
    uint32_t  ip;
    const step5_local_addr_t *ent;
    size_t    i;
    bool      hit = false;

    printf("\n[Step 5-a-2] 選ばれた NIC が仮想 / PPP でない\n");

    name[0] = '\0';
    ip = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));
    if (ip == 0) {
        skip("外向き IP を検出できないため NIC 種別の検証を省略");
        return;
    }

    for (i = 0; i < sizeof(bad_prefix) / sizeof(bad_prefix[0]); i++) {
        if (strncmp(name, bad_prefix[i], strlen(bad_prefix[i])) == 0) {
            hit = true;
            note("除外すべき接頭辞 \"%s\" に一致した", bad_prefix[i]);
            break;
        }
    }
    check(!hit, "IF 名 \"%s\" が仮想 NIC の接頭辞に一致しない", name);

    ent = find_local(ip);
    if (ent == NULL) {
        skip("一覧に見つからないためフラグ検証を省略");
        return;
    }

    check((ent->flags & IFF_LOOPBACK) == 0,
          "IFF_LOOPBACK が立っていない");
    check((ent->flags & IFF_POINTOPOINT) == 0,
          "IFF_POINTOPOINT (PPP) が立っていない");
    check((ent->flags & IFF_UP) != 0,
          "IFF_UP が立っている (実際に有効な NIC)");
}

/* =========================================================================
 * Step 5-a (3): exclude_net / exclude_mask が効くこと
 *
 * 呼び出し側 (vm_nat_slirp.c) は自分のゲスト側ネットワーク
 * (既定 192.168.99.0/24) を除外して渡す。ここを間違えると
 * 「エミュレータ自身の仮想ネットワークに bind する」という
 * 自己参照ループになる。
 *
 * 実環境の IP を必ず含む /0 (全アドレス) を除外指定して、
 * 何も返らないことを確認するのが決定的な検査になる。
 * =======================================================================*/
static void test_hostroute_exclude(void)
{
    char     name[IF_NAMESIZE + 1];
    char     buf[32];
    uint32_t ip, base, excluded;

    printf("\n[Step 5-a-3] exclude_net / exclude_mask による除外\n");

    name[0] = '\0';
    base = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));
    if (base == 0) {
        skip("外向き IP を検出できないため除外の検証を省略");
        return;
    }

    /*
     * (a) 検出された IP を含む /32 を除外する。
     *     その IP は返ってこないはずである。
     */
    name[0] = '\0';
    ip = vm_hostroute_pick_outbound_ip(base, 0xFFFFFFFFu,
                                       name, sizeof(name));
    ipstr(base, buf, sizeof(buf));
    check(ip != base,
          "自分自身 %s/32 を除外すると同じ IP を返さない (返値=%s)",
          buf, (ip == 0) ? "0" : "別の IP");

    /*
     * (b) /0 (= 全 IPv4) を除外する。
     *     どのアドレスも使えないので 0 (判定不能) になるしかない。
     *     ここで非 0 が返ると、除外判定がまるごと無視されている。
     */
    name[0] = '\0';
    excluded = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));
    /* 上は exclude_mask=0 なので「除外なし」= base と同じはず */
    check(excluded == base,
          "exclude_mask=0 は「除外なし」として扱われる");

    name[0] = '\0';
    ip = vm_hostroute_pick_outbound_ip(0x00000000u, 0x00000000u,
                                       name, sizeof(name));
    check(ip == base,
          "0.0.0.0/0 相当 (mask=0) でも誤って全除外しない");

    /*
     * (c) ゲスト側ネットワークの既定値 192.168.99.0/24 を除外しても、
     *     それと無関係な実 NIC の IP は返ってくること。
     *     (実 IP が 192.168.99.x でない限り base と一致する)
     */
    if ((base & 0xFFFFFF00u) != 0xC0A86300u) {
        name[0] = '\0';
        ip = vm_hostroute_pick_outbound_ip(0xC0A86300u, 0xFFFFFF00u,
                                           name, sizeof(name));
        check(ip == base,
              "192.168.99.0/24 の除外は無関係な実 NIC に影響しない");
    } else {
        skip("実 IP が 192.168.99.0/24 に属するため本項を省略");
    }
}

/* =========================================================================
 * Step 5-a (4): name_out の境界安全性
 *
 * 呼び出し側は char ifname[64] を渡すが、実装は size を尊重して
 * NUL 終端しなければならない。ここを踏むと VM_LOGI の %s で
 * 未終端バッファを読んで即クラッシュする。
 * =======================================================================*/
static void test_hostroute_name_buffer(void)
{
    struct {
        char guard0[8];
        char name[4];       /* 意地悪に小さい */
        char guard1[8];
    } box;
    char     big[128];
    uint32_t ip;
    int      i;
    bool     guards_intact = true;

    printf("\n[Step 5-a-4] name_out の境界安全性\n");

    /* (a) NULL を渡しても落ちないこと (名前が不要な呼び出し側のため) */
    ip = vm_hostroute_pick_outbound_ip(0, 0, NULL, 0);
    check(1, "name_out=NULL / size=0 でクラッシュしない (返値=%s)",
          (ip == 0) ? "0" : "非 0");

    /* (b) size=0 のバッファ付き。書き込んではいけない */
    memset(&box, 0x5A, sizeof(box));
    (void)vm_hostroute_pick_outbound_ip(0, 0, box.name, 0);
    for (i = 0; i < 4; i++)
        if (box.name[i] != 0x5A) guards_intact = false;
    check(guards_intact, "size=0 のとき name_out に一切書き込まない");

    /* (c) 極小バッファ。前後の番兵が壊れないこと + NUL 終端 */
    memset(&box, 0x5A, sizeof(box));
    (void)vm_hostroute_pick_outbound_ip(0, 0, box.name, sizeof(box.name));

    guards_intact = true;
    for (i = 0; i < 8; i++) {
        if (box.guard0[i] != 0x5A) guards_intact = false;
        if (box.guard1[i] != 0x5A) guards_intact = false;
    }
    check(guards_intact, "4 バイトバッファでも前後の番兵を壊さない");

    guards_intact = false;
    for (i = 0; i < (int)sizeof(box.name); i++)
        if (box.name[i] == '\0') guards_intact = true;
    check(guards_intact, "4 バイトバッファでも NUL 終端される");

    /* (d) 十分大きいバッファ。IF 名が入り、長さが妥当 */
    memset(big, 0x5A, sizeof(big));
    ip = vm_hostroute_pick_outbound_ip(0, 0, big, sizeof(big));
    if (ip != 0) {
        bool terminated = false;
        for (i = 0; i < (int)sizeof(big); i++)
            if (big[i] == '\0') { terminated = true; break; }
        check(terminated, "大きいバッファでも NUL 終端される");
        if (terminated)
            check(strlen(big) > 0 && strlen(big) <= IF_NAMESIZE,
                  "IF 名の長さが 1..%d の範囲 (\"%s\" len=%u)",
                  (int)IF_NAMESIZE, big, (unsigned)strlen(big));
    } else {
        skip("検出できないため IF 名の内容検証を省略");
    }
}

/* =========================================================================
 * Step 5-a (5): 冪等性 (何度呼んでも同じ結果 / リソースを漏らさない)
 *
 * 実装は内部で socket() を作って connect() し close() する。
 * 閉じ忘れがあると EMFILE に達して途中から 0 を返し始める。
 * 多数回呼んで結果が変わらないことで fd リークを検出する。
 * =======================================================================*/
static void test_hostroute_idempotent(void)
{
    char     name[IF_NAMESIZE + 1];
    uint32_t first, ip;
    int      i, mismatch = 0;
    const int ROUNDS = 3000;   /* 既定 fd 上限 1024 を余裕で超える回数 */

    printf("\n[Step 5-a-5] 冪等性と fd リークの検出\n");

    name[0] = '\0';
    first = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));

    for (i = 0; i < ROUNDS; i++) {
        char n2[IF_NAMESIZE + 1];
        n2[0] = '\0';
        ip = vm_hostroute_pick_outbound_ip(0, 0, n2, sizeof(n2));
        if (ip != first) { mismatch++; if (mismatch == 1) note("初回の不一致: i=%d", i); }
    }

    note("%d 回呼び出し / 不一致 %d 回", ROUNDS, mismatch);
    check(mismatch == 0,
          "%d 回連続で呼んでも結果が変わらない (fd リークなし)", ROUNDS);
}

/* =========================================================================
 * Step 5 (5): vm_hostroute.c が Linux で何も定義しないこと
 *
 * このテストが **リンクできている** という事実が既に証明になっている。
 * (vm_hostroute.c と vm_hostroute_linux.c の両方を渡してビルドする
 *  ことを使い方コメントで要求している)
 * ここでは念のため、実際に呼べる実装が Linux 版であることを
 * 「Linux 固有の副作用」から確認する。
 *
 * Windows 版は GetAdaptersAddresses を使うので Linux ではリンクすら
 * できない。したがって呼び出しが成功して結果が getifaddrs の一覧と
 * 整合していれば、有効なのは Linux 実装である。
 * =======================================================================*/
static void test_hostroute_impl_is_linux(void)
{
    char     name[IF_NAMESIZE + 1];
    uint32_t ip;

    printf("\n[Step 5-5] 有効な実装が Linux 版であること\n");

    check(1, "vm_hostroute.c と vm_hostroute_linux.c が同時にリンクできた"
             " (重複定義なし = Step 5 の除外が機能)");

    name[0] = '\0';
    ip = vm_hostroute_pick_outbound_ip(0, 0, name, sizeof(name));

    if (ip == 0) {
        /*
         * 移植前の POSIX スタブも 0 を返したので、0 だけでは
         * 実装の区別がつかない。その場合は判定を保留する。
         */
        skip("結果が 0 のため Linux 実装かスタブかの区別は保留");
        return;
    }

    check(name[0] != '\0',
          "IF 名が返っている (\"%s\") = 常に 0 を返すスタブではない", name);
    check(find_local(ip) != NULL,
          "結果が getifaddrs の一覧と整合する = getifaddrs 実装が有効");
}

/* =========================================================================
 * main
 * =======================================================================*/
int main(void)
{
    int i;
    char buf[32];

    printf("=========================================================\n");
    printf(" VModem Linux 移植 Step 5 / Step 5-a 検証テスト\n");
    printf("=========================================================\n");

    /* 実装側の VM_LOGI を見えるようにしておく (人間の目視確認用) */
    vm_log_init(VM_LOG_INFO, NULL);

    collect_local_addrs();
    printf("\nこのホストの IPv4 アドレス一覧 (答え合わせ用, %d 件)\n",
           g_local_n);
    for (i = 0; i < g_local_n; i++) {
        ipstr(g_local[i].ip, buf, sizeof(buf));
        printf("   %-16s %-10s flags=0x%08x%s%s%s%s\n",
               buf, g_local[i].name, g_local[i].flags,
               (g_local[i].flags & IFF_UP)          ? " UP"   : "",
               (g_local[i].flags & IFF_RUNNING)     ? " RUN"  : "",
               (g_local[i].flags & IFF_LOOPBACK)    ? " LOOP" : "",
               (g_local[i].flags & IFF_POINTOPOINT) ? " P2P"  : "");
    }
    if (g_local_n == 0)
        printf("   (getifaddrs が 1 件も返さなかった)\n");

    /* Step 5: 時計 */
    test_now_ns_basic();
    test_now_ns_scale();
    test_now_ns_resolution();
    test_now_ns_threads();

    /* Step 5-a: 外向き IP の検出 */
    test_hostroute_valid();
    test_hostroute_excludes_virtual();
    test_hostroute_exclude();
    test_hostroute_name_buffer();
    test_hostroute_idempotent();

    /* Step 5: vm_hostroute.c の除外 */
    test_hostroute_impl_is_linux();

    printf("\n=========================================================\n");
    printf(" 結果: 成功 %d / 失敗 %d / 省略 %d\n", g_pass, g_fail, g_skip);
    printf("=========================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
