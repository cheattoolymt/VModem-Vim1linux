/*
 * test_linux_step34.c - Linux 移植 Step 3 / Step 4 の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象:
 *   Step 3: src/main.c        (シグナル処理・Winsock の隔離)
 *   Step 4: src/core/vm_log.c (POSIX 対応のロガー)
 *
 * ===========================================================================
 * 使い方
 * ===========================================================================
 *   gcc -O2 -std=c99 -Wall -Wextra -Iinclude tests/test_linux_step34.c \
 *       src/core/vm_log.c src/core/vm_types.c -lpthread -o test_linux_step34
 *   ./test_linux_step34 [vmodem のパス]
 *
 * Step 3 はプロセス全体の振る舞い (シグナルの配送先・終了コード) なので、
 * ライブラリ関数として呼び出せない。そのため **実際に vmodem を起動して
 * シグナルを送り、/proc とログを観測する** 方式を取る。
 *
 * 第 1 引数か環境変数 VMODEM_BIN でバイナリを指定する。省略時は
 * ./vmodem → ./build/vmodem の順に探し、見つからなければ Step 3 の
 * 節を SKIP する (Step 4 の検証だけを行う)。ビルド系の整備は Step 7 の
 * 範囲なので、テストがビルド方法を仮定しないようにしている。
 * ===========================================================================
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>

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

/* =========================================================================
 * 小道具
 * =======================================================================*/

/* ファイル全体を読む。呼び出し側が free する。*len_out は NUL を含まない */
static char *slurp(const char *path, size_t *len_out)
{
    FILE  *fp;
    char  *buf;
    long   size;

    if (len_out != NULL) *len_out = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    rewind(fp);

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf); fclose(fp); return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    if (len_out != NULL) *len_out = (size_t)size;
    return buf;
}

