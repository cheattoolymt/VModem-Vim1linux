/*
 * vm_nat_slirp.c - libslirp バックエンド
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *   (このファイル自体は BSD。libslirp は LGPL-2.1+ で **動的リンク**する。
 *    詳細なライセンス隔離の議論は include/vmodem/vm_nat.h を参照)
 *
 * ===========================================================================
 * ビルド条件
 * ===========================================================================
 * VMODEM_HAVE_LIBSLIRP が定義されている時のみ実体を持つ。
 * 未定義なら vm_nat_ops_slirp() が NULL を返し、上位が
 * 「libslirp が組み込まれていません」と案内する。
 *
 * libslirp.h が必要な理由 (難所 5) はヘッダに書いた通り。要点だけ再掲:
 *   SlirpConfig / SlirpCb は **値渡しの構造体** で、version により
 *   メンバが増える。手書きで再現すると DLL バージョン差でスタックを
 *   踏み抜き、「動くが時々落ちる」最悪の症状になる。
 *   よって必ず本物のヘッダを使い、動的リンク (import library) にする。
 * ===========================================================================
 */
/*
 * ===========================================================================
 * Step 6 (Linux 移植) で必要になった機能テストマクロ
 * ===========================================================================
 * ★どの #include より先に定義しなければ効かない★
 *
 * _GNU_SOURCE が必要な理由:
 *   <dlfcn.h> の RTLD_DEFAULT / RTLD_NOLOAD は POSIX ではなく GNU 拡張。
 *   -std=c99 では __STRICT_ANSI__ が立ち、_DEFAULT_SOURCE も無効化される
 *   ため、これらのマクロが見えず
 *       error: 'RTLD_DEFAULT' undeclared
 *   になる。Step 6-b の dlsym 実装はこれらに依存するので必須。
 *
 * _GNU_SOURCE は _POSIX_C_SOURCE 200809L を含意するので、
 * vm_nat.c のように _POSIX_C_SOURCE を別途定義する必要はない
 * (両方定義しても矛盾しないが、重複定義の警告を避けるため片方だけにする)。
 *
 * Windows ビルドでは完全に無害 (MinGW は _GNU_SOURCE を無視する)。
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "vm_nat_internal.h"

#ifndef VMODEM_HAVE_LIBSLIRP

/* libslirp 無しビルド: 利用不可を返すだけ */
const vm_nat_backend_ops_t *vm_nat_ops_slirp(void)
{
    return NULL;
}

#else /* VMODEM_HAVE_LIBSLIRP */

/*
 * ★重要★ libslirp.h は内部で winsock2.h -> windows.h の順に include する。
 * その順序を我々が壊さないよう、必ず vm_winsock.h を **先に** 通しておく。
 * (vm_winsock.h は WIN32_LEAN_AND_MEAN + winsock2.h 先行を保証する)
 */
#include "vmodem/vm_winsock.h"
#include "vmodem/vm_hostroute.h"

#include <libslirp.h>

/* ==========================================================================
 * ★難所 7 / Step 6-c★ poll 抽象と「fd を入れる型」の隔離
 * ==========================================================================
 * Windows と POSIX で違うのは関数名だけではない。
 *
 *   型      : WSAPOLLFD.fd は SOCKET (UINT_PTR, Win64 では 8 バイト)
 *             struct pollfd.fd は int (4 バイト)
 *   戻り値  : WSAPoll は SOCKET_ERROR、poll は -1 (値は同じ -1 だが
 *             エラーの取り出しが WSAGetLastError() と errno で違う)
 *   空リスト: WSAPoll(fds, 0, t) は WSAEINVAL、poll(NULL, 0, t) は sleep
 *   EINTR   : POSIX にしか存在しない (Step 6-c で対応)
 *
 * ここで型と呼び出しを 1 箇所に閉じ込め、以降のコードから
 * Winsock 固有の型名 (WSAPOLLFD / SOCKET / INVALID_SOCKET) が
 * 一切出てこないようにする。これが Step 6-c の要求「Winsock 型の除去」
 * の実体である。**Linux 側の翻訳単位には winsock の識別子が
 * 1 つも現れてはならない**。
 *
 * 【Step 6-c の監査結果 (機械的に検証した)】
 *   指示書の要求は「WSAPOLLFD / SOCKET / INVALID_SOCKET 等が
 *   #ifdef _WIN32 で正しく隔離されているか確認」である。
 *   目視ではなくプリプロセッサ出力で確認した:
 *
 *     gcc -std=c99 -Iinclude -Isrc/net -DVMODEM_HAVE_LIBSLIRP \
 *         -E src/net/vm_nat_slirp.c |
 *       grep -E 'WSAPOLLFD|INVALID_SOCKET|WSAPoll|UINT_PTR|SOCKET_ERROR|
 *                GetProcAddress|GetModuleHandle|closesocket|
 *                WSAGetLastError|\bSOCKET\b'
 *     → 一致 0 件
 *
 *   つまり Linux ビルドの翻訳単位からは Winsock 由来の識別子が
 *   完全に消えている。隔離されている箇所は以下の 4 つだけ:
 *
 *     1. この poll 抽象ブロック
 *        (WSAPOLLFD / WSAPoll / POLL* 定数の補完)
 *     2. VM_SLIRP_HAVE_SOCKET_API == 0 時の vm_slirp_socket_t 補完
 *        (Windows 側だけ UINT_PTR を使う)
 *     3. slirp_to_native()          … Step 6-a で完全分離
 *     4. cb_add_poll_fd()           … UINT_PTR キャストは _WIN32 内のみ
 *
 *   ★残っている唯一の「Windows 語」は libslirp.h 自身が
 *     公開している slirp_os_socket 型だが、これは libslirp が
 *     Linux では `int` に typedef するので問題無い
 *     (include/libslirp.h の #ifdef _WIN32 分岐)。
 */
#ifdef _WIN32
#  ifndef POLLIN
     /* WSAPoll の定数は winsock2.h にあるが古い SDK では欠ける */
#    define POLLRDNORM 0x0100
#    define POLLRDBAND 0x0200
#    define POLLIN     (POLLRDNORM | POLLRDBAND)
#    define POLLPRI    0x0400
#    define POLLWRNORM 0x0010
#    define POLLOUT    (POLLWRNORM)
#    define POLLERR    0x0001
#    define POLLHUP    0x0002
#    define POLLNVAL   0x0004
#  endif
   typedef WSAPOLLFD vm_pollfd_t;
#  define VM_POLL(fds, n, to) WSAPoll((fds), (ULONG)(n), (int)(to))
#else
#  include <poll.h>
#  include <errno.h>
#  include <dlfcn.h>         /* Step 6-b: dlsym / RTLD_DEFAULT */
   typedef struct pollfd vm_pollfd_t;
#  define VM_POLL(fds, n, to) poll((fds), (nfds_t)(n), (int)(to))
#endif

/* --------------------------------------------------------------------------
 * libslirp のバージョン差の吸収 (難所 5 の続き)
 * --------------------------------------------------------------------------
 * libslirp 4.9.0 で以下が導入された:
 *
 *   - slirp_os_socket 型 (Windows では SOCKET = UINT_PTR)
 *   - slirp_pollfds_fill_socket()  (slirp_pollfds_fill は deprecated)
 *   - SlirpCb.register_poll_socket / unregister_poll_socket
 *   - SlirpConfig version 6
 *
 * ★なぜ int ではダメなのか★
 *   Win64 は LLP64 (sizeof(int)=4, sizeof(void*)=8) であり、
 *   SOCKET は UINT_PTR = 8 バイト。旧 API の SlirpAddPollCb は
 *   `int fd` を取るので、libslirp 内部で
 *
 *       int fd = (int) socket;
 *       if ((slirp_os_socket) fd != socket)
 *           g_warning_once("Truncating socket to int failed!");
 *
 *   と切り詰めが起きる。実際のハンドル値は通常小さいので大半は
 *   通ってしまうが、ソケットを大量に開くと上位ビットが立ち、
 *   **突然一部の接続だけ死ぬ**という極めて厄介な症状になる。
 *   4.9 以降では新 API を使い、SOCKET をそのまま扱う。
 *
 * 古い libslirp (4.8 以前) でもビルドできるようにフォールバックを置く。
 */
#if SLIRP_CHECK_VERSION(4, 9, 0)
#  define VM_SLIRP_HAVE_SOCKET_API 1
   typedef slirp_os_socket vm_slirp_socket_t;
#else
#  define VM_SLIRP_HAVE_SOCKET_API 0
   typedef int vm_slirp_socket_t;
   /*
    * 4.9 未満のヘッダには slirp_os_socket / SlirpAddPollSocketCb が無い。
    * 下の実行時解決コードが型として必要とするので、ここで補う。
    * (実体を呼ぶのは DLL が 4.9 以上だった時だけなので、
    *  この定義で ABI 上の不整合は起きない)
    */
#  ifdef _WIN32
     typedef SOCKET slirp_os_socket;
#  else
     typedef int slirp_os_socket;
#  endif
   typedef int (*SlirpAddPollSocketCb)(slirp_os_socket fd, int events,
                                      void *opaque);
#endif

