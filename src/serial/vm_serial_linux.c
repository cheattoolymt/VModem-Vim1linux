/*
 * vm_serial_linux.c - Linux 実 tty バックエンド (/dev/ttyGS0 = USB Gadget CDC-ACM)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Windows 版 vm_serial_win32.c と同じインタフェース
 * (vm_serial_internal.h の vm_serial_*_linux 群) を実装する。
 *
 * ===========================================================================
 * 位置付け: com0com の代わりに USB Gadget を使う
 * ===========================================================================
 * Windows 版は「同一 PC 内の仮想 COM ペア (com0com)」で
 *
 *     [RAS] --- CNCA0 <=> CNCB0 --- [VModem]
 *
 * を作っていた。Linux 版 (Khadas VIM1) では PC とボードが物理的に分かれ、
 * USB-C ケーブルがシリアルケーブルの役を果たす。
 *
 *     [Windows RAS] --- COMn --- USB-C --- /dev/ttyGS0 --- [VModem]
 *                      (host側)          (gadget側)
 *
 * host 側では usbser.sys が COM ポートを生やし、gadget 側では
 * u_serial.c が /dev/ttyGS0 を生やす。両者は USB のバルク転送で
 * 直結されており、片方に書いたバイトがもう片方から読める。
 * つまり com0com のペアと全く同じ構図で、我々は gadget 側を開く。
 *
 * ===========================================================================
 * 実装上の難所 (1): 制御線 (DCD/DTR) が使えない
 * ===========================================================================
 * これが Windows 版との最大の相違点であり、移植の要である。
 *
 * Linux 6.12 の drivers/usb/gadget/function/u_serial.c を読むと、
 * gs_tty_ops は以下しか実装していない:
 *
 *     .open .close .write .put_char .flush_chars .write_room
 *     .chars_in_buffer .unthrottle .break_ctl .get_icount
 *
 * .tiocmget / .tiocmset が【無い】。そして drivers/tty/tty_io.c の
 *
 *     int tty_get_tiocm(struct tty_struct *tty) {
 *         int retval = -ENOTTY;
 *         if (tty->ops->tiocmget)
 *             retval = tty->ops->tiocmget(tty);
 *         return retval;
 *     }
 *
 * より、ttyGS0 に対する TIOCMGET は必ず errno=ENOTTY で失敗する。
 * TIOCMSET も tty_tiocmset() が ops->tiocmset==NULL で -ENOTTY を返す。
 *
 * ---------------------------------------------------------------------------
 * ではどうやって Windows に「回線が繋がった/切れた」を伝えるか
 * ---------------------------------------------------------------------------
 * f_acm.c が答えを持っている:
 *
 *     static void acm_connect(struct gserial *port) {
 *         acm->serial_state |= USB_CDC_SERIAL_STATE_DSR | ..._DCD;
 *         acm_notify_serial_state(acm);
 *     }
 *     static void acm_disconnect(struct gserial *port) {
 *         acm->serial_state &= ~(USB_CDC_SERIAL_STATE_DSR | ..._DCD);
 *         acm_notify_serial_state(acm);
 *     }
 *
 * これらは u_serial.c の gs_open()/gs_close() から呼ばれる。つまり
 *
 *     「VModem が /dev/ttyGS0 を open している間だけ host の DCD が上がる」
 *
 * DCD はプログラムから制御できず、fd の生存期間そのものが DCD になる。
 * 従って ATH による切断で host に DCD 落下を見せたい場合は、
 * "NO CARRIER" を送出した後に fd を閉じて開き直す事になる。
 * (本バックエンドは vm_serial_cycle_carrier() でこれを提供する)
 *
 * 逆方向の DTR も検出できない。f_acm.c の acm_setup() は
 * SET_CONTROL_LINE_STATE を
 *     acm->port_handshake_bits = w_value;
 * に保存するだけで、sysfs にも ioctl にも公開していない。
 * よって「RAS がポートを閉じた」の検出は AT コマンド層
 * (エスケープシーケンス +++ / ATH) と PPP の LCP Terminate、
 * および read の EOF に頼る。
 *
 * ★ 重要な設計判断 ★
 *   vm_serial_get_dtr() は制御線が読めない環境では【常に true】を返す。
 *   false を返すと上位層 (vm_modem.c) が「ユーザが切断した」と誤判定し、
 *   接続直後に即ハングアップしてしまう。
 *
 * なお本バックエンドは ttyGS0 専用ではない。VIM1 の 40 ピン GPIO の
 * 実 UART (/dev/ttyAML*) や USB シリアル変換器 (/dev/ttyUSB*) を
 * 指定した場合は TIOCMGET が成功するので、その時は Windows 版と同じく
 * 本来の DCD/DTR 制御を行う。open 時に 1 回だけ能力検出する。
 *
 * ===========================================================================
 * 実装上の難所 (2): raw モードにしないと PPP が壊れる
 * ===========================================================================
 * tty は既定で「行編集付き端末」として振る舞う。これを放置すると:
 *
 *   ICRNL  : 受信 CR を LF に化かす        -> AT コマンドが壊れる
 *   ONLCR  : 送信 LF を CRLF に化かす      -> 応答が壊れる
 *   ECHO   : 受信したバイトを送り返す      -> 無限ループ
 *   ICANON : 改行までバッファリングする    -> PPP が流れない
 *   ISIG   : 0x03 を SIGINT に変換する     -> PPP フレームで落ちる
 *   IXON   : 0x11/0x13 をフロー制御に使う  -> PPP が壊れる
 *
 * PPP フレームは 0x00-0xFF の任意のバイト列なので、これら全部が致命的。
 * cfmakeraw() で一括して落とすのが正解 (Windows 版で
 * fBinary=TRUE / fNull=FALSE / fOutX=fInX=FALSE にしたのと同じ意図)。
 *
 * さらに cfmakeraw() が触らない項目を明示的に設定する:
 *
 *   CLOCAL : これを立てないと open(2) が DCD を待ってブロックする。
 *            O_NONBLOCK で開けば open 自体は通るが、その後の
 *            read/write が EIO になる事があるので必ず立てる。
 *   CRTSCTS: ハードウェアフロー制御。ttyGS0 では無意味だが、
 *            実 UART では相手が RTS を上げないと送信できなくなり
 *            「なぜか無反応」というデバッグ困難な症状を生むので落とす。
 *   HUPCL  : close 時に DTR を落とす。実 UART では相手に切断を
 *            伝えられるので立てておく。
 *   VMIN=0 / VTIME=0 : 「今あるだけ読んで即戻る」。
 *            Windows 版の COMMTIMEOUTS の「魔法の組み合わせ」
 *            (ReadIntervalTimeout=MAXDWORD) と全く同じ意味になる。
 *
 * ===========================================================================
 * 実装上の難所 (3): 読み込み中に安全に止める (自己パイプ)
 * ===========================================================================
 * Windows 版は OVERLAPPED I/O + cancel_ev + WaitForMultipleObjects で
 * 「読み込み待ち中でも即座に止められる」を実現していた。
 *
 * Linux では poll() が複数 fd を待てるので、もっと素直に書ける:
 *
 *     pipe(wake_pipe);
 *     poll([tty_fd(POLLIN), wake_pipe[0](POLLIN)], timeout);
 *
 * close 時に wake_pipe[1] へ 1 バイト書けば poll が即座に戻る。
 * これは self-pipe trick と呼ばれる定番手法である。
 * signal ハンドラからでも安全 (write(2) は async-signal-safe)。
 *
 * Windows 版で問題だった「CancelIo 後に OVERLAPPED を解放してはいけない」
 * 類のライフタイム問題は、poll ベースでは一切発生しない。
 * その代わり pipe の fd 2 本を管理する必要がある。
 *
 * ===========================================================================
 * 実装上の難所 (4): ボーレートは無意味だが設定は通す
 * ===========================================================================
 * ttyGS0 は USB バルク転送なので、ボーレートという概念が無い
 * (実効速度は USB 2.0 の帯域で決まり、数 MB/s 出る)。
 * host が SET_LINE_CODING を送ってきても f_acm.c は保存するだけ。
 *
 * それでも cfsetispeed/cfsetospeed を呼ぶのは
 *   - 実 UART を指定された場合に正しく動作させるため
 *   - termios 構造体を中途半端な状態にしないため
 * である。B115200 固定で十分 (DTE 速度は回線速度と無関係。
 * 回線速度は CONNECT メッセージで通知する)。
 */
