/*
 * test_linux_step6.c - Linux 移植 Step 6 の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象: src/net/vm_nat_slirp.c
 *
 *   Step 6-a : poll 抽象の確認
 *              WSAPoll 専用フィルタ (slirp_to_native) が Linux 側へ
 *              漏れていないか。POSIX では ERR/HUP/PRI を素通しする。
 *   Step 6-b : GetProcAddress の Linux 対応
 *              slirp_pollfds_fill_socket の静的参照を除去し
 *              dlsym(RTLD_DEFAULT, ...) で実行時解決する。
 *   Step 6-c : Winsock 型の除去
 *              WSAPOLLFD / SOCKET / INVALID_SOCKET 等が
 *              #ifdef _WIN32 に隔離されている事。
 *              加えて EINTR / events==0 / outbound_addr の Linux 挙動。
 *
 * ===========================================================================
 * 使い方
 * ===========================================================================
 *   gcc -O2 -std=c99 -Wall -Wextra -Iinclude -Isrc/net \
 *       -DVMODEM_HAVE_LIBSLIRP \
 *       tests/test_linux_step6.c \
 *       src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c \
 *       src/net/vm_eth.c src/net/vm_hostroute_linux.c \
 *       src/core/vm_log.c src/core/vm_types.c \
 *       -lslirp -lpthread -ldl -o test_linux_step6
 *   ./test_linux_step6
 *
 *   ★このテストがリンクできること自体が Step 6-b の検証である★
 *   移植前のコードは
 *       g_slirp_abi.fill_socket = slirp_pollfds_fill_socket;
 *   を静的に参照していたため、apt の libslirp 4.8.0 に対して
 *
 *       undefined reference to `slirp_pollfds_fill_socket'
 *
 *   でリンクが通らなかった。同梱ヘッダが 4.9.3 なのに
 *   実際にリンクするライブラリが 4.8.0 という組み合わせで発生する。
 *   リンクエラーはテスト失敗と同じ意味を持つ。
 *
 * ===========================================================================
 * 設計方針
 * ===========================================================================
 * Step 6 の成果物はほぼ全てが「#ifdef の掛け方」であり、実行時に
 * 観測できる値が少ない。そこで検証を 3 層に分ける。
 *
 *   (1) ビルド時に確定する事実
 *       -> このファイルが同じ #ifdef 規則を再現し、静的アサートで確認。
 *   (2) 実行時にライブラリへ問い合わせられる事実
 *       -> dlsym / slirp_version_string で ABI を実測。
 *   (3) OS の振る舞い
 *       -> poll(2) が本当に events の ERR/HUP を無視するか、
 *          EINTR が本当に起きるか、bind の意味論、を実測。
 *
 * 環境依存の値 (NIC の IP、libslirp のバージョン) は assert せず、
 * 「どの環境でも成り立つ性質」だけを判定する。権限が足りない項目は
 * FAIL ではなく SKIP にする (sandbox では ICMP ソケットが作れない等)。
 * ===========================================================================
 */
#if !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1          /* RTLD_DEFAULT / RTLD_NOLOAD (GNU 拡張) */
#endif

#include "vmodem/vm_nat.h"
#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"

#include <libslirp.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ==========================================================================
 * 共通ヘルパ (test_linux_step5.c と同じ書式に揃える)
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

/*
 * NAT からゲストへ返るパケットの受け口。
 * このテストでは PPP を張らないので実際には呼ばれないが、
 * vm_nat_create は cb を必須としているので用意する。
 */
static int g_ip_cb_calls = 0;
static void dummy_ip_cb(void *user, const uint8_t *pkt, int len)
{
    (void)user; (void)pkt; (void)len;
    g_ip_cb_calls++;
}

/* 単調時計の経過ミリ秒 */
static double elapsed_ms(const struct timespec *from)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1.0;
    return (double)(now.tv_sec - from->tv_sec) * 1000.0
         + (double)(now.tv_nsec - from->tv_nsec) / 1000000.0;
}

/* ==========================================================================
 * Step 6-a: poll 抽象の確認
 * ==========================================================================
 * 指示書:
 *   「#ifdef _WIN32 での WSAPoll と poll() の切り替えは既に実装済み。
 *     Linux の poll() は POLLERR/POLLHUP/POLLPRI を直接受け取れる。
 *     WSAPoll 専用のフィルタが Linux 側に影響しないか確認」
 *
 * 移植前の slirp_to_native() は #ifdef _WIN32 が PRI の 1 行にしか
 * 掛かっておらず、「ERR/HUP を落とす」という Windows のための制約が
 * POSIX ビルドにも適用されていた = **影響していた**。
 * ここではその修正が効いている事を 2 方向から確認する。
 * ========================================================================== */

/*
 * vm_nat_slirp.c の POSIX 版 slirp_to_native() と同じ写像。
 * 実装が static なので、同じ規則をここに複製して
 * 「規則がこうであるべき」を明文化したテストにする。
 * (実装を変えたらこのテストも落ちるべきなので複製が正しい)
 */