/* ==========================================================================
 * ★難所 8★ ヘッダと DLL のバージョンが食い違う場合の完全な吸収
 * ==========================================================================
 * これは「実際に起きた」問題である。
 *
 * 本リポジトリが同梱する include/libslirp-version.h は **4.9.3** だが、
 * 利用者が手元に置いている libslirp-0.dll は **4.8.0** である事があり、
 * その組み合わせで以下の 2 つが同時に壊れる。
 *
 *   (1) slirp_new() が NULL を返す
 *       4.8.0: #define SLIRP_CONFIG_VERSION_MAX 5   (src/libslirp.h:109)
 *       4.9.3: #define SLIRP_CONFIG_VERSION_MAX 6   (src/libslirp.h:161)
 *       slirp_new は先頭で
 *           g_return_val_if_fail(cfg->version <= SLIRP_CONFIG_VERSION_MAX, NULL);
 *       を行う。この定数は **DLL 側にコンパイル済みの値**なので、
 *       ヘッダを見て version=6 を渡すと 4.8.0 の DLL では即 NULL。
 *       ログには「slirp_new に失敗」しか出ず、原因が全く分からない。
 *
 *   (2) プロセスが起動すらしない
 *       slirp_pollfds_fill_socket は 4.9.0 で新設された。
 *       4.8.0 の DLL には **エクスポートが存在しない**。
 *       静的にインポートしていると Windows ローダが解決に失敗し、
 *       「プロシージャ エントリ ポイント slirp_pollfds_fill_socket が
 *         ダイナミック リンク ライブラリから見つかりませんでした」
 *       というダイアログが出て exe が全く動かない。
 *
 * 【対策】
 *   a. slirp_version_string() (4.8/4.9 双方に存在) を実行時に読み、
 *      DLL の実バージョンから「渡して良い cfg.version」を決める。
 *   b. slirp_pollfds_fill_socket は **静的に参照しない**。
 *      GetProcAddress で取得を試み、無ければ従来の
 *      slirp_pollfds_fill にフォールバックする。
 *      静的参照を残すとローダ段階で死ぬので、これは必須。
 *
 * ★我々が最低限必要な cfg.version は 2 である★
 *   version >= 2 でなければ SlirpConfig.outbound_addr が読まれない
 *   (src/slirp.c:689  if (cfg->version >= 2))。
 *   outbound_addr は「繋がらない」問題の本命の対策なので、
 *   ここを落とすと修正そのものが無効化される。
 *   version 2 は libslirp 4.1 以降の全バージョンに存在するため、
 *   フォールバック先として安全。
 *
 * ===========================================================================
 * ★★★ Step 6-b: 「POSIX なら遅延束縛で助かる」は誤りだった ★★★
 * ===========================================================================
 * 移植前のコードは POSIX 側でこう書いていた:
 *
 *     #elif VM_SLIRP_HAVE_SOCKET_API
 *         // POSIX では ELF の遅延束縛のおかげで Windows のような
 *         // 起動時全滅は起きない。ヘッダが新しければそのまま使う。
 *         g_slirp_abi.fill_socket = slirp_pollfds_fill_socket;
 *
 * これは **リンク時に落ちる**。実際に再現させた:
 *
 *   $ gcc ... -DVMODEM_HAVE_LIBSLIRP src/net/vm_nat_slirp.c ... -lslirp
 *   /usr/bin/ld: vm_nat_slirp.c:(.text+0x7b3): undefined reference to
 *                `slirp_pollfds_fill_socket'
 *   collect2: error: ld returned 1 exit status
 *
 * 【なぜ遅延束縛では救われないのか】
 *   遅延束縛 (lazy binding) が効くのは「シンボルは存在するが、
 *   呼ばれるまで解決を遅らせる」場合であって、
 *   **シンボルが共有ライブラリに存在しない場合は静的リンク段階で
 *   即エラーになる**。ld は -lslirp の .dynsym を読み、
 *   未定義参照が埋まらない事をその場で検出する。
 *   (--allow-shlib-undefined を付けても、今度は実行時に
 *    「symbol lookup error」でプロセスが起動しない。Windows と同じ結末)
 *
 * 【なぜこれが Linux 移植で必ず問題になるのか】
 *   apt で入る libslirp は Ubuntu Noble / Debian Trixie ともに **4.8.0**。
 *   このバージョンには当該シンボルが存在しない。実測:
 *
 *   $ nm -D --defined-only /usr/lib/x86_64-linux-gnu/libslirp.so.0.4.0 \
 *       | grep fill
 *   slirp_pollfds_fill@@SLIRP_4.0        ← 旧 API だけ
 *   (slirp_pollfds_fill_socket は無い)
 *
 *   一方、本リポジトリが同梱する include/libslirp.h は 4.9.3 なので
 *   SLIRP_CHECK_VERSION(4,9,0) が真になり、上のコードが有効化される。
 *   → **apt の libslirp では絶対にビルドが通らない**。
 *   指示書は「ソースから 4.9.1 を入れる」方針だが、そこで失敗した
 *   利用者が apt に戻した瞬間にビルドが壊れるのは受け入れられない。
 *
 * 【対策 (Windows の GetProcAddress と 1:1 対応させる)】
 *   dlsym(RTLD_DEFAULT, "slirp_pollfds_fill_socket") で **実行時に**
 *   解決する。静的な参照が消えるのでリンクは常に通り、
 *   4.8 では NULL が返って旧 API へフォールバックする。
 *
 *   RTLD_DEFAULT を使う理由:
 *     プロセスに既にロード済みの全オブジェクトを既定の順序で検索する。
 *     libslirp は -lslirp で静的にリンクされて既にロードされているので、
 *     ここで新たに dlopen する必要が無い
 *     (Windows 側で GetModuleHandleA を使い LoadLibrary を避けたのと
 *      全く同じ理屈)。実測でこの方式だけが動く事を確認した:
 *
 *       dlopen("libslirp.so.0", RTLD_LAZY|RTLD_NOLOAD) → NULL
 *         (SONAME が違う / dlopen 経由でロードされていないため)
 *       dlsym(RTLD_DEFAULT, "slirp_version_string")    → 有効なアドレス
 *       dlsym(RTLD_DEFAULT, "slirp_pollfds_fill")      → 有効なアドレス
 *       dlsym(RTLD_DEFAULT, "slirp_pollfds_fill_socket") → NULL (4.8 なので)
 *
 *     ★注意★ RTLD_DEFAULT は libslirp が実際にリンクされている
 *     プロセスでのみ有効。libslirp を全く参照していない実行ファイル
 *     (例: リンクだけ試すテスト) では 3 つとも NULL になるが、
 *     その場合はそもそも slirp バックエンドを使えないので問題ない。
 *
 *   -ldl が必要 (Makefile.linux は Step 7 で用意する。指示書の
 *    LDFLAGS に -ldl が入っているのはこのため)。
 *   なお glibc 2.34 以降は libdl が libc に統合されたので
 *   -ldl は無害な no-op になる (VIM1 の Noble は glibc 2.39)。
 */

/* 動的解決する関数の型 (ヘッダの宣言に依存しない形で自前で持つ) */
typedef void (*vm_fill_socket_fn)(Slirp *slirp, uint32_t *timeout,
                                  SlirpAddPollSocketCb add_poll, void *opaque);

/* 実行時に確定する情報。プロセスで 1 度だけ調べる。 */
static struct {
    bool              probed;
    int               major, minor, micro;
    uint32_t          cfg_version_max;   /* DLL が受け付ける最大 cfg.version */
    vm_fill_socket_fn fill_socket;       /* NULL なら旧 API を使う */
} g_slirp_abi;

/* --------------------------------------------------------------------------
 * 監視 fd の上限
 * --------------------------------------------------------------------------
 * libslirp は TCP セッション 1 本ごとに 1 fd を要求する。
 * 電話回線速度 (最大 33.6kbps) で同時に張れるセッション数は
 * 現実には数十本が上限だが、ブラウザは平気で 100 本開けようとするので
 * 余裕を持たせる。
 */
#define VM_SLIRP_MAX_FDS 512

/* libslirp のタイマ登録上限 */
#define VM_SLIRP_MAX_TIMERS 64

typedef struct {
    void (*cb)(void *opaque);
    void     *cb_opaque;
    int64_t   expire_ns;      /* 0 = 停止中 */
    bool      used;
} slirp_timer_t;

typedef struct {
    Slirp        *slirp;
    vm_nat_t     *nat;        /* 逆参照 (コールバックから使う) */

    /* ---- ポーリング用 (難所 7) ---- */
    vm_pollfd_t   fds[VM_SLIRP_MAX_FDS];
    int           nfds;

    /* ---- タイマ (難所 6) ---- */
    slirp_timer_t timers[VM_SLIRP_MAX_TIMERS];
    int           n_timers;

    uint64_t      poll_calls;
    uint64_t      timer_fires;
    uint64_t      poll_eintr;   /* Step 6-c: poll() が EINTR で戻った回数 */

    /* ---- ★難所 9★ outbound_addr の実体 ---- */
    /*
     * libslirp は SlirpConfig.outbound_addr を **ポインタのまま保持する**。
     *
     *   src/slirp.c:689
     *       if (cfg->version >= 2) {
     *           slirp->outbound_addr = cfg->outbound_addr;   ← コピーしない
     *
     * そして送信ソケットを作るたびに slirp_bind_outbound() 経由で
     * この領域を参照して bind() する。
     * つまり slirp_be_open() のスタック上に置いた sockaddr_in を
     * 渡すと、関数を抜けた瞬間に **解放済みスタックを読み続ける**。
     * 最初の数パケットは偶然通り、その後ランダムに壊れるという
     * 最悪の再現性の低いバグになる。
     *
     * よって Slirp インスタンスと同じ寿命を持つここに実体を置く。
     */
    struct sockaddr_in outbound;
    bool               outbound_valid;
} slirp_impl_t;

/* ==========================================================================
 * 実行時 ABI 検出 (難所 8 の実装)
 * ========================================================================== */
/*
 * DLL の実バージョンを slirp_version_string() から取得する。
 * この関数は 4.8.0 にも 4.9.3 にも存在するので安全に呼べる。
 *   4.8.0: src/libslirp.h:336
 *   4.9.3: include/libslirp.h:393
 * 返す文字列は "4.8.0" のような形式。
 */
