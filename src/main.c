/*
 * main.c - VModem エントリポイント
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * やることは 5 つだけ。
 *   1. config.ini を読む
 *   2. ログを初期化する
 *   3. モデムを作る
 *   4. 終了シグナル (Ctrl-C 等) のハンドラを設置する
 *   5. vm_modem_run() を呼ぶ
 *
 * ===========================================================================
 * Step 3: Linux (POSIX) 対応
 * ===========================================================================
 *
 *   Windows                        Linux (POSIX)
 *   -----------------------------  ----------------------------------------
 *   WSAStartup (vm_winsock_init)   不要。socket() はそのまま使える
 *   SetConsoleCtrlHandler          sigaction(SIGINT / SIGTERM / SIGHUP)
 *   (SIGPIPE 相当なし)             SIGPIPE を SIG_IGN  ★難所 3
 *
 * ---------------------------------------------------------------------------
 * 難所 1: ハンドラの中で後片付けをしてはいけない
 * ---------------------------------------------------------------------------
 * Windows の SetConsoleCtrlHandler は **別スレッド** でハンドラを呼ぶ。
 * POSIX の signal はシグナルコンテキストで呼ぶ。どちらの場合も
 * 「そこで後片付けをする」のは危険である。COM / tty を閉じている最中に
 * イベントループが同じ fd を触れば、最良でも EBADF、最悪では
 * 別の目的で再利用された fd に PPP フレームを書き込む。
 *
 * したがってハンドラでは vm_modem_request_stop() でフラグを立てるだけに
 * 留め、実際の後片付けはイベントループが自分で抜けてから main が行う。
 *
 * POSIX 側で更に厳しいのは「ハンドラ内で呼べるのは
 * async-signal-safe な関数だけ」という制約である。上流の Windows 版は
 * ハンドラで fprintf() を呼んでいるが、これは POSIX では未定義動作
 * (stdio のロックを取るため、malloc のロック中に割り込むとデッドロック
 *  する)。よって Linux 側は write(2) を直接使う。
 *
 * ---------------------------------------------------------------------------
 * 難所 2: シグナルはどのスレッドに配送されるか分からない
 * ---------------------------------------------------------------------------
 * プロセス宛のシグナル (Ctrl-C = SIGINT) は「そのシグナルをブロックして
 * いない任意の 1 スレッド」に配送される。VModem は Step 2 の ALSA
 * レンダースレッドを持つので、Ctrl-C が **音声スレッドに** 配送される
 * 事が普通に起こる。
 *
 * request_stop はフラグを立てるだけなので停止自体は成立するが、
 * その場合 **メインスレッドの poll() は EINTR で抜けない**。つまり
 * 停止が最大 200ms (VM_MODEM_COM_BLOCK_MS) 遅れる。
 *
 * そこで「モデム生成 (= 音声スレッド生成) の前に対象シグナルをブロック
 * しておく」。スレッドは生成時にシグナルマスクを継承するので、
 * レンダースレッドはこれらのシグナルを永久にブロックしたままになる。
 * その後メインスレッドだけがマスクを解除すれば、配送先はメイン
 * スレッドに限定される。これは POSIX スレッドプログラムの定石。
 *
 * さらに sa_flags に SA_RESTART を **付けない**。付けると read() が
 * 自動再開してしまい、せっかく EINTR で抜けさせた意味が無くなる
 * (poll()/select() は SA_RESTART の有無に関わらず再開されないが、
 *  read()/write() は再開される)。
 *
 * ---------------------------------------------------------------------------
 * 難所 3: SIGPIPE で無言のうちに死ぬ
 * ---------------------------------------------------------------------------
 * Linux の既定では、切断済みソケットへ write すると **プロセスが
 * SIGPIPE で即死する** (既定動作 = Term)。Windows にこの概念は無い。
 *
 * VModem は libslirp 経由で多数の TCP ソケットを扱う。相手が RST を
 * 返した直後に送信すれば SIGPIPE が飛ぶ。これを放置すると
 * 「ダイアルアップ中にブラウザを閉じたら vmodem が消えた」という
 * 極めて再現しにくい障害になる。ログにも何も残らない。
 *
 * よって SIGPIPE は SIG_IGN にする。write() は EPIPE を返すようになり、
 * libslirp のエラー処理に正しく載る。
 *
 * ---------------------------------------------------------------------------
 * 難所 4: 2 回目の Ctrl-C
 * ---------------------------------------------------------------------------
 * 正常な停止では PPP の Terminate 交換や ALSA のドレインを待つので、
 * 停止までに数百 ms 掛かる。何かの理由でそこが詰まると
 * 「Ctrl-C が効かない」と見える。ユーザは必ずもう一度 Ctrl-C を押す。
 *
 * そこで 2 回目は _exit(128 + signo) で即座に落とす。_exit() は
 * async-signal-safe であり、atexit ハンドラも stdio のフラッシュも
 * 行わないので、ロックを持ったまま割り込んでいても安全に終われる。
 * ===========================================================================
 */