#if !defined(__linux__)
/* 非 Linux では空翻訳単位にする (ビルドシステムが常に渡しても安全) */
typedef int vm_serial_linux_dummy_t;
#else

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "vm_serial_internal.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>

/* ---------------------------------------------------------------------------
 * ボーレート -> termios 定数
 * ---------------------------------------------------------------------------
 * cfsetspeed() は glibc 拡張で任意の数値を受けるが、移植性のため
 * B* 定数に明示的に変換する。表に無い値は 115200 に落とす
 * (ttyGS0 では無意味なので、失敗させるより続行する方が良い)。
 * -------------------------------------------------------------------------*/
static speed_t baud_to_speed(int baud, int *actual)
{
    struct { int baud; speed_t sp; } tbl[] = {
        {   9600, B9600   }, {  19200, B19200  }, {  38400, B38400  },
        {  57600, B57600  }, { 115200, B115200 }, { 230400, B230400 },
#ifdef B460800
        { 460800, B460800 },
#endif
#ifdef B921600
        { 921600, B921600 },
#endif
    };
    size_t i;
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (tbl[i].baud == baud) { *actual = baud; return tbl[i].sp; }
    }
    *actual = 115200;
    return B115200;
}

/* ---------------------------------------------------------------------------
 * 制御線の能力検出
 * ---------------------------------------------------------------------------
 * TIOCMGET を 1 回試し、成功するかどうかを記録する。
 * ttyGS0 では ENOTTY で失敗するのが【正常】なので、
 * 警告は出すがエラーにはしない。
 * -------------------------------------------------------------------------*/