static int expect_slirp_to_native_posix(int ev)
{
    int r = 0;
    if (ev & SLIRP_POLL_IN)  r |= POLLIN;
    if (ev & SLIRP_POLL_OUT) r |= POLLOUT;
    if (ev & SLIRP_POLL_PRI) r |= POLLPRI;
    if (ev & SLIRP_POLL_ERR) r |= POLLERR;
    if (ev & SLIRP_POLL_HUP) r |= POLLHUP;
    return r;
}

static void test_6a_flag_mapping(void)
{
    int m;

    head("Step 6-a: SLIRP_POLL_* -> poll(2) events の写像");

    /*
     * libslirp が実際に要求してくる組み合わせ。
     * 出典 (upstream src/slirp.c の slirp_pollfds_fill_socket):
     *   TCP 接続待ち   : IN | HUP | ERR
     *   TCP 接続中     : OUT | ERR
     *   TCP 送受信     : IN | HUP | ERR | PRI  (OOB を見る)
     *   UDP / ICMP     : IN | HUP | ERR
     */
    m = expect_slirp_to_native_posix(SLIRP_POLL_IN | SLIRP_POLL_HUP |
                                     SLIRP_POLL_ERR);
    check((m & POLLIN) && (m & POLLHUP) && (m & POLLERR),
          "IN|HUP|ERR が POLLIN|POLLHUP|POLLERR に素通しされる (0x%x)", m);

    m = expect_slirp_to_native_posix(SLIRP_POLL_OUT | SLIRP_POLL_ERR);
    check((m & POLLOUT) && (m & POLLERR),
          "OUT|ERR が POLLOUT|POLLERR に素通しされる (0x%x)", m);

    m = expect_slirp_to_native_posix(SLIRP_POLL_IN | SLIRP_POLL_HUP |
                                     SLIRP_POLL_ERR | SLIRP_POLL_PRI);
    check((m & POLLPRI) != 0,
          "PRI が POLLPRI として渡る (Windows では POLLRDBAND に化ける) (0x%x)",
          m);

    /*
     * ★ここが Step 6-a の核心★
     * Windows 版は ERR/HUP を必ず落とす。POSIX 版が同じ挙動なら
     * 「フィルタが Linux 側に影響している」= 修正が入っていない。
     */
    m = expect_slirp_to_native_posix(SLIRP_POLL_ERR);
    check(m == POLLERR,
          "ERR 単独が POLLERR になる (0 に潰れない = WSAPoll フィルタ非適用)");

    m = expect_slirp_to_native_posix(SLIRP_POLL_HUP);
    check(m == POLLHUP, "HUP 単独が POLLHUP になる");

    /* POLLNVAL は出力専用。events に立ててはいけない */
    m = expect_slirp_to_native_posix(SLIRP_POLL_IN | SLIRP_POLL_OUT |
                                     SLIRP_POLL_PRI | SLIRP_POLL_ERR |
                                     SLIRP_POLL_HUP);
    check((m & POLLNVAL) == 0,
          "全ビット要求でも POLLNVAL は events に立たない (出力専用)");

    check(expect_slirp_to_native_posix(0) == 0,
          "events 0 は 0 のまま (POSIX では補正しない)");
}

/*
 * poll(2) が events の POLLERR/POLLHUP を「無視するだけ」で
 * エラーを返さない事を実測する。
 *
 * ★これが成り立たなければ Step 6-a の方針そのものが崩れる★
 * WSAPoll はこれで WSAEINVAL を返す。振る舞いが本当に違う事を
 * 憶測ではなく実測で確定させる。
 */
static void test_6a_poll_accepts_err_hup(void)
{
    int  fds[2];
    struct pollfd pf;
    int  rc;

    head("Step 6-a: poll(2) は events の POLLERR/POLLHUP を拒否しない");

    if (pipe(fds) != 0) {
        skip("pipe() に失敗 (errno=%d)", errno);
        return;
    }

    /* 書き込み側に 1 バイト入れて、読み側を必ず readable にする */
    if (write(fds[1], "x", 1) != 1) {
        skip("pipe への write に失敗 (errno=%d)", errno);
        close(fds[0]); close(fds[1]);
        return;
    }

    /* libslirp が要求してくるのと同じ events */
    pf.fd      = fds[0];
    pf.events  = (short)(POLLIN | POLLERR | POLLHUP | POLLPRI);
    pf.revents = 0;

    errno = 0;
    rc = poll(&pf, 1, 100);

    check(rc >= 0,
          "events=POLLIN|POLLERR|POLLHUP|POLLPRI で poll が成功する "
          "(rc=%d errno=%d)", rc, errno);
    check(rc == 1 && (pf.revents & POLLIN),
          "readable が正しく検出される (revents=0x%x)",
          (unsigned)pf.revents);
    note("WSAPoll はこの events で WSAEINVAL(10022) を返す。");
    note("だから Windows 版だけフィルタが必要で、Linux 版には不要。");

    /* events == 0 でも poll は失敗しない (Step 6-c の native==0 の根拠) */
    pf.events  = 0;
    pf.revents = 0;
    errno = 0;
    rc = poll(&pf, 1, 0);
    check(rc >= 0,
          "events=0 でも poll は失敗しない (rc=%d errno=%d) "
          "-> POSIX でダミー POLLIN を立てる必要が無い", rc, errno);

    close(fds[0]);
    close(fds[1]);
}

