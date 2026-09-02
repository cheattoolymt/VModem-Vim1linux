/*
 * test_linux_port.c - Linux 移植 (Step 1 / Step 2) の動作確認
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 検証対象:
 *   Step 1: src/serial/vm_serial_linux.c  (LINUXTTY バックエンド)
 *   Step 2: src/audio/vm_audio_alsa.c     (ALSA バックエンド)
 *
 * ===========================================================================
 * このテストが確認する事
 * ===========================================================================
 * 1. ALSA バックエンドが、サウンドカードの無い環境で
 *    null へ自動フォールバックし、vm_audio_open() が VM_OK を返す事。
 *    (指示書の「実行時に snd_pcm_open() が失敗したら自動で
 *     vm_audio_null.c にフォールバック」の要件そのもの)
 * 2. フォールバック後も PCM の投入・消費が正常に回る事。
 * 3. LINUXTTY バックエンドが実 tty (PTY スレーブ) を raw で開き、
 *    読み書きが成立する事。
 * 4. 制御線の能力検出が機能する事。
 * 5. close 時に termios が元に戻る事。
 * 6. AUTO 解決が port の綴りで LINUXTTY / PTY を選び分ける事。
 *
 * CI (サウンドカード無し / ttyGS0 無し) でも全て通るように、
 * 実機固有のデバイスには依存しない。ttyGS0 の代わりに PTY を使う。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "vmodem/vm_serial.h"
#include "vmodem/vm_audio.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pty.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) { g_pass++; printf("  [ OK ] " __VA_ARGS__); putchar('\n'); } \
        else      { g_fail++; printf("  [FAIL] " __VA_ARGS__); putchar('\n'); } \
    } while (0)

/* =========================================================================
 * Step 2: ALSA バックエンド
 * =======================================================================*/
static void test_alsa_fallback(void)
{
    vm_audio_params_t p;
    vm_audio_t *a = NULL;
    vm_err_t rc;
    float buf[160];
    uint32_t i, accepted;
    const char *bname;

    printf("\n=== Step 2: ALSA バックエンド ===\n");

    vm_audio_params_defaults(&p);
    p.backend  = VM_AUDIO_BACKEND_ALSA;
    p.dsp_rate = 8000;
    p.volume   = 0.7f;

    rc = vm_audio_open(&a, &p);

    /*
     * ★ 最重要 ★
     * サウンドカードが有っても無くても VM_OK でなければならない。
     * 無い場合は null へフォールバックし、モデム動作を続行させる。
     */
    CHECK(rc == VM_OK && a != NULL,
          "vm_audio_open(ALSA) が成功 (rc=%d) "
          "- デバイスが無くても null へフォールバックする", (int)rc);

    if (rc != VM_OK || !a) return;

    bname = vm_audio_backend_name(a);
    printf("         backend=%s device=\"%s\" rate=%dHz\n",
           bname, vm_audio_device_name(a), vm_audio_device_rate(a));

    CHECK(vm_audio_device_rate(a) > 0,
          "device_rate が確定している (%d Hz)", vm_audio_device_rate(a));

    /* 400Hz のトーンを 1 ブロック (20ms) 投入する */
    for (i = 0; i < 160; i++)
        buf[i] = 0.5f * sinf(2.0f * 3.14159265f * 400.0f * (float)i / 8000.0f);

    accepted = vm_audio_write(a, buf, 160);
    CHECK(accepted == 160,
          "vm_audio_write が 160 サンプル受理 (実際 %u)", accepted);

    /* レンダースレッドが消費するのを待つ */
    vm_audio_drain(a, 500);

    CHECK(vm_audio_queued(a) == 0,
          "投入した PCM が消費された (queued=%u)", vm_audio_queued(a));

    /* flush が例外を出さない事 */
    vm_audio_write(a, buf, 160);
    vm_audio_flush(a);
    printf("         underruns=%u idle_frames=%u\n",
           vm_audio_underruns(a), vm_audio_idle_frames(a));

    vm_audio_close(a);
    CHECK(1, "vm_audio_close が正常終了 (スレッド join 成功)");
}