static bool probe_modem_lines(int fd, const char *name)
{
    int bits = 0;

    if (ioctl(fd, TIOCMGET, &bits) == 0) {
        VM_LOGI("%s: モデム制御線が利用可能 (TIOCMGET=0x%04X)", name, bits);
        return true;
    }

    if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS) {
        /*
         * USB Gadget CDC-ACM の想定どおりの結果。
         * DCD は「fd を開いている間」で表現され、
         * DTR は検出不能になる。上位層は has_mlines=false を見て
         * 「NO CARRIER + close」で切断を伝える。
         */
        VM_LOGI("%s: モデム制御線は使用不可 (errno=%d %s)",
                name, errno, strerror(errno));
        VM_LOGI("%s: USB Gadget CDC-ACM では正常な結果です。"
                "host 側 DCD は「このポートを開いている間」で表現され、"
                "DTR 落下は検出できません", name);
    } else {
        VM_LOGW("%s: TIOCMGET が予期しないエラー: %s",
                name, strerror(errno));
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * オープン
 * -------------------------------------------------------------------------*/
vm_err_t vm_serial_open_linux(vm_serial_t *s, char *errbuf, size_t errbuf_size)
{
    const char *path;
    struct termios tio;
    struct termios *saved;
    speed_t sp;
    int actual_baud = 0;
    int fd = -1;

    path = (s->params.port && s->params.port[0]) ? s->params.port
                                                 : "/dev/ttyGS0";

    /*
     * ------------------------------------------------------------------
     * open のフラグ
     * ------------------------------------------------------------------
     * O_RDWR     : 双方向
     * O_NOCTTY   : ★必須★ これを忘れると、制御端末を持たないプロセスが
     *              この tty を「制御端末」として獲得してしまう。すると
     *              相手から届いた 0x03 が SIGINT になって VModem が死ぬ。
     *              PPP フレームには 0x03 が普通に出現するので致命的。
     * O_NONBLOCK : DCD を待たずに即座に open する。CLOCAL を立てる前は
     *              open(2) が DCD 待ちでブロックし得るため。
     *              termios 設定後に O_NONBLOCK は維持する
     *              (read/write を自前で poll 制御するので都合が良い)。
     * O_CLOEXEC  : exec 時に閉じる。ガジェットスクリプトを system() で
     *              呼ぶ実装に発展しても fd が漏れない。
     */
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT) {
            snprintf(errbuf, errbuf_size,
                     "シリアルポート \"%s\" が存在しません。"
                     "USB Gadget が未設定の可能性があります。"
                     "先に scripts/setup-gadget-linux.sh を実行し、"
                     "ls -l /dev/ttyGS* で確認してください", path);
        } else if (e == EACCES) {
            snprintf(errbuf, errbuf_size,
                     "シリアルポート \"%s\" への権限がありません。"
                     "sudo で実行するか、実行ユーザを dialout グループに "
                     "追加してください (usermod -aG dialout $USER)", path);
        } else if (e == EBUSY) {
            snprintf(errbuf, errbuf_size,
                     "シリアルポート \"%s\" は他プロセスが使用中です。"
                     "getty が動いていないか確認してください "
                     "(systemctl status serial-getty@ttyGS0)", path);
        } else {
            snprintf(errbuf, errbuf_size,
                     "open(\"%s\") に失敗: %s", path, strerror(e));
        }
        return VM_ERR_IO;
    }

    /*
     * ------------------------------------------------------------------
     * 元の termios を保存 (close 時に復元する)
     * ------------------------------------------------------------------
     * ttyGS0 を raw のまま放置すると、次に getty や minicom が
     * 使う時に挙動が変わって混乱するので礼儀として戻す。
     */
    saved = (struct termios *)calloc(1, sizeof(*saved));
    if (!saved) {
        close(fd);
        snprintf(errbuf, errbuf_size, "out of memory");
        return VM_ERR_NOMEM;
    }

    if (tcgetattr(fd, saved) != 0) {
        /*
         * ここで失敗するのは「tty ではない」場合 (ENOTTY)。
         * 通常ファイルや named pipe を port に指定された等。
         * termios 設定ができないので続行しない。
         */
        snprintf(errbuf, errbuf_size,
                 "tcgetattr(\"%s\") に失敗: %s "
                 "(このパスは tty ではありません)", path, strerror(errno));
        free(saved);
        close(fd);
        return VM_ERR_IO;
    }

    tio = *saved;

    /*
     * ------------------------------------------------------------------
     * raw モード化
     * ------------------------------------------------------------------
     * cfmakeraw() は次を行う (glibc):
     *   iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON)
     *   oflag &= ~OPOST
     *   lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN)
     *   cflag &= ~(CSIZE|PARENB);  cflag |= CS8
     */
    cfmakeraw(&tio);

    /* cfmakeraw が触らない / 明示したい項目 */
    tio.c_cflag |= (CLOCAL | CREAD);   /* DCD を無視 / 受信を有効化 */
    tio.c_cflag |= HUPCL;              /* close で DTR を落とす     */
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;           /* HW フロー制御は使わない   */
#endif
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);  /* SW フロー制御も全切り */
    tio.c_iflag &= ~(INPCK | ISTRIP);        /* 8bit を素通し         */

    /* データビット / ストップビット / パリティ */
    tio.c_cflag &= ~CSIZE;
    switch (s->params.data_bits) {
    case 7:  tio.c_cflag |= CS7; break;
    case 8:  /* fall through */
    default: tio.c_cflag |= CS8; break;
    }
    if (s->params.stop_bits == 2) tio.c_cflag |=  CSTOPB;
    else                          tio.c_cflag &= ~CSTOPB;
    /* パリティは常に none (PPP でパリティは使わない) */
    tio.c_cflag &= ~(PARENB | PARODD);

    /*
     * VMIN=0 / VTIME=0 = 「バッファにある分だけ読んで即戻る」。
     * Windows 版の
     *     ReadIntervalTimeout = MAXDWORD
     *     ReadTotalTimeoutMultiplier = 0
     *     ReadTotalTimeoutConstant = 0
     * と完全に等価な意味を持つ。待ち時間の制御は poll() 側で行う。
     */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    sp = baud_to_speed(s->params.baud, &actual_baud);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    /*
     * TCSANOW = 即座に適用 (送信中のデータを待たない)。
     * TCSADRAIN だと相手が居ない ttyGS0 で固まる可能性がある。
     */
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        snprintf(errbuf, errbuf_size,
                 "tcsetattr(\"%s\") に失敗: %s", path, strerror(errno));
        free(saved);
        close(fd);
        return VM_ERR_IO;
    }

    /* 残留データを破棄 (前回の接続の PPP 断片が残っている事がある) */
    tcflush(fd, TCIOFLUSH);

    /*
     * ------------------------------------------------------------------
     * 自己パイプ (読み込み中断用)
     * ------------------------------------------------------------------
     */
    s->wake_pipe[0] = s->wake_pipe[1] = -1;
    if (pipe(s->wake_pipe) != 0) {
        snprintf(errbuf, errbuf_size, "pipe() に失敗: %s", strerror(errno));
        tcsetattr(fd, TCSANOW, saved);
        free(saved);
        close(fd);
        return VM_ERR_IO;
    }
    /*
     * 両端を非ブロッキング + CLOEXEC に。
     * 書き込み側が非ブロッキングでないと、誰も読まないまま
     * 何度も close を呼ばれた場合にパイプが詰まって固まる。
     */
    fcntl(s->wake_pipe[0], F_SETFL,
          fcntl(s->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(s->wake_pipe[1], F_SETFL,
          fcntl(s->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(s->wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(s->wake_pipe[1], F_SETFD, FD_CLOEXEC);

    s->tty_fd    = fd;
    s->saved_tio = saved;
    snprintf(s->name, sizeof(s->name), "%s", path);

    /* 制御線の能力検出 */
    s->has_mlines = probe_modem_lines(fd, s->name);

    /*
     * 初期状態: オンフック相当。
     * 制御線が使える環境では DTR/RTS を落としておく
     * (Windows 版と同じく、我々の DTR が相手の DCD に相当する配線を想定)。
     */
    s->dcd = false;
    s->dsr = false;
    s->cts = false;
    if (s->has_mlines) {
        vm_serial_set_lines_linux(s);
    } else {
        /*
         * 制御線が読めない = DTR 落下を検出できない。
         * 上位層が誤ってハングアップしないよう true 固定にする
         * (vm_serial_get_dtr_linux も同じ判断をするが、
         *  構造体の値としても整合させておく)。
         */
        s->dtr = true;
        s->rts = true;
    }

    VM_LOGI("Linux tty を開いた: %s (%d bps, raw, %s)",
            s->name, actual_baud,
            s->has_mlines ? "制御線あり" : "制御線なし/CDC-ACM");

    if (!s->has_mlines) {
        VM_LOGI("%s: 切断を host に伝えるには "
                "\"NO CARRIER\" 送出後にポートを閉じてください "
                "(close で acm_disconnect が走り DCD が落ちます)", s->name);
    }
    return VM_OK;
}

/* ---------------------------------------------------------------------------
 * クローズ
 * -------------------------------------------------------------------------*/
void vm_serial_close_linux(vm_serial_t *s)
{
    if (!s) return;

    /*
     * 進行中の読み込みを起こす。
     * write が失敗しても (パイプ満杯 = 既に通知済み) 問題ない。
     */
    if (s->wake_pipe[1] >= 0) {
        char b = 'q';
        ssize_t ignored = write(s->wake_pipe[1], &b, 1);
        (void)ignored;
    }

    if (s->tty_fd >= 0) {
        /*
         * 制御線が使えるなら DTR/RTS を落として相手に切断を伝える。
         * ttyGS0 では no-op になるが、close そのものが
         * acm_disconnect() を呼んで host の DCD を落とすので
         * 結果的に切断は伝わる。
         */
        if (s->has_mlines) {
            int bits = TIOCM_DTR | TIOCM_RTS;
            if (ioctl(s->tty_fd, TIOCMBIC, &bits) != 0)
                VM_LOGD("TIOCMBIC(DTR|RTS) 失敗: %s", strerror(errno));
        }

        /* 未送信データを捨ててから termios を復元する */
        tcflush(s->tty_fd, TCIOFLUSH);

        if (s->saved_tio) {
            (void)tcsetattr(s->tty_fd, TCSANOW,
                            (struct termios *)s->saved_tio);
            free(s->saved_tio);
            s->saved_tio = NULL;
        }

        close(s->tty_fd);
        s->tty_fd = -1;
    }

    if (s->wake_pipe[0] >= 0) { close(s->wake_pipe[0]); s->wake_pipe[0] = -1; }
    if (s->wake_pipe[1] >= 0) { close(s->wake_pipe[1]); s->wake_pipe[1] = -1; }

    VM_LOGI("Linux tty を閉じた (rx=%llu tx=%llu bytes)",
            (unsigned long long)s->rx_bytes,
            (unsigned long long)s->tx_bytes);
}

/* ---------------------------------------------------------------------------
 * 読み込み
 * ---------------------------------------------------------------------------
 * 戻り値 = 読めたバイト数 (0 = 今は何も来ていない, 負 = エラー)。
 * timeout_ms > 0 : 最初の 1 バイトが来るまで最大その時間待つ
 * timeout_ms == 0: 即座に戻る
 * timeout_ms < 0 : 無限に待つ (close されるまで)
 * -------------------------------------------------------------------------*/
int vm_serial_read_linux(vm_serial_t *s, void *buf, int len, int timeout_ms)
{
    struct pollfd pfd[2];
    ssize_t n;
    int pr;

    if (!s || s->tty_fd < 0) return VM_ERR_STATE;

    /*
     * まず非ブロッキング read を試す。
     * 既にデータがあれば poll のシステムコールを 1 回節約できる。
     * PPP 転送中は毎回データがあるので、この最適化は効果が大きい。
     */
    n = read(s->tty_fd, buf, (size_t)len);
    if (n > 0) return (int)n;

    if (n == 0) {
        /*
         * EOF。ttyGS0 では USB ケーブルが抜けた / host 側が
         * ポートを閉じた場合に起こり得る。
         * 「相手が居ない」だけでエラーではないので 0 を返す。
         */
        if (timeout_ms == 0) return 0;
    } else {
        int e = errno;
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR) {
            if (e == EIO) {
                /*
                 * ガジェットが未接続 (USB ケーブル未挿入) の時に出る。
                 * 回復可能なので 0 を返して上位層のループを継続させる。
                 */
                return 0;
            }
            VM_LOGW("read(%s) 失敗: %s", s->name, strerror(e));
            return VM_ERR_IO;
        }
        if (timeout_ms == 0) return 0;   /* 即戻り指定 */
    }

    /*
     * ------------------------------------------------------------------
     * poll で [tty, 中断パイプ] を待つ
     * ------------------------------------------------------------------
     * POLLIN 以外に POLLHUP/POLLERR も見る。ttyGS0 では USB が
     * 切れると POLLHUP が立つので、これを検出して 0 を返し
     * 上位層に「今は読めない」と伝える。
     * (revents は poll が設定するので events に指定する必要は無いが、
     *  明示的に扱う事でコードの意図を明確にする)
     */
    pfd[0].fd      = s->tty_fd;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd      = s->wake_pipe[0];
    pfd[1].events  = POLLIN;
    pfd[1].revents = 0;

    pr = poll(pfd, 2, timeout_ms);

    if (pr < 0) {
        if (errno == EINTR) return 0;    /* シグナルで中断: 次回再試行 */
        VM_LOGW("poll(%s) 失敗: %s", s->name, strerror(errno));
        return VM_ERR_IO;
    }
    if (pr == 0) return 0;               /* タイムアウト */

    /* 中断要求が来ていたら読まずに抜ける (close 中) */
    if (pfd[1].revents & POLLIN) {
        VM_LOGD("serial read cancelled by shutdown request");
        return 0;
    }

    if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        /*
         * USB ケーブルが抜けた等。POLLIN も同時に立っている場合は
         * まだ読めるデータが残っているので、read を試してから判断する。
         */
        if (!(pfd[0].revents & POLLIN)) {
            VM_LOGD("%s: POLLHUP/POLLERR (revents=0x%X)",
                    s->name, pfd[0].revents);
            return 0;
        }
    }

    if (!(pfd[0].revents & POLLIN)) return 0;

    n = read(s->tty_fd, buf, (size_t)len);
    if (n > 0) return (int)n;
    if (n == 0) return 0;                /* EOF */

    if (errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == EINTR  || errno == EIO) return 0;

    VM_LOGW("read(%s) 失敗: %s", s->name, strerror(errno));
    return VM_ERR_IO;
}

/* ---------------------------------------------------------------------------
 * 書き込み
 * ---------------------------------------------------------------------------
 * len 全部書けるまで試みる。相手が読まない場合に無限に粘らないよう
 * 合計 5 秒でタイムアウトする (Windows 版の
 * WriteTotalTimeoutConstant=5000 と同じ方針)。
 * -------------------------------------------------------------------------*/
int vm_serial_write_linux(vm_serial_t *s, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int done = 0;
    int waited_ms = 0;
    enum { WRITE_TIMEOUT_MS = 5000 };

    if (!s || s->tty_fd < 0) return VM_ERR_STATE;

    while (done < len) {
        ssize_t n = write(s->tty_fd, p + done, (size_t)(len - done));

        if (n > 0) {
            done += (int)n;
            waited_ms = 0;      /* 進捗があったのでタイマをリセット */
            continue;
        }

        if (n < 0) {
            int e = errno;

            if (e == EINTR) continue;

            if (e == EAGAIN || e == EWOULDBLOCK) {
                /*
                 * 送信バッファが満杯。poll で「書けるようになる」のを待つ。
                 * ここでも中断パイプを一緒に待ち、close 要求に即応する。
                 */
                struct pollfd pfd[2];
                int pr;

                if (waited_ms >= WRITE_TIMEOUT_MS) {
                    VM_LOGW("write(%s) タイムアウト (%d/%d bytes)",
                            s->name, done, len);
                    return done;
                }

                pfd[0].fd      = s->tty_fd;
                pfd[0].events  = POLLOUT;
                pfd[0].revents = 0;
                pfd[1].fd      = s->wake_pipe[0];
                pfd[1].events  = POLLIN;
                pfd[1].revents = 0;

                pr = poll(pfd, 2, 100);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    VM_LOGW("poll(write %s) 失敗: %s",
                            s->name, strerror(errno));
                    return done > 0 ? done : VM_ERR_IO;
                }
                if (pr == 0) { waited_ms += 100; continue; }

                if (pfd[1].revents & POLLIN) return done;   /* 中断 */

                if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    VM_LOGD("%s: 書き込み中に POLLHUP/POLLERR", s->name);
                    return done;
                }
                continue;
            }

            if (e == EIO || e == ENXIO) {
                /*
                 * ガジェット未接続 / USB ケーブル未挿入。
                 * 書けなかった事だけを返し、エラー扱いにはしない
                 * (AT 応答が捨てられるだけで、接続時に再送される)。
                 */
                return done;
            }

            VM_LOGW("write(%s) 失敗: %s", s->name, strerror(e));
            return done > 0 ? done : VM_ERR_IO;
        }

        /* n == 0: 進捗なし。稀だが起こり得るので少し待つ */
        {
            struct timespec ts;
            ts.tv_sec = 0; ts.tv_nsec = 1000000L;   /* 1ms */
            nanosleep(&ts, NULL);
            waited_ms += 1;
            if (waited_ms >= WRITE_TIMEOUT_MS) return done;
        }
    }
    return done;
}