static void slirp_probe_abi(void)
{
    const char *ver;

    if (g_slirp_abi.probed)
        return;
    g_slirp_abi.probed = true;

    /* 既定値: 最も保守的な組み合わせ */
    g_slirp_abi.major = g_slirp_abi.minor = g_slirp_abi.micro = 0;
    g_slirp_abi.cfg_version_max = 2;      /* outbound_addr が使える最小値 */
    g_slirp_abi.fill_socket     = NULL;   /* 旧 API を使う */

    ver = slirp_version_string();
    if (ver != NULL) {
        int a = 0, b = 0, c = 0;
        if (sscanf(ver, "%d.%d.%d", &a, &b, &c) >= 2) {
            g_slirp_abi.major = a;
            g_slirp_abi.minor = b;
            g_slirp_abi.micro = c;
        }
    }

    /*
     * DLL の実バージョンから受け付け可能な cfg.version の上限を決める。
     * (libslirp の歴史。src/libslirp.h の SLIRP_CONFIG_VERSION_MAX)
     *   4.1 〜 4.6  : 3 or 4
     *   4.7 〜 4.8  : 5
     *   4.9 〜      : 6
     * 判定を誤って上を渡すと slirp_new が NULL を返して即死するので、
     * 「確実に存在する所まで」しか上げない。
     */
    if (g_slirp_abi.major > 4 ||
        (g_slirp_abi.major == 4 && g_slirp_abi.minor >= 9)) {
        g_slirp_abi.cfg_version_max = 6;
    } else if (g_slirp_abi.major == 4 && g_slirp_abi.minor >= 7) {
        g_slirp_abi.cfg_version_max = 5;
    } else if (g_slirp_abi.major == 4 && g_slirp_abi.minor >= 4) {
        g_slirp_abi.cfg_version_max = 4;
    } else {
        g_slirp_abi.cfg_version_max = 2;
    }

    /*
     * slirp_pollfds_fill_socket を **動的に**解決する。
     *
     * ★なぜ静的リンクではダメか★
     *   4.8.0 の DLL にはこのシンボルが無い。静的にインポートすると
     *   Windows のローダが exe 起動時点で解決に失敗し、
     *   「エントリ ポイントが見つかりません」で一切起動しなくなる。
     *   GetProcAddress なら「無ければ NULL」で済むので生存できる。
     */
#if defined(_WIN32)
    {
        /*
         * DLL 名は環境で揺れる。MSYS2/MinGW は libslirp-0.dll、
         * 自前ビルドだと slirp.dll や libslirp.dll になる事もある。
         * GetModuleHandle は **既にロード済み**のものだけを返すので、
         * ここで新たに LoadLibrary してしまう心配は無い
         * (libslirp 自体は静的インポートで既にロードされている)。
         */
        static const char *names[] = {
            "libslirp-0.dll", "libslirp.dll", "slirp.dll", NULL
        };
        int i;

        for (i = 0; names[i] != NULL; i++) {
            HMODULE h = GetModuleHandleA(names[i]);
            FARPROC p;

            if (h == NULL)
                continue;

            p = GetProcAddress(h, "slirp_pollfds_fill_socket");
            if (p != NULL) {
                /*
                 * 関数ポインタ <-> FARPROC のキャストは C 標準では
                 * 未定義だが、Win32 API はこの用法を前提にしている。
                 * -Wcast-function-type を黙らせるため void* を経由する。
                 */
                g_slirp_abi.fill_socket = (vm_fill_socket_fn)(void *)p;
            }
            break;
        }
    }
#else
    /*
     * ★Step 6-b の実装★ POSIX 版の GetProcAddress = dlsym。
     *
     * ここで **絶対に slirp_pollfds_fill_socket を名前で直接
     * 参照してはいけない**。参照した時点でリンカが未定義シンボルを
     * 検出し、apt の libslirp 4.8 ではビルドが通らなくなる。
     * (上のコメントに再現ログを載せた通り)
     *
     * ヘッダのバージョンで #if 分岐する必要も無い。文字列で引くので
     * ヘッダが 4.8 でも 4.9 でも同じコードが正しく動く。
     * これは Windows 側が「ヘッダに関係なく GetProcAddress で引く」
     * のと完全に対称であり、両 OS で同じ判断ロジックになる。
     */
    {
        /*
         * 関数ポインタと void* の相互キャストは C 標準 (6.3.2.3) では
         * 未定義だが、POSIX.1-2008 は dlsym の戻り値をこう使う事を
         * 明示的に要求している (dlsym の RATIONALE 参照)。
         * -Wpedantic を黙らせるため union 経由にする。
         *   (POSIX 自身が推奨している回避策)
         */
        union {
            void             *ptr;
            vm_fill_socket_fn fn;
        } u;

        u.ptr = dlsym(RTLD_DEFAULT, "slirp_pollfds_fill_socket");
        g_slirp_abi.fill_socket = u.fn;   /* 無ければ NULL = 旧 API */

        /*
         * ★dlsym が失敗した理由は区別しなくてよい★
         * 「libslirp が 4.8 だからシンボルが無い」と
         * 「dlsym 自体が使えない」の区別は付かないが、どちらでも
         * 取るべき行動 (旧 API へのフォールバック) は同じ。
         * dlerror() は呼ばない: 呼ぶとエラーキューを消費するため、
         * 他のライブラリの dlerror() 診断を壊す可能性がある。
         */
    }
#endif

    /*
     * ログ表記を OS 中立にする。
     * Linux では "DLL" ではなく共有ライブラリなので、
     * 実機のログを読む人が混乱しないよう言い分ける。
     */
    VM_LOGI("nat(slirp): libslirp %s を検出 "
#ifdef _WIN32
            "(DLL)"
#else
            "(共有ライブラリ)"
#endif
            " cfg.version 上限=%u, fill=%s",
            (ver != NULL) ? ver : "不明",
            (unsigned)g_slirp_abi.cfg_version_max,
            (g_slirp_abi.fill_socket != NULL)
                ? "slirp_pollfds_fill_socket (新API)"
                : "slirp_pollfds_fill (旧API・fd は int に切り詰められる)");

    /*
     * ★Linux 固有の注意喚起★
     * 旧 API でも Linux の fd は int なので切り詰めの害は無い
     * (害があるのは SOCKET が 8 バイトの Win64 だけ)。
     * その事を明記しないと、実機ログを見た人が
     * 「旧 API だから繋がらないのか」と誤った方向に調査を始める。
     */
#ifndef _WIN32
    if (g_slirp_abi.fill_socket == NULL) {
        VM_LOGI("nat(slirp): 旧 API を使うが Linux の fd は int なので "
                "切り詰めの問題は起きない (apt の libslirp 4.8 は "
                "slirp_pollfds_fill_socket を持たないため正常な経路)");
    }
#endif

    /*
     * ヘッダと DLL が食い違っていたら警告する。
     * 「なぜか動かない」を「なぜ動かないか分かる」に変えるのが目的。
     */
    if (g_slirp_abi.major != SLIRP_MAJOR_VERSION ||
        g_slirp_abi.minor != SLIRP_MINOR_VERSION) {
        VM_LOGW("nat(slirp): ★ヘッダ (%d.%d.%d) と DLL (%d.%d.%d) の "
                "バージョンが一致していない。動作はするが、"
                "同じバージョンの libslirp-0.dll に揃える事を強く推奨する",
                SLIRP_MAJOR_VERSION, SLIRP_MINOR_VERSION, SLIRP_MICRO_VERSION,
                g_slirp_abi.major, g_slirp_abi.minor, g_slirp_abi.micro);
    }
}

/* ==========================================================================
 * poll フラグ変換 (難所 7 の Windows 固有部)
 * ==========================================================================
 * libslirp の SLIRP_POLL_* は **独自 enum** であり、POSIX の POLLIN 等と
 * 値が一致する保証はない。ヘッダを見ると
 *      SLIRP_POLL_IN = 1 << 0, OUT = 1 << 1, PRI = 1 << 2,
 *      ERR = 1 << 3, HUP = 1 << 4
 * だが、これに依存して直接ビットを渡すと libslirp の更新で壊れる。
 * 必ず変換関数を通す。
 */