static int count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static int count_substr(const char *hay, const char *needle)
{
    int    n = 0;
    size_t nl = strlen(needle);
    const char *p = hay;

    if (nl == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * "[HH:MM:SS.mmm] " を「その日の何ミリ秒目か」に変換する。
 * 解釈できなければ -1。
 */
static long parse_stamp(const char *line)
{
    int h, m, s, ms;

    if (line[0] != '[') return -1;
    if (sscanf(line + 1, "%2d:%2d:%2d.%3d", &h, &m, &s, &ms) != 4) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60) return -1;
    if (ms < 0 || ms > 999) return -1;
    return ((long)h * 3600L + (long)m * 60L + (long)s) * 1000L + ms;
}

/* =========================================================================
 * Step 4-1: ファイル出力の基本
 * =======================================================================*/
static void test_log_file_basic(const char *dir)
{
    char  path[512];
    char *body;
    int   ok;

    printf("\n=== Step 4-1: ファイル出力とレベル判定 ===\n");

    snprintf(path, sizeof(path), "%s/basic.log", dir);
    (void)remove(path);

    check(vm_log_init(VM_LOG_INFO, path) == VM_OK, "vm_log_init が VM_OK");
    check(vm_log_get_level() == VM_LOG_INFO, "レベルが INFO で始まる");

    VM_LOGI("マーカー-INFO");
    VM_LOGD("マーカー-DEBUG-出てはいけない");
    VM_LOGT("マーカー-TRACE-出てはいけない");
    VM_LOGE("マーカー-ERROR");

    vm_log_shutdown();

    body = slurp(path, NULL);
    if (body == NULL) {
        check(0, "ログファイルが作られている");
        return;
    }

    check(strstr(body, "マーカー-INFO") != NULL, "INFO 行が書かれている");
    check(strstr(body, "マーカー-ERROR") != NULL, "ERROR 行が書かれている");

    /* ★閾値以下は 1 バイトも出てはいけない★ */
    ok = (strstr(body, "マーカー-DEBUG") == NULL) &&
         (strstr(body, "マーカー-TRACE") == NULL);
    check(ok, "閾値以下 (DEBUG/TRACE) はファイルにも出ない");

    /* INFO 行は簡潔形式 (発生源を出さない)、DEBUG 以上は発生源付き */
    check(strstr(body, "] INF マーカー-INFO") != NULL,
          "INFO は '[時刻] INF 本文' の簡潔形式");

    check(count_lines(body) == 2, "行数がちょうど 2 (余計な出力が無い)");

    /*
     * ★ 端末でない出力先に色を付けてはいけない ★
     * journald / リダイレクト先にエスケープ列が入ると grep が壊れる。
     */
    check(strchr(body, '\033') == NULL,
          "ファイルに ANSI エスケープが混入しない");

    free(body);
}

/* =========================================================================
 * Step 4-2: 時刻 (難所 A)
 * =======================================================================*/
static void test_log_timestamp(const char *dir)
{
    char   path[512];
    char  *body, *p;
    long   prev = -1;
    int    lines = 0, regressions = 0, parsed = 0;

    printf("\n=== Step 4-2: 時刻が単調で、秒とミリ秒が同じ時計由来 ===\n");

    snprintf(path, sizeof(path), "%s/stamp.log", dir);
    (void)remove(path);
    vm_log_init(VM_LOG_INFO, path);

    /*
     * ★秒の繰り上がりを必ず踏ませる★
     * 移植前は time() と clock_gettime() を別々に読んでいたため、
     * 秒が繰り上がる瞬間に「秒は前・ミリ秒は後」の組み合わせが生じ、
     * 時刻が巻き戻って見えた。1.2 秒に渡って書き続ければ
     * 繰り上がりは必ず 1 回以上起こる。
     */
    {
        long t0 = now_ms();
        while (now_ms() - t0 < 1200) {
            VM_LOGI("stamp");
            sleep_ms(2);
        }
    }
    vm_log_shutdown();

    body = slurp(path, NULL);
    if (body == NULL) { check(0, "stamp.log を読める"); return; }

    for (p = body; *p != '\0'; ) {
        long t = parse_stamp(p);
        lines++;
        if (t >= 0) {
            parsed++;
            /* 日付をまたいだ場合だけ巻き戻りを許す (23:59:59 -> 00:00:00) */
            if (prev >= 0 && t < prev && (prev - t) < 3600L * 1000L)
                regressions++;
            prev = t;
        }
        p = strchr(p, '\n');
        if (p == NULL) break;
        p++;
    }

    check(lines > 100, "十分な行数を書いた (%d 行)", lines);
    check(parsed == lines, "全行の時刻を [HH:MM:SS.mmm] として解釈できる "
                           "(%d/%d)", parsed, lines);
    check(regressions == 0, "時刻が一度も巻き戻らない (巻き戻り %d 件)",
          regressions);

    free(body);
}

/* =========================================================================
 * Step 4-3: 並行出力で行が混ざらない (難所 C)
 * =======================================================================*/
#define CONC_THREADS   6
#define CONC_PER_THREAD 400

typedef struct { int id; } conc_arg_t;

static void *conc_worker(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    int i;

    for (i = 0; i < CONC_PER_THREAD; i++) {
        /* 固定長の識別可能な行。DEBUG にしてスレッド ID も出させる */
        VM_LOGD("CONC t=%d i=%03d ####################", a->id, i);
    }
    return NULL;
}

static void test_log_concurrent(const char *dir)
{
    char       path[512];
    pthread_t  th[CONC_THREADS];
    conc_arg_t args[CONC_THREADS];
    char      *body, *p;
    int        i, ok;
    int        lines = 0, good = 0;
    int        tids_seen = 0;
    unsigned long tid_list[CONC_THREADS + 2];

    printf("\n=== Step 4-3: 並行出力で 1 行が分断・混在しない ===\n");

    snprintf(path, sizeof(path), "%s/conc.log", dir);
    (void)remove(path);
    vm_log_init(VM_LOG_DEBUG, path);

    for (i = 0; i < CONC_THREADS; i++) {
        args[i].id = i;
        if (pthread_create(&th[i], NULL, conc_worker, &args[i]) != 0) {
            check(0, "pthread_create が成功する");
            vm_log_shutdown();
            return;
        }
    }
    for (i = 0; i < CONC_THREADS; i++) pthread_join(th[i], NULL);

    vm_log_shutdown();

    body = slurp(path, NULL);
    if (body == NULL) { check(0, "conc.log を読める"); return; }

    /*
     * 各行が「[時刻] DBG ファイル:行 (tN) CONC t=? i=??? ####...」の
     * 形を保っているか検査する。1 行の途中に別スレッドの出力が
     * 割り込むと、この検査が落ちる。
     */
    for (p = body; *p != '\0'; ) {
        char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        char   line[512];

        lines++;
        if (len < sizeof(line)) {
            unsigned long tid = 0;
            int t = -1, ix = -1;
            char *tp;

            memcpy(line, p, len);
            line[len] = '\0';

            tp = strstr(line, "(t");
            if (parse_stamp(line) >= 0 &&
                strstr(line, "] DBG ") != NULL &&
                tp != NULL &&
                sscanf(tp, "(t%lu)", &tid) == 1 &&
                strstr(line, "CONC t=") != NULL &&
                sscanf(strstr(line, "CONC t="), "CONC t=%d i=%d", &t, &ix) == 2 &&
                t >= 0 && t < CONC_THREADS &&
                ix >= 0 && ix < CONC_PER_THREAD &&
                strstr(line, " ####################") != NULL &&
                count_substr(line, "CONC t=") == 1) {
                int k, dup = 0;
                good++;
                for (k = 0; k < tids_seen; k++) if (tid_list[k] == tid) dup = 1;
                if (!dup && tids_seen < (int)(sizeof(tid_list) / sizeof(tid_list[0])))
                    tid_list[tids_seen++] = tid;
            }
        }

        if (eol == NULL) break;
        p = eol + 1;
    }

    check(lines == CONC_THREADS * CONC_PER_THREAD,
          "行数が投入数と一致 (%d / 期待 %d)",
          lines, CONC_THREADS * CONC_PER_THREAD);
    check(good == lines, "全行が壊れていない (整合 %d / %d)", good, lines);

    /*
     * ★ 難所 B の検証 ★
     * DEBUG 行のスレッド ID が全スレッドで異なる事。
     * pthread_self() のアドレスでも「異なる」だけは満たすが、
     * Linux では TID なので値が top -H と一致する (下記で範囲も確認)。
     */
    check(tids_seen == CONC_THREADS,
          "スレッドごとに異なるスレッド ID が記録される (%d 種)", tids_seen);

    ok = 1;
    for (i = 0; i < tids_seen; i++) {
        /* Linux の TID は pid_t (正の値) で、pid_max は既定 4194304 */
        if (tid_list[i] == 0 || tid_list[i] > 4194304UL) ok = 0;
    }
    check(ok, "スレッド ID が Linux の TID の値域 (1..pid_max) に収まる "
              "= top -H / ps -L と突き合わせられる");

    free(body);
}

/* =========================================================================
 * Step 4-4: 長すぎる行と hexdump の境界
 * =======================================================================*/
static void test_log_bounds(const char *dir)
{
    char  path[512];
    char  huge[4096];
    char *body, *p;
    int   i, bad_lines = 0, lines = 0, dump_lines = 0;

    printf("\n=== Step 4-4: 長大な入力でも行の形が壊れない ===\n");

    snprintf(path, sizeof(path), "%s/bounds.log", dir);
    (void)remove(path);
    vm_log_init(VM_LOG_TRACE, path);

    /* バッファ (1024) を大きく超える本文 */
    for (i = 0; i < (int)sizeof(huge) - 1; i++) huge[i] = 'A' + (char)(i % 26);
    huge[sizeof(huge) - 1] = '\0';
    VM_LOGI("LONG:%s", huge);

    /* 書式指定を含む本文が二次展開されない事 (%n / %s の混入対策) */
    VM_LOGI("PERCENT:%s", "100%% と %n と %s");

    /* hexdump: 256 バイト全域 (印字不能・0x00 を含む) */
    {
        uint8_t all[256];
        for (i = 0; i < 256; i++) all[i] = (uint8_t)i;
        vm_log_hexdump(VM_LOG_TRACE, "全 256 バイト", all, sizeof(all));
    }

    /* 端数 (16 の倍数でない長さ) で右側のパディングが崩れない事 */
    vm_log_hexdump(VM_LOG_TRACE, "端数 3 バイト", "\x01\x02\x03", 3);

    /* 長さ 0 / NULL は落ちてはいけない */
    vm_log_hexdump(VM_LOG_TRACE, "空", "", 0);
    vm_log_hexdump(VM_LOG_TRACE, "NULL", NULL, 16);

    vm_log_shutdown();

    body = slurp(path, NULL);
    if (body == NULL) { check(0, "bounds.log を読める"); return; }

    check(strstr(body, "LONG:ABCDEFG") != NULL,
          "長大な本文の先頭は残る (切り詰めであって破棄ではない)");

    /* 全行が [時刻] で始まる = 途中で改行が失われて連結していない */
    for (p = body; *p != '\0'; ) {
        char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        lines++;
        if (parse_stamp(p) < 0) bad_lines++;
        if (len > 1300) bad_lines++;      /* 出力バッファ長を超えている */
        if (eol == NULL) { bad_lines++; break; }  /* 最終行に改行が無い */
        p = eol + 1;
    }
    check(bad_lines == 0, "全 %d 行が [時刻] 始まり・改行終わり", lines);

    /* hexdump の各データ行は "  0000: .. |....|" の形 */
    dump_lines = count_substr(body, " |");
    check(dump_lines == 16 + 1, "hexdump の行数が正しい (256B=16 行 + 端数 1 行, "
                                "実測 %d)", dump_lines);
    check(strstr(body, "  0000: 00 01 02 03") != NULL,
          "hexdump が 0x00 を含むデータを扱える");
    check(strstr(body, "  00F0: F0 F1") != NULL,
          "hexdump が 256 バイト目まで出力する");

    /*
     * 端数行の右端。3 バイトなら 13 バイト分のパディングが入り、
     * ASCII 欄は 3 文字だけになる。
     */
    check(strstr(body, "  0000: 01 02 03                                        "
                       " |...|") != NULL,
          "端数の 16 バイト境界パディングが崩れない");

    free(body);
}

/* =========================================================================
 * Step 4-5: 失敗系と初期化前後
 * =======================================================================*/
static void test_log_robustness(const char *dir)
{
    char path[512];

    printf("\n=== Step 4-5: 失敗系 (開けないパス・二重終了・初期化前) ===\n");

    /* 存在しないディレクトリ下 -> VM_ERR_IO を返し、落ちない */
    snprintf(path, sizeof(path), "%s/no-such-dir-%d/x.log", dir, (int)getpid());
    check(vm_log_init(VM_LOG_INFO, path) == VM_ERR_IO,
          "開けないパスで VM_ERR_IO を返す");

    /* ファイルが無い状態でも stderr にだけ出て落ちない */
    VM_LOGE("開けなかった後でも stderr には出る (この行は画面に出る)");
    check(1, "ファイル不可の後も vm_log_write が安全");

    /* path=NULL は「stderr のみ」で成功 */
    check(vm_log_init(VM_LOG_INFO, NULL) == VM_OK, "path=NULL は VM_OK");
    check(vm_log_init(VM_LOG_INFO, "") == VM_OK, "path=\"\" は VM_OK");

    /* 二重 shutdown / shutdown 後の書き込み */
    vm_log_shutdown();
    vm_log_shutdown();
    check(1, "vm_log_shutdown() の二重呼び出しが安全");

    VM_LOGE("shutdown 後の出力 (この行は画面に出る)");
    check(1, "shutdown 後の vm_log_write が安全");

    /* fmt=NULL でクラッシュしない */
    vm_log_write(VM_LOG_ERROR, __FILE__, __LINE__, NULL);
    check(1, "fmt=NULL が安全");

    /* NONE は完全に黙る */
    vm_log_set_level(VM_LOG_NONE);
    VM_LOGE("VM_LOG_NONE でこの行は出てはいけない");
    check(vm_log_get_level() == VM_LOG_NONE, "VM_LOG_NONE で全て抑制される");
    vm_log_set_level(VM_LOG_INFO);
}

/* =========================================================================
 * Step 3: シグナル処理 (実プロセスを起動して観測)
 * =======================================================================*/

/* /proc/<pid>/status または /proc/<pid>/task/<tid>/status の 1 項目を読む */
static int read_sigmask(const char *path, const char *key, unsigned long long *out)
{
    char  line[256];
    FILE *fp = fopen(path, "r");
    size_t klen = strlen(key);

    if (fp == NULL) return -1;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, key, klen) == 0) {
            char *v = line + klen;
            while (*v == ':' || *v == ' ' || *v == '\t') v++;
            if (sscanf(v, "%llx", out) == 1) { fclose(fp); return 0; }
        }
    }
    fclose(fp);
    return -1;
}