/* ---------------------------------------------------------------------------
 * 送信バッファを吐き切るまで待つ
 * ---------------------------------------------------------------------------
 * tcdrain(3) は「送信が完了するまでブロックする」。
 * ttyGS0 では USB 転送完了まで待つ事になるが、通常は即座に返る。
 * -------------------------------------------------------------------------*/
void vm_serial_flush_linux(vm_serial_t *s)
{
    if (!s || s->tty_fd < 0) return;

    if (tcdrain(s->tty_fd) != 0) {
        /*
         * ガジェット未接続だと EIO になる事がある。
         * 「吐き切れなかった」だけなので警告に留める。
         */
        if (errno != EINTR)
            VM_LOGD("tcdrain(%s) 失敗: %s", s->name, strerror(errno));
    }
}

void vm_serial_purge_rx_linux(vm_serial_t *s)
{
    if (!s || s->tty_fd < 0) return;
    if (tcflush(s->tty_fd, TCIFLUSH) != 0)
        VM_LOGD("tcflush(%s) 失敗: %s", s->name, strerror(errno));
}

/* ---------------------------------------------------------------------------
 * モデム制御線の出力
 * ---------------------------------------------------------------------------
 * Windows 版 vm_serial_set_lines_win32() と同じ役割。
 *
 * 我々はモデム役なので本来 DCD を出力したいが、DCD は入力線であり
 * ioctl で立てる事はできない (これは Linux でも Windows でも同じ)。
 * com0com のクロス配線と同様に「我々の DTR が相手の DCD になる」
 * 結線を前提とし、DTR を DCD 相当として操作する。
 *
 * ttyGS0 では TIOCMSET 自体が使えないので何もしない (記録のみ)。
 * -------------------------------------------------------------------------*/