/*
 * ★ 機能テストマクロは全てのヘッダより前 ★
 * -std=c99 (__STRICT_ANSI__) では sigaction / pthread_sigmask /
 * SA_RESTART / STDERR_FILENO が glibc に隠される。
 *   _POSIX_C_SOURCE 200809L : sigaction, pthread_sigmask, write
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ★順序厳守★ (Windows のみ関係する話)
 * vm_winsock.h は winsock2.h -> ws2tcpip.h -> windows.h の順を保証する。
 * windows.h を先に通すと古い winsock.h (Winsock 1.1) が入り、
 * 後から winsock2.h を読んだ翻訳単位と型が食い違う。
 * したがって **他のどのヘッダより先に** これを置く。
 *
 * POSIX ではこのヘッダは <sys/socket.h> 等を通すだけの薄い層になる。
 */
#include "vmodem/vm_winsock.h"

#include "vmodem/vm_modem.h"
#include "vmodem/vm_config.h"
#include "vmodem/vm_log.h"
#include "vmodem/vm_nat.h"

#if !defined(_WIN32)
#  include <signal.h>
#  include <unistd.h>
#  include <errno.h>
#  include <pthread.h>
#endif

/*
 * ハンドラから触るので volatile。
 *
 * ★ 解放との競合を避ける手順 ★
 * vm_modem_destroy() の **前に** NULL を代入する。こうしておけば、
 * 破棄中に飛んできたシグナルは「まだモデムが無い」と見なされ、
 * 解放済みメモリを触らない。ポインタ 1 語の代入は、対象アーキテクチャ
 * (x86-64 / ARMv8) では分割されないので、ハンドラは NULL か有効値の
 * どちらかしか見ない。
 */
static vm_modem_t *volatile g_modem = NULL;

#ifdef _WIN32

static BOOL WINAPI ctrl_handler(DWORD type)
{
    vm_modem_t *m;

    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        /*
         * Windows のハンドラは専用スレッドで呼ばれる (シグナル
         * コンテキストではない) ので stdio を使ってよい。
         */
        fprintf(stderr, "\n終了要求を受けました。切断処理をします...\n");
        m = g_modem;
        if (m != NULL) vm_modem_request_stop(m);
        return TRUE;
    default:
        return FALSE;
    }
}

static int install_signal_handlers(void)
{
    return SetConsoleCtrlHandler(ctrl_handler, TRUE) ? 0 : -1;
}

/* Windows では何もしない (難所 2 は POSIX 固有) */
static void block_exit_signals(void)   { }
static void unblock_exit_signals(void) { }

#else  /* ------------------------------- POSIX ------------------------------ */

/* 受け取ったシグナル番号。0 = 未受信。ハンドラから書くので sig_atomic_t */
static volatile sig_atomic_t g_signo = 0;

/* 停止対象。SIGHUP を入れるのは、端末が消えた時も後片付けをするため。 */
static const int g_exit_signals[] = { SIGINT, SIGTERM, SIGHUP };
#define G_EXIT_SIGNAL_COUNT ((int)(sizeof(g_exit_signals) / sizeof(g_exit_signals[0])))

