/*
 * vm_log.c - ログ出力実装
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * Step 4: Linux (POSIX) 対応
 * ===========================================================================
 * Windows 専用 API は全て #ifdef _WIN32 の内側に閉じ込め、POSIX 側は
 * fprintf/fwrite + pthread_mutex だけで完結させる。
 *
 * 対応表:
 *
 *   Windows                      POSIX (Linux)
 *   ---------------------------  ------------------------------------------
 *   CRITICAL_SECTION             pthread_mutex_t (静的初期化子)
 *   GetCurrentThreadId()         syscall(SYS_gettid)  ★下記「難所 B」
 *   GetLocalTime() + localtime_s clock_gettime() + localtime_r() ★難所 A
 *   OutputDebugStringA()         (相当物なし。stderr のみ) ★難所 C
 *
 * ---------------------------------------------------------------------------
 * 難所 A: 秒とミリ秒を別の時計から取ってはいけない
 * ---------------------------------------------------------------------------
 * 移植前の実装は
 *
 *     t  = time(NULL);                       /- 秒     -/
 *     clock_gettime(CLOCK_REALTIME, &ts);    /- ミリ秒 -/
 *
 * と **2 回別々に** 時刻を読んでいた。この 2 行の間に秒が繰り上がると
 * 「秒は繰り上がり前、ミリ秒は繰り上がり後」の組み合わせになり、
 *
 *     12:00:00.998
 *     12:00:00.001   <- 巻き戻って見える
 *     12:00:01.003
 *
 * のようにログ上で時間が逆行する。ダイアルアップのタイミング問題
 * (LCP の再送間隔、DCD を上げる順序) を追う時に、ログの時刻が信用
 * できないのは致命的なので、**1 回の clock_gettime() から秒とミリ秒の
 * 両方を作る**ように直した。Windows 側も同様に GetLocalTime() の
 * SYSTEMTIME だけで完結させている (localtime_s を併用しない)。
 *
 * ---------------------------------------------------------------------------
 * 難所 B: pthread_self() はログに出しても役に立たない
 * ---------------------------------------------------------------------------
 * glibc の pthread_self() はスレッド記述子の**アドレス**なので、
 * ログには 140234876262208 のような 15 桁が並ぶ。しかもこの値は
 * top -H / ps -L / gdb "info threads" のどの表示とも一致しないため、
 * 「音が途切れた時に動いていたのはどのスレッドか」を突き合わせられない。
 *
 * Linux では代わりにカーネルの TID (gettid) を出す。これは
 *   - top -H の PID 列
 *   - ps -L の LWP 列
 *   - /proc/<pid>/task/<tid>
 * と完全に一致するので、ALSA レンダースレッド (Step 2) と
 * イベントループ (単一スレッド) の区別が目で見て付く。
 *
 * gettid() のラッパ関数は glibc 2.30 以降にしか無く、かつ -std=c99 では
 * 隠れてしまうため、syscall(SYS_gettid) を直接呼ぶ。これは Linux 2.4.11
 * 以降で常に使える。
 *
 * ---------------------------------------------------------------------------
 * 難所 C: 1 行を 1 回の書き込みで出す
 * ---------------------------------------------------------------------------
 * stderr は行バッファではなく**無バッファ**なので、fprintf に複数の
 * 変換指定を渡すと glibc は書式単位で複数回 write(2) を発行する事がある。
 * VModem は ALSA レンダースレッド (Step 2) を持つマルチスレッドなので、
 * その隙間に別スレッドの出力が割り込むと 1 行が途中で混ざる。
 * ミューテックスで守っていても、printf() 経由の stdout や libslirp /
 * alsa-lib が直接 stderr に吐く警告とは同期できない。
 *
 * よって「まず 1 本のバッファに組み立て、fwrite で 1 回だけ書く」形に
 * している。こうすると PIPE_BUF (4096) 以下の行は事実上分断されない。
 * ===========================================================================
 */