/*
 * ==========================================================================
 * ★★★ ここが「PPP は繋がるのにインターネットに出られない」真の原因 ★★★
 * ==========================================================================
 *
 * 【症状】
 *   nat tx: 192.168.99.2 > 8.8.8.8 UDP 69B      (送信は出ている)
 *   nat rx: 192.168.99.1 > 192.168.99.2 ICMP 97B (毎回 ICMP が返る)
 *   8.8.8.8 からの応答は一切来ない。TCP も同様に無応答。
 *
 * 【原因】
 *   Windows の WSAPoll() は、events メンバに指定できるフラグが
 *   MSDN で厳密に限定されている:
 *
 *       POLLRDNORM / POLLRDBAND / POLLWRNORM
 *       (および合成値 POLLIN = RDNORM|RDBAND, POLLOUT = WRNORM)
 *
 *   これ以外を events に立てると、WSAPoll は 1 つのソケットも待たずに
 *   **即座に SOCKET_ERROR (WSAEINVAL = 10022) を返す**。MSDN 原文:
 *
 *       "This error is also returned if invalid flags were specified in
 *        the events member of any of the WSAPOLLFD structures"
 *       "If the POLLPRI flag is set on a socket for the Microsoft Winsock
 *        provider, the WSAPoll function will fail."
 *
 *   POLLERR (0x0001) / POLLHUP (0x0002) / POLLNVAL (0x0004) は
 *   **revents 専用の「出力だけ」のフラグ**であり、events に立ててはいけない。
 *   POLLPRI (0x0400) は Microsoft の provider が非対応で、立てると失敗する。
 *
 *   ところが libslirp は fill の中で堂々と
 *
 *       add_poll(so->s, SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR, ..)
 *       add_poll(so->s, SLIRP_POLL_OUT | SLIRP_POLL_ERR, ..)
 *       add_poll(so->s, SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR
 *                       | SLIRP_POLL_PRI, ..)
 *
 *   を要求してくる (POSIX の poll(2) では events の余分なビットは
 *   無視されるだけなので、これは Linux では完全に正しい)。
 *
 *   旧実装はこれを素直に POLLERR|POLLHUP|POLLPRI へ変換していたため、
 *   **WSAPoll が毎回 WSAEINVAL で即エラー**になっていた。すると:
 *
 *     1. rc < 0 なので slirp_pollfds_poll(slirp, select_error=1, ...) を呼ぶ。
 *     2. libslirp 側は `if (!select_error)` でガードしているので、
 *        **ソケットの受信処理を丸ごとスキップする**。
 *        つまり sorecvfrom() / soread() が永久に呼ばれない。
 *     3. 送信 (slirp_input -> sosendto) だけは poll と無関係に動くので
 *        「nat tx は出ている」ように見える。
 *     4. UDP ソケットは 4 分 (SO_EXPIRE) 経つと so_expire で回収される。
 *        DNS は 10 秒 (SO_EXPIREFAST)。回収も fill の中で行われるが、
 *        fill 自体は成功しているので expire だけは進む。
 *     5. 応答を読まないまま送り続けるので、いずれ recvfrom がエラーになり
 *        libslirp が icmp_send_error(ICMP_UNREACH) を返す。
 *        これが「nat rx: ... ICMP 97B」の正体。
 *        (14 eth + 20 ip + 8 icmp + 55 元パケット断片 = 97 バイト)
 *
 * 【対策】
 *   events には **POLLIN / POLLOUT だけ**を渡す。
 *   POLLERR / POLLHUP / POLLNVAL は要求しなくても revents に勝手に載る
 *   (MSDN: "Error conditions are always returned, so information on them
 *    need not be requested.")。よって取りこぼしは発生しない。
 *
 *   POSIX 側は従来どおり全部渡してよいが、挙動を Windows と揃えるため
 *   同じ方針で統一する (POSIX でも POLLERR/POLLHUP/POLLNVAL は
 *   events で無視され revents で必ず返る、と poll(2) に明記されている)。
 *
 *   SLIRP_POLL_PRI (TCP 緊急データ = OOB) は Windows では POLLRDBAND に
 *   相当するので、そちらへマップする。POLLRDBAND は events に指定可能。
 *
 * ===========================================================================
 * ★★★ Step 6-a: この関数を Linux と Windows で完全に分ける ★★★
 * ===========================================================================
 * 指示書 Step 6-a の要求は
 *   「WSAPoll 専用フィルタ (slirp_to_native()) が Linux 側に
 *     影響しないか確認」
 * である。**確認した結果、影響していた。** 移植前のこの関数は
 * #ifdef _WIN32 が PRI の 1 行だけに掛かっており、
 * 「ERR / HUP を落とす」という **Windows のためだけの制約が
 *  POSIX ビルドにもそのまま適用されていた**。
 *
 * 【なぜ POSIX で ERR / HUP を落としてはいけないのか】
 *
 * poll(2) の規格上、events に立てなくても revents には
 * POLLERR / POLLHUP / POLLNVAL が返る。ここまでは移植前のコメントの
 * 通りで、「取りこぼしは発生しない」も **正しい**。
 * だが問題は revents ではなく **「そもそも poll が起きるか」** である。
 *
 *   POSIX poll(2):
 *     "If none of the defined events have occurred on any selected
 *      file descriptor, poll() shall wait at least timeout milliseconds"
 *
 * つまり revents に何が返るかとは別に、Linux カーネルの poll 実装は
 * 「要求された events に対応する条件が揃ったら起こす」という形で
 * wait queue の起床条件を決める。ソケットに対しては
 * (net/socket.c の sock_poll → tcp_poll 等)
 * エラー・切断状態は常に mask に載るので実際には起床するが、
 * これは **実装の詳細に依存した幸運**であり、規格が保証するのは
 * 「events で要求した条件」だけである。
 *
 * より本質的な理由は 2 つある:
 *
 *   (1) events が空になる場合の意味が変わる
 *       libslirp は接続中 TCP に対して
 *           add_poll(so->s, SLIRP_POLL_OUT | SLIRP_POLL_ERR, ..)
 *       を要求する (src/slirp.c:865)。移植前の変換では OUT が残るので
 *       これは無事だが、libslirp が将来 ERR 単独を要求すると
 *       native == 0 になり、下の add_poll_common が「仕方なく POLLIN を
 *       立てる」という **意味の違うイベント**にすり替えていた。
 *       接続失敗の検出が POLLIN 待ちに化けるという致命的な誤りである。
 *
 *   (2) デバッグ時に「libslirp が何を待っているか」が消える
 *       ERR/HUP を落とすと、poll のダンプを見ても libslirp の意図が
 *       分からない。Step 6 以降で「繋がらない」を追う時、
 *       これは調査時間を無駄に伸ばす。
 *
 * 【結論】
 *   POSIX 版は libslirp が要求した通りに全ビットを渡す。これは
 *   QEMU の net/slirp.c (slirp_poll_to_gio) が
 *       IN/OUT/PRI/ERR/HUP を **すべて** G_IO_* に変換している
 *   のと同じ方針であり、libslirp が想定している唯一の正しい使い方。
 *   Windows 版は WSAPoll の制約があるため従来のフィルタを維持する。
 *
 *   ★重要★ この関数から「Windows のための都合」を Linux 側へ
 *   漏らさない事が Step 6-a の成果物である。
 */
#ifdef _WIN32

static int slirp_to_native(int ev)
{
    int r = 0;

    if (ev & SLIRP_POLL_IN)  r |= POLLIN;
    if (ev & SLIRP_POLL_OUT) r |= POLLOUT;

    /*
     * 緊急データ (OOB)。
     *   Windows: POLLPRI は使用禁止。POLLRDBAND が OOB 相当で events 可。
     *            なお POLLIN は (POLLRDNORM|POLLRDBAND) なので、
     *            SLIRP_POLL_IN が既に立っていれば実質含まれている。
     */
    if (ev & SLIRP_POLL_PRI) r |= POLLRDBAND;

    /*
     * ★ SLIRP_POLL_ERR / SLIRP_POLL_HUP は意図的に無視する ★
     * events に立てると WSAPoll が WSAEINVAL で即死する。
     * これらは revents では必ず報告されるので、無視して問題ない。
     */

    return r;
}

#else  /* POSIX (Linux / Khadas VIM1) */

static int slirp_to_native(int ev)
{
    int r = 0;

    if (ev & SLIRP_POLL_IN)  r |= POLLIN;
    if (ev & SLIRP_POLL_OUT) r |= POLLOUT;
    if (ev & SLIRP_POLL_PRI) r |= POLLPRI;

    /*
     * ★ここが Windows と決定的に違う★
     * poll(2) は events の POLLERR / POLLHUP を無視するだけで
     * エラーを返さない。libslirp が要求した意図を保つため素直に渡す。
     * (POLLNVAL は「fd が不正」という出力専用の意味しか持たないので
     *  events には立てない。libslirp も要求してこない)
     */
    if (ev & SLIRP_POLL_ERR) r |= POLLERR;
    if (ev & SLIRP_POLL_HUP) r |= POLLHUP;

    return r;
}

#endif /* _WIN32 */

/*
 * revents -> SLIRP_POLL_* の逆変換。
 *
 * こちらは「出力」なので全フラグを受け取ってよい。むしろ
 * 取りこぼすと libslirp が接続の切断やエラーを永久に検出できなくなる。
 */
static int native_to_slirp(int ev)
{
    int r = 0;

    if (ev & POLLIN)   r |= SLIRP_POLL_IN;
    if (ev & POLLOUT)  r |= SLIRP_POLL_OUT;
    if (ev & POLLERR)  r |= SLIRP_POLL_ERR;
    if (ev & POLLHUP)  r |= SLIRP_POLL_HUP;

#ifdef _WIN32
    /*
     * Windows の Microsoft provider は POLLPRI を **revents に返さない**
     * (MSDN: "This flag is not returned by the Microsoft Winsock provider")。
     * OOB データは POLLRDBAND で通知されるので、そちらを PRI に写す。
     *
     * なお POLLIN == (POLLRDNORM | POLLRDBAND) なので、POLLRDBAND が
     * 立っていれば上の POLLIN 判定でも SLIRP_POLL_IN が立つ。
     * libslirp 側は IN と PRI を独立に見るので、両方立てるのが正しい。
     */
    if (ev & POLLRDBAND) r |= SLIRP_POLL_PRI;
#else
    if (ev & POLLPRI)  r |= SLIRP_POLL_PRI;
#endif

    /*
     * POLLNVAL (無効な fd) は libslirp に SLIRP_POLL_ERR として伝える。
     * これを落とすと libslirp が閉じたソケットを永久に監視し続け、
     * poll が即座に返り続けて 100% CPU になる。
     */
    if (ev & POLLNVAL) r |= SLIRP_POLL_ERR;

    return r;
}

/* ==========================================================================
 * SlirpCb: ゲストへのフレーム送出
 * ==========================================================================
 * libslirp が「ゲスト (= Windows RAS) へこのフレームを届けろ」と言ってくる。
 * 我々は偽 Ethernet 層で剥がして PPP へ流す。その処理は共通部の
 * vm_nat_backend_recv_frame() に集約してあるので、ここは中継だけ。
 *
 * 戻り値は「送れたバイト数」。負値や 0 を返すと libslirp は
 * 送信失敗と見なして再送を試みるので、必ず len を返す。
 * (PPP 側でバッファが溢れても、ここで嘘をつく方が全体としては安定する。
 *  33.6kbps という帯域では TCP の輻輳制御が自然に落ち着かせてくれる)
 */
static ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;

    if (si == NULL || si->nat == NULL || buf == NULL || len == 0)
        return (ssize_t)len;

    if (len > VM_NAT_FRAME_MAX) {
        VM_LOGW("nat(slirp): 過大なフレーム %u バイトを破棄",
                (unsigned)len);
        return (ssize_t)len;
    }

    vm_nat_backend_recv_frame(si->nat, (const uint8_t *)buf, (int)len);
    return (ssize_t)len;
}