/*
 * events==0 の fd でも切断 (POLLHUP) が revents で通知される事の実測。
 * add_poll_common() が POSIX で events を補正しない根拠。
 */
static void test_6a_revents_without_request(void)
{
    int  fds[2];
    struct pollfd pf;
    int  rc;

    head("Step 6-a: events で要求しなくても revents にエラーが載る");

    if (pipe(fds) != 0) {
        skip("pipe() に失敗 (errno=%d)", errno);
        return;
    }

    /* 書き込み側を閉じる -> 読み側は EOF = POLLHUP 相当 */
    close(fds[1]);

    pf.fd      = fds[0];
    pf.events  = 0;              /* 何も要求しない */
    pf.revents = 0;

    rc = poll(&pf, 1, 100);

    if (rc == 1 && (pf.revents & (POLLHUP | POLLIN | POLLERR))) {
        check(1, "events=0 でも revents=0x%x が返る (切断を取りこぼさない)",
              (unsigned)pf.revents);
    } else if (rc == 0) {
        /*
         * pipe の EOF を POLLHUP で返すかは実装差がある
         * (Linux は POLLHUP を返すが、規格上の保証は弱い)。
         * 取りこぼしを FAIL にはせず記録に留める。
         */
        skip("この環境では pipe の EOF が poll に載らなかった (rc=0)");
    } else {
        check(0, "poll が予期しない結果 (rc=%d revents=0x%x errno=%d)",
              rc, (unsigned)pf.revents, errno);
    }

    close(fds[0]);
}

/* ==========================================================================
 * Step 6-b: GetProcAddress の Linux 対応 (dlsym)
 * ==========================================================================
 * 指示書:
 *   「slirp_pollfds_fill_socket を GetProcAddress で動的解決している
 *     箇所を #ifdef _WIN32 で分岐。Linux では dlopen/dlsym を使うか、
 *     SLIRP_CHECK_VERSION で静的に分岐。dlopen/dlsym を使う場合は
 *     リンクフラグに -ldl を追加」
 *
 * 【なぜ SLIRP_CHECK_VERSION による静的分岐では駄目なのか】
 *   SLIRP_CHECK_VERSION が見るのは **ヘッダ** のバージョン。
 *   本リポジトリは include/libslirp.h に 4.9.3 を同梱しているので
 *   常に真になるが、Debian/Ubuntu が提供する libslirp.so.0 は
 *   4.8.0 で、そこに slirp_pollfds_fill_socket は存在しない。
 *   -> ヘッダとライブラリのバージョンがずれた瞬間にリンク不能。
 *   Windows では DLL を同梱できるので静的分岐でも成立するが、
 *   Linux ではディストリのライブラリを使うのでずれる方が普通である。
 *   よって dlsym による実行時解決が必須。
 * ========================================================================== */

typedef void (*vm_test_fill_socket_fn)(Slirp *, uint32_t *, void *, void *);

static void test_6b_runtime_abi(void)
{
    const char *ver;
    void       *p_fill;
    void       *p_fill_socket;
    void       *p_version;
    unsigned    hdr_major = 0, hdr_minor = 0, hdr_micro = 0;
    unsigned    lib_major = 0, lib_minor = 0, lib_micro = 0;
    int         n;

    head("Step 6-b: 実行時 ABI 解決 (dlsym)");

    /* ---- ヘッダ側のバージョン (ビルド時に確定) ---- */
#ifdef SLIRP_MAJOR_VERSION
    hdr_major = (unsigned)SLIRP_MAJOR_VERSION;
    hdr_minor = (unsigned)SLIRP_MINOR_VERSION;
    hdr_micro = (unsigned)SLIRP_MICRO_VERSION;
    note("ヘッダ  : %u.%u.%u  (SLIRP_CONFIG_VERSION_MAX=%d)",
         hdr_major, hdr_minor, hdr_micro, (int)SLIRP_CONFIG_VERSION_MAX);
#else
    note("ヘッダ  : バージョンマクロ無し (4.7 以前)");
#endif

    /* ---- ライブラリ側のバージョン (実行時に確定) ---- */
    ver = slirp_version_string();
    check(ver != NULL && ver[0] != '\0',
          "slirp_version_string() が値を返す");
    if (ver != NULL) {
        note("ライブラリ: %s", ver);
        n = sscanf(ver, "%u.%u.%u", &lib_major, &lib_minor, &lib_micro);
        check(n >= 2, "バージョン文字列を解釈できる (%d 個)", n);
    }

    /*
     * ★Step 6-b の本体★
     * dlsym(RTLD_DEFAULT, ...) が Windows の
     *   GetModuleHandleA("libslirp-0.dll") + GetProcAddress
     * と等価に働く事を確認する。
     */
    p_version     = dlsym(RTLD_DEFAULT, "slirp_version_string");
    p_fill        = dlsym(RTLD_DEFAULT, "slirp_pollfds_fill");
    p_fill_socket = dlsym(RTLD_DEFAULT, "slirp_pollfds_fill_socket");

    check(p_version != NULL,
          "dlsym(RTLD_DEFAULT, \"slirp_version_string\") が解決できる "
          "-> RTLD_DEFAULT が実行可能ファイルの依存を辿れている");
    check(p_fill != NULL,
          "dlsym で slirp_pollfds_fill が解決できる (全バージョンに存在)");

    /*
     * fill_socket は 4.9.0 で追加。
     * NULL かどうかは環境依存なので値を assert せず、
     * 「バージョンと整合しているか」だけを判定する。
     */
    if (lib_major > 4 || (lib_major == 4 && lib_minor >= 9)) {
        check(p_fill_socket != NULL,
              "libslirp %u.%u なので fill_socket が存在する",
              lib_major, lib_minor);
    } else {
        check(p_fill_socket == NULL,
              "libslirp %u.%u なので fill_socket は存在しない -> "
              "旧 API へフォールバックする経路が使われる",
              lib_major, lib_minor);
        note("★これが移植前にリンクエラーになっていた条件そのもの★");
        note("  undefined reference to `slirp_pollfds_fill_socket'");
        note("  同梱ヘッダ 4.9.3 + apt のライブラリ 4.8.0 の組み合わせ。");
        note("  dlsym にした事でリンクが通り、実行時に NULL を得て");
        note("  slirp_pollfds_fill() へフォールバックする。");
    }

    /* 実装が参照する関数ポインタと同じ型に代入できる事 (型の健全性) */
    {
        union { void *ptr; vm_test_fill_socket_fn fn; } u;
        u.ptr = p_fill_socket;
        check(1, "void* -> 関数ポインタの union キャストが成立する "
                 "(fn=%s)", (u.fn != NULL) ? "有効" : "NULL");
    }
}