/*
 * ★ 機能テストマクロは全てのヘッダより前 ★
 *
 * -std=c99 は __STRICT_ANSI__ を定義し、glibc は POSIX 宣言を隠す。
 * その結果 localtime_r / clock_gettime / isatty / syscall が
 * implicit declaration になり、64bit 環境では戻り値が int に切り詰められて
 * 静かに壊れる。
 *
 *   _POSIX_C_SOURCE : localtime_r / clock_gettime / isatty
 *   _DEFAULT_SOURCE : syscall() の宣言 (<unistd.h>)
 */
#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if !defined(_DEFAULT_SOURCE)
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
static CRITICAL_SECTION g_lock;
static int  g_lock_ready = 0;
#  define LOCK_INIT()    do { if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; } } while (0)
#  define LOCK_FINI()    do { if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; } } while (0)
#  define LOCK()         do { if (g_lock_ready) EnterCriticalSection(&g_lock); } while (0)
#  define UNLOCK()       do { if (g_lock_ready) LeaveCriticalSection(&g_lock); } while (0)
#else
#  include <pthread.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
/*
 * PTHREAD_MUTEX_INITIALIZER で静的初期化しておく理由:
 * vm_log_write() は vm_log_init() より前に呼ばれる可能性がある
 * (main.c は設定ファイルの読み込み失敗を fprintf で報告してから
 *  ログを初期化する)。静的初期化ならその順序に依存しない。
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK_INIT()    do { } while (0)
#  define LOCK_FINI()    do { } while (0)
#  define LOCK()         (void)pthread_mutex_lock(&g_lock)
#  define UNLOCK()       (void)pthread_mutex_unlock(&g_lock)
#endif

static vm_log_level_t g_level = VM_LOG_INFO;
static FILE          *g_fp    = NULL;

/*
 * stderr が端末の時だけ色を付ける。
 * systemd の journal / パイプ / リダイレクト先ではエスケープ列が
 * そのまま記録されて grep の邪魔になるため、必ず判定してから使う。
 */
static int g_color = 0;

/* --------------------------------------------------------------------------
 * スレッド ID (難所 B)
 * -------------------------------------------------------------------------- */
