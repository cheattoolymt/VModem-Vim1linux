/*
 * test_linux_step78.c - Linux 移植 Step 7 / 8 / 8-a の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象:
 *   Step 7   : Makefile.linux
 *   Step 8   : scripts/setup-gadget-linux.sh
 *              scripts/99-vmodem.rules
 *              scripts/vmodem.service
 *   Step 8-a : windows/vim1modem.inf
 *
 * ===========================================================================
 * 使い方
 * ===========================================================================
 *   gcc -O2 -std=c99 -Wall -Wextra -Iinclude -D_POSIX_C_SOURCE=200809L \
 *       tests/test_linux_step78.c \
 *       src/core/vm_config.c src/core/vm_log.c \
 *       src/core/vm_ringbuf.c src/core/vm_types.c \
 *       -lm -lpthread -o test_linux_step78
 *   ./test_linux_step78            # リポジトリのルートで実行する事
 *
 *   Makefile 経由なら:
 *       make -f Makefile.linux check
 *
 * ===========================================================================
 * 設計方針
 * ===========================================================================
 * Step 7/8 の成果物はビルドスクリプトと設定ファイルであり、C の関数を
 * 呼んで確かめられる物が無い。かつ本物の検証には
 *   ・VIM1 の実機 (UDC c9100000.usb)
 *   ・Windows XP 機
 * が要るので、CI や開発機では原理的に完走できない。
 *
 * そこで「開発機でも必ず判定できる事」に絞る。
 *
 *   (1) 成果物が存在し、指示書が要求した要素を含んでいる事
 *       -> テキストとして読んで検査する。
 *   (2) 相互参照が壊れていない事
 *       -> Makefile の install が参照するファイルが実在するか、
 *          udev rules が起こす unit が実在するか、等。
 *          ここが食い違うと実機で初めて転ぶので、静的に潰す価値が高い。
 *   (3) シェルスクリプトのロジック
 *       -> setup-gadget-linux.sh は VM_TEST_ROOT で疑似 configfs 上を
 *          走れるようにしてあるので、**実際に起動して**
 *          べき等性と teardown の順序を確かめる。
 *
 * 実機依存の項目 (USB が本当に列挙されるか、XP が COM を出すか) は
 * assert せず SKIP にし、何を人手で確かめるべきかを表示する。
 * ===========================================================================
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ==========================================================================
 * 共通ヘルパ (test_linux_step5.c / step6.c と同じ書式に揃える)
 * ========================================================================== */
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