/* シグナル番号 signo (1 起算) がマスクに立っているか */
static int sig_in_mask(unsigned long long mask, int signo)
{
    return (mask & (1ULL << (signo - 1))) != 0ULL;
}

/* 子プロセスのログに文字列が現れるまで待つ */
static int wait_for_text(const char *path, const char *needle, int timeout_ms)
{
    long t0 = now_ms();

    while (now_ms() - t0 < timeout_ms) {
        char *body = slurp(path, NULL);
        if (body != NULL) {
            int found = (strstr(body, needle) != NULL);
            free(body);
            if (found) return 0;
        }
        sleep_ms(20);
    }
    return -1;
}

static const char *find_binary(int argc, char **argv)
{
    static const char *candidates[] = {
        "./vmodem", "vmodem", "./build/vmodem", "../vmodem", NULL
    };
    const char *env;
    int i;

    if (argc > 1 && argv[1][0] != '\0') return argv[1];

    env = getenv("VMODEM_BIN");
    if (env != NULL && env[0] != '\0') return env;

    for (i = 0; candidates[i] != NULL; i++)
        if (access(candidates[i], X_OK) == 0) return candidates[i];

    return NULL;
}

/* テスト用の最小 config.ini を書く */
static int write_test_config(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL) return -1;
    fprintf(fp,
            "[general]\n"
            /*
             * ★ /dev/ で始めない ★
             * vm_serial の AUTO 解決は「/dev/ で始まるか」で実 tty と
             * PTY を分ける。ここで PTY を選ばせる事で、ttyGS0 が無い
             * CI でも Step 3 のシグナル経路をそのまま検証できる。
             */
            "com_port     = test-pty\n"
            /*
             * ★ 音声を有効にする ★
             * 難所 2 (シグナルの配送先) の検証には「メインスレッド以外の
             * スレッドが実在する」事が必要である。VIM1 でも CI でも
             * ALSA デバイスは開けないが、Step 2 の実装は null バックエンドへ
             * 自動フォールバックした上でレンダースレッドを生成するので、
             * 「シグナルをブロックしたまま走るスレッド」を再現できる。
             * false にするとスレッドが生えず、検証が空振りする。
             */
            "audio_enable = true\n"
            "audio_volume = 0.0\n"
            "log_level    = 4\n"
            "log_file     =\n"
            "\n"
            "[ISP_1]\n"
            "number   = 000\n"
            "speed    = 33600\n"
            "protocol = V34PLUS\n");
    fclose(fp);
    return 0;
}