static unsigned long log_tid(void)
{
#if defined(_WIN32)
    return (unsigned long)GetCurrentThreadId();
#elif defined(__linux__) && defined(SYS_gettid)
    return (unsigned long)syscall(SYS_gettid);
#else
    /* 他の POSIX。値の意味は薄いが「別スレッドである」事は分かる。 */
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

/* --------------------------------------------------------------------------
 * 時刻 (難所 A: 必ず 1 つの時計から秒とミリ秒を作る)
 * -------------------------------------------------------------------------- */
static void log_stamp(char *buf, size_t size)
{
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);          /* 秒とミリ秒が同一スナップショット */
    snprintf(buf, size, "%02d:%02d:%02d.%03d",
             (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
             (int)st.wMilliseconds);
#else
    struct timespec ts;
    struct tm       tmv;
    time_t          t;
    long            ms;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        t  = (time_t)ts.tv_sec;
        ms = (long)(ts.tv_nsec / 1000000L);
    } else {
        /* CLOCK_REALTIME が無い環境は考えにくいが、無音で壊さない */
        t  = time(NULL);
        ms = 0;
    }

    if (localtime_r(&t, &tmv) == NULL) {
        snprintf(buf, size, "--:--:--.---");
        return;
    }
    snprintf(buf, size, "%02d:%02d:%02d.%03ld",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
#endif
}

static const char *level_tag(vm_log_level_t l)
{
    switch (l) {
    case VM_LOG_ERROR: return "ERR";
    case VM_LOG_WARN:  return "WRN";
    case VM_LOG_INFO:  return "INF";
    case VM_LOG_DEBUG: return "DBG";
    case VM_LOG_TRACE: return "TRC";
    default:           return "---";
    }
}

/* ANSI SGR。g_color が立っている時だけ使う。 */
static const char *level_color(vm_log_level_t l)
{
    switch (l) {
    case VM_LOG_ERROR: return "\033[1;31m";   /* 赤     */
    case VM_LOG_WARN:  return "\033[1;33m";   /* 黄     */
    case VM_LOG_DEBUG: return "\033[2m";      /* 薄字   */
    case VM_LOG_TRACE: return "\033[2m";
    default:           return NULL;           /* INFO は素のまま */
    }
}

/* ソースパスからファイル名部分だけを取り出す */
static const char *basename_only(const char *p)
{
    const char *s = p, *q;
    if (!p) return "?";
    for (q = p; *q; q++)
        if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

/* --------------------------------------------------------------------------
 * 初期化 / 終了
 * -------------------------------------------------------------------------- */
vm_err_t vm_log_init(vm_log_level_t level, const char *path)
{
    FILE *fp = NULL;
    int   saved_errno = 0;

    /*
     * ★ fopen はロックの外で行う ★
     * ネットワーク越しのファイルシステムだと fopen が数百 ms 掛かる事が
     * あり、その間ログ全体 (= 別スレッドの ERROR も) が止まってしまう。
     */
    if (path != NULL && path[0] != '\0') {
        fp = fopen(path, "a");
        if (fp == NULL) saved_errno = errno;
    }

    LOCK_INIT();
    LOCK();

    g_level = level;

#if !defined(_WIN32)
    g_color = isatty(2) ? 1 : 0;
#else
    g_color = 0;   /* 旧 conhost は ANSI を解釈しないので付けない */
#endif

    if (g_fp != NULL && g_fp != stderr) {
        fclose(g_fp);
        g_fp = NULL;
    }

    if (fp != NULL) {
        /*
         * 行バッファにする。既定 (ファイルなので全バッファ) のままだと
         * SIGKILL や電源断で直近 4KB のログが消える。ダイアルアップの
         * 不具合は「落ちる直前の数行」が一番重要なので必ず行単位で出す。
         * vm_log_write() 側でも fflush しているが、二重の保険。
         */
        setvbuf(fp, NULL, _IOLBF, 0);
        g_fp = fp;
    }

    UNLOCK();

    if (path != NULL && path[0] != '\0' && fp == NULL) {
        /*
         * 失敗理由を必ず添える。sudo で起動した後に一般ユーザで
         * 起動し直すと root 所有の vmodem.log を開けず EACCES になる、
         * という実際に起きるパターンの切り分けに必要。
         */
        fprintf(stderr, "[vmodem] ログファイルを開けません: %s (%s)\n",
                path, strerror(saved_errno));
        return VM_ERR_IO;
    }
    return VM_OK;
}

void vm_log_shutdown(void)
{
    FILE *fp;

    LOCK();
    fp   = (g_fp != stderr) ? g_fp : NULL;
    g_fp = NULL;
    UNLOCK();

    /* fclose もロックの外。理由は vm_log_init() の fopen と同じ。 */
    if (fp != NULL) fclose(fp);

    LOCK_FINI();
}

void vm_log_set_level(vm_log_level_t level) { g_level = level; }
vm_log_level_t vm_log_get_level(void)       { return g_level; }

/* --------------------------------------------------------------------------
 * 1 行の出力 (難所 C: 組み立ててから 1 回で書く)
 * --------------------------------------------------------------------------
 *
 * ★ 時刻の取得をロックの内側で行う理由 ★
 * ロックの外で時刻を取ると、
 *
 *   スレッド A: 時刻取得 (12:00:00.100) ─────────┐ (プリエンプト)
 *   スレッド B: 時刻取得 (12:00:00.150) → 書込   │
 *   スレッド A: ────────────────────────────────┘ → 書込
 *
 * という順序が起こり、ファイル上では .150 の行が .100 の行より前に
 * 現れる。ログを時系列として読む前提が崩れ、「DCD を上げる前に
 * PPP が来ている」といった存在しない因果を疑う羽目になる。
 *
 * そこで「時刻取得 → 行組立 → 書込」を 1 つのクリティカルセクションに
 * 収める。ロック内でやるのはスタックバッファへの snprintf だけで
 * syscall は伴わないので、コストは無視できる。ユーザ書式の展開
 * (vsnprintf、%s で任意長の文字列を触る可能性がある) はロックの外で
 * 済ませてある。
 */
static void log_emit(vm_log_level_t level, const char *file, int line,
                     const char *msg)
{
    char        stamp[32];
    char        out[1280];
    const char *col;
    unsigned long tid;
    int         n;

    /* tid はロック外で取れる (呼び出しスレッド固有で変化しない) */
    tid = (level >= VM_LOG_DEBUG) ? log_tid() : 0;
    col = g_color ? level_color(level) : NULL;

    LOCK();

    log_stamp(stamp, sizeof(stamp));

    if (level >= VM_LOG_DEBUG) {
        /* DEBUG 以上は発生源とスレッドを出す (INFO 以下はユーザ向け) */
        n = snprintf(out, sizeof(out), "[%s] %s %s:%d (t%lu) %s\n",
                     stamp, level_tag(level), basename_only(file), line,
                     tid, msg);
    } else {
        n = snprintf(out, sizeof(out), "[%s] %s %s\n",
                     stamp, level_tag(level), msg);
    }

    if (n < 0) {                             /* 書式エラー */
        UNLOCK();
        return;
    }
    if ((size_t)n >= sizeof(out)) {
        /* 切り詰められた。改行だけは必ず残す (行が繋がると grep が壊れる) */
        n = (int)sizeof(out) - 1;
        out[n - 1] = '\n';
    }

    if (col != NULL) {
        fputs(col, stderr);
        fwrite(out, 1, (size_t)n, stderr);
        fputs("\033[0m", stderr);
    } else {
        fwrite(out, 1, (size_t)n, stderr);
    }
    if (g_fp != NULL) {
        /* ファイルには色を入れない (grep / journal で邪魔になる) */
        fwrite(out, 1, (size_t)n, g_fp);
        fflush(g_fp);
    }

    UNLOCK();
}

void vm_log_write(vm_log_level_t level, const char *file, int line,
                  const char *fmt, ...)
{
    char    msg[1024];
    va_list ap;
    int     n;

    /* レベル判定はロック外で行う (無効レベルのコストをほぼ 0 にする) */
    if (level > g_level || level == VM_LOG_NONE) return;
    if (fmt == NULL) return;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    log_emit(level, file, line, msg);
}

/* --------------------------------------------------------------------------
 * 16 進ダンプ
 * -------------------------------------------------------------------------- */

/*
 * バッファ末尾を越えない追記ヘルパ。
 *
 * ★なぜ必要か★
 * snprintf は「書き込んだ長さ」ではなく「書き込みたかった長さ」を返す。
 * off += snprintf(buf + off, sizeof(buf) - off, ...) を素朴に繰り返すと、
 * 一度でも切り詰めが起きた瞬間に off > sizeof(buf) となり、次の
 * sizeof(buf) - off が size_t の巨大な値に化けてバッファ外へ書き込む。
 * 現状のフォーマットは 75 バイトで収まっているので事故は起きていないが、
 * 「フォーマットを 1 文字足したら壊れる」コードを残す理由が無い。
 */
static void app(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (buf == NULL || size == 0 || *off >= size - 1) return;

    va_start(ap, fmt);
    n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    /* 実際に書けた分だけ進める (切り詰め時は末尾に張り付く) */
    *off += ((size_t)n < size - *off) ? (size_t)n : (size - *off - 1);
}

void vm_log_hexdump(vm_log_level_t level, const char *tag,
                    const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    char   line[128];
    size_t i;

    if (level > g_level || level == VM_LOG_NONE) return;
    if (p == NULL) return;

    vm_log_write(level, __FILE__, __LINE__, "%s (%u bytes)",
                 tag ? tag : "hexdump", (unsigned)len);

    for (i = 0; i < len; i += 16) {
        size_t off = 0;
        size_t j;

        app(line, sizeof(line), &off, "  %04X: ", (unsigned)i);
        for (j = 0; j < 16; j++) {
            if (i + j < len) app(line, sizeof(line), &off, "%02X ", p[i + j]);
            else             app(line, sizeof(line), &off, "   ");
        }
        app(line, sizeof(line), &off, " |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            app(line, sizeof(line), &off, "%c",
                (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        app(line, sizeof(line), &off, "|");

        vm_log_write(level, __FILE__, __LINE__, "%s", line);
    }
}