static void check(int ok, const char *fmt, ...)
{
    char    buf[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (ok) { g_pass++; printf("  [ OK ] %s\n", buf); }
    else    { g_fail++; printf("  [FAIL] %s\n", buf); }
}

static void skip(const char *fmt, ...)
{
    char    buf[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_skip++;
    printf("  [SKIP] %s\n", buf);
}

static void note(const char *fmt, ...)
{
    char    buf[400];
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

/* ==========================================================================
 * ファイル読み込み
 * ========================================================================== */
#define MAX_FILE (512 * 1024)

/* ファイル全体を malloc したバッファに読む。失敗時 NULL。 */
static char *slurp(const char *path, size_t *out_len)
{
    FILE   *fp;
    char   *buf;
    size_t  n;

    if (out_len) *out_len = 0;

    fp = fopen(path, "rb");
    if (!fp) return NULL;

    buf = (char *)malloc(MAX_FILE + 1);
    if (!buf) { fclose(fp); return NULL; }

    n = fread(buf, 1, MAX_FILE, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* 実行ビットが立っているか (どれか 1 つでも) */
static int is_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

/* haystack に needle が含まれるか (大文字小文字を区別しない) */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nl) == 0) return 1;
    }
    return 0;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/*
 * 「コメント行以外に」needle が現れるか。
 *
 * これが必要な理由:
 *   本リポジトリの成果物は解説コメントが非常に多く、
 *   「# UDC: c9100000.usb と指示書にある」のような *説明* だけで
 *   contains() が真になってしまう。それでは
 *   「実際に設定しているか」の検証にならない。
 *   comment_char で始まる行 (先頭の空白は無視) を除いて探す。
 */
static int contains_in_code(const char *hay, const char *needle,
                            char comment_char)
{
    const char *line = hay;
    size_t      nl   = strlen(needle);

    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t      len = eol ? (size_t)(eol - line) : strlen(line);
        const char *p   = line;
        size_t      i;

        /* 先頭の空白を飛ばす */
        while ((size_t)(p - line) < len && (*p == ' ' || *p == '\t')) p++;

        if ((size_t)(p - line) < len && *p != comment_char) {
            for (i = 0; i + nl <= len; i++) {
                if (strncmp(line + i, needle, nl) == 0) return 1;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return 0;
}

/* system() を実行し終了ステータスを返す。実行できなければ -1。 */
static int run(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (!WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

/* ==========================================================================
 * Step 7: Makefile.linux
 * ==========================================================================
 * 指示書の骨組み:
 *   CC = gcc
 *   CFLAGS = -O2 -std=c99 -Wall -Iinclude -DVMODEM_LINUX -DVMODEM_HAVE_LIBSLIRP
 *   LDFLAGS = -lm -lslirp -lpthread -lasound -ldl
 *   SRCS = (25 ファイル)
 *   TARGET = vmodem
 *   clean:
 * ========================================================================== */

/* 指示書が列挙した 25 ファイル。ここは指示書の写しであり、
 * 1 つでも欠けたら「移植対象の取りこぼし」を意味する。 */
static const char *g_expected_srcs[] = {
    "src/main.c",
    "src/core/vm_config.c",
    "src/core/vm_log.c",
    "src/core/vm_ringbuf.c",
    "src/core/vm_types.c",
    "src/dsp/vm_handshake.c",
    "src/dsp/vm_resample.c",
    "src/dsp/vm_tone.c",
    "src/dsp/vm_v34.c",
    "src/dsp/vm_v8.c",
    "src/audio/vm_audio.c",
    "src/audio/vm_audio_alsa.c",
    "src/audio/vm_audio_null.c",
    "src/modem/vm_at.c",
    "src/modem/vm_modem.c",
    "src/modem/vm_sequence.c",
    "src/net/vm_eth.c",
    "src/net/vm_hdlc.c",
    "src/net/vm_hostroute_linux.c",
    "src/net/vm_nat.c",
    "src/net/vm_nat_loopback.c",
    "src/net/vm_nat_slirp.c",
    "src/net/vm_ppp.c",
    "src/serial/vm_serial.c",
    "src/serial/vm_serial_linux.c"
};
#define N_EXPECTED_SRCS \
    ((int)(sizeof(g_expected_srcs) / sizeof(g_expected_srcs[0])))

static void test_step7_makefile(void)
{
    char  *mk;
    size_t len;
    int    i, missing = 0, not_on_disk = 0;

    head("Step 7: Makefile.linux の内容");

    if (!file_exists("Makefile.linux")) {
        check(0, "Makefile.linux が存在しない (Step 7 の成果物)");
        note("リポジトリのルートで実行していますか?");
        return;
    }
    check(1, "Makefile.linux が存在する");

    mk = slurp("Makefile.linux", &len);
    if (!mk) {
        check(0, "Makefile.linux を読めない: %s", strerror(errno));
        return;
    }
    note("%zu バイト", len);

    /* --- 必須のフラグ --- */
    check(contains(mk, "-std=c99"),   "-std=c99 を指定している");
    check(contains(mk, "-Wall"),      "-Wall を指定している");
    check(contains(mk, "-Iinclude"),  "-Iinclude を指定している");
    check(contains(mk, "-DVMODEM_LINUX"),
          "-DVMODEM_LINUX を定義している (指示書の要求)");
    check(contains(mk, "-DVMODEM_HAVE_LIBSLIRP"),
          "-DVMODEM_HAVE_LIBSLIRP を定義している");

    /* --- 必須のライブラリ --- */
    check(contains(mk, "-lm"),       "-lm をリンクする");
    check(contains(mk, "-lslirp"),   "-lslirp をリンクする");
    check(contains(mk, "-lpthread"), "-lpthread をリンクする");
    check(contains(mk, "-lasound"),  "-lasound をリンクする (ALSA)");
    check(contains(mk, "-ldl"),      "-ldl をリンクする (dlsym 用)");

    /*
     * -lutil は指示書には無いが必要。
     * src/serial/vm_serial.c が openpty(3) を呼ぶ。
     * glibc 2.34 以降は libc に統合済みなので手元では通るが、
     * それ未満の glibc や musl では undefined reference になる。
     */
    check(contains(mk, "-lutil"),
          "-lutil をリンクする (openpty; 指示書に無いが古い glibc で必要)");

    /* --- ターゲット --- */
    check(contains(mk, "vmodem"), "TARGET が vmodem");
    check(contains_in_code(mk, "clean", '#'), "clean ターゲットがある");

    /* --- SRCS の 25 ファイル --- */
    for (i = 0; i < N_EXPECTED_SRCS; i++) {
        if (!contains(mk, g_expected_srcs[i])) {
            check(0, "SRCS に %s が無い", g_expected_srcs[i]);
            missing++;
        }
        /* Makefile に書いてあってもファイルが無ければビルドできない */
        if (!file_exists(g_expected_srcs[i])) {
            note("実ファイルが見つからない: %s", g_expected_srcs[i]);
            not_on_disk++;
        }
    }
    check(missing == 0, "指示書の %d ファイルすべてが SRCS にある",
          N_EXPECTED_SRCS);
    check(not_on_disk == 0, "SRCS の全ファイルが実際に存在する");

    /*
     * Windows 専用ファイルが混ざっていない事。
     * vm_winsock.c / vm_audio_wasapi.c は Linux ビルドから
     * 外れているのが正しい。
     */
    check(!contains_in_code(mk, "src/net/vm_winsock.c", '#'),
          "Windows 専用の vm_winsock.c を SRCS に含まない");
    check(!contains_in_code(mk, "vm_audio_wasapi.c", '#'),
          "Windows 専用の vm_audio_wasapi.c を SRCS に含まない");

    free(mk);
}

/*
 * SRCS の定義範囲に vm_hostroute.c (Windows 版) が
 * 「_linux 抜きで」入っていないかを見る。
 *
 * これを別関数にした理由:
 *   単純な contains() は vm_hostroute_linux.c に誤反応する。
 *   また DEPS_test_linux_step5 では **意図的に**
 *   vm_hostroute.c と vm_hostroute_linux.c を両方リンクする
 *   (Step 5-3 の重複定義検証そのもの) ので、
 *   ファイル全体を見ると必ず引っかかる。
 *   よって SRCS = ... の継続行の範囲だけを切り出して調べる。
 */
static void test_step7_no_dup_hostroute(void)
{
    char       *mk;
    size_t      len;
    const char *srcs;
    const char *end;
    size_t      span;
    const char *p;
    int         dup = 0;

    head("Step 7: SRCS に Windows 版 vm_hostroute.c が混ざっていない");

    if (!file_exists("Makefile.linux")) {
        skip("Makefile.linux が無いので省略");
        return;
    }
    mk = slurp("Makefile.linux", &len);
    if (!mk) { skip("Makefile.linux を読めない"); return; }

    srcs = strstr(mk, "\nSRCS");
    if (!srcs) {
        check(0, "SRCS 変数が見つからない");
        free(mk);
        return;
    }
    /* SRCS の継続行は次の空行までとみなす */
    end  = strstr(srcs + 1, "\n\n");
    span = end ? (size_t)(end - srcs) : strlen(srcs);

    p = srcs;
    while ((p = strstr(p, "vm_hostroute.c")) != NULL) {
        if ((size_t)(p - srcs) >= span) break;   /* SRCS の範囲外 */
        /* 直前が "_linux" でなければ Windows 版 */
        if (p < srcs + 6 || strncmp(p - 6, "_linux", 6) != 0) {
            dup = 1;
        }
        p += 1;
    }

    check(!dup,
          "SRCS は vm_hostroute_linux.c のみ "
          "-> vm_hostroute.c との重複定義が起きない");
    note("(DEPS_test_linux_step5 は Step 5-3 の検証で意図的に両方リンクする)");

    free(mk);
}

/* ==========================================================================
 * Step 8: setup-gadget-linux.sh の内容
 * ========================================================================== */
static void test_step8_gadget_script(void)
{
    char  *sh;
    size_t len;

    head("Step 8: scripts/setup-gadget-linux.sh の内容");

    if (!file_exists("scripts/setup-gadget-linux.sh")) {
        check(0, "scripts/setup-gadget-linux.sh が存在しない");
        return;
    }
    check(1, "scripts/setup-gadget-linux.sh が存在する");
    check(is_executable("scripts/setup-gadget-linux.sh"),
          "実行ビットが立っている");

    sh = slurp("scripts/setup-gadget-linux.sh", &len);
    if (!sh) {
        check(0, "読めない: %s", strerror(errno));
        return;
    }
    note("%zu バイト", len);

    /* --- 指示書の必須要素 --- */
    check(contains_in_code(sh, "/sys/kernel/config", '#'),
          "configfs (/sys/kernel/config) を使う");
    check(contains_in_code(sh, "usb_gadget", '#'),
          "usb_gadget 配下を操作する");
    check(contains_in_code(sh, "c9100000.usb", '#'),
          "UDC の既定値が c9100000.usb (VIM1 固有)");
    check(contains_in_code(sh, "0x1209", '#'), "VID が 0x1209 (pid.codes)");
    check(contains_in_code(sh, "0x0001", '#'), "PID が 0x0001");
    check(contains_in_code(sh, "acm", '#'),
          "CDC-ACM 機能 (acm) を設定する");
    check(contains_in_code(sh, "UDC", '#'), "UDC へ bind する");

    /*
     * べき等性。
     * 「既に設定済みなら再設定しない」は指示書の明示要求。
     * 実際の挙動は後段の test_step8_gadget_behavior() で走らせて確かめる。
     */
    check(contains_ci(sh, "べき等") || contains_ci(sh, "idempot"),
          "べき等である事が明記されている");

    /*
     * Windows で COM ポートとして見えるための IAD 宣言。
     * bDeviceClass=0xEF / SubClass=0x02 / Protocol=0x01 が無いと
     * Windows は CDC の 2 本組を認識できず「不明なデバイス」になる。
     * Linux ホストは IAD 無しでも解釈するため気づきにくい罠。
     */
    check(contains_in_code(sh, "bDeviceClass", '#'),
          "bDeviceClass を設定する (Windows の IAD 認識に必須)");
    check(contains_in_code(sh, "0xef", '#') ||
          contains_in_code(sh, "0xEF", '#'),
          "bDeviceClass = 0xef (Miscellaneous)");
    check(contains_in_code(sh, "bDeviceSubClass", '#'),
          "bDeviceSubClass を設定する");
    check(contains_in_code(sh, "bDeviceProtocol", '#'),
          "bDeviceProtocol を設定する");

    /* 文字列ディスクリプタ (en-US) */
    check(contains_in_code(sh, "0x409", '#'),
          "文字列ディスクリプタの言語 0x409 を設定する");

    /*
     * Test PID の警告。
     * pid.codes の 1209:0001 は社内試験専用で、配布物に載せてはいけない。
     * スクリプトが黙って使うと利用者が気づけないので警告が要る。
     */
    check(contains_ci(sh, "Test PID"),
          "1209:0001 が Test PID である事を警告する");

    free(sh);
}

/* ==========================================================================
 * Step 8: setup-gadget-linux.sh を実際に走らせる
 * ==========================================================================
 * VM_TEST_ROOT で疑似 configfs を指すと、configfs も UDC も無い
 * 開発機でロジックを走らせられる。ここでは
 *   1) 1 回目の setup が成功する
 *   2) 2 回目の setup が「変更しない」で成功する = べき等
 *   3) teardown で完全に消える
 *   4) teardown 後に再度 setup できる
 *   5) UDC が無い環境では黙って成功せず失敗する
 * を確かめる。
 *
 * ★ これは実機での USB 列挙の検証にはならない ★
 *   あくまでスクリプトのロジックの検証である。
 * ========================================================================== */
static void test_step8_gadget_behavior(void)
{
    const char *root = "/tmp/vmodem_gadget_test";
    char        cmd[1200];
    int         rc;

    head("Step 8: setup-gadget-linux.sh の挙動 (疑似 configfs)");

    if (!file_exists("scripts/setup-gadget-linux.sh")) {
        skip("スクリプトが無いので実行検証を省略");
        return;
    }

    /* bash が無い環境ではロジック検証を諦める */
    if (run("command -v bash >/dev/null 2>&1") != 0) {
        skip("bash が見つからないので実行検証を省略");
        return;
    }

    /* --- 構文チェック --- */
    check(run("bash -n scripts/setup-gadget-linux.sh 2>/dev/null") == 0,
          "bash -n の構文チェックを通る");

    /* 疑似ルートを作る。UDC も VIM1 の名前で置く。 */
    snprintf(cmd, sizeof(cmd),
             "rm -rf %s && mkdir -p %s/sys/class/udc/c9100000.usb %s/dev",
             root, root, root);
    if (run(cmd) != 0) {
        skip("疑似ルートを作れなかった (%s)", root);
        return;
    }

    /* --- 1 回目 --- */
    snprintf(cmd, sizeof(cmd),
             "VM_TEST_ROOT=%s bash scripts/setup-gadget-linux.sh "
             ">%s/out1.txt 2>&1", root, root);
    rc = run(cmd);
    check(rc == 0, "1 回目の setup が成功する (rc=%d)", rc);

    /* ガジェットのツリーが出来ているか */
    snprintf(cmd, sizeof(cmd),
             "test -f %s/sys/kernel/config/usb_gadget/vmodem/UDC", root);
    check(run(cmd) == 0, "gadget/UDC が作られた");

    snprintf(cmd, sizeof(cmd),
             "test -L %s/sys/kernel/config/usb_gadget/vmodem"
             "/configs/c.1/acm.usb0", root);
    check(run(cmd) == 0,
          "configs/c.1/acm.usb0 が functions への symlink になっている");

    /* UDC に bind 済みか = UDC ファイルが空でない */
    snprintf(cmd, sizeof(cmd),
             "test -s %s/sys/kernel/config/usb_gadget/vmodem/UDC", root);
    check(run(cmd) == 0, "UDC へ bind された (ファイルが空でない)");

    /* IAD 用のクラス値が実際に書かれたか */
    snprintf(cmd, sizeof(cmd),
             "grep -q 0xef %s/sys/kernel/config/usb_gadget/vmodem"
             "/bDeviceClass", root);
    check(run(cmd) == 0, "bDeviceClass に 0xef が書かれた");

    snprintf(cmd, sizeof(cmd),
             "grep -q 0x1209 %s/sys/kernel/config/usb_gadget/vmodem"
             "/idVendor", root);
    check(run(cmd) == 0, "idVendor に 0x1209 が書かれた");

    /* --- 2 回目: べき等 --- */
    /* ツリーのスナップショットを取り、2 回目の後で差分を見る。
     * 出力ファイル (out*.txt / snap*.txt) は除外する。 */
    snprintf(cmd, sizeof(cmd),
             "find %s -printf '%%P %%y\\n' 2>/dev/null "
             "| grep -v -e snap -e 'out[0-9]' | sort > /tmp/vm_snap1.txt",
             root);
    (void)run(cmd);

    snprintf(cmd, sizeof(cmd),
             "VM_TEST_ROOT=%s bash scripts/setup-gadget-linux.sh "
             ">%s/out2.txt 2>&1", root, root);
    rc = run(cmd);
    check(rc == 0, "2 回目の setup も成功する (rc=%d)", rc);

    snprintf(cmd, sizeof(cmd),
             "find %s -printf '%%P %%y\\n' 2>/dev/null "
             "| grep -v -e snap -e 'out[0-9]' | sort > /tmp/vm_snap2.txt",
             root);
    (void)run(cmd);
    check(run("diff -q /tmp/vm_snap1.txt /tmp/vm_snap2.txt >/dev/null") == 0,
          "2 回目でツリーが全く変化しない -> べき等 (指示書の要求)");

    /* 2 回目の出力に「変更しない」旨が出ているか */
    snprintf(cmd, sizeof(cmd),
             "grep -q -e 'bind 済み' -e 'べき等' %s/out2.txt", root);
    check(run(cmd) == 0, "2 回目は既設定を検出して再設定しないと報告する");

    /* --- teardown --- */
    snprintf(cmd, sizeof(cmd),
             "VM_TEST_ROOT=%s bash scripts/setup-gadget-linux.sh teardown "
             ">%s/out3.txt 2>&1", root, root);
    rc = run(cmd);
    check(rc == 0, "teardown が成功する (rc=%d)", rc);

    snprintf(cmd, sizeof(cmd),
             "test ! -d %s/sys/kernel/config/usb_gadget/vmodem", root);
    check(run(cmd) == 0, "teardown でガジェットが完全に消える");

    /* --- teardown をもう一度 (べき等) --- */
    snprintf(cmd, sizeof(cmd),
             "VM_TEST_ROOT=%s bash scripts/setup-gadget-linux.sh teardown "
             ">%s/out4.txt 2>&1", root, root);
    check(run(cmd) == 0, "存在しない物への teardown も成功する (べき等)");

    /* --- teardown 後に再構築できるか --- */
    snprintf(cmd, sizeof(cmd),
             "VM_TEST_ROOT=%s bash scripts/setup-gadget-linux.sh "
             ">%s/out5.txt 2>&1", root, root);
    check(run(cmd) == 0, "teardown 後に再度 setup できる");

    /* --- UDC が無い環境では明確に失敗するか --- */
    snprintf(cmd, sizeof(cmd),
             "rm -rf %s/nodc && mkdir -p %s/nodc/sys/class/udc %s/nodc/dev "
             "&& VM_TEST_ROOT=%s/nodc bash scripts/setup-gadget-linux.sh "
             ">%s/out6.txt 2>&1", root, root, root, root, root);
    rc = run(cmd);
    check(rc != 0,
          "UDC が 1 つも無い環境では失敗する (黙って成功しない, rc=%d)", rc);

    /* --- 指定と違う UDC 名でも自動で拾うか --- */
    snprintf(cmd, sizeof(cmd),
             "rm -rf %s/alt && mkdir -p %s/alt/sys/class/udc/fe200000.usb "
             "%s/alt/dev "
             "&& VM_TEST_ROOT=%s/alt bash scripts/setup-gadget-linux.sh "
             ">%s/out7.txt 2>&1", root, root, root, root, root);
    rc = run(cmd);
    check(rc == 0,
          "UDC 名が c9100000.usb でなくても実在する物を使う (rc=%d)", rc);
    snprintf(cmd, sizeof(cmd),
             "grep -q fe200000.usb "
             "%s/alt/sys/kernel/config/usb_gadget/vmodem/UDC", root);
    check(run(cmd) == 0, "その UDC 名が実際に書き込まれている");

    /* --- VM_PID を上書きすると Test PID 警告が消えるか --- */
    snprintf(cmd, sizeof(cmd),
             "rm -rf %s/own && mkdir -p %s/own/sys/class/udc/c9100000.usb "
             "%s/own/dev "
             "&& VM_TEST_ROOT=%s/own VM_PID=0x9999 "
             "bash scripts/setup-gadget-linux.sh >%s/out8.txt 2>&1",
             root, root, root, root, root);
    (void)run(cmd);
    snprintf(cmd, sizeof(cmd), "grep -q 'Test PID' %s/out8.txt", root);
    check(run(cmd) != 0,
          "自前の PID を指定した時は Test PID 警告を出さない");

    /* 後片付け */
    snprintf(cmd, sizeof(cmd), "rm -rf %s /tmp/vm_snap1.txt /tmp/vm_snap2.txt",
             root);
    (void)run(cmd);

    note("");
    note("※ 疑似 configfs はロジックの検証。実機では別途:");
    note("   sudo bash scripts/setup-gadget-linux.sh status");
}

/* ==========================================================================
 * Step 8: udev ルールと systemd unit
 * ==========================================================================
 * ★ 指示書の例をそのまま実装しなかった箇所 ★
 *
 *   指示書 Step 8 の例:
 *     ACTION=="add", KERNEL=="ttyGS0", RUN+="/usr/local/bin/vmodem-start.sh"
 *
 *   systemd-udevd(8) は RUN で起動したプロセスとその子を
 *   「イベント処理の完了後に無条件に kill する」。
 *   したがって RUN+= で常駐デーモンは起こせない。
 *   起動直後に殺されるので、症状は
 *   「繋ぐと一瞬 vmodem が立ち上がってすぐ消える」になる。
 *
 *   常駐させる正しい方法は ENV{SYSTEMD_WANTS} で unit を引く事。
 *   ここではその形になっている事を検証する。
 * ========================================================================== */
static void test_step8_udev_systemd(void)
{
    char  *ru, *sv;
    size_t len;

    head("Step 8: udev ルール / systemd unit");

    /* ---------------- udev ルール ---------------- */
    if (!file_exists("scripts/99-vmodem.rules")) {
        check(0, "scripts/99-vmodem.rules が存在しない");
    } else {
        check(1, "scripts/99-vmodem.rules が存在する");
        ru = slurp("scripts/99-vmodem.rules", &len);
        if (!ru) {
            check(0, "99-vmodem.rules を読めない");
        } else {
            check(contains_in_code(ru, "ttyGS0", '#'),
                  "ttyGS0 を対象にしている");
            check(contains_in_code(ru, "ACTION==\"add\"", '#'),
                  "add イベントで発火する");

            /* RUN+= で常駐を起こしていない事 */
            check(!contains_in_code(ru, "RUN+=", '#'),
                  "RUN+= を使っていない "
                  "(udev は RUN の子プロセスを無条件に kill する)");

            /* 正しい方法を使っている事 */
            check(contains_in_code(ru, "SYSTEMD_WANTS", '#'),
                  "ENV{SYSTEMD_WANTS} で unit を起動する (常駐可能な方法)");
            check(contains_in_code(ru, "TAG+=\"systemd\"", '#'),
                  "TAG+=\"systemd\" を付けている (SYSTEMD_WANTS の前提)");

            /* 起こす unit 名が実在するファイルと一致するか */
            check(contains_in_code(ru, "vmodem.service", '#'),
                  "vmodem.service を起動する");
            check(file_exists("scripts/vmodem.service"),
                  "参照先の scripts/vmodem.service が実在する "
                  "(udev ルールと unit の食い違いが無い)");
            free(ru);
        }
    }

    /* ---------------- systemd unit ---------------- */
    if (!file_exists("scripts/vmodem.service")) {
        check(0, "scripts/vmodem.service が存在しない");
        return;
    }
    check(1, "scripts/vmodem.service が存在する");

    sv = slurp("scripts/vmodem.service", &len);
    if (!sv) {
        check(0, "vmodem.service を読めない");
        return;
    }

    check(contains(sv, "[Unit]"),    "[Unit] セクションがある");
    check(contains(sv, "[Service]"), "[Service] セクションがある");
    check(contains(sv, "[Install]"), "[Install] セクションがある");
    check(contains_in_code(sv, "ExecStart", '#'), "ExecStart がある");
    check(contains_in_code(sv, "/dev/ttyGS0", '#'),
          "ttyGS0 をポートとして渡す");
    check(contains_in_code(sv, "--net", '#'), "--net を指定している");

    /*
     * vmodem は SIGTERM で PPP を切ってから終了する
     * (Step 3 のシグナル経路)。KillSignal を書くなら SIGTERM である事。
     */
    check(!contains_in_code(sv, "KillSignal", '#') ||
           contains_in_code(sv, "SIGTERM", '#'),
          "KillSignal は SIGTERM (PPP を切ってから終了する経路)");

    /* デバイスが消えたら止まる構成になっているか */
    check(contains_in_code(sv, "dev-ttyGS0.device", '#'),
          "dev-ttyGS0.device と関連付けている");

    free(sv);
}

/* ==========================================================================
 * Step 8-a: windows/vim1modem.inf
 * ========================================================================== */
static void test_step8a_inf(void)
{
    char  *inf;
    size_t len, i;
    int    non_ascii = 0;

    head("Step 8-a: windows/vim1modem.inf の内容");

    if (!file_exists("windows/vim1modem.inf")) {
        check(0, "windows/vim1modem.inf が存在しない");
        return;
    }
    check(1, "windows/vim1modem.inf が存在する");

    inf = slurp("windows/vim1modem.inf", &len);
    if (!inf) {
        check(0, "読めない: %s", strerror(errno));
        return;
    }
    note("%zu バイト", len);

    /* --- INF の必須セクション --- */
    check(contains_ci(inf, "[Version]"),      "[Version] セクションがある");
    check(contains_ci(inf, "[Manufacturer]"), "[Manufacturer] セクションがある");
    check(contains_ci(inf, "[Strings]"),      "[Strings] セクションがある");
    check(contains_ci(inf, "Signature"),      "Signature がある");

    /*
     * Class=Ports / ClassGuid。
     * usbser.sys を COM ポートとして出すには Ports クラスに属する必要がある。
     * GUID は Windows 固定値。
     */
    check(contains_ci(inf, "Class=Ports") || contains_ci(inf, "Class = Ports"),
          "Class=Ports (COM ポートとして出す)");
    check(contains_ci(inf, "{4D36E978-E325-11CE-BFC1-08002BE10318}"),
          "Ports クラスの ClassGuid が正しい");

    /* --- ドライバ --- */
    check(contains_ci(inf, "usbser.sys"),
          "usbser.sys をロードする (指示書の要求)");
    check(contains_ci(inf, "mdmcpq.inf"),
          "mdmcpq.inf を include する (XP 標準の定型処理を借りる)");
    check(contains_ci(inf, "AddService"), "AddService でサービスを関連付ける");

    /* --- ハードウェア ID が gadget スクリプトの VID/PID と一致するか --- */
    check(contains_ci(inf, "VID_1209"), "ハードウェア ID の VID が 1209");
    check(contains_ci(inf, "PID_0001"), "ハードウェア ID の PID が 0001");

    /*
     * IAD 付き複合デバイスでは子デバイスの ID に &MI_00 が付く。
     * gadget 側が bDeviceClass=0xEF を宣言しているので
     * Windows は IAD を読み、&MI_00 で来る。
     * IAD 無しで列挙された場合の素の ID も併記しておくのが安全。
     */
    check(contains_ci(inf, "MI_00"),
          "&MI_00 付きの ID がある (IAD 複合デバイスの子として来る形)");

    /*
     * XP 対応。
     * DriverVer が新しすぎると OS 内蔵の usbser より優先されて
     * 意図しない方が当たる事がある。XP 時代の値にしておく。
     */
    check(contains_ci(inf, "DriverVer"), "DriverVer がある");
    check(contains_ci(inf, "DestinationDirs"), "DestinationDirs がある");

    /*
     * 未署名で配る前提なので CatalogFile を書かない。
     * 書いてあるのに .cat が無いと XP のインストールで失敗する。
     */
    if (file_exists("windows/vim1modem.cat")) {
        note(".cat が存在する -> CatalogFile を書く構成も検討できる");
    } else {
        check(!contains_in_code(inf, "CatalogFile", ';'),
              ".cat が無く CatalogFile も書いていない "
              "(整合している = XP では「続行」で入る)");
    }

    /*
     * INF は ASCII だけで書く。
     * XP の setupapi は ANSI として読むため、UTF-8 の日本語を入れると
     * コードページ依存で化ける (最悪パースに失敗する)。
     */
    for (i = 0; i < len; i++) {
        if ((unsigned char)inf[i] > 0x7f) non_ascii++;
    }
    check(non_ascii == 0,
          "全体が ASCII のみ (非 ASCII バイト %d 個) "
          "-> XP の ANSI 読みで化けない", non_ascii);

    /*
     * 改行は CRLF である事。
     * XP の setupapi は LF だけでも読めるが、INF は Windows の
     * テキストファイルなので CRLF が本来の形。
     * git の autocrlf 設定によっては勝手に変換される事もあるため、
     * 「LF 単独が混ざっていない」事を明示的に確かめる。
     */
    {
        int lone_lf = 0;
        for (i = 0; i < len; i++) {
            if (inf[i] == '\n' && (i == 0 || inf[i - 1] != '\r')) lone_lf++;
        }
        check(lone_lf == 0,
              "改行が全て CRLF (LF 単独 %d 個) -> Windows のテキスト形式",
              lone_lf);
    }

    free(inf);
}

/* ==========================================================================
 * 相互参照の整合性
 * ==========================================================================
 * Makefile の install / gadget-install が参照するファイルが実在するか。
 * ここが食い違うと「make install で初めて転ぶ」ので静的に潰す。
 * ========================================================================== */
static void test_cross_references(void)
{
    char  *mk;
    size_t len;

    head("整合性: 相互参照が壊れていない事");

    if (!file_exists("Makefile.linux")) {
        skip("Makefile.linux が無いので省略");
        return;
    }
    mk = slurp("Makefile.linux", &len);
    if (!mk) { skip("Makefile.linux を読めない"); return; }

    if (contains(mk, "scripts/setup-gadget-linux.sh")) {
        check(file_exists("scripts/setup-gadget-linux.sh"),
              "install が参照する scripts/setup-gadget-linux.sh が実在する");
    }
    if (contains(mk, "scripts/99-vmodem.rules")) {
        check(file_exists("scripts/99-vmodem.rules"),
              "gadget-install が参照する scripts/99-vmodem.rules が実在する");
    }
    if (contains(mk, "scripts/vmodem.service")) {
        check(file_exists("scripts/vmodem.service"),
              "gadget-install が参照する scripts/vmodem.service が実在する");
    }
    if (contains(mk, "config.ini")) {
        check(file_exists("config.ini"),
              "install が参照する config.ini が実在する");
    }

    /*
     * Makefile の TESTS に並んだテストのソースが実在するか。
     * 自分自身 (test_linux_step78) も含まれる。
     * 参照だけ書いて実体が無いと make tests が途中で止まる。
     */
    {
        static const char *tests[] = {
            "test_core", "test_dsp", "test_resample", "test_at",
            "test_sequence", "test_audio", "test_ppp", "test_nat",
            "test_linux_port", "test_linux_step34", "test_linux_step5",
            "test_linux_step6", "test_linux_step78"
        };
        int n = (int)(sizeof(tests) / sizeof(tests[0]));
        int i, miss = 0;
        for (i = 0; i < n; i++) {
            char path[256];
            snprintf(path, sizeof(path), "tests/%s.c", tests[i]);
            if (contains(mk, tests[i]) && !file_exists(path)) {
                note("Makefile が参照するのに存在しない: %s", path);
                miss++;
            }
        }
        check(miss == 0, "TESTS に並ぶテストのソースが全て実在する");
    }

    /*
     * gadget スクリプトの VID/PID と INF のハードウェア ID が
     * 一致しているか。片方だけ変えると Windows が認識しなくなる。
     */
    if (file_exists("scripts/setup-gadget-linux.sh") &&
        file_exists("windows/vim1modem.inf")) {
        char *sh  = slurp("scripts/setup-gadget-linux.sh", NULL);
        char *inf = slurp("windows/vim1modem.inf", NULL);
        if (sh && inf) {
            int sh_vid  = contains_in_code(sh, "0x1209", '#');
            int sh_pid  = contains_in_code(sh, "0x0001", '#');
            int inf_vid = contains_ci(inf, "VID_1209");
            int inf_pid = contains_ci(inf, "PID_0001");
            check(sh_vid == inf_vid && sh_pid == inf_pid,
                  "gadget スクリプトの VID/PID と INF の "
                  "ハードウェア ID が一致する");
        }
        free(sh);
        free(inf);
    }

    /*
     * systemd unit が起動するバイナリのパスと
     * Makefile の install 先 (PREFIX/bin) が整合しているか。
     */
    if (file_exists("scripts/vmodem.service")) {
        char *sv = slurp("scripts/vmodem.service", NULL);
        if (sv) {
            check(contains(sv, "/usr/local/bin/vmodem"),
                  "unit の ExecStart が make install の既定 "
                  "(PREFIX=/usr/local) と一致する");
            check(contains(sv, "/etc/vmodem/config.ini"),
                  "unit の --config が make install の配置先と一致する");
            free(sv);
        }
    }

    free(mk);
}

/* ==========================================================================
 * 実機でしか確認できない事の一覧
 * ==========================================================================
 * FAIL にせず SKIP として明示する。何を人手で確かめるべきかを
 * 残す事に意味がある。
 * ========================================================================== */
static void test_hardware_dependent(void)
{
    struct stat st;

    head("実機依存 (この環境では判定不能)");

    /* configfs が使えるか */
    if (stat("/sys/kernel/config/usb_gadget", &st) == 0) {
        note("configfs/usb_gadget が使える環境です");
        note("実機確認: sudo bash scripts/setup-gadget-linux.sh");
    } else {
        skip("configfs/usb_gadget が無い -> 実際のガジェット生成は未検証");
    }

    /* UDC があるか */
    if (stat("/sys/class/udc", &st) == 0) {
        note("/sys/class/udc が存在します");
    } else {
        skip("/sys/class/udc が無い -> UDC への bind は未検証");
    }

    /* ttyGS0 があるか */
    if (access("/dev/ttyGS0", F_OK) == 0) {
        note("/dev/ttyGS0 が存在します");
        note("実機確認: sudo ./vmodem --port /dev/ttyGS0 --net slirp -v");
    } else {
        skip("/dev/ttyGS0 が無い -> CDC-ACM の TTY は未検証");
    }

    skip("Windows XP での vim1modem.inf インストールは実機でのみ確認可能");
    note("  デバイスマネージャに COM ポートが出る事");
    note("  「標準 33600bps モデム」を割り当ててダイヤルアップできる事");

    /*
     * DTR/DCD について (指示書の調査項目 4)。
     * Linux 6.12 の u_serial.c の gs_tty_ops には
     * .tiocmget / .tiocmset が無い。よって ttyGS0 への TIOCMGET は
     * ENOTTY で失敗し、ホストの DTR をユーザ空間から読めない。
     * f_acm.c は port_handshake_bits にホストの DTR を保持しているが
     * ユーザ空間へは公開されない。
     */
    note("");
    note("調査済み: Linux 6.12 の ttyGS0 は TIOCMGET 非対応");
    note("  u_serial.c の gs_tty_ops に .tiocmget/.tiocmset が無いため");
    note("  ENOTTY になり、ホストの DTR は読めない。");
    note("  VModem はポートを開いている間を接続中として扱う設計。");
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(void)
{
    printf("=========================================================\n");
    printf(" Linux 移植 Step 7 / 8 / 8-a の検証\n");
    printf("=========================================================\n");
    printf("(リポジトリのルートで実行してください)\n");

    test_step7_makefile();
    test_step7_no_dup_hostroute();
    test_step8_gadget_script();
    test_step8_gadget_behavior();
    test_step8_udev_systemd();
    test_step8a_inf();
    test_cross_references();
    test_hardware_dependent();

    printf("\n=========================================================\n");
    printf(" 結果: 成功 %d / 失敗 %d / 省略 %d\n", g_pass, g_fail, g_skip);
    printf("=========================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