void vm_serial_set_lines_linux(vm_serial_t *s)
{
    int set = 0, clr = 0;

    if (!s || s->tty_fd < 0) return;

    if (!s->has_mlines) {
        /*
         * CDC-ACM: DCD は fd の生存期間で表現される。
         * ここで出来る事は無い。呼び出し側が何度も呼ぶので
         * ログは DEBUG レベルに留める。
         */
        VM_LOGD("%s: 制御線が無いため set_lines は no-op "
                "(dcd=%d dsr=%d cts=%d)",
                s->name, (int)s->dcd, (int)s->dsr, (int)s->cts);
        return;
    }

    /* DCD 相当 = 我々の DTR */
    if (s->dcd) set |= TIOCM_DTR; else clr |= TIOCM_DTR;
    /* CTS 相当 = 我々の RTS */
    if (s->cts) set |= TIOCM_RTS; else clr |= TIOCM_RTS;

    /*
     * TIOCMBIS / TIOCMBIC で「立てるビット」「落とすビット」を
     * 別々に指定する。TIOCMSET で一括設定すると、我々が管理して
     * いない線 (実 UART の場合の DSR 等) を巻き込んでしまう。
     */
    if (set && ioctl(s->tty_fd, TIOCMBIS, &set) != 0)
        VM_LOGD("TIOCMBIS(0x%04X) 失敗: %s", set, strerror(errno));
    if (clr && ioctl(s->tty_fd, TIOCMBIC, &clr) != 0)
        VM_LOGD("TIOCMBIC(0x%04X) 失敗: %s", clr, strerror(errno));
}