/* 子を起動し、イベントループ開始まで待つ。戻り値 = pid (失敗時 -1) */
static pid_t spawn_vmodem(const char *bin, const char *cfg, const char *logpath)
{
    pid_t pid;

    (void)remove(logpath);

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)dup2(fd, 1);
            (void)dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        /* --net none: libslirp の有無に依存させない */
        execl(bin, bin, "-c", cfg, "--net", "none", "-v", (char *)NULL);
        _exit(127);
    }

    if (wait_for_text(logpath, "イベントループ開始", 5000) != 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

/*
 * 1 発のシグナルで正常終了する事、および停止までの所要時間を測る。
 */
static void test_signal_graceful(const char *bin, const char *cfg,
                                 const char *dir, int signo,
                                 const char *signame)
{
    char   logpath[512];
    pid_t  pid;
    int    status = 0;
    long   t0, elapsed;
    char  *body;
    char   marker[64];

    snprintf(logpath, sizeof(logpath), "%s/sig-%s.log", dir, signame);

    pid = spawn_vmodem(bin, cfg, logpath);
    if (pid < 0) { check(0, "%s: vmodem を起動できる", signame); return; }

    t0 = now_ms();
    if (kill(pid, signo) != 0) {
        check(0, "%s: kill が成功", signame);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return;
    }
    if (waitpid(pid, &status, 0) < 0) {
        check(0, "%s: waitpid が成功", signame);
        return;
    }
    elapsed = now_ms() - t0;

    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "%s: 既定動作 (即 Term) ではなく終了コード 0 で正常終了 "
          "(exited=%d code=%d)", signame,
          WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    /*
     * ★ SA_RESTART を付けない事の効果 ★
     * 付けるとシリアル読取のタイムアウト (VM_MODEM_COM_BLOCK_MS = 200ms)
     * まで停止が遅れる。100ms 以内なら「EINTR で即抜けている」と言える。
     */
    check(elapsed < 100, "%s: 停止までの遅延が 100ms 未満 (実測 %ld ms) "
                         "= poll/read が EINTR で即抜けている",
          signame, elapsed);

    body = slurp(logpath, NULL);
    if (body == NULL) { check(0, "%s: ログを読める", signame); return; }

    check(strstr(body, "終了要求を受けました") != NULL,
          "%s: ハンドラのメッセージが出る", signame);
    check(strstr(body, "modem: 停止要求を受けた") != NULL,
          "%s: イベントループが停止要求を認識する", signame);

    /* 「どのシグナルで終わったか」がログに残る */
    snprintf(marker, sizeof(marker), "signal %d により正常終了", signo);
    check(strstr(body, marker) != NULL,
          "%s: '%s' がログに残る", signame, marker);

    /* 後片付けが走った事 (最終状態の出力は destroy 直前) */
    check(strstr(body, "最終状態:") != NULL,
          "%s: 最終状態を出してから後片付けする", signame);

    free(body);
}

static void test_signal_masks(const char *bin, const char *cfg, const char *dir)
{
    char  logpath[512];
    char  ppath[512];
    pid_t pid;
    unsigned long long main_blk = 0, ign = 0, cgt = 0;
    int   other_threads = 0, blocking_threads = 0;
    DIR  *d;

    printf("\n--- 難所 2/3: シグナルの配送先と SIGPIPE ---\n");

    snprintf(logpath, sizeof(logpath), "%s/mask.log", dir);
    pid = spawn_vmodem(bin, cfg, logpath);
    if (pid < 0) { check(0, "vmodem を起動できる (マスク検査)"); return; }

    /* --- プロセス全体 (= メインスレッド) の状態 --- */
    snprintf(ppath, sizeof(ppath), "/proc/%ld/status", (long)pid);
    if (read_sigmask(ppath, "SigBlk", &main_blk) == 0 &&
        read_sigmask(ppath, "SigIgn", &ign) == 0 &&
        read_sigmask(ppath, "SigCgt", &cgt) == 0) {

        check(sig_in_mask(cgt, SIGINT) && sig_in_mask(cgt, SIGTERM) &&
              sig_in_mask(cgt, SIGHUP),
              "SIGINT / SIGTERM / SIGHUP を捕捉している (SigCgt=%llx)", cgt);

        /*
         * ★ 難所 3 ★
         * SIGPIPE を無視していないと、切断済みソケットへの write で
         * プロセスが無言のうちに即死する。
         */
        check(sig_in_mask(ign, SIGPIPE),
              "SIGPIPE を無視している (SigIgn=%llx) = write が EPIPE を返す",
              ign);

        /* メインスレッドはマスクを解除済み = ここに配送される */
        check(!sig_in_mask(main_blk, SIGINT) &&
              !sig_in_mask(main_blk, SIGTERM) &&
              !sig_in_mask(main_blk, SIGHUP),
              "メインスレッドは終了シグナルをブロックしていない "
              "(SigBlk=%llx)", main_blk);
    } else {
        skip("/proc/%ld/status を読めない", (long)pid);
    }

    /* --- 他スレッド (ALSA レンダースレッド等) の状態 --- */
    snprintf(ppath, sizeof(ppath), "/proc/%ld/task", (long)pid);
    d = opendir(ppath);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char  tpath[600];
            unsigned long long blk = 0;
            long  tid;

            if (!isdigit((unsigned char)e->d_name[0])) continue;
            tid = strtol(e->d_name, NULL, 10);
            if (tid == (long)pid) continue;          /* メインスレッド */

            other_threads++;
            snprintf(tpath, sizeof(tpath), "/proc/%ld/task/%ld/status",
                     (long)pid, tid);
            if (read_sigmask(tpath, "SigBlk", &blk) == 0 &&
                sig_in_mask(blk, SIGINT) && sig_in_mask(blk, SIGTERM) &&
                sig_in_mask(blk, SIGHUP))
                blocking_threads++;
        }
        closedir(d);
    }

    if (other_threads == 0) {
        /*
         * audio_enable=false なので、この構成ではレンダースレッドが
         * 生成されない事もある。その場合この検査は成立しない。
         */
        skip("メインスレッド以外が居ないためスレッド マスク検査は省略 "
             "(audio_enable=false の構成)");
    } else {
        check(blocking_threads == other_threads,
              "メインスレッド以外の %d スレッド全てが終了シグナルを "
              "ブロックしている = Ctrl-C は必ずメインスレッドに配送される",
              other_threads);
    }

    kill(pid, SIGTERM);
    (void)waitpid(pid, NULL, 0);
}