/* ==========================================================================
 * SlirpCb: ゲストエラー通知
 * ========================================================================== */
static void cb_guest_error(const char *msg, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    if (si != NULL)
        vm_nat_backend_guest_error(si->nat, msg);
}

/* ==========================================================================
 * SlirpCb: クロック (難所 6)
 * ==========================================================================
 * 単調増加が絶対条件。実装は vm_nat_now_ns() に集約済み。
 */
static int64_t cb_clock_get_ns(void *opaque)
{
    (void)opaque;
    return vm_nat_now_ns();
}

/* ==========================================================================
 * SlirpCb: タイマ (難所 6)
 * ==========================================================================
 * libslirp は TCP の再送・遅延 ACK・DHCP リースなどのために
 * タイマを要求する。NULL を置くと最初の TCP 接続で即クラッシュする。
 *
 * 重要な仕様:
 *   - timer_mod(t, expire_ms) の expire_ms は **絶対時刻 (ms)** で、
 *     clock_get_ns() / 1000000 と同じ基準。相対時間ではない。
 *     ここを相対だと勘違いすると、タイマが即座に発火し続けて
 *     CPU を焼くか、逆に永久に発火しなくなる。
 *   - 同じタイマに対して timer_mod が何度も呼ばれる。上書きが正しい。
 *   - timer_free 後に発火させてはいけない。
 *
 * 本実装は malloc を避け固定配列にする。理由: libslirp は
 * TCP セッションごとにタイマを作らず、グローバルに数本しか作らない
 * (fasttimo / slowtimo / DHCP / ICMP)。64 本で十分すぎる。
 */
static void *cb_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    int           i;

    if (si == NULL)
        return NULL;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        if (!si->timers[i].used) {
            si->timers[i].used      = true;
            si->timers[i].cb        = cb;
            si->timers[i].cb_opaque = cb_opaque;
            si->timers[i].expire_ns = 0;
            si->n_timers++;
            return &si->timers[i];
        }
    }

    VM_LOGE("nat(slirp): タイマ枠が枯渇 (%d 本)", VM_SLIRP_MAX_TIMERS);
    return NULL;
}

static void cb_timer_free(void *timer, void *opaque)
{
    slirp_impl_t  *si = (slirp_impl_t *)opaque;
    slirp_timer_t *t  = (slirp_timer_t *)timer;

    if (si == NULL || t == NULL)
        return;

    if (t->used) {
        t->used      = false;
        t->cb        = NULL;
        t->cb_opaque = NULL;
        t->expire_ns = 0;
        si->n_timers--;
    }
}

static void cb_timer_mod(void *timer, int64_t expire_time_ms, void *opaque)
{
    slirp_timer_t *t = (slirp_timer_t *)timer;
    (void)opaque;

    if (t == NULL || !t->used)
        return;

    /*
     * expire_time_ms は絶対時刻 (ms)。ns に直して保持する。
     * 0 以下を渡されたら「即発火」の意味なので現在時刻にする。
     */
    if (expire_time_ms <= 0)
        t->expire_ns = vm_nat_now_ns();
    else
        t->expire_ns = expire_time_ms * 1000000LL;
}

/* ==========================================================================
 * SlirpCb: poll fd の登録通知
 * ==========================================================================
 * Windows では実質何もする必要がないが、**NULL を置いてはいけない**。
 * libslirp のソースは NULL チェックせずに呼ぶ。
 *
 * POSIX で意味を持つのは「この fd を epoll に足しておけ」という
 * ヒントを与えたい実装向け。我々は毎回 fill でリストを作り直すので不要。
 */
static void cb_register_poll_fd(int fd, void *opaque)
{
    (void)fd; (void)opaque;
}

static void cb_unregister_poll_fd(int fd, void *opaque)
{
    (void)fd; (void)opaque;
}

#if VM_SLIRP_HAVE_SOCKET_API
/*
 * version >= 6 用。SOCKET を int に切り詰めずに受け取れる版。
 * libslirp は cfg_version >= 6 かつ非 NULL の時だけこちらを呼び、
 * それ以外は上の register_poll_fd を **NULL チェックなしで** 呼ぶ。
 * よって両方埋めておくのが唯一の安全策。
 */
static void cb_register_poll_socket(slirp_os_socket socket, void *opaque)
{
    (void)socket; (void)opaque;
}

static void cb_unregister_poll_socket(slirp_os_socket socket, void *opaque)
{
    (void)socket; (void)opaque;
}
#endif

/* ==========================================================================
 * SlirpCb: 別スレッドからの起床通知
 * ==========================================================================
 * libslirp が内部スレッド (DNS 解決など) から「poll を抜けろ」と
 * 言ってくる。我々は poll のタイムアウトを短く保っているので
 * 空実装でよいが、NULL は不可。
 */
static void cb_notify(void *opaque)
{
    (void)opaque;
}

/* ==========================================================================
 * SlirpCb 構造体
 * ==========================================================================
 * ★難所 6 の核心 (旧コメントの訂正を含む)★
 *
 * 旧実装は「version 2 以上を宣言すると init_completed / timer_new_opaque を
 * 存在すると見なして呼びに来るので危険」として version=1 に固定していた。
 * これは **libslirp の実際のソースを読むと誤り** である:
 *
 *   src/slirp.c:616  if (slirp->cfg_version >= 4 && slirp->cb->timer_new_opaque)
 *   src/slirp.c:709  if (slirp->cfg_version >= 4 && slirp->cb->init_completed)
 *   src/slirp.c:988  if (slirp->cfg_version >= 6 && slirp->cb->register_poll_socket)
 *
 * いずれも **NULL チェック付き**であり、埋めなければ従来の経路に
 * フォールバックする。つまり version を上げても NULL 呼び出しは起きない。
 *
 * 逆に version=1 に固定する事の実害が大きい:
 *
 *   1. cfg_version < 6 なので libslirp は register_poll_socket ではなく
 *      **register_poll_fd を NULL チェックなしで**呼ぶ (src/slirp.c:998)。
 *      その際 `int fd = (int) so->s;` の切り詰めが起き、Win64 (LLP64) で
 *      SOCKET (UINT_PTR, 8 バイト) の上位ビットが落ちる可能性がある。
 *   2. version < 2 だと outbound_addr が無視される (src/slirp.c:689)。
 *      RAS がデフォルトルートを奪った時の対策 (送信元 IF 固定) が打てない。
 *
 * よって version は 6 (SLIRP_CONFIG_VERSION_MAX) まで上げ、
 * **宣言した範囲のメンバを全部埋める**方針に改める。
 * 「宣言した範囲を確実に埋める」という原則自体は正しいので維持する。
 *
 * なお register_poll_fd / unregister_poll_fd は deprecated だが
 * 古い libslirp (< 4.9) 向けに **必ず残す**。消すと NULL 呼び出しになる。
 */
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static const SlirpCb slirp_cb = {
    /* .send_packet            = */ cb_send_packet,
    /* .guest_error            = */ cb_guest_error,
    /* .clock_get_ns           = */ cb_clock_get_ns,
    /* .timer_new              = */ cb_timer_new,
    /* .timer_free             = */ cb_timer_free,
    /* .timer_mod              = */ cb_timer_mod,
    /* .register_poll_fd       = */ cb_register_poll_fd,
    /* .unregister_poll_fd     = */ cb_unregister_poll_fd,
    /* .notify                 = */ cb_notify,

    /* ---- version >= 4 で追加。埋めない (NULL チェックされる) ---- */
    /* .init_completed         = */ NULL,
    /*
     * timer_new_opaque は **意図的に NULL**。
     * これを埋めると libslirp は timer_new を使わず、発火時に
     * アプリが slirp_handle_timer(slirp, id, cb_opaque) を呼ぶ責任を負う。
     * 我々の timer_new 実装で足りているので、余計な責任は負わない。
     */
    /* .timer_new_opaque       = */ NULL

#if VM_SLIRP_HAVE_SOCKET_API
    /* ---- version >= 6 で追加 ---- */
    , /* .register_poll_socket   = */ cb_register_poll_socket,
      /* .unregister_poll_socket = */ cb_unregister_poll_socket
#endif
};
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

/* ==========================================================================
 * ポーリング用コールバック (難所 7)
 * ==========================================================================
 * slirp_pollfds_fill が add_poll を呼んでくる。
 * **返した index を slirp が覚えていて、後で get_revents で聞いてくる**。
 * したがって index は fds 配列の添字と 1:1 で一致させなければならない。
 * ここでズレると slirp が別のソケットのイベントを誤認し、
 * 「TCP が異常に遅い / 時々止まる」という症状になる。
 */
/*
 * ★共通実装★
 * 引数を slirp_os_socket (Windows では SOCKET = UINT_PTR) で受ける。
 * 新 API 経路ではこの型がそのまま渡ってくるので切り詰めが起きない。
 */