/* async-signal-safe な固定文字列出力 */
static void sig_puts(const char *s, size_t n)
{
    ssize_t ignored = write(STDERR_FILENO, s, n);
    (void)ignored;   /* 失敗しても何もできない。EINTR も無視して良い */
}

static void on_signal(int sig)
{
    /*
     * ★ errno を壊さない ★
     * ハンドラは「read() が -1 を返した直後、errno を読む前」に
     * 割り込む事がある。ハンドラ内の write() が errno を書き換えると、
     * 呼び出し側が EAGAIN を EINTR と誤認する等の不可解な挙動になる。
     */
    int saved_errno = errno;
    vm_modem_t *m;

    if (g_signo != 0) {
        /* 難所 4: 2 回目は即座に落とす */
        static const char msg[] =
            "\n[vmodem] 2 回目の終了要求。強制終了します\n";
        sig_puts(msg, sizeof(msg) - 1);
        _exit(128 + sig);
    }

    g_signo = (sig_atomic_t)sig;

    {
        static const char msg[] =
            "\n[vmodem] 終了要求を受けました。切断処理をします "
            "(もう一度送ると強制終了)\n";
        sig_puts(msg, sizeof(msg) - 1);
    }

    /*
     * フラグを立てるだけ (難所 1)。
     * vm_modem_request_stop() は m->stop = 1 の代入のみなので
     * シグナルコンテキストから呼んでも安全。
     */
    m = g_modem;
    if (m != NULL) vm_modem_request_stop(m);

    errno = saved_errno;
}

/*
 * 難所 2 前半: 対象シグナルをブロックする。
 * モデム生成 (= ALSA レンダースレッド生成) より前に呼ぶ事に意味がある。
 */