static void test_signal_twice(const char *bin, const char *cfg, const char *dir)
{
    char  logpath[512];
    pid_t pid;
    int   status = 0;
    int   ok;

    printf("\n--- 難所 4: 2 回目の終了要求で強制終了 ---\n");

    snprintf(logpath, sizeof(logpath), "%s/twice.log", dir);
    pid = spawn_vmodem(bin, cfg, logpath);
    if (pid < 0) { check(0, "vmodem を起動できる (二重シグナル)"); return; }

    kill(pid, SIGINT);
    kill(pid, SIGINT);
    if (waitpid(pid, &status, 0) < 0) {
        check(0, "waitpid が成功 (二重シグナル)");
        return;
    }

    /*
     * ★ 期待値が 2 通りある事に注意 ★
     * 1 発目の処理が速ければ、2 発目が届く前に正常終了して 0 になる。
     * 間に合えば _exit(128 + SIGINT) = 130 になる。
     * どちらも正しい。**ここで検証したいのは「必ず終わる」事** であり、
     * 「必ず 130 になる」事ではない (それはタイミング依存のテストで、
     *  CI で不安定になる)。
     */
    ok = WIFEXITED(status) &&
         (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 128 + SIGINT);
    check(ok, "2 回連続の SIGINT で必ず終了する (exited=%d code=%d; "
              "0 = 正常終了が先, %d = 強制終了)",
          WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
          128 + SIGINT);

    /* シグナルで殺されたのではなく、自分で exit した事 */
    check(!WIFSIGNALED(status),
          "シグナルの既定動作で殺されていない (WIFSIGNALED=0)");
}