static int add_poll_common(slirp_os_socket fd, int events, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    int           idx;
    int           native;

    if (si == NULL || si->nfds >= VM_SLIRP_MAX_FDS)
        return -1;

    native = slirp_to_native(events);

    /*
     * ★ events が空になるケースの扱い (Step 6-c) ★
     *
     * ここは移植前は OS 共通で
     *     if (native == 0) native = POLLIN;
     * だった。**これは Windows のための細工であり、POSIX では有害**。
     *
     * 【Windows】
     *   slirp_to_native() が ERR / HUP を落とすので、libslirp が
     *   SLIRP_POLL_ERR だけを要求すると native == 0 になり得る。
     *   WSAPoll に events == 0 のエントリを渡すと実装によっては
     *   WSAEINVAL を返して poll 全体が即死するため、
     *   ダミーとして POLLIN を立てる必要がある。
     *   エラーは events に関係なく revents に載る (MSDN 明記) ので
     *   取りこぼしは起きない。
     *
     * 【POSIX】
     *   Step 6-a で slirp_to_native() が全ビットを素通しするように
     *   なったため、libslirp が非 0 の events を要求している限り
     *   native == 0 には **ならない**。仮に libslirp が events == 0 を
     *   要求してきたなら、それは「この fd では起床しなくてよい (revents で
     *   エラーだけ拾う)」という意味であり、poll(2) はそれを正しく扱う
     *   (events == 0 でも POLLERR/POLLHUP/POLLNVAL は revents に返る)。
     *   ここで勝手に POLLIN を立てると
     *     - libslirp の意図と違う「読み取り可能待ち」にすり替わる
     *     - 読めるデータがある fd で poll が即戻りして busy loop になる
     *   という二重の劣化を招く。よって **何もしない** のが正しい。
     */
#ifdef _WIN32
    if (native == 0)
        native = POLLIN;        /* WSAPoll(events == 0) 回避のダミー */
#else
    /*
     * POSIX: 意図的に補正しない。
     * ただし「events == 0 が来た」という事実は繋がらない時の
     * 手掛かりになるので、一度だけ記録しておく。
     */
    if (native == 0) {
        static int warned_once = 0;
        if (!warned_once) {
            warned_once = 1;
            VM_LOGW("nat(slirp): libslirp が events==0 で fd を登録した "
                    "(slirp_events=0x%x)。revents のエラー通知のみ有効", events);
        }
    }
#endif

    idx = si->nfds++;
    si->fds[idx].fd      = fd;
    si->fds[idx].events  = (short)native;
    si->fds[idx].revents = 0;

    return idx;                 /* ← この値が後で get_revents に渡る */
}

/*
 * 新 API (slirp_pollfds_fill_socket) 用のアダプタ。
 * DLL が 4.9 以上の時だけ、GetProcAddress 経由で呼ばれる。
 */
static int cb_add_poll_socket(slirp_os_socket fd, int events, void *opaque)
{
    return add_poll_common(fd, events, opaque);
}

/*
 * 旧 API (slirp_pollfds_fill) 用のアダプタ。
 * 引数が int なので、libslirp 側で既に切り詰められている。
 * Win64 で上位ビットが落ちていれば復元は不可能なので、
 * せめて検出してログに出す (無音で一部の接続だけ死ぬのを防ぐ)。
 */
static int cb_add_poll_fd(int fd, int events, void *opaque)
{
#ifdef _WIN32
    /*
     * Windows の SOCKET はカーネルオブジェクトのハンドルで、
     * 実際には 4 の倍数の小さな値が使われるため通常は問題無い。
     * ただし「通常は」であって保証は無い。負値になっていたら異常。
     */
    if (fd < 0) {
        VM_LOGW("nat(slirp): 旧 API 経由で不正な fd (%d) を受けた。"
                "libslirp を 4.9 以上に更新すると解消する", fd);
        return -1;
    }
    return add_poll_common((slirp_os_socket)(UINT_PTR)(unsigned int)fd,
                           events, opaque);
#else
    return add_poll_common(fd, events, opaque);
#endif
}

static int cb_get_revents(int idx, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;

    if (si == NULL || idx < 0 || idx >= si->nfds)
        return 0;

    return native_to_slirp((int)si->fds[idx].revents);
}

/* ==========================================================================
 * タイマ処理
 * ========================================================================== */
static void run_expired_timers(slirp_impl_t *si)
{
    int64_t now = vm_nat_now_ns();
    int     i;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        slirp_timer_t *t = &si->timers[i];

        if (!t->used || t->expire_ns == 0 || t->cb == NULL)
            continue;

        if (t->expire_ns <= now) {
            /*
             * 発火前に expire_ns を 0 にする。
             * コールバック内で timer_mod が呼ばれて再設定されるのが
             * 通常の流れなので、先に 0 にしないと再設定が上書きされて
             * 「タイマが二度と発火しない」ことになる。
             */
            t->expire_ns = 0;
            si->timer_fires++;
            t->cb(t->cb_opaque);
        }
    }
}

/* 次のタイマ期限までの ms。無ければ -1。 */
static int next_timer_ms(slirp_impl_t *si)
{
    int64_t now  = vm_nat_now_ns();
    int64_t best = -1;
    int     i;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        slirp_timer_t *t = &si->timers[i];
        if (!t->used || t->expire_ns == 0)
            continue;
        if (best < 0 || t->expire_ns < best)
            best = t->expire_ns;
    }

    if (best < 0)
        return -1;
    if (best <= now)
        return 0;

    return (int)((best - now) / 1000000LL);
}

/* ==========================================================================
 * vtable 実装
 * ========================================================================== */