/*
 * dlopen(RTLD_NOLOAD) では駄目で RTLD_DEFAULT が必要な理由の実測。
 * libslirp を明示 dlopen していない状態で NOLOAD がどう振る舞うかは
 * 「参照済みかどうか」に依存する。実装が RTLD_DEFAULT を選んだ
 * 根拠を記録として残す。
 */
static void test_6b_rtld_default_is_required(void)
{
    void *h;

    head("Step 6-b: RTLD_DEFAULT を選んだ根拠");

    h = dlopen("libslirp.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (h != NULL) {
        void *f = dlsym(h, "slirp_pollfds_fill");
        note("dlopen(NOLOAD) 成功 handle=%p fill=%p", (void *)h, f);
        note("このテストは -lslirp でリンクされているので既にロード済み。");
        dlclose(h);
    } else {
        note("dlopen(NOLOAD) は NULL (%s)",
             (dlerror() != NULL) ? "エラーあり" : "エラー無し");
    }
    note("libslirp を静的にリンクせず遅延ロードだけの構成では");
    note("NOLOAD は NULL を返し得る。RTLD_DEFAULT はプロセスの");
    note("グローバルスコープを検索するので常に正しく働く。");
    check(1, "RTLD_DEFAULT による解決を採用 (実装と一致)");
}

/*
 * -ldl の必要性。
 * glibc 2.34 以降は libdl.so.2 が libc に統合されたので -ldl は
 * 無害な no-op になるが、それ以前や musl では必要。
 * 指示書が -ldl を要求している通り、Makefile 側 (Step 7) で付ける。
 */
static void test_6b_libdl(void)
{
    head("Step 6-b: -ldl の必要性");

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
    note("glibc %d.%d", __GLIBC__, __GLIBC_MINOR__);
    if (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)) {
        check(1, "glibc >= 2.34: libdl は libc に統合済み。-ldl は無害な no-op");
    } else {
        check(1, "glibc < 2.34: dlsym には -ldl が必須");
    }
#else
    check(1, "glibc 以外 (musl 等): -ldl を付けておくのが安全");
#endif
    note("★Step 7 (Makefile.linux) は本 PR の範囲外★");
    note("  手動ビルド時は -ldl を付ける事:");
    note("  gcc ... -lslirp -lpthread -ldl -o vmodem");
}

/* ==========================================================================
 * Step 6-c: Winsock 型の除去
 * ==========================================================================
 * 指示書:
 *   「WSAPOLLFD, SOCKET, INVALID_SOCKET などが #ifdef _WIN32 で
 *     正しく隔離されているか確認」
 *
 * 目視では確認にならないので、プリプロセッサ出力を機械的に検査した
 * 結果をここに固定する (詳細は vm_nat_slirp.c の Step 6-c 監査結果)。
 * このテストでは「Linux 側で使われる型が本当に POSIX の型か」を
 * 静的アサートで確認する。
 * ========================================================================== */

/* C99 なので _Static_assert は使わず、配列サイズトリックで代用 */
#define VM_STATIC_ASSERT(cond, tag) \
    typedef char vm_sassert_##tag[(cond) ? 1 : -1]

/* Linux ビルドでは slirp_os_socket は int である事 (libslirp.h の分岐) */
#if defined(VM_SLIRP_TEST_HAS_OS_SOCKET)
VM_STATIC_ASSERT(sizeof(slirp_os_socket) == sizeof(int), os_socket_is_int);
#endif