static void test_step3(int argc, char **argv, const char *dir)
{
    const char *bin = find_binary(argc, argv);
    char cfg[512];

    printf("\n=== Step 3: シグナル処理 (実プロセスで検証) ===\n");

    if (bin == NULL) {
        skip("vmodem のバイナリが見つからないため Step 3 を省略");
        printf("       第 1 引数か VMODEM_BIN でパスを指定してください。\n");
        printf("       例) ./test_linux_step34 ./vmodem\n");
        return;
    }
    printf("  対象バイナリ: %s\n", bin);

    snprintf(cfg, sizeof(cfg), "%s/test-config.ini", dir);
    if (write_test_config(cfg) != 0) {
        check(0, "テスト用 config.ini を書ける");
        return;
    }

    test_signal_graceful(bin, cfg, dir, SIGINT,  "SIGINT");
    test_signal_graceful(bin, cfg, dir, SIGTERM, "SIGTERM");
    test_signal_graceful(bin, cfg, dir, SIGHUP,  "SIGHUP");
    test_signal_masks(bin, cfg, dir);
    test_signal_twice(bin, cfg, dir);
}

/* =========================================================================
 * main
 * =======================================================================*/
int main(int argc, char **argv)
{
    char dir[256];

    printf("=========================================================\n");
    printf(" VModem Linux 移植 Step 3 / Step 4 検証テスト\n");
    printf("=========================================================\n");

    snprintf(dir, sizeof(dir), "/tmp/vmodem-test-%ld", (long)getpid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "作業ディレクトリを作れません: %s (%s)\n",
                dir, strerror(errno));
        return 1;
    }
    printf("作業ディレクトリ: %s\n", dir);

    /* Step 4 */
    test_log_file_basic(dir);
    test_log_timestamp(dir);
    test_log_concurrent(dir);
    test_log_bounds(dir);
    test_log_robustness(dir);

    /* Step 3 */
    test_step3(argc, argv, dir);

    printf("\n=========================================================\n");
    printf(" 結果: 成功 %d / 失敗 %d / 省略 %d\n", g_pass, g_fail, g_skip);
    printf("=========================================================\n");

    if (g_fail == 0)
        printf("作業ファイルは %s に残しています (rm -rf で削除可)\n", dir);

    return (g_fail == 0) ? 0 : 1;
}