/* ---------------------------------------------------------------------------
 * 相手 (DTE) の DTR を読む
 * ---------------------------------------------------------------------------
 * ★ 最重要の注意点 ★
 * 制御線が読めない環境 (ttyGS0) では【必ず true を返す】。
 *
 * false を返すと上位層 (vm_modem.c) が
 * 「RAS がポートを閉じた = ユーザが切断した」と解釈して
 * 接続直後にハングアップしてしまう。CDC-ACM では DTR を
 * 知る手段が無いので、「常に上がっている」と仮定するのが
 * 唯一まともに動く選択である。
 *
 * 制御線が読める場合は Windows 版と同じく、
 * クロス配線を前提に DCD 入力 (TIOCM_CAR) を「相手の DTR」として読む。
 * -------------------------------------------------------------------------*/
bool vm_serial_get_dtr_linux(vm_serial_t *s)
{
    int bits = 0;

    if (!s || s->tty_fd < 0) return false;

    if (!s->has_mlines) return true;     /* ← CDC-ACM: 常に接続扱い */

    if (ioctl(s->tty_fd, TIOCMGET, &bits) != 0) {
        /*
         * open 時には成功したのに今失敗した = デバイスが消えた。
         * 切断と解釈するのが自然だが、一時的なエラーで
         * 誤切断するリスクを避けて true を返す。
         * 本当の切断は read の EOF / POLLHUP で検出される。
         */
        VM_LOGD("TIOCMGET(%s) 失敗: %s", s->name, strerror(errno));
        return true;
    }

    /* TIOCM_CAR = DCD 入力 (Windows 版の MS_RLSD_ON と同じ) */
    return (bits & TIOCM_CAR) != 0;
}

#endif /* __linux__ */