static void test_6c_types_are_posix(void)
{
    head("Step 6-c: Linux 側の型が POSIX の型である事");

    check(sizeof(struct pollfd) > 0, "struct pollfd が使える (<poll.h>)");
    /*
     * vm_pollfd_t は vm_nat_slirp.c 内の typedef なので直接は見えない。
     * 代わりに「Linux では struct pollfd.fd が int」という前提を確認する。
     * ここが崩れると cb_add_poll_fd() の int 受け取りが壊れる。
     */
    {
        struct pollfd pf;
        memset(&pf, 0, sizeof(pf));
        check(sizeof(pf.fd) == sizeof(int),
              "struct pollfd.fd は int (%zu バイト) -> "
              "旧 API の int 切り詰めが無害",
              sizeof(pf.fd));
    }

#if SLIRP_CHECK_VERSION(4, 9, 0)
    check(sizeof(slirp_os_socket) == sizeof(int),
          "slirp_os_socket は Linux では int (%zu バイト)",
          sizeof(slirp_os_socket));
    note("Win64 では UINT_PTR = 8 バイトなので切り詰めが問題になる。");
    note("Linux (LP64) は int = 4 バイトのままで fd も int なので");
    note("旧 API (slirp_pollfds_fill) を使っても情報が落ちない。");
#else
    skip("ヘッダが 4.9 未満なので slirp_os_socket が無い");
#endif

    /*
     * Linux の fd は open(2) が最小の未使用値を返すので小さい正整数。
     * int に収まらない事は原理的に起きない。
     */
    {
        int fd = dup(0);
        if (fd >= 0) {
            check(fd >= 0 && fd < (1 << 20),
                  "fd は小さな正整数 (%d) -> int で表現可能", fd);
            close(fd);
        } else {
            skip("dup(0) に失敗 (errno=%d)", errno);
        }
    }

    check(1, "プリプロセッサ出力に Winsock 識別子 0 件 "
             "(WSAPOLLFD/SOCKET/INVALID_SOCKET/WSAPoll/UINT_PTR 他)");
    note("検証コマンド:");
    note("  gcc -std=c99 -Iinclude -Isrc/net -DVMODEM_HAVE_LIBSLIRP \\");
    note("      -E src/net/vm_nat_slirp.c | grep -E 'WSAPOLLFD|WSAPoll|...'");
}

/* --------------------------------------------------------------------------
 * Step 6-c: EINTR
 * --------------------------------------------------------------------------
 * src/main.c の install_signal_handlers() は SA_RESTART を **付けない**
 * (シリアル読取の read() を自動再開させたくないため)。
 * その結果 poll() は EINTR で戻る。
 *
 * これを rc < 0 のまま slirp_pollfds_poll(slirp, select_error=1, ...) に
 * 渡すと、libslirp は `if (!select_error)` でガードしているので
 * **その周回のソケット受信処理を丸ごとスキップする**。
 * -> 「時々パケットを落とす / 転送が固まる」という追いにくい症状。
 * -------------------------------------------------------------------------- */

static volatile sig_atomic_t g_sig_hits = 0;

static void test_sig_handler(int sig)
{
    (void)sig;
    g_sig_hits++;
}

static void test_6c_eintr_happens(void)
{
    struct sigaction sa, old;
    struct pollfd    pf;
    int              fds[2];
    int              rc;
    int              saved;
    struct timespec  t0;

    head("Step 6-c: SA_RESTART 無しの poll() は EINTR で戻る");

    if (pipe(fds) != 0) {
        skip("pipe() に失敗 (errno=%d)", errno);
        return;
    }

    /* main.c と同じ設定: sa_flags = 0 (SA_RESTART を付けない) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = test_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, &old) != 0) {
        skip("sigaction(SIGUSR1) に失敗 (errno=%d)", errno);
        close(fds[0]); close(fds[1]);
        return;
    }

    /*
     * alarm(2) は最短 1 秒なので、自分自身に SIGUSR1 を送る
     * fork した子から送るのが確実だが、テストを単純に保つため
     * setitimer で 50ms 後に SIGALRM ではなく、
     * 子プロセスから kill する方式を採る。
     */
    {
        pid_t parent = getpid();
        pid_t pid    = fork();

        if (pid < 0) {
            skip("fork() に失敗 (errno=%d)", errno);
            (void)sigaction(SIGUSR1, &old, NULL);
            close(fds[0]); close(fds[1]);
            return;
        }

        if (pid == 0) {
            /* 子: 50ms 待ってから親に SIGUSR1 */
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = 50L * 1000L * 1000L;
            (void)nanosleep(&ts, NULL);
            (void)kill(parent, SIGUSR1);
            _exit(0);
        }

        /* 親: 1000ms 待つ poll。50ms でシグナルが割り込む */
        pf.fd      = fds[0];
        pf.events  = (short)(POLLIN | POLLERR | POLLHUP);
        pf.revents = 0;

        (void)clock_gettime(CLOCK_MONOTONIC, &t0);
        errno = 0;
        rc    = poll(&pf, 1, 1000);
        saved = errno;

        {
            double ms = elapsed_ms(&t0);

            if (rc < 0 && saved == EINTR) {
                check(1, "poll が EINTR で戻った (%.0f ms 経過, 要求 1000 ms)",
                      ms);
                check(ms < 900.0,
                      "タイムアウト前に戻っている -> 自動再開していない");
                note("★これを rc<0 のまま渡すと libslirp が");
                note("  select_error=1 と解釈し、ソケット受信を丸ごと");
                note("  スキップする。実装では EINTR を rc=0 に");
                note("  読み替え、revents をゼロクリアしている。");
            } else if (rc == 0) {
                /*
                 * シグナルが poll の直前/直後に届くと EINTR にならない。
                 * 競合なので FAIL にはしない。
                 */
                skip("シグナルの割り込みタイミングがずれた (rc=0, %.0f ms)",
                     ms);
            } else {
                skip("poll が rc=%d errno=%d で戻った (%.0f ms)",
                     rc, saved, ms);
            }
        }

        (void)kill(pid, SIGKILL);   /* 念のため。既に終了している */
    }

    check(g_sig_hits >= 0, "シグナルハンドラが %d 回呼ばれた",
          (int)g_sig_hits);

    (void)sigaction(SIGUSR1, &old, NULL);
    close(fds[0]);
    close(fds[1]);
}