static void block_exit_signals(void)
{
    sigset_t set;
    int i;

    sigemptyset(&set);
    for (i = 0; i < G_EXIT_SIGNAL_COUNT; i++)
        sigaddset(&set, g_exit_signals[i]);

    /*
     * sigprocmask ではなく pthread_sigmask を使う。
     * マルチスレッドプログラムでの sigprocmask の挙動は
     * POSIX では未規定 (Linux では動くが、規格に沿う方を選ぶ)。
     */
    (void)pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* 難所 2 後半: メインスレッドだけマスクを解除する */
static void unblock_exit_signals(void)
{
    sigset_t set;
    int i;

    sigemptyset(&set);
    for (i = 0; i < G_EXIT_SIGNAL_COUNT; i++)
        sigaddset(&set, g_exit_signals[i]);

    (void)pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    struct sigaction ign;
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /*
     * ハンドラ実行中は他の終了シグナルもブロックする。
     * こうしないと SIGINT のハンドラ実行中に SIGTERM が入り込み、
     * 「1 回目」の判定が二重に走る。
     */
    for (i = 0; i < G_EXIT_SIGNAL_COUNT; i++)
        sigaddset(&sa.sa_mask, g_exit_signals[i]);

    /*
     * ★ SA_RESTART を付けない ★ (難所 2)
     * 付けると read()/write() が自動再開し、シリアル読取の
     * タイムアウト (200ms) まで停止が遅れる。
     */
    sa.sa_flags = 0;

    for (i = 0; i < G_EXIT_SIGNAL_COUNT; i++) {
        if (sigaction(g_exit_signals[i], &sa, NULL) != 0) {
            VM_LOGE("signal: sigaction(%d) が失敗 (%s)",
                    g_exit_signals[i], strerror(errno));
            return -1;
        }
    }

    /* 難所 3: SIGPIPE を無視して write() に EPIPE を返させる */
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    if (sigaction(SIGPIPE, &ign, NULL) != 0) {
        VM_LOGE("signal: SIGPIPE を無視できない (%s)", strerror(errno));
        return -1;
    }

    VM_LOGD("signal: SIGINT/SIGTERM/SIGHUP を捕捉、SIGPIPE を無視");
    return 0;
}

#endif /* _WIN32 */

static void usage(const char *argv0)
{
    printf(
"VModem - 本物のダイアルアップモデムエミュレータ\n"
#if defined(__linux__)
"        (Khadas VIM1 / Armbian Linux 移植版)\n"
#endif
"\n"
"使い方: %s [オプション]\n"
"\n"
"  -c, --config <path>   設定ファイル (既定: config.ini)\n"
#if defined(__linux__)
"  -p, --port <name>     シリアルポートを上書き (例: /dev/ttyGS0)\n"
"                        /dev/ で始まらない名前を渡すと、実機の代わりに\n"
"                        疑似端末 (PTY) が割り当てられる (手動テスト用)\n"
#else
"  -p, --port <name>     COM ポート名を上書き (例: CNCB0)\n"
#endif
"  -v, --verbose         ログレベルを DEBUG に\n"
"      --trace           ログレベルを TRACE に (バイト単位のダンプ)\n"
"  -q, --quiet           音を出さない\n"
"      --wav <path>      合成音を WAV に書き出す (デバッグ用)\n"
"      --net <mode>      slirp | loopback | none\n"
"  -h, --help            このヘルプ\n"
"\n"
"設定例 (config.ini):\n"
"  [ISP_1]\n"
"  number   = 0120-000-0000\n"
"  speed    = 33600\n"
"  protocol = V34PLUS\n"
"\n"
#if defined(__linux__)
"実機での起動例:\n"
"  sudo %s --port /dev/ttyGS0 --net slirp -v\n"
"\n"
"終了は Ctrl-C (SIGINT)。systemd からは SIGTERM でも同じ経路で\n"
"PPP を切ってから終了します。\n"
"\n"
#endif
    , argv0
#if defined(__linux__)
    , argv0
#endif
    );
}

/*
 * モデムを開けなかった時の案内。
 * 原因の 8 割は「ポートが無い」「他プロセスが掴んでいる」「権限が無い」の
 * 3 つで、どれなのかで対処が全く違う。プラットフォーム別に具体的な
 * コマンドまで書く。
 */
static void print_open_failure_hint(const vm_config_t *cfg, vm_err_t rc)
{
    fprintf(stderr,
            "エラー: モデムを初期化できませんでした (%s)\n",
            vm_strerror(rc));
#if defined(__linux__)
    fprintf(stderr,
            "\n"
            "ポート '%s' を確認してください:\n"
            "\n"
            "  1. デバイスが存在するか\n"
            "       ls -l %s\n"
            "     無い場合は USB Gadget (CDC-ACM) が未設定です。\n"
            "     ホスト PC と USB-C で接続し、configfs で acm 機能を\n"
            "     有効にすると /dev/ttyGS0 が生えます。\n"
            "     (設定スクリプトは Step 7 の成果物です)\n"
            "\n"
            "  2. 他のプロセスが掴んでいないか\n"
            "       sudo fuser -v %s\n"
            "     serial-getty が有効だとログインプロンプトが\n"
            "     ポートを占有します:\n"
            "       sudo systemctl disable --now serial-getty@ttyGS0.service\n"
            "\n"
            "  3. 権限があるか (root 以外で動かす場合)\n"
            "       sudo usermod -aG dialout $USER   # 再ログインが必要\n"
            "\n",
            cfg->com_port, cfg->com_port, cfg->com_port);
#else
    fprintf(stderr,
            "COM ポート '%s' が存在し、他のプロセスが開いていないか\n"
            "確認してください (com0com のセットアップは\n"
            "scripts/setup-com0com.ps1 を参照)。\n",
            cfg->com_port);
#endif
}

int main(int argc, char **argv)
{
    const char *cfg_path = "config.ini";
    const char *port_override = NULL;
    const char *net_override  = NULL;
    const char *wav_override  = NULL;
    int  level_override = -1;
    int  quiet = 0;
    vm_config_t cfg;
    char errbuf[256];
    char status[256];
    vm_err_t rc;
    int exit_code = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc)
            cfg_path = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--port")) && i + 1 < argc)
            port_override = argv[++i];
        else if (!strcmp(a, "--net") && i + 1 < argc)
            net_override = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc)
            wav_override = argv[++i];
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))
            level_override = VM_LOG_DEBUG;
        else if (!strcmp(a, "--trace"))
            level_override = VM_LOG_TRACE;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))
            quiet = 1;
        else {
            fprintf(stderr, "不明なオプション: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    /* --- 設定 --- */
    vm_config_defaults(&cfg);
    errbuf[0] = '\0';
    rc = vm_config_load(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (rc != VM_OK) {
        /*
         * 設定が読めなくても既定値で起動できるようにする。
         * 「まず動かして音を聞きたい」というのが最初の使い方なので、
         * ここで終了するとハードルが上がる。
         */
        fprintf(stderr,
                "警告: '%s' を読み込めませんでした (%s)。既定値で起動します。\n",
                cfg_path, errbuf[0] ? errbuf : "理由不明");
    }

    if (port_override != NULL)
        snprintf(cfg.com_port, sizeof(cfg.com_port), "%s", port_override);
    if (net_override != NULL)
        snprintf(cfg.net_mode, sizeof(cfg.net_mode), "%s", net_override);
    if (wav_override != NULL) {
        cfg.dump_wav = true;
        snprintf(cfg.dump_wav_path, sizeof(cfg.dump_wav_path), "%s", wav_override);
    }
    if (quiet) cfg.audio_enable = false;
    if (level_override >= 0) cfg.log_level = level_override;

    /* --- ログ --- */
    vm_log_init((vm_log_level_t)cfg.log_level,
                cfg.log_file[0] ? cfg.log_file : NULL);

    /* --- ソケット層の初期化 ---
     *
     * ★必ず vm_modem_create() より前★
     *
     * Windows では WSAStartup() を呼ぶまで socket() / sendto() /
     * WSAPoll() が WSANOTINITIALISED (10093) で全部失敗する。
     * 詳細は include/vmodem/vm_winsock.h 冒頭「罠 2」。
     *
     * Linux ではソケット層の事前初期化という概念そのものが無い
     * (socket(2) は最初の呼び出しから使える) ので、この節を
     * #ifdef _WIN32 で完全に囲い、POSIX では 1 命令も実行しない。
     * vm_winsock.c には POSIX 用の no-op も残してあるが、
     * 「Linux では初期化不要」という設計意図をコード上で明示するため、
     * 呼び出し自体を Windows 限定にしている。
     *
     * ログ初期化より後に置いているのは、失敗理由をログに残すため。
     */
#ifdef _WIN32
    errbuf[0] = '\0';
    if (vm_winsock_init(errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr,
                "エラー: Winsock を初期化できませんでした (%s)\n",
                errbuf[0] ? errbuf : "理由不明");
        VM_LOGE("winsock: 初期化失敗 (%s)", errbuf[0] ? errbuf : "理由不明");
        vm_log_shutdown();
        return 1;
    }
#endif

    VM_LOGI("VModem 起動 (config=%s port=%s net=%s isp=%d 件)",
            cfg_path, cfg.com_port, cfg.net_mode, cfg.isp_count);
    for (i = 0; i < cfg.isp_count; i++) {
        VM_LOGI("  [%s] %s (正規化 %s) %d bps %s",
                cfg.isp[i].section, cfg.isp[i].number_raw,
                cfg.isp[i].number_norm, cfg.isp[i].speed,
                vm_standard_name(cfg.isp[i].protocol));
    }
    if (cfg.isp_count == 0)
        VM_LOGW("ISP エントリが 0 件です。どの番号にダイアルしても "
                "NO CARRIER になります");

#if defined(__linux__)
    /*
     * 実機での取り違えが多いので注意を出す。
     * vm_serial の AUTO 解決は「/dev/ で始まるか」で実 tty と PTY を
     * 分けるため、上流の既定値 (com0com の "CNCB0") をそのまま使うと
     * 黙って PTY が割り当てられ、「USB で繋がらない」と悩む事になる。
     */
    if (strncmp(cfg.com_port, "/dev/", 5) != 0)
        VM_LOGW("port '%s' は /dev/ で始まらないため疑似端末 (PTY) を "
                "使います。実機では --port /dev/ttyGS0 を指定してください",
                cfg.com_port);
#endif

    /* --- 難所 2 前半: 音声スレッド生成より前にシグナルをブロック --- */
    block_exit_signals();

    /* --- モデム ---
     *
     * ★ 一旦ローカルに受けてから g_modem に代入する ★
     * g_modem は volatile なので &g_modem を vm_modem_t ** として渡すと
     * volatile 修飾が捨てられる (-Wdiscarded-qualifiers)。単に警告を
     * 消したいだけではなく、「生成が完全に終わってからハンドラに
     * 見せる」という順序を明示する意味がある。生成途中の半端な
     * オブジェクトをシグナルハンドラに触らせない。
     */
    {
        vm_modem_t *m = NULL;
        rc = vm_modem_create(&m, &cfg);
        if (rc != VM_OK) {
            print_open_failure_hint(&cfg, rc);
            unblock_exit_signals();
#ifdef _WIN32
            vm_winsock_cleanup();
#endif
            vm_log_shutdown();
            return 1;
        }
        g_modem = m;
    }

    /* --- 終了シグナル --- */
    if (install_signal_handlers() != 0) {
        /*
         * ここで諦めるのが正しい。ハンドラ無しで走ると Ctrl-C が
         * 既定動作 (即 Term) になり、PPP を切らず tty も復元されずに
         * 死ぬ。次回の起動が「ポートが壊れている」状態から始まる。
         */
        vm_modem_t *m = g_modem;

        fprintf(stderr,
                "エラー: シグナルハンドラを設置できませんでした\n");
        g_modem = NULL;              /* 難所 1: 破棄より前に外す */
        vm_modem_destroy(m);
        unblock_exit_signals();
#ifdef _WIN32
        vm_winsock_cleanup();
#endif
        vm_log_shutdown();
        return 1;
    }

    /* --- 難所 2 後半: 以後シグナルはこのスレッドに配送される --- */
    unblock_exit_signals();

    printf("VModem 待機中。"
#if defined(__linux__)
           "PC のダイアルアップ接続から発信してください。\n"
#else
           "Windows のダイアルアップ接続から発信してください。\n"
#endif
           "終了は Ctrl-C。\n");
    fflush(stdout);

    /*
     * ★ ブロック解除の直後に取りこぼしを確認する ★
     * ブロック中に届いたシグナルは解除の瞬間に配送されるので、
     * この時点で g_signo が立っている事があり得る (systemd が起動直後に
     * stop した場合など)。イベントループに入る前に確認しないと、
     * 「停止要求済みなのに待ち受けを始める」1 周分の無駄が生じる。
     */
#if !defined(_WIN32)
    if (g_signo != 0) {
        VM_LOGI("起動処理中に終了要求 (signal %d) を受けていたため "
                "イベントループには入りません", (int)g_signo);
        rc = VM_OK;
    } else
#endif
    {
        rc = vm_modem_run(g_modem);
    }

    VM_LOGI("最終状態: %s", vm_modem_status(g_modem, status, sizeof(status)));

    {
        /* 難所 1: 破棄する前にハンドラから見えなくする */
        vm_modem_t *m = g_modem;
        g_modem = NULL;
        vm_modem_destroy(m);
    }

#ifdef _WIN32
    vm_winsock_cleanup();
#endif

    exit_code = (rc == VM_OK) ? 0 : 1;

#if !defined(_WIN32)
    /*
     * シグナルで終わった事をログに残す。
     * 「自分で落ちた」のか「systemd が止めた」のかは、後から
     * ログだけで切り分けられるようにしておく価値がある。
     */
    if (g_signo != 0)
        VM_LOGI("signal %d により正常終了", (int)g_signo);
#endif

    /* ★ ログの後始末は最後 ★ 上の VM_LOGI より後である事が必要 */
    vm_log_shutdown();

    return exit_code;
}