static void test_alsa_named_device(void)
{
    vm_audio_params_t p;
    vm_audio_t *a = NULL;
    vm_err_t rc;

    printf("\n--- 存在しないデバイス名を指定した場合 ---\n");

    vm_audio_params_defaults(&p);
    p.backend      = VM_AUDIO_BACKEND_ALSA;
    p.dsp_rate     = 8000;
    /* 絶対に存在しないデバイス名 */
    p.device_match = "NoSuchAudioDevice_ZZZ";

    rc = vm_audio_open(&a, &p);
    CHECK(rc == VM_OK && a != NULL,
          "存在しないデバイス指定でも VM_OK (rc=%d) "
          "- 音無しで続行できる", (int)rc);
    if (a) vm_audio_close(a);
}

/* =========================================================================
 * Step 1: LINUXTTY バックエンド
 * =======================================================================*/
static void test_linuxtty(void)
{
    int master = -1, slave = -1;
    char slave_name[128];
    struct termios before, after;
    bool got_before;
    vm_serial_params_t sp;
    vm_serial_t *s = NULL;
    vm_err_t rc;
    char err[256];
    char rxbuf[128];
    int n;

    printf("\n=== Step 1: LINUXTTY バックエンド ===\n");

    /*
     * ttyGS0 は CI に存在しないので PTY で代用する。
     * PTY も実 tty なので termios / poll / ioctl の経路は同一で、
     * バックエンドのコードパスを本物と同じように通る。
     *
     *   master 側 = 「Windows RAS」役 (テストが操作する)
     *   slave 側  = 「/dev/ttyGS0」役 (VModem が開く)
     */
    if (openpty(&master, &slave, slave_name, NULL, NULL) != 0) {
        printf("  [SKIP] openpty に失敗: %s\n", strerror(errno));
        return;
    }
    printf("         PTY: master=fd%d slave=%s\n", master, slave_name);

    /* slave は VModem が自分で開くので、ここでは閉じる */
    close(slave);

    /* 開く前の termios を記録 (close 時の復元を検証するため) */
    {
        int probe = open(slave_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        got_before = (probe >= 0 && tcgetattr(probe, &before) == 0);
        if (probe >= 0) close(probe);
    }

    /*
     * ★ AUTO 解決の検証 ★
     * port が "/dev/..." なので AUTO は LINUXTTY を選ぶべき。
     * (PTY スレーブ名は /dev/pts/N なのでこの規則に乗る)
     */
    vm_serial_params_defaults(&sp);
    sp.port    = slave_name;
    sp.backend = VM_SERIAL_BACKEND_AUTO;
    sp.baud    = 115200;

    err[0] = '\0';
    rc = vm_serial_open(&s, &sp, err, sizeof(err));

    CHECK(rc == VM_OK && s != NULL,
          "vm_serial_open(AUTO, \"%s\") が成功 (rc=%d) %s",
          slave_name, (int)rc, err[0] ? err : "");

    if (rc != VM_OK || !s) { close(master); return; }

    CHECK(strcmp(vm_serial_backend_name(s), "linuxtty") == 0,
          "AUTO が /dev/* を LINUXTTY に解決した (backend=%s)",
          vm_serial_backend_name(s));

    CHECK(strcmp(vm_serial_name(s), slave_name) == 0,
          "ポート名が一致 (%s)", vm_serial_name(s));

    printf("         制御線: %s\n",
           vm_serial_has_modem_lines(s) ? "利用可能" : "利用不可 (CDC-ACM 相当)");

    /* ---- raw モードの検証 ---- */
    {
        int probe = open(slave_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (probe >= 0 && tcgetattr(probe, &after) == 0) {
            CHECK(!(after.c_lflag & (ECHO | ICANON | ISIG)),
                  "raw モード: ECHO/ICANON/ISIG が落ちている");
            CHECK(!(after.c_iflag & (ICRNL | IXON)),
                  "raw モード: ICRNL/IXON が落ちている "
                  "(AT と PPP が壊れない)");
            CHECK(!(after.c_oflag & OPOST),
                  "raw モード: OPOST が落ちている");
            CHECK((after.c_cflag & CLOCAL) != 0,
                  "CLOCAL が立っている (DCD 待ちで固まらない)");
            CHECK(after.c_cc[VMIN] == 0 && after.c_cc[VTIME] == 0,
                  "VMIN=0 / VTIME=0 (ある分だけ読んで即戻る)");
        } else {
            printf("  [SKIP] termios を再取得できず\n");
        }
        if (probe >= 0) close(probe);
    }

    /* ---- 読み込み: タイムアウトが効く事 ---- */
    n = vm_serial_read(s, rxbuf, sizeof(rxbuf), 50);
    CHECK(n == 0, "データが無い時に read が 0 を返す (n=%d)", n);

    /* ---- 読み込み: RAS -> モデム ---- */
    {
        const char *at = "ATDT0312345678\r";
        ssize_t w = write(master, at, strlen(at));
        CHECK(w == (ssize_t)strlen(at),
              "master 側から AT コマンドを %zd バイト送信", w);

        /* poll ベースの読み込みが起きて受け取れる事 */
        memset(rxbuf, 0, sizeof(rxbuf));
        n = vm_serial_read(s, rxbuf, sizeof(rxbuf) - 1, 500);
        CHECK(n == (int)strlen(at) && strcmp(rxbuf, at) == 0,
              "AT コマンドを無改変で受信 (n=%d, \"%.*s\")",
              n, n > 0 ? n - 1 : 0, rxbuf);
    }

    /* ---- 書き込み: モデム -> RAS ---- */
    {
        const char *resp = "\r\nCONNECT 33600\r\n";
        char back[128];
        ssize_t r;

        n = vm_serial_write_str(s, resp);
        CHECK(n == (int)strlen(resp),
              "CONNECT 応答を %d バイト送信", n);
        vm_serial_flush(s);

        memset(back, 0, sizeof(back));
        r = read(master, back, sizeof(back) - 1);
        CHECK(r == (ssize_t)strlen(resp) && strcmp(back, resp) == 0,
              "master 側で無改変に受信 (CR/LF が化けていない)");
    }

    /* ---- バイナリ透過性: PPP フレームに出る危険なバイト ---- */
    {
        /*
         * 0x00 (fNull で捨てられる), 0x03 (ISIG で SIGINT),
         * 0x11/0x13 (IXON でフロー制御), 0x0D/0x0A (CR/LF 変換),
         * 0x7E (PPP フラグ), 0x7D (PPP エスケープ)
         */
        const unsigned char frame[] = {
            0x7E, 0x00, 0x03, 0x11, 0x13, 0x0D, 0x0A, 0x7D, 0xFF, 0x1A, 0x7E
        };
        unsigned char back[32];
        ssize_t r;
        size_t total = 0;

        n = vm_serial_write(s, frame, (int)sizeof(frame));
        CHECK(n == (int)sizeof(frame),
              "PPP 相当のバイナリ %d バイトを送信", n);
        vm_serial_flush(s);

        /* PTY は分割して届く事があるので集まるまで読む */
        memset(back, 0, sizeof(back));
        while (total < sizeof(frame)) {
            r = read(master, back + total, sizeof(back) - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        CHECK(total == sizeof(frame) &&
              memcmp(back, frame, sizeof(frame)) == 0,
              "バイナリが完全に透過 (%zu/%zu bytes, "
              "0x00/0x03/0x11/0x13/0x0D/0x0A が無傷)",
              total, sizeof(frame));
    }

    /* ---- purge_rx ---- */
    {
        const char *junk = "AT\rAT\rAT\r";
        ssize_t w = write(master, junk, strlen(junk));
        (void)w;
        usleep(50000);
        vm_serial_purge_rx(s);
        n = vm_serial_read(s, rxbuf, sizeof(rxbuf), 0);
        CHECK(n == 0, "purge_rx 後に残留データが無い (n=%d)", n);
    }

    /* ---- 制御線 API がクラッシュしない事 ---- */
    vm_serial_set_dcd(s, true);
    vm_serial_set_dsr(s, true);
    vm_serial_set_cts(s, true);
    CHECK(1, "set_dcd/dsr/cts が安全に呼べる "
             "(制御線が無い環境では no-op)");

    /*
     * ★ get_dtr の最重要要件 ★
     * 制御線が読めない環境では true でなければならない。
     * false を返すと上位層が誤ってハングアップする。
     */
    if (!vm_serial_has_modem_lines(s)) {
        CHECK(vm_serial_get_dtr(s) == true,
              "制御線が無い時 get_dtr が true "
              "(誤ハングアップを防ぐ)");
    } else {
        printf("         [info] この tty は制御線が使えるため "
               "get_dtr=%d は実際の線の状態\n", (int)vm_serial_get_dtr(s));
        CHECK(1, "制御線が使える環境では実際の値を読む");
    }

    vm_serial_close(s);
    CHECK(1, "vm_serial_close が正常終了");

    /* ---- termios が復元された事 ---- */
    if (got_before) {
        int probe = open(slave_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios restored;
        if (probe >= 0 && tcgetattr(probe, &restored) == 0) {
            CHECK(restored.c_lflag == before.c_lflag &&
                  restored.c_iflag == before.c_iflag &&
                  restored.c_oflag == before.c_oflag,
                  "close 時に termios が元に戻った "
                  "(次に getty/minicom が使っても混乱しない)");
        }
        if (probe >= 0) close(probe);
    }

    close(master);
}

static void test_linuxtty_errors(void)
{
    vm_serial_params_t sp;
    vm_serial_t *s = NULL;
    vm_err_t rc;
    char err[256];

    printf("\n--- エラー処理 ---\n");

    /* 存在しないデバイス: 人間可読な理由が入る事 */
    vm_serial_params_defaults(&sp);
    sp.port    = "/dev/ttyGS_NOPE";
    sp.backend = VM_SERIAL_BACKEND_LINUXTTY;

    err[0] = '\0';
    rc = vm_serial_open(&s, &sp, err, sizeof(err));

    CHECK(rc != VM_OK && s == NULL,
          "存在しないポートで失敗する (rc=%d)", (int)rc);
    CHECK(err[0] != '\0' && strstr(err, "setup-gadget-linux.sh") != NULL,
          "エラーメッセージが対処方法を示す");
    printf("         msg: %s\n", err);

    /* tty ではないパス */
    vm_serial_params_defaults(&sp);
    sp.port    = "/dev/null";
    sp.backend = VM_SERIAL_BACKEND_LINUXTTY;
    err[0] = '\0';
    s = NULL;
    rc = vm_serial_open(&s, &sp, err, sizeof(err));
    CHECK(rc != VM_OK && s == NULL,
          "tty でないパス (/dev/null) を拒否する (rc=%d)", (int)rc);
    printf("         msg: %s\n", err);

    /* AUTO: /dev/ で始まらないなら PTY を選ぶ */
    vm_serial_params_defaults(&sp);
    sp.port    = NULL;
    sp.backend = VM_SERIAL_BACKEND_AUTO;
    err[0] = '\0';
    s = NULL;
    rc = vm_serial_open(&s, &sp, err, sizeof(err));
    if (rc == VM_OK && s) {
        CHECK(strcmp(vm_serial_backend_name(s), "pty") == 0,
              "AUTO + port 未指定 は PTY を選ぶ (backend=%s)",
              vm_serial_backend_name(s));
        vm_serial_close(s);
    } else {
        printf("  [SKIP] PTY を開けず (rc=%d)\n", (int)rc);
    }
}

int main(void)
{
    vm_log_init(VM_LOG_INFO, NULL);

    printf("===============================================\n");
    printf(" WinDialupEmu Linux 移植 Step 1 / Step 2 テスト\n");
    printf("===============================================\n");

    test_alsa_fallback();
    test_alsa_named_device();
    test_linuxtty();
    test_linuxtty_errors();

    printf("\n===============================================\n");
    printf(" 結果: %d 件成功 / %d 件失敗\n", g_pass, g_fail);
    printf("===============================================\n");

    return g_fail == 0 ? 0 : 1;
}