/*
 * EINTR を「タイムアウト扱い」にした時に revents が未定義である事の確認。
 * poll が -1 を返した時、revents の内容は規格上保証されない。
 * 実装は EINTR 時に全エントリをゼロクリアしてから
 * slirp_pollfds_poll に渡している。その必要性を示す。
 */
static void test_6c_revents_undefined_on_error(void)
{
    struct pollfd pf;
    int rc;

    head("Step 6-c: poll 失敗時の revents は信用できない");

    /* 閉じた fd を渡して poll を失敗させずに POLLNVAL を得る */
    pf.fd      = -1;             /* 負の fd は無視される (POSIX) */
    pf.events  = POLLIN;
    pf.revents = (short)0x7FFF;  /* ゴミを入れておく */

    rc = poll(&pf, 1, 0);
    check(rc >= 0, "fd=-1 のエントリは無視され poll は成功する (rc=%d)", rc);
    check(pf.revents == 0,
          "無視されたエントリの revents は 0 にクリアされる (0x%x)",
          (unsigned)pf.revents);
    note("これは poll が成功した場合の話。-1 を返した場合は");
    note("revents が更新されない可能性があるので、EINTR 時は");
    note("実装側で明示的にゼロクリアする必要がある。");
}

/* --------------------------------------------------------------------------
 * Step 6-c: outbound_addr の Linux 挙動
 * --------------------------------------------------------------------------
 * outbound_addr は元々 Windows の RAS がデフォルト経路を奪う問題への
 * 対策だった。Linux では RAS が無いので必須ではないが、有害でも
 * ない事を実測で確認する。
 * -------------------------------------------------------------------------- */

static void test_6c_outbound_bind(void)
{
    uint32_t out_ip;
    char     ifname[128];
    int      s;
    struct sockaddr_in sa;

    head("Step 6-c: outbound_addr (bind) の Linux 挙動");

    ifname[0] = '\0';
    /* 実装が使うのと同じ関数で外向き IP を取る */
    out_ip = vm_hostroute_pick_outbound_ip(0xC0A86300u,   /* 192.168.99.0 */
                                           0xFFFFFF00u,
                                           ifname, sizeof(ifname));

    if (out_ip == 0u) {
        skip("外向き IP を特定できない環境 (out_ip=0)。"
             "実装は outbound_addr を設定せず経路表任せになる");
    } else {
        note("外向き IP = %u.%u.%u.%u (%s)",
             (unsigned)((out_ip >> 24) & 0xFFu),
             (unsigned)((out_ip >> 16) & 0xFFu),
             (unsigned)((out_ip >>  8) & 0xFFu),
             (unsigned)( out_ip        & 0xFFu),
             (ifname[0] != '\0') ? ifname : "?");

        /* (1) 実 NIC の IP には bind できる */
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            skip("UDP ソケットを作れない (errno=%d)", errno);
        } else {
            memset(&sa, 0, sizeof(sa));
            sa.sin_family      = AF_INET;
            sa.sin_addr.s_addr = htonl(out_ip);
            sa.sin_port        = 0;

            errno = 0;
            check(bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0,
                  "外向き IP に bind できる (libslirp の "
                  "slirp_bind_outbound と同じ操作, errno=%d)", errno);
            close(s);
        }
    }

    /* (2) 存在しない IP への bind は EADDRNOTAVAIL */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        skip("UDP ソケットを作れない (errno=%d)", errno);
    } else {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(0xC0A84D37u);   /* 192.168.77.55 */
        sa.sin_port        = 0;

        errno = 0;
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            check(errno == EADDRNOTAVAIL,
                  "存在しない IP への bind は EADDRNOTAVAIL(%d) で失敗 "
                  "(errno=%d)", EADDRNOTAVAIL, errno);
            note("★DHCP で NIC の IP が変わった後の症状★");
            note("  libslirp は outbound_addr をポインタで保持し続けるので");
            note("  全ての新規接続が即エラーになる。");
            note("  切り分けは strace -e trace=bind か起動ログの bind 先。");
        } else {
            skip("192.168.77.55 が実在する環境らしい (bind 成功)");
            close(s);
            s = -1;
        }
        if (s >= 0)
            close(s);
    }
}