static vm_err_t slirp_be_open(vm_nat_t *n)
{
    slirp_impl_t *si;
    SlirpConfig   cfg;
    struct in_addr net, mask, host, dhcp_start, dns;

    si = (slirp_impl_t *)calloc(1, sizeof(*si));
    if (si == NULL)
        return VM_ERR_NOMEM;

    si->nat = n;

    /* DLL の実バージョンを調べる (プロセスで 1 度だけ) */
    slirp_probe_abi();

    /*
     * ★難所 5 の実践★
     * memset で全域を 0 にしてから version を立て、必要なメンバだけ埋める。
     * こうすれば libslirp が version 1 より後に追加したメンバは
     * すべて 0 (= 既定動作) になり、ABI が食い違わない。
     * 構造体を手書きで再現しないこと、version を明示すること、
     * この 2 点で ABI 安全が保てる。
     */
    memset(&cfg, 0, sizeof(cfg));

    /*
     * version は「ヘッダが知っている最大値」と「我々が埋められる最大値」の
     * 小さい方にする。ヘッダに SLIRP_CONFIG_VERSION_MAX があるので、
     * DLL が古くても slirp_new が version を見て安全に落としてくれる…
     * ではなく、実際には **DLL が知らない version を渡すと slirp_new が
     * NULL を返す** (src/slirp.c の g_return_val_if_fail)。
     * よってヘッダと DLL のバージョンを一致させる前提で
     * SLIRP_CONFIG_VERSION_MAX を使う。
     * (本プロジェクトは include/libslirp.h と libslirp-0.dll を同梱するので
     *  この前提は常に成立する)
     */
    /*
     * ★難所 8 / 難所 9 の交点★
     *
     * 旧実装は #if で 6 か 1 に固定していたが、これは 2 重に間違っていた。
     *
     *   (a) 6 固定 : DLL が 4.8.0 だと SLIRP_CONFIG_VERSION_MAX が 5 なので
     *                slirp_new が g_return_val_if_fail で NULL を返す。
     *   (b) 1 固定 : version < 2 だと libslirp は outbound_addr を
     *                **読まない** (src/slirp.c:689)。
     *                「繋がらない」問題の本命の対策が丸ごと無効になる。
     *
     * よって「ヘッダが知っている上限」と「DLL が受け付ける上限」の
     * 小さい方を採る。どちらの場合でも 2 は必ず確保されるので、
     * outbound_addr は常に有効になる。
     */
    {
        uint32_t want = (uint32_t)SLIRP_CONFIG_VERSION_MAX;

        if (want > g_slirp_abi.cfg_version_max)
            want = g_slirp_abi.cfg_version_max;

        /* outbound_addr のために 2 は死守する */
        if (want < 2u)
            want = 2u;

        cfg.version = want;
    }

    /*
     * libslirp のアドレスはネットワークバイトオーダの in_addr。
     * 我々はホストオーダで持っているので htonl する。
     * ここを間違えると「192.168.99.1 のはずが 1.99.168.192 になる」
     * という分かりやすい壊れ方をするので、まだ幸運な部類。
     */
    net.s_addr        = htonl(n->cfg.network);
    mask.s_addr       = htonl(n->cfg.netmask);
    host.s_addr       = htonl(n->cfg.host_ip);
    dns.s_addr        = htonl(n->cfg.dns_ip);
    /*
     * DHCP は使わない (IP は PPP の IPCP で払い出す) が、
     * libslirp は dhcp_start が 0 だと内部で経路を作らないことがある。
     * guest_ip を指定しておく。
     */
    dhcp_start.s_addr = htonl(n->cfg.guest_ip);

    cfg.restricted           = n->cfg.restricted;
    cfg.in_enabled           = true;
    cfg.vnetwork             = net;
    cfg.vnetmask             = mask;
    cfg.vhost                = host;
    cfg.in6_enabled          = false;      /* IPV6CP は Reject 済み */
    cfg.vhostname            = "vmodem";
    cfg.tftp_server_name     = NULL;
    cfg.tftp_path            = NULL;
    cfg.bootfile             = NULL;
    cfg.vdhcp_start          = dhcp_start;
    cfg.vnameserver          = dns;
    cfg.vdnssearch           = NULL;
    cfg.vdomainname          = NULL;
    cfg.if_mtu               = (size_t)n->cfg.mtu;
    cfg.if_mru               = (size_t)n->cfg.mtu;
    cfg.disable_host_loopback = n->cfg.disable_host_loopback;
    cfg.enable_emu           = false;      /* 非推奨機能。使わない */

    /* ======================================================================
     * ★★★ ここが「PPP は繋がるのにインターネットに出られない」の本命 ★★★
     * ======================================================================
     *
     * 【症状】
     *   nat tx: 192.168.99.2 > 8.8.8.8 UDP 69B      ← 大量に出ている
     *   nat rx: 192.168.99.1 > 192.168.99.2 ICMP 97B ← ICMP だけ返る
     *   8.8.8.8 からの応答は一切無い。TCP も同様。
     *
     * 【この 97B が決定的な証拠になる】
     *   libslirp の ICMP エラー生成 (src/ip_icmp.c):
     *     L312  #define ICMP_MAXDATALEN (IP_MSS - 28)      = 576 - 28 = 548
     *     L388  s_ip_len = MIN(orig_ip_len, ICMP_MAXDATALEN);
     *     L392  m->m_len = ICMP_MINLEN + s_ip_len;
     *     L42   #define WITH_ICMP_ERROR_MSG 0   ← 文字列は付かない
     *   よって
     *     IP ヘッダ(20) + ICMP ヘッダ(8) + 元パケット(69) = 97
     *   と **ぴったり一致**する。返ってきた ICMP は
     *   「我々が送った 69B の UDP に対する Destination Unreachable」である。
     *
     * 【どこが発行したのか】
     *   UDP で ICMP_UNREACH を出す箇所は 2 つしかない。
     *     (A) src/udp.c:233     sosendto() が失敗した時   ← 送信失敗
     *     (B) src/socket.c:687  recvfrom() が失敗した時   ← 受信失敗
     *   8.8.8.8 から何も返って来ていない = POLLIN が立たない
     *   = recvfrom が呼ばれる事すら無い。よって (B) は原理的に不可能。
     *   ⇒ **sendto() がその場で失敗している** と確定する。
     *   (src/util.c:223  case WSAENETUNREACH: return ENETUNREACH;)
     *
     * 【なぜ sendto が失敗するのか】
     *   Windows の RAS は、ダイアルアップ接続が確立すると既定で
     *       0.0.0.0/0 → 192.168.99.1 (PPP インタフェース)
     *   というデフォルト経路を追加する
     *   (「リモート ネットワークでデフォルト ゲートウェイを使う」)。
     *   VPN を繋ぐと全通信が VPN に入るのと全く同じ挙動である。
     *
     *   その結果、**我々自身のプロセス**が libslirp 経由で
     *   sendto(8.8.8.8) を発行すると、OS はその経路表に従って
     *   「PPP インタフェースへ送れ」と判断する。
     *   つまり自分が作った仮想回線に自分で送り返す自己参照ループになる。
     *   Windows はこれを検出して即座に
     *   WSAENETUNREACH / WSAEHOSTUNREACH / WSAEADDRNOTAVAIL を返す。
     *   パケットは NIC から 1 バイトも出ていない。
     *
     * 【対策】
     *   送信ソケットを **ホストの実 NIC のアドレスに bind** する。
     *   Windows は Vista 以降 IPv4 で strong host model を既定とするため
     *   (RFC 6419)、送信元アドレスを固定すると出力インタフェースも
     *   そのアドレスを持つ実 NIC に固定される。
     *   これで PPP へのデフォルト経路を迂回できる。
     *
     *   SlirpConfig.outbound_addr (version >= 2) がまさにこの用途で、
     *   libslirp は全ての外向きソケットに対して
     *   slirp_bind_outbound() → bind() を行う。
     *
     * 【2 つの落とし穴】
     *   1. libslirp は outbound_addr を **ポインタのまま保持する**
     *      (src/slirp.c:690)。コピーされない。
     *      → 実体は si (Slirp と同寿命) の中に置く。スタックは不可。
     *   2. cfg.version >= 2 でなければ読まれない (src/slirp.c:689)。
     *      → 上の version 決定ロジックで 2 を死守している。
     *
     * 【呼ぶタイミング】
     *   vm_nat_create() は vm_modem_create() の中、つまり
     *   **ダイアルアップ確立より前**に呼ばれる。
     *   まだ RAS の経路が入っていないので GetBestRoute が実 NIC を返す。
     *   (念のため vm_hostroute 側でも IF_TYPE_PPP を弾いている)
     *
     * ======================================================================
     * ★★★ Step 6-c: Linux での outbound_addr 再検証 ★★★
     * ======================================================================
     * 上の説明は全て Windows / RAS を前提にしている。Linux では前提が
     * 違うので、「同じコードのままで害が無いか」を実測で確認した。
     *
     * 【前提の違い】
     *   - Linux に RAS は無い。デフォルト経路を奪うのは
     *     pppd の `defaultroute` オプションだが、本実装は pppd を使わず
     *     PPP を自前で処理するので、経路表は一切触られない。
     *     → 自己参照ループは原理的に起きない。
     *   - Linux の IPv4 は既定で **weak host model**
     *     (net.ipv4.conf.*.rp_filter とは別の話)。送信元アドレスを
     *     bind しても出力インタフェースは経路表で決まる。
     *     → bind は「送信元 IP の固定」であって
     *        「出力 IF の固定」ではない。Windows と意味が違う。
     *
     * 【つまり Linux では outbound_addr は必要か】
     *   必要ではない。しかし **有害でもない**。むしろ VIM1 に
     *   有線 eth0 と Wi-Fi wlan0 が両方生きている場合に送信元を
     *   安定させる効果があるので、そのまま有効にしておく。
     *
     * 【実測で確認した 3 点】
     *   (1) 実 NIC の IP への bind() は成功する。
     *   (2) 存在しない IP (192.168.77.55) への bind() は
     *       errno 99 = EADDRNOTAVAIL で失敗する。
     *       → NIC の IP が DHCP で変わった後や、USB gadget を
     *          抜き差しして IF が消えた後も libslirp は古い
     *          outbound_addr を持ち続ける (ポインタ保持なので
     *          si->outbound を書き換えれば追従はできるが、
     *          既存ソケットは作り直されない)。
     *          症状は「全ての新規接続が即エラー」。
     *          切り分けは `strace -e trace=bind` か、
     *          si->outbound の中身をログで確認する事。
     *   (3) NIC の IP に bind した UDP ソケットから
     *       127.0.0.1 へ sendto しても応答が返る
     *       (Linux は loopback 宛を weak host model で通す)。
     *       → outbound_addr を設定しても、libslirp が
     *          vnameserver 宛 DNS を host の 127.0.0.53
     *          (systemd-resolved) へ中継する経路は壊れない。
     *          これは Linux 移植で最も壊れそうな箇所だったが、
     *          実測で問題無しと確認できた。
     *
     * 【Linux で「繋がらない」時に見る順番】
     *   1. si->outbound_valid と bind 先 IP がログに出ているか
     *   2. `ip route get 8.8.8.8` の src がその IP と一致するか
     *      (一致しないなら bind が経路と矛盾している)
     *   3. 一致しないなら vm_hostroute_pick_outbound_ip() の
     *      connect(2) プローブが別 IF を選んでいる
     * ====================================================================== */
    {
        char     ifname[128];
        uint32_t out_ip;

        ifname[0] = '\0';

        /*
         * 我々の仮想ネットワーク (既定 192.168.99.0/24) は除外する。
         * そこに bind したら結局自分に送る事になる。
         */
        out_ip = vm_hostroute_pick_outbound_ip(n->cfg.network, n->cfg.netmask,
                                               ifname, sizeof(ifname));

        if (out_ip != 0u) {
            memset(&si->outbound, 0, sizeof(si->outbound));
            si->outbound.sin_family      = AF_INET;
            si->outbound.sin_addr.s_addr = htonl(out_ip);
            si->outbound.sin_port        = 0;   /* ポートは OS に任せる */
            si->outbound_valid           = true;

            /* ★ポインタ渡し。実体は si の中にあるので寿命は安全★ */
            cfg.outbound_addr = &si->outbound;

            VM_LOGI("nat(slirp): 外向きソケットを %u.%u.%u.%u (%s) に bind する "
                    "(RAS のデフォルト経路による自己参照ループを回避)",
                    (unsigned)((out_ip >> 24) & 0xFFu),
                    (unsigned)((out_ip >> 16) & 0xFFu),
                    (unsigned)((out_ip >> 8) & 0xFFu),
                    (unsigned)(out_ip & 0xFFu),
                    (ifname[0] != '\0') ? ifname : "?");
        } else {
            /*
             * 特定できなかった場合は outbound_addr を設定しない。
             * 従来通りの動作 (= OS の経路表任せ) になるので、
             * RAS がデフォルト経路を奪っていると繋がらない可能性がある。
             * その時の逃げ道を明示的に案内する。
             */
            si->outbound_valid = false;
            VM_LOGW("nat(slirp): ホストの外向きアドレスを特定できなかった。"
                    "インターネットに出られない場合は、ダイアルアップ接続の "
                    "プロパティ → ネットワーク → TCP/IPv4 → 詳細設定 で "
                    "「リモート ネットワークでデフォルト ゲートウェイを使う」の "
                    "チェックを外す事");
        }
    }

    si->slirp = slirp_new(&cfg, &slirp_cb, si);
    if (si->slirp == NULL) {
        VM_LOGE("nat(slirp): slirp_new に失敗");
        free(si);
        return VM_ERR_IO;
    }

    n->impl = si;

    {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%u.%u.%u.%u",
                 (unsigned)((n->cfg.host_ip >> 24) & 0xFFu),
                 (unsigned)((n->cfg.host_ip >> 16) & 0xFFu),
                 (unsigned)((n->cfg.host_ip >> 8) & 0xFFu),
                 (unsigned)(n->cfg.host_ip & 0xFFu));
        snprintf(b, sizeof(b), "%u.%u.%u.%u",
                 (unsigned)((n->cfg.guest_ip >> 24) & 0xFFu),
                 (unsigned)((n->cfg.guest_ip >> 16) & 0xFFu),
                 (unsigned)((n->cfg.guest_ip >> 8) & 0xFFu),
                 (unsigned)(n->cfg.guest_ip & 0xFFu));
        VM_LOGI("nat(slirp): 起動 gateway=%s guest=%s mtu=%d restricted=%d",
                a, b, n->cfg.mtu, (int)n->cfg.restricted);
    }

    return VM_OK;
}