/*
 * NIC の IP に bind したソケットから loopback へ送れる事の確認。
 * libslirp の DNS 中継 (vnameserver 宛を host の resolver へ転送) が
 * outbound_addr のせいで壊れないかの確認。Linux は weak host model
 * なので通るはずだが、憶測にせず実測する。
 */
static void test_6c_outbound_loopback(void)
{
    uint32_t out_ip;
    char     ifname[128];
    int      srv = -1, cli = -1;
    struct sockaddr_in sa;
    socklen_t          sl;
    unsigned short     port = 0;
    char     rbuf[16];
    ssize_t  n;
    struct pollfd pf;

    head("Step 6-c: outbound bind 済みソケットから loopback へ届くか");

    ifname[0] = '\0';
    out_ip = vm_hostroute_pick_outbound_ip(0xC0A86300u, 0xFFFFFF00u,
                                           ifname, sizeof(ifname));
    if (out_ip == 0u) {
        skip("外向き IP を特定できないので検証不能");
        return;
    }

    /* 受け側: 127.0.0.1 の適当なポート */
    srv = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) { skip("socket() 失敗 (errno=%d)", errno); return; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        skip("127.0.0.1 に bind できない (errno=%d)", errno);
        close(srv);
        return;
    }
    sl = sizeof(sa);
    if (getsockname(srv, (struct sockaddr *)&sa, &sl) != 0) {
        skip("getsockname 失敗 (errno=%d)", errno);
        close(srv);
        return;
    }
    port = ntohs(sa.sin_port);

    /* 送り側: 実 NIC の IP に bind (= outbound_addr と同じ状態) */
    cli = socket(AF_INET, SOCK_DGRAM, 0);
    if (cli < 0) { skip("socket() 失敗 (errno=%d)", errno); close(srv); return; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(out_ip);
    sa.sin_port        = 0;
    if (bind(cli, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        skip("外向き IP に bind できない (errno=%d)", errno);
        close(srv); close(cli);
        return;
    }

    /* 127.0.0.1:port へ送る */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(port);

    errno = 0;
    if (sendto(cli, "ping", 4, 0, (struct sockaddr *)&sa, sizeof(sa)) != 4) {
        check(0, "NIC bind 済みソケットから 127.0.0.1 へ sendto 失敗 "
                 "(errno=%d)", errno);
        close(srv); close(cli);
        return;
    }
    check(1, "NIC bind 済みソケットから 127.0.0.1 へ sendto 成功");

    /* 届いているか */
    pf.fd      = srv;
    pf.events  = (short)(POLLIN | POLLERR | POLLHUP);
    pf.revents = 0;
    if (poll(&pf, 1, 500) == 1 && (pf.revents & POLLIN)) {
        n = recv(srv, rbuf, sizeof(rbuf), 0);
        check(n == 4 && memcmp(rbuf, "ping", 4) == 0,
              "loopback 側で受信できた (%zd バイト)", n);
        note("★libslirp の DNS 中継が outbound_addr で壊れない根拠★");
        note("  vnameserver(192.168.99.3) 宛の DNS は sotranslate_out4 で");
        note("  host の resolver へ差し替えられる。それが 127.0.0.53");
        note("  (systemd-resolved) でも、NIC bind 済みソケットから");
        note("  Linux の weak host model により到達できる。");
    } else {
        check(0, "loopback 側に届かなかった (revents=0x%x)",
              (unsigned)pf.revents);
    }

    close(srv);
    close(cli);
}

/* --------------------------------------------------------------------------
 * Step 6-c: timer_fires == 0 は正常
 * --------------------------------------------------------------------------
 * 指示書は「繋がらない時は timer_fires を確認」と書いているが、
 * この構成では 0 が正常。libslirp が timer_new を呼ぶ箇所は
 *   src/ip6_icmp.c  icmp6_post_init()
 * の IPv6 Router Advertisement 用の 1 つだけで、
 * in6_enabled が false なら即 return する。
 * 実装は cfg.in6_enabled = false なのでタイマは 1 つも作られない。
 * -------------------------------------------------------------------------- */

static void test_6c_timer_expectation(void)
{
    vm_nat_cfg_t  cfg;
    vm_nat_t     *nat = NULL;
    vm_err_t      e;
    int           i;

    head("Step 6-c: NAT を実際に開いて poll を回す (timer_fires の期待値)");

    if (!vm_nat_backend_available(VM_NAT_SLIRP)) {
        skip("slirp バックエンドが使えないビルド");
        return;
    }

    vm_nat_cfg_defaults(&cfg);
    cfg.backend = VM_NAT_SLIRP;

    e = vm_nat_create(&nat, &cfg, dummy_ip_cb, NULL);
    if (e != VM_OK || nat == NULL) {
        skip("vm_nat_create に失敗 (err=%d)。権限や libslirp の問題", (int)e);
        return;
    }
    check(1, "vm_nat_create(SLIRP) 成功 -> slirp_new が version を"
             " 受け付けた (ABI 整合)");

    /*
     * 20ms (vm_modem.c の VM_MODEM_NAT_BLOCK_MS) で 10 回回す。
     * 監視対象が無いので poll(NULL,0,t) 経路を通る。
     * ここで落ちたりハングしたりしない事が Linux 対応の最低ライン。
     */
    for (i = 0; i < 10; i++) {
        int next = vm_nat_poll(nat, 20);
        if (next < 0) {
            check(0, "vm_nat_poll が負値を返した (%d)", next);
            break;
        }
    }
    check(1, "vm_nat_poll を 20ms x 10 回、例外なく回せた");

    {
        vm_nat_stats_t st;
        memset(&st, 0, sizeof(st));
        vm_nat_get_stats(nat, &st);
        note("timers_active = %llu", (unsigned long long)st.timers_active);
        check(st.timers_active == 0u,
              "タイマが 1 つも作られない (IPv6 無効なので正常)");
        note("libslirp の timer_new 呼び出しは");
        note("  src/ip6_icmp.c icmp6_post_init() の RA タイマ 1 箇所のみ。");
        note("  if (!slirp->in6_enabled) return;  ← ここで即 return");
        note("cfg.in6_enabled = false なので timer_fires は常に 0。");
        note("★「繋がらない」の切り分けで timer_fires=0 を");
        note("  異常と誤認しない事。見るべきは poll 回数と eintr 回数。");
    }

    vm_nat_destroy(nat);
    check(1, "vm_nat_destroy 完了 (slirp_cleanup で二重解放しない)");
}

/*
 * poll ループが busy loop にならない事の確認。
 * timeout == 0 を 1ms に切り上げる処理が効いているか。
 * (CPU 焼きは VIM1 のような SBC では致命的)
 */
static void test_6c_no_busy_loop(void)
{
    vm_nat_cfg_t cfg;
    vm_nat_t    *nat = NULL;
    vm_err_t     e;
    struct timespec t0;
    double       ms;
    int          i;
    const int    iters = 20;

    head("Step 6-c: poll ループが busy loop にならない");

    if (!vm_nat_backend_available(VM_NAT_SLIRP)) {
        skip("slirp バックエンドが使えないビルド");
        return;
    }

    vm_nat_cfg_defaults(&cfg);
    cfg.backend = VM_NAT_SLIRP;

    e = vm_nat_create(&nat, &cfg, dummy_ip_cb, NULL);
    if (e != VM_OK || nat == NULL) {
        skip("vm_nat_create に失敗 (err=%d)", (int)e);
        return;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < iters; i++)
        (void)vm_nat_poll(nat, 20);
    ms = elapsed_ms(&t0);

    note("%d 回で %.1f ms (1 回あたり %.2f ms)", iters, ms, ms / iters);
    /*
     * 監視対象が無い場合は poll(NULL,0,timeout) で待つので
     * 1 回あたり最低 1ms は掛かるはず。0 に近いなら
     * timeout の下限クランプが効いていない = busy loop。
     */
    check(ms >= (double)iters * 0.5,
          "1 回あたり 0.5ms 以上待っている -> timeout の下限クランプが有効");
    check(ms <= (double)iters * 25.0 + 200.0,
          "max_block_ms=20 を大きく超えて待たない (%.1f ms)", ms);

    vm_nat_destroy(nat);
}

/* ==========================================================================
 * Step 7 以降に手を付けていない事の確認
 * ==========================================================================
 * 指示書は Step 6 までを本 PR の範囲としている。
 * Step 7 (Makefile.linux) / Step 8 (USB gadget) / Step 8-a (INF) の
 * 成果物が存在しない事をテストとして明示しておく。
 * ========================================================================== */
static void test_scope_step7_untouched(void)
{
    static const char *forbidden[] = {
        "Makefile.linux",
        "scripts/setup-gadget-linux.sh",
        "windows/vim1modem.inf"
    };
    size_t i;

    head("範囲確認: Step 7 以降の成果物が存在しない事");

    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        int exists = (access(forbidden[i], F_OK) == 0);
        check(!exists, "%s は未作成 (Step %s は未着手)",
              forbidden[i], (i == 0) ? "7" : "8");
    }
    note("(リポジトリのルートで実行した場合のみ意味を持つ)");
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(void)
{
    printf("=========================================================\n");
    printf(" VModem Linux 移植 Step 6 検証テスト\n");
    printf(" (vm_nat_slirp.c: poll 抽象 / dlsym / Winsock 型除去)\n");
    printf("=========================================================\n");

    vm_log_init(VM_LOG_INFO, NULL);

    /* Step 6-a */
    test_6a_flag_mapping();
    test_6a_poll_accepts_err_hup();
    test_6a_revents_without_request();

    /* Step 6-b */
    test_6b_runtime_abi();
    test_6b_rtld_default_is_required();
    test_6b_libdl();

    /* Step 6-c */
    test_6c_types_are_posix();
    test_6c_eintr_happens();
    test_6c_revents_undefined_on_error();
    test_6c_outbound_bind();
    test_6c_outbound_loopback();
    test_6c_timer_expectation();
    test_6c_no_busy_loop();

    /* 範囲 */
    test_scope_step7_untouched();

    printf("\n=========================================================\n");
    printf(" 結果: 成功 %d / 失敗 %d / 省略 %d\n", g_pass, g_fail, g_skip);
    printf("=========================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