static void slirp_be_close(vm_nat_t *n)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;

    if (si == NULL)
        return;

    /*
     * ★Step 6-c: timer_fires == 0 は正常★
     *
     * 指示書は「インターネットに出られない時は timer_fires を確認せよ」と
     * 書いているが、**この構成では timer_fires は 0 のままが正常**である。
     * libslirp が timer_new を呼ぶ箇所は upstream 全体で 1 つだけで、
     *
     *   src/ip6_icmp.c  icmp6_post_init()
     *       if (!slirp->in6_enabled) {
     *           return;                      ← ここで即 return
     *       }
     *       slirp->ra_timer = slirp_timer_new(slirp, SLIRP_TIMER_RA, NULL);
     *
     * つまり **IPv6 Router Advertisement 専用**。
     * 本実装は IPV6CP を Reject して cfg.in6_enabled = false にしている
     * ので、タイマは 1 つも作られない。4.8.0 / master 双方で確認済み。
     *
     * したがって切り分けは
     *   timer_fires == 0 かつ poll > 0        → 正常。別の原因を疑う
     *   poll == 0                             → poll ループまで来ていない
     *   eintr が poll と同オーダーで増える    → シグナル嵐 (Step 6-c の罠)
     * と読む。
     */
    VM_LOGI("nat(slirp): 終了 poll=%llu timer_fires=%llu eintr=%llu"
            " (timer_fires=0 は IPv6 無効時の正常値)",
            (unsigned long long)si->poll_calls,
            (unsigned long long)si->timer_fires,
            (unsigned long long)si->poll_eintr);

    if (si->slirp != NULL)
        slirp_cleanup(si->slirp);

    free(si);
    n->impl = NULL;
}

static vm_err_t slirp_be_send_frame(vm_nat_t *n, const uint8_t *frame, int len)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;

    if (si == NULL || si->slirp == NULL)
        return VM_ERR_STATE;

    /*
     * slirp_input は void。失敗を通知しない。
     * 内部で長さ不足なら黙って捨てるので、こちらで検算しておく。
     */
    if (len < VM_ETH_HLEN)
        return VM_ERR_INVAL;

    slirp_input(si->slirp, frame, len);
    return VM_OK;
}

/* --------------------------------------------------------------------------
 * ポーリング本体 (難所 7 の 3 段構え)
 * -------------------------------------------------------------------------- */
static int slirp_be_poll(vm_nat_t *n, int max_block_ms)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;
    uint32_t      timeout;
    int           rc;
    int           tmr_ms;

    if (si == NULL || si->slirp == NULL)
        return 10;

    si->poll_calls++;

    /* ---- 段 1: 監視すべき fd を集める ---- */
    si->nfds = 0;
    timeout  = (max_block_ms > 0) ? (uint32_t)max_block_ms : 0u;

    /*
     * ★難所 8 の実践★ 新旧 API の実行時切り替え。
     *
     * slirp_pollfds_fill() は 4.9 で deprecated になった。
     * 中身は slirp_pollfds_fill_socket() へのラッパだが、その途中で
     *
     *     int fd = (int) socket;
     *     if ((slirp_os_socket) fd != socket)
     *         g_warning_once("Truncating socket to int failed!");
     *
     * という切り詰めが起きる (src/slirp.c:965 slirp_pollfds_fill_wrap)。
     * Win64 (LLP64) では SOCKET が 8 バイトなので、可能なら新 API を使う。
     *
     * ただし 4.8.0 の DLL には新 API が **存在しない**。
     * そのため静的参照はせず、GetProcAddress で取れた時だけ使う。
     * (静的参照すると 4.8.0 DLL で exe が起動すらしない。難所 8 参照)
     */
    if (g_slirp_abi.fill_socket != NULL) {
        g_slirp_abi.fill_socket(si->slirp, &timeout, cb_add_poll_socket, si);
    } else {
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        slirp_pollfds_fill(si->slirp, &timeout, cb_add_poll_fd, si);
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif
    }

    /*
     * ★ CPU 焼きの罠 ★
     * slirp_pollfds_fill は timeout を「もっと短くしたい」方向にだけ
     * 書き換える。TCP セッションが活発だと 0 になることがあり、
     * そのまま poll(…, 0) すると busy loop になって 1 コアを焼き切る。
     * 電話回線速度では 1ms の遅延など無意味なので、下限を設ける。
     */
    if (timeout == 0)
        timeout = 1;
    if (max_block_ms >= 0 && timeout > (uint32_t)max_block_ms)
        timeout = (uint32_t)max_block_ms;

    /* ---- 段 2: 実際に待つ ---- */
    if (si->nfds > 0) {
        rc = VM_POLL(si->fds, si->nfds, (int)timeout);

#ifndef _WIN32
        /*
         * ================================================================
         * ★★★ Step 6-c: EINTR の扱い (Linux 固有の致命的な罠) ★★★
         * ================================================================
         * Windows の WSAPoll に EINTR は存在しない。よって移植前の
         * コードには EINTR の処理が無く、そのまま Linux に持ち込むと
         * **シグナルが 1 発届くだけで通信が壊れる**。
         *
         * 【なぜ Linux では必ず起きるのか】
         *   src/main.c の install_signal_handlers() は
         *       sa.sa_flags = 0;            ← SA_RESTART を付けない
         *   としている。これは意図的で、SA_RESTART を付けると
         *   シリアル読取の read() が自動再開してしまい、Ctrl-C の反応が
         *   タイムアウト (200ms) 分遅れるため。
         *   その代償として、シグナル配送中に走っていた poll() は
         *       return -1, errno == EINTR
         *   で戻ってくる。SIGWINCH や SIGCHLD、プロファイラの SIGPROF、
         *   デバッガの停止/再開など、SIGINT 以外でも普通に起きる。
         *
         * 【EINTR を放置すると何が起きるか】
         *   rc < 0 のまま段 3 に進むと select_error = 1 になる。
         *   libslirp 側は
         *       if (!select_error) { ... 全ソケットの受信処理 ... }
         *   でガードしているので、**その周回のソケット I/O が丸ごと
         *   スキップされる**。送信 (slirp_input 経由) だけは動くため、
         *   症状は「時々パケットを取りこぼす / 転送が固まる」となり、
         *   原因が極めて分かりにくい。
         *
         * 【対策】
         *   EINTR は「何も起きていない」= タイムアウトと等価に扱う。
         *   ここで poll をやり直すのではなく rc = 0 にするのが正しい:
         *     - 呼び出し元 (vm_modem.c) は 20ms ごとに poll する
         *       ポーリングループなので、待ち時間が短くなるのは無害。
         *     - 逆にループで再試行すると、終了シグナルを受けた時に
         *       ここから抜け出せず、Ctrl-C が効かなくなる
         *       (SA_RESTART を付けないという main.c の設計と矛盾する)。
         *   revents は poll が -1 を返した時点で未定義なので、
         *   全エントリをゼロクリアしてから段 3 に渡す。
         *
         * EAGAIN / ENOMEM も「今回は諦めて次周回」で足りるが、
         * こちらは select_error として libslirp に伝える価値があるので
         * そのまま rc < 0 で通す (ログには残す)。
         */
        if (rc < 0 && errno == EINTR) {
            int i;
            for (i = 0; i < si->nfds; i++)
                si->fds[i].revents = 0;
            si->poll_eintr++;
            rc = 0;             /* タイムアウトと同一視 */
        } else if (rc < 0) {
            /*
             * EINTR 以外の失敗。ここに来るのは
             *   EFAULT (fds が不正 = 我々のバグ)
             *   EINVAL (nfds > RLIMIT_NOFILE)
             *   ENOMEM
             * のいずれか。復旧できないので select_error として伝え、
             * 無音で止まらないようにログを出す (最初の 1 回だけ)。
             */
            static int warned_poll_fail = 0;
            if (!warned_poll_fail) {
                warned_poll_fail = 1;
                VM_LOGE("nat(slirp): poll() が失敗した (errno=%d, nfds=%d)。"
                        "以降このメッセージは抑制する", errno, si->nfds);
            }
        }
#endif /* !_WIN32 */
    } else {
        /*
         * 監視対象が無い。poll(NULL, 0, t) は POSIX では単なる sleep だが
         * Windows の WSAPoll は fds が空だと WSAEINVAL を返す。
         * 移植性のため明示的に分岐する (これも典型的な Windows の罠)。
         */
#ifdef _WIN32
        Sleep(timeout);
#else
        /*
         * nanosleep() は -std=c99 (__STRICT_ANSI__) だと <time.h> から
         * 見えなくなり implicit declaration になる。_POSIX_C_SOURCE を
         * 定義して回避する事もできるが、翻訳単位ごとに定義順を気にする
         * 羽目になるので避ける。
         *
         * POSIX の poll(NULL, 0, timeout) は「単に timeout ミリ秒待つ」と
         * 規格に明記されており、追加のヘッダも feature マクロも要らない。
         * すでに <poll.h> は include 済みなので、これが最も素直。
         */
        (void)poll(NULL, 0, (int)timeout);
#endif
        rc = 0;
    }

    /* ---- 段 3: 結果を slirp に返す ---- */
    /*
     * select_error の意味に注意。libslirp のソースは
     *      if (select_error) { ... 全 fd をエラー扱い ... }
     * としているので、**タイムアウト (rc == 0) はエラーではない**。
     * ここで rc <= 0 を渡すと、待つたびに全 TCP セッションが
     * エラー扱いされて接続が全部切れる。
     */
    slirp_pollfds_poll(si->slirp, (rc < 0) ? 1 : 0, cb_get_revents, si);

    /* ---- タイマ処理 ---- */
    run_expired_timers(si);

    n->stats.timers_active = (uint64_t)si->n_timers;

    /* 次回までの推奨待ち時間 */
    tmr_ms = next_timer_ms(si);
    if (tmr_ms < 0)
        return (max_block_ms > 0) ? max_block_ms : 10;
    if (tmr_ms < 1)
        tmr_ms = 1;
    if (max_block_ms > 0 && tmr_ms > max_block_ms)
        tmr_ms = max_block_ms;

    return tmr_ms;
}

static const vm_nat_backend_ops_t slirp_ops = {
    "slirp",
    slirp_be_open,
    slirp_be_close,
    slirp_be_send_frame,
    slirp_be_poll,
    NULL,
    NULL
};

const vm_nat_backend_ops_t *vm_nat_ops_slirp(void)
{
    return &slirp_ops;
}

#endif /* VMODEM_HAVE_LIBSLIRP */
