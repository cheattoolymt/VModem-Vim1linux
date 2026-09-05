# VModem-Vim1linux

[WinDialupEmu (VModem)](https://github.com/cheattoolymt/WinDialupEmu) —
Windows 用ダイアルアップモデム エミュレータ — を **Khadas VIM1 / Armbian Linux**
上で動かすための移植版です。

VIM1 を USB-C でレガシー PC に繋ぐと、PC からは「USB 接続の外付けモデム」に
見えます。PC のダイアルアップ接続 (Windows XP〜11 の RAS など) から発信すると、
VIM1 側の VModem が AT コマンドに応答し、PPP で IP を払い出し、VIM1 の Wi-Fi
経由で実際のインターネットへ中継します。

```
┌──────────────────┐            ┌────────────────────┐          ┌──────────┐
│ レガシー PC       │            │   Khadas VIM1      │          │          │
│ WinXP 〜 Win11    │◄── USB-C ─►│   Armbian 6.12     │◄─ Wi-Fi ─►│ Internet │
│ ダイアルアップ    │  CDC-ACM   │   VModem           │          │          │
│ (RAS)            │            │   AT / PPP / NAT   │          │          │
└──────────────────┘            └─────────┬──────────┘          └──────────┘
   host: usbser.sys                       │ ALSA
   gadget: /dev/ttyGS0                    ▼
                                    🔊 ネゴシエーション音
```

Windows 版は com0com の仮想 COM ペアを「電話線」に使っていましたが、本移植版は
**USB Gadget CDC-ACM** に置き換えます。ドライバのインストールが要らず、
レガシー PC 側は標準の `usbser.sys` (Win7 以降なら追加ドライバ不要) で
そのまま COM ポートとして見えます。

---

## 移植の進捗

本リポジトリは移植手順書 (`linux_port_instructions.md`) の
**Step 0〜8 (8-a を含む) すべて**を実装した段階です。

| Step | 内容 | 状態 |
|---|---|---|
| 0 | 上流コードの取り込みと調査 | ✅ 完了 |
| 1 | `src/serial/vm_serial_linux.c` (USB Gadget CDC-ACM シリアル) | ✅ 完了 |
| 2 | `src/audio/vm_audio_alsa.c` (ALSA 音声出力) | ✅ 完了 |
| 3 | `src/main.c` のシグナル処理 | ✅ 完了 |
| 4 | `src/core/vm_log.c` の POSIX 対応 | ✅ 完了 |
| 5 | `src/net/vm_nat.c` の時計 / `vm_hostroute_linux.c` (新規) | ✅ 完了 |
| 6 | `src/net/vm_nat_slirp.c` (libslirp の POSIX 対応) | ✅ 完了 |
| 7 | `Makefile.linux` | ✅ 完了 |
| 8 | `scripts/setup-gadget-linux.sh` / `windows/vim1modem.inf` | ✅ 完了 |
| 9 | DNS 解決の環境依存と ICMP 権限の恒久化 (`src/net/vm_netdiag.c` 新規) | ✅ 完了 |

Step 3・4 が入ったので、**libslirp を使わない構成 (`--net none` /
`--net loopback`) なら Linux 上で実際に起動・常駐・正常終了できます**。
Step 5 で `src/net/` の Windows 依存のうち **時計 (`GetTickCount64`) と
外向きアドレスの検出 (`GetAdaptersAddresses`)** が解消されました。
Step 6 で `vm_nat_slirp.c` の `GetProcAddress` / `WSAPOLLFD` 依存が解消され、
**`--net slirp` も Linux でビルド・リンク・起動できます**
(Debian/Ubuntu の libslirp 4.8 と同梱ヘッダ 4.9.3 の組み合わせでも動作)。

```bash
# Step 5 までで動く構成の例 (PTY をモデム側の回線として使う)
#
# ★ vm_hostroute.c ではなく vm_hostroute_linux.c を渡すこと ★
#   (Step 5 の要求。詳細は下の「Step 5」節を参照。なお両方渡しても
#    vm_hostroute.c は _WIN32 で囲まれているのでリンクは通ります)
gcc -O2 -std=c99 -Iinclude \
    src/core/*.c src/dsp/*.c src/modem/*.c \
    src/net/vm_eth.c src/net/vm_hdlc.c src/net/vm_hostroute_linux.c \
    src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c \
    src/net/vm_ppp.c src/net/vm_winsock.c \
    src/audio/vm_audio.c src/audio/vm_audio_null.c src/audio/vm_audio_alsa.c \
    src/serial/vm_serial.c src/serial/vm_serial_linux.c src/main.c \
    -lm -lpthread -lasound -lutil -o vmodem
./vmodem --net none -v      # Ctrl-C で PPP を切ってから正常終了する
```

---

## Step 1: シリアル バックエンド (`src/serial/vm_serial_linux.c`)

`vm_serial.h` の公開インタフェースを Windows 版 (`vm_serial_win32.c`) と
同一のセマンティクスで実装します。新しいバックエンド ID は
`VM_SERIAL_BACKEND_LINUXTTY` で、`VM_SERIAL_BACKEND_AUTO` はポート名が
`/dev/` で始まる時にこれを選びます (それ以外の POSIX 環境では従来の PTY)。

### ★最重要★ ttyGS0 では DTR / DCD が取れない

移植で最も注意が必要な点です。**推測ではなくカーネル v6.12 のソースで確認
した事実**として記録しておきます。

1. `drivers/usb/gadget/function/u_serial.c` の `gs_tty_ops` には
   **`.tiocmget` / `.tiocmset` が存在しません**。
2. そのため `drivers/tty/tty_io.c` の `tty_get_tiocm()` / `tty_tiocmset()` は
   `-ENOTTY` を返します。
   → **`ioctl(fd, TIOCMGET, ...)` は必ず `errno == ENOTTY` で失敗します。**
3. `f_acm.c` を見ると、DCD は `acm_connect()` / `acm_disconnect()` によって
   **gadget 側 tty の open / close と連動**しています。つまり
   **DCD の状態 = fd の寿命**です。
4. ホストが送った DTR は `acm->port_handshake_bits` に保存されるだけで
   **ユーザ空間からは一切観測できません**。

この制約から、実装は次の設計を取っています。

- **能力プローブ** — open 直後に `TIOCMGET` を一度試し、結果を
  `has_mlines` に記録します。`ENOTTY` は異常ではなく「CDC-ACM では正常な
  結果」として INFO ログを出します。公開 API
  `vm_serial_has_modem_lines()` で上位層から問い合わせできます。
- **制御線操作は記録のみの no-op** — `set_dcd` / `set_dsr` / `set_cts` は
  制御線が無い環境では状態を覚えるだけです。切断を相手に伝えるには
  **ポートを閉じる** (= CDC-ACM の DCD を落とす) 必要があります。
- **`get_dtr()` は制御線が無ければ常に `true`** — ここを `false` にすると
  上位層が「ユーザが切断した」と誤判定して接続直後にハングアップします。
  取得できない情報は「接続中」と見なすのが安全側です。

### その他の実装ポイント

| 項目 | 内容 |
|---|---|
| termios | `cfmakeraw()` + `CLOCAL \| CREAD \| HUPCL`、`ICRNL`/`IXON`/`IXOFF`/`IXANY`/`OPOST`/`ECHO`/`ICANON`/`ISIG` を全て解除 |
| `VMIN` / `VTIME` | 共に 0。Win32 版の `ReadIntervalTimeout = MAXDWORD` (即時リターン) と等価 |
| `O_NOCTTY` | **必須**。これが無いと PPP フレーム中の 0x03 が SIGINT になりプロセスが死ぬ |
| 非同期 I/O | `poll()` で `[tty_fd, wake_pipe[0]]` を監視。self-pipe trick により Win32 版の `OVERLAPPED` + `cancel_ev` + `WaitForMultipleObjects` を置き換え |
| バイナリ透過 | PPP フレームは 0x00〜0xFF 任意の値を含むため、0x00 / 0x03 / 0x11 / 0x13 / 0x0D / 0x0A が無改変で通る事をテストで確認 |
| 速度 | 115200 固定。USB Gadget では実際のボーレートは意味を持たない |
| `EIO` | gadget 未接続時に返るため、エラーではなく 0 バイトとして扱う |
| termios の保存 / 復元 | open 前の設定を保存し close で戻す |

### `poll()` を使う理由

Win32 版は `OVERLAPPED` + イベント + `CancelIo` で中断可能な I/O を実現して
いますが、`OVERLAPPED` 構造体の寿命管理は誤りやすく、`CancelIo` 前に
バッファを解放すると解放済みメモリへの DMA が起きます。POSIX では
self-pipe と `poll()` の組み合わせでこの危険が構造的に発生しません。

---

## Step 2: 音声バックエンド (`src/audio/vm_audio_alsa.c`)

`vm_audio.h` の公開インタフェースを WASAPI 版 (`vm_audio_wasapi.c`) と同一
セマンティクスで実装します。DSP の出力 (8000Hz / mono / float32) を alsa-lib
経由でスピーカーへ流します。**スタブではなく完全な実装**です。

### WASAPI との対応

| WASAPI | ALSA |
|---|---|
| `IMMDeviceEnumerator` | `snd_device_name_hint()` |
| `IAudioClient::Initialize` | `snd_pcm_hw_params_*` / `snd_pcm_sw_params_*` |
| `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` | `sw_params` の `avail_min` = period |
| `SetEventHandle` + `WaitForSingleObject` | `snd_pcm_wait()` |
| `GetCurrentPadding` | `snd_pcm_avail_update()` |
| `GetBuffer` / `ReleaseBuffer` | `snd_pcm_writei()` |
| `AUDCLNT_E_DEVICE_INVALIDATED` | `-ENODEV` |
| デバイス リセット | `snd_pcm_recover()` (`-EPIPE` / `-ESTRPIPE`) |
| MMCSS (`AvSetMmThreadCharacteristics`) | **採用せず** (後述) |

### デバイスを開けない時は自動で null にフォールバック

VIM1 の HDMI 音声は Armbian で動作しない事例が報告されています。
そのため本実装は**ビルド時に除外するのではなく、実行時に**判定します。

```c
err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
if (err < 0) {
    VM_LOGE("ALSA デバイス \"%s\" を開けません: %s", dev, snd_strerror(err));
    VM_LOGW("音声を無効化して続行します (null バックエンドへフォールバック)");
    vm_audio_run_null(a);   /* ready_ev はフォールバック先が set する */
    return;
}
```

音が出ないだけでモデムとしての機能 (AT / PPP / NAT) は完全に動作します。
音声はあくまで「懐古のための演出」なので、ここで起動を失敗させません。

### フォーマット交渉

固定値を要求せず、順に妥協していきます。

1. `SND_PCM_FORMAT_FLOAT_LE` → 失敗したら `S16_LE` (実行時に変換)
2. `snd_pcm_hw_params_set_channels_near(1)` → mono が無ければ得られた
   チャンネル数へ複製
3. `snd_pcm_hw_params_set_rate_near(8000)` → 8kHz が無ければ得られた
   レートへ内部リサンプラ (`vm_audio_backend_resampler_init()`) で変換
4. period 20ms / buffer 80ms (4 period) を `*_near` で要求

VIM1 の HDMI は典型的に「S16_LE / 48000Hz / stereo」しか受け付けないため、
この 3 つの妥協パスは**実際に通る経路**です。LD_PRELOAD で
alsa-lib の応答を差し替えて全分岐を検証済みです。

### リアルタイム レンダ スレッドの規律

レンダ ループ内では以下を**一切行いません**。

- `malloc` / `free` (バッファはループ開始前に確保)
- mutex のロック (リング バッファはロックフリー SPSC)
- ファイル I/O や `printf`

`snd_pcm_wait()` のタイムアウトは無限ではなく 200ms です。無限で待つと
`quit` フラグを確認できず、終了時にスレッドが固まります。

### SCHED_FIFO を使わない理由

WASAPI 版の MMCSS に相当するのは `SCHED_FIFO` ですが、意図的に採用して
いません。root か `RLIMIT_RTPRIO` の設定が必要で、優先度を誤ると
**システム全体が固まります**。8kHz mono / 80ms バッファという極めて緩い
要求に対して、リスクが利益を大きく上回ります。

### glibc の機能テストマクロに関する注意

`-std=c99` は `__STRICT_ANSI__` を定義するため glibc が POSIX 宣言を隠し、
`alsa/global.h` が `struct timespec` を自前定義して衝突します。そのため
`vm_audio_alsa.c` は `_POSIX_C_SOURCE` / `_DEFAULT_SOURCE` を
**`vm_audio_internal.h` (内部で `pthread.h` を引く) より前に**定義しています。
この include 順序は動作の前提です。

---

## Step 3: シグナル処理 (`src/main.c`)

Windows 専用の初期化と終了処理を POSIX に置き換えます。

| Windows | Linux (POSIX) |
|---|---|
| `WSAStartup` (`vm_winsock_init`) | **不要**。`socket()` はそのまま使える |
| `SetConsoleCtrlHandler` | `sigaction(SIGINT / SIGTERM / SIGHUP)` |
| (SIGPIPE 相当なし) | `SIGPIPE` を `SIG_IGN` |

`vm_winsock_init()` / `vm_winsock_cleanup()` の呼び出しは `#ifdef _WIN32` で
完全に囲みました。`vm_winsock.c` には POSIX 用の no-op も残っていますが、
「Linux では初期化不要」という設計意図をコード上で明示するため、
**呼び出し自体を Windows 限定**にしています。

### ★最重要★ シグナルはどのスレッドに配送されるか分からない

Ctrl-C (`SIGINT`) は「そのシグナルをブロックしていない**任意の 1
スレッド**」に配送されます。VModem は Step 2 の ALSA レンダー スレッドを
持つので、Ctrl-C が**音声スレッドに配送される事が普通に起こります**。

`vm_modem_request_stop()` はフラグを立てるだけなので停止自体は成立します
が、その場合**メインスレッドの `poll()` は `EINTR` で抜けません**。
つまり停止が最大 200ms (`VM_MODEM_COM_BLOCK_MS`) 遅れます。

そこで次の順序を取ります。

1. **`vm_modem_create()` より前**に対象シグナルを `pthread_sigmask` で
   ブロックする
2. モデム生成 (= ALSA レンダー スレッド生成) — スレッドは生成時に
   シグナルマスクを**継承**するので、レンダー スレッドは以後これらを
   永久にブロックしたままになる
3. `sigaction` でハンドラを設置
4. メインスレッドだけマスクを解除する

これで配送先がメインスレッドに限定されます。加えて `sa_flags` に
**`SA_RESTART` を付けません**。付けると `read()` が自動再開してしまい、
`EINTR` で抜けさせた意味が無くなります (`poll()` は `SA_RESTART` の
有無に関わらず再開されませんが、`read()`/`write()` は再開されます)。

実測で **停止までの遅延は 9ms** です (この設計を採らないと最大 200ms)。

### SIGPIPE で無言のうちに死ぬ問題

Linux の既定では、切断済みソケットへ `write` すると**プロセスが SIGPIPE で
即死**します (既定動作 = Term)。Windows にこの概念はありません。

VModem は libslirp 経由で多数の TCP ソケットを扱います。相手が RST を
返した直後に送信すれば SIGPIPE が飛びます。放置すると「ダイアルアップ中に
ブラウザを閉じたら vmodem が消えた」という極めて再現しにくい障害になり、
**ログにも何も残りません**。

よって `SIGPIPE` を `SIG_IGN` にします。`write()` は `EPIPE` を返すように
なり、libslirp のエラー処理に正しく載ります。

### その他の実装ポイント

| 項目 | 内容 |
|---|---|
| ハンドラ内の処理 | `vm_modem_request_stop()` (フラグ代入) と `write(2)` のみ。上流は `fprintf` を使っているが、POSIX では async-signal-safe でないため `write` に置換 |
| `errno` の保存 | ハンドラは「`read()` が -1 を返した直後、`errno` を読む前」に割り込む事がある。ハンドラ内で `errno` を書き換えると呼び出し側が `EAGAIN` を `EINTR` と誤認する |
| 2 回目の Ctrl-C | `_exit(128 + signo)` で即座に落とす。`_exit()` は async-signal-safe で、`atexit` も stdio のフラッシュも行わないのでロックを持ったまま割り込んでも安全 |
| `SIGHUP` も捕捉 | 端末が消えた時 (SSH 切断など) も後片付けをする |
| ハンドラ実行中の再入 | `sa_mask` に全終了シグナルを入れ、「1 回目」の判定が二重に走らないようにする |
| `g_modem` の寿命 | `vm_modem_destroy()` の**前に** `NULL` を代入し、破棄中のシグナルが解放済みメモリを触らないようにする |
| 起動中の取りこぼし | ブロック中に届いたシグナルは解除の瞬間に配送される。イベントループに入る前に確認する (systemd が起動直後に stop した場合) |

ハンドラを設置**できなかった**場合は起動を中止します。ハンドラ無しで走ると
Ctrl-C が既定動作 (即 Term) になり、PPP を切らず tty も復元されずに死ぬため、
次回の起動が「ポートが壊れている」状態から始まります。

### Linux 向けのエラーメッセージ

モデムを開けない原因の 8 割は「デバイスが無い」「他プロセスが掴んでいる」
「権限が無い」の 3 つで、対処が全く違います。Linux ではそれぞれの確認
コマンド (`ls -l` / `fuser -v` / `usermod -aG dialout`) と、
`serial-getty@ttyGS0` の無効化手順まで表示します。

また `com_port` が `/dev/` で始まらない場合は警告を出します。上流の既定値は
com0com の `CNCB0` なので、そのまま Linux で起動すると黙って PTY が
割り当てられ、「USB で繋がらない」と悩む事になります。

---

## Step 4: ロガーの POSIX 対応 (`src/core/vm_log.c`)

Windows 専用 API を `#ifdef _WIN32` の内側に閉じ込め、POSIX 側は
`fprintf`/`fwrite` + `pthread_mutex` だけで完結させます。

| Windows | Linux (POSIX) |
|---|---|
| `CRITICAL_SECTION` | `pthread_mutex_t` (静的初期化子) |
| `GetCurrentThreadId()` | `syscall(SYS_gettid)` |
| `GetLocalTime()` + `localtime_s` | `clock_gettime()` + `localtime_r()` |
| `OutputDebugStringA()` | (相当物なし。stderr のみ) |

なお上流には `OutputDebugStringA` の呼び出しは**ありません**でした
(手順書の指示は予防的なものでした)。実際に Windows 専用だったのは
`CRITICAL_SECTION` / `GetCurrentThreadId` / `GetLocalTime` / `localtime_s`
の 4 つで、いずれも既に `#ifdef` で分岐済みでした。そのうえで、
移植の過程で見つけた**実害のある 3 つの問題**を修正しています。

### 問題 1: 秒とミリ秒を別の時計から取っていた

移植前の実装は

```c
t  = time(NULL);                       /* 秒     */
clock_gettime(CLOCK_REALTIME, &ts);    /* ミリ秒 */
```

と **2 回別々に**時刻を読んでいました。この 2 行の間に秒が繰り上がると
「秒は繰り上がり前、ミリ秒は繰り上がり後」の組み合わせになり、

```
12:00:00.998
12:00:00.001   <- 巻き戻って見える
12:00:01.003
```

のようにログ上で時間が逆行します。ダイアルアップのタイミング問題
(LCP の再送間隔、DCD を上げる順序) を追う時にログの時刻が信用できないのは
致命的なので、**1 回の `clock_gettime()` から秒とミリ秒の両方を作る**
ように直しました。Windows 側も `GetLocalTime()` の `SYSTEMTIME` だけで
完結させています。

さらに**時刻の取得をロックの内側**に移しました。外で取ると

```
スレッド A: 時刻取得 (12:00:00.100) ─────────┐ (プリエンプト)
スレッド B: 時刻取得 (12:00:00.150) → 書込   │
スレッド A: ────────────────────────────────┘ → 書込
```

の順序が起こり、ファイル上では `.150` の行が `.100` の行より前に現れます。
ロック内でやるのはスタックバッファへの `snprintf` だけで syscall を伴わない
ので、コストは無視できます。

### 問題 2: `pthread_self()` はログに出しても役に立たない

glibc の `pthread_self()` はスレッド記述子の**アドレス**なので、ログには
`140234876262208` のような 15 桁が並びます。しかもこの値は
`top -H` / `ps -L` / gdb `info threads` の**どの表示とも一致しない**ため、
「音が途切れた時に動いていたのはどのスレッドか」を突き合わせられません。

Linux では代わりにカーネルの TID (`gettid`) を出します。これは
`top -H` の PID 列・`ps -L` の LWP 列・`/proc/<pid>/task/<tid>` と完全に
一致するので、ALSA レンダー スレッドとイベントループの区別が目で見て
付きます。`gettid()` のラッパ関数は glibc 2.30 以降にしか無く、かつ
`-std=c99` では隠れるため `syscall(SYS_gettid)` を直接呼びます
(Linux 2.4.11 以降で常に使える)。

### 問題 3: 1 行が複数回の `write(2)` に分割される

`stderr` は行バッファではなく**無バッファ**なので、`fprintf` に複数の変換
指定を渡すと glibc は書式単位で複数回 `write(2)` を発行する事があります。
VModem は ALSA レンダー スレッドを持つマルチスレッドなので、その隙間に別
スレッドの出力が割り込むと 1 行が途中で混ざります。ミューテックスで守って
いても、`printf()` 経由の stdout や libslirp / alsa-lib が直接 stderr に
吐く警告とは同期できません。

よって「まず 1 本のバッファに組み立て、`fwrite` で 1 回だけ書く」形に
しました。PIPE_BUF (4096) 以下の行は事実上分断されません。

### 修正したバッファ境界の問題

`vm_log_hexdump()` は

```c
off += snprintf(line + off, sizeof(line) - off, ...);
```

を繰り返していました。`snprintf` は「書き込んだ長さ」ではなく
**「書き込みたかった長さ」**を返すため、一度でも切り詰めが起きた瞬間に
`off > sizeof(line)` となり、次の `sizeof(line) - off` が `size_t` の
巨大な値に化けて**バッファ外へ書き込みます**。現行のフォーマットは
75 バイトで収まっているので事故は起きていませんが、「フォーマットを
1 文字足したら壊れる」コードを残す理由が無いので、境界を守る追記
ヘルパに置き換えました。

### その他の実装ポイント

| 項目 | 内容 |
|---|---|
| 静的初期化 | `PTHREAD_MUTEX_INITIALIZER`。`vm_log_write()` は `vm_log_init()` より前に呼ばれる事がある (`main.c` は設定読込の失敗を先に報告する) |
| `fopen`/`fclose` はロック外 | ネットワーク FS だと数百 ms 掛かり、その間ログ全体が止まる |
| ログファイルは行バッファ | `setvbuf(_IOLBF)`。全バッファのままだと SIGKILL や電源断で直近 4KB が消える。障害解析で最も重要なのは「落ちる直前の数行」 |
| ANSI 色 | `isatty(2)` が真の時だけ。journald / リダイレクト先にエスケープ列が入ると grep が壊れる。**ファイル出力には常に付けない** |
| 失敗理由を添える | `sudo` で起動した後に一般ユーザで起動し直すと root 所有の `vmodem.log` を開けず `EACCES` になる。`strerror()` を必ず出す |

---

## Step 5: NAT の時計と外向きアドレス検出

Step 5 の成果物は 3 つです。`vm_nat_slirp.c` 本体の移植は Step 6 で
行いました (下の「Step 6」節を参照)。

| 対象 | 変更 |
|---|---|
| `src/net/vm_nat.c` | `GetTickCount64()` → `clock_gettime(CLOCK_MONOTONIC)` |
| `src/net/vm_hostroute_linux.c` | **新規**。`getifaddrs()` による外向き IPv4 の検出 |
| `src/net/vm_hostroute.c` | Linux ビルドから除外 (翻訳単位ごと `_WIN32` で囲む) |

### 5-1: なぜ `CLOCK_MONOTONIC` でなければならないのか

`vm_nat_now_ns()` は libslirp に `cb_clock_get_ns` として渡される
**唯一の時計**で、TCP の再送タイマ・DHCP のリース期限・ARP キャッシュの
寿命がすべてここに乗ります。候補を検討した結果は以下の通りです。

| 候補 | 採否 | 理由 |
|---|---|---|
| `CLOCK_REALTIME` | ✗ | NTP や `date` で**巻き戻る**。巻き戻った瞬間に libslirp は「まだ時間が経っていない」と判断し、再送が止まって接続が固まる。**VIM1 は RTC バックアップ電池を持たないため、起動直後の時刻は必ず狂っており、NTP 同期で必ず大きく飛ぶ** |
| `CLOCK_MONOTONIC_RAW` | ✗ | NTP の周波数補正を受けないため、実時間と最大 500ppm ずれる。Linux 固有で移植性も低い |
| `CLOCK_BOOTTIME` | △ | サスペンド中も進む。VIM1 は常時通電なので差は出ないが、`CLOCK_MONOTONIC` より対応環境が狭い |
| `CLOCK_MONOTONIC` | ✅ | 巻き戻らず、NTP の周波数補正は受ける。POSIX 標準 |

Windows 側も同時に改善しました。`GetTickCount64()` の分解能は
**15.6ms** (タイマ割り込み周期) しかなく、libslirp の 100ms 単位の
タイマ判定には粗すぎます。`QueryPerformanceCounter` を第一候補にし、
取得に失敗した時だけ `GetTickCount64()` に落とします。

なお ns への換算は
`sec * 1e9 + (rem * 1e9) / freq` と**商と余りに分けて**計算しています。
`count * 1000000000 / freq` と素朴に書くと、QPC の周波数が 10MHz の
環境で **約 29 年でオーバーフロー**しますが、それ以前に
`count * 1000000000` が 64bit を溢れて即座に破綻します。

### 5-2: 失敗時に 0 を返してはいけない

`clock_gettime` が失敗する状況はほぼありませんが、失敗時に 0 を返すと
libslirp から見て時刻が起動直後に巻き戻り、**全てのタイマが即発火する**
という最悪の壊れ方をします。そこで `last_ns + 1ms` を返して
「僅かに進んだ」ことにし、`VM_LOGE` は**初回だけ**出します
(毎回出すとログが溢れて本当の原因が埋もれます)。

さらに最終防衛線として、返す直前に `now_ns < last_ns` なら
`last_ns` に切り上げます。これは「巻き戻らない」という libslirp が
依存する不変条件を、時計の実装に関係なく関数の出口で保証するためです。

### 5-a: 外向きアドレスの検出 (`vm_hostroute_linux.c`)

Windows 版と**同じインタフェース** (`vm_hostroute.h`) を実装するので、
呼び出し側 (`vm_nat_slirp.c`) を `#ifdef` で分岐させる必要がありません。
これは libslirp の `SlirpConfig.outbound_addr` に渡す値で、
存在しないアドレスを渡すと `slirp_bind_outbound()` の `bind()` が
`EADDRNOTAVAIL` で落ち、**外向き接続が全滅**します
(実際に `bind()` で `EADDRNOTAVAIL(99)` を再現して確認しました)。

検出方法として 3 案を比較しました。

| 方法 | 採否 | 理由 |
|---|---|---|
| `/proc/net/route` の解析 | ✗ | テキスト形式に依存。IPv6 やポリシールーティング (`ip rule`) を考慮できない |
| rtnetlink を手書き | ✗ | 最も正確だが数百行になる。Step 5 の範囲に対して過大 |
| `connect()` + `getsockname()` | ✅ | **カーネルの経路表そのものに聞く**ので、ポリシールーティングも VPN も自動的に反映される |

`SOCK_DGRAM` に対する `connect(2)` は**パケットを 1 バイトも送りません**。
カーネル内で経路検索を行って送信元アドレスを確定するだけなので、
`getsockname(2)` でそれを読み出せます。`8.8.8.8:53` を宛先に使いますが、
**そこへ通信は発生しません** (名前解決も行いません)。

検出は 4 段構えです。

1. `connect()` 探索で候補を得る
2. その候補が**実 NIC のもの**であることを `getifaddrs()` で照合する
3. 照合に失敗したら、実 NIC を総当たりで走査する
4. それでも決まらなければ **0 を返す** (= 判定不能)

**0 は「失敗」ではなく仕様上正当な戻り値です。** libslirp 側は
`outbound_addr` が NULL の時 `slirp_bind_outbound()` を no-op にするので、
「カーネルの既定動作に任せる」という安全側に倒れます。単一 NIC の
環境ではそもそも `outbound_addr` を設定する必要がありません。

除外する対象は指示書の要求通りです。

| 除外対象 | 判定方法 |
|---|---|
| ループバック `127.0.0.0/8` | アドレス範囲 + `IFF_LOOPBACK` |
| APIPA `169.254.0.0/16` | アドレス範囲 (ただし後述の例外あり) |
| PPP | `IFF_POINTOPOINT` |
| トンネル / 仮想 NIC | IF 名の接頭辞 (`tun` `tap` `ppp` `docker` `veth` `br-` `virbr` `vnet` `wg` `tailscale` `zt` `gre` `sit` `ip6tnl` `erspan` `dummy` `usb` `rndis` `lo`) |
| 未稼働の NIC | `IFF_UP` かつ `IFF_RUNNING` を要求 |

`usb` / `rndis` を除外しているのは、**それが本エミュレータ自身の
USB Gadget 側**だからです。ここに bind すると PC 側へ折り返す
自己参照ループになります。

#### APIPA の例外について

指示書は `169.254.0.0/16` の除外を要求していますが、**カーネルの
既定経路がその範囲のアドレスを指している環境が実在します**
(本リポジトリの開発 sandbox がまさにそれで、`default via 169.254.0.22
dev eth0` です)。この場合 APIPA を機械的に除外すると
「使える唯一のアドレスを捨てて 0 を返す」ことになります。

そこで段階 1〜3 では通常通り APIPA を除外し、**そこで何も決まらなかった
時に限り**、段階 1 の候補 (= カーネル自身がグローバル向け経路として
選んだアドレス) を実 NIC 上にあることを再確認してから採用します。
採用時は理由をログに出すので、意図しない挙動と区別できます。

```
hostroute: 外向きアドレスに 169.254.0.21 (eth0) を採用
           (169.254/16 だがカーネルがグローバル向け経路として
            選んだ実 NIC なので使用可能と判断)
```

### 5-3: `vm_hostroute.c` の除外は Makefile だけでは足りない

指示書は「Makefile 側で `vm_hostroute.c` を除外する」ことを要求して
いますが、**それだけでは不十分**です。移植前の `vm_hostroute.c` は
`_WIN32` の否定側に「常に 0 を返す POSIX スタブ」を持っていたため、
両ファイルを同時にコンパイルすると

```
/usr/bin/ld: multiple definition of vm_hostroute_pick_outbound_ip
             vm_hostroute_linux.c: first defined here
```

でリンクが落ちます (実際に再現させました)。上の「Step 5 までで動く構成の
例」のように `.c` を手で並べるビルドや、`src/net/*.c` をワイルドカードで
拾う CI では、Makefile の `SRCS` 指定は何の防御にもなりません。

そこで**翻訳単位ごと `_WIN32` で囲みました**。Linux では
`vm_hostroute.c` が外部シンボルを 1 つも定義しなくなるので、
誤って両方渡しても衝突しません (`nm` で確認済み)。ISO C は空の翻訳
単位を許さない (C99 6.9) ので、末尾にダミーの `typedef` を置いています。

---

## 検証状況

Step 1〜5 で追加・変更した翻訳単位は、警告ゼロでコンパイルできます
(`-Wall -Wextra -Wpedantic`)。

```bash
gcc -c -O2 -std=c99 -Wall -Wextra -Wpedantic -Iinclude \
    src/serial/vm_serial.c src/serial/vm_serial_linux.c \
    src/audio/vm_audio.c src/audio/vm_audio_alsa.c src/audio/vm_audio_null.c \
    src/core/vm_log.c src/main.c \
    src/net/vm_nat.c src/net/vm_hostroute_linux.c src/net/vm_hostroute.c
```

最後の `vm_hostroute.c` は Linux では**シンボルを 1 つも生成しない**
ことも確認済みです (Step 5-3)。

```bash
$ gcc -c -O2 -std=c99 -Wall -Wextra -Wpedantic -Iinclude \
      src/net/vm_hostroute.c -o /tmp/hrw.o && nm /tmp/hrw.o
$          # 出力なし = 外部シンボルなし = 重複定義が起こり得ない
```

Step 3・4 の完了により、`--net none` / `--net loopback` 構成では
**実行可能なバイナリが作れ、起動・常駐・正常終了まで通ります**
(「移植の進捗」節のコマンド例を参照)。

### Step 5 の新規テスト

`tests/test_linux_step5.c` が Step 5・5-a の実行時検証を行います
(**29 項目すべて成功 / 省略 0**)。

```bash
# ★ vm_hostroute.c と vm_hostroute_linux.c を意図的に両方渡す ★
#   リンクが通ること自体が Step 5-3 (除外) の検証になっている
gcc -O2 -std=c99 -Wall -Wextra -Iinclude tests/test_linux_step5.c \
    src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c \
    src/net/vm_eth.c src/net/vm_hostroute_linux.c src/net/vm_hostroute.c \
    src/core/vm_log.c src/core/vm_types.c -lpthread -o test_linux_step5
./test_linux_step5
```

外向き IP の検出結果はマシンごとに違うので、**値そのものを assert して
いません**。代わりに「どの環境でも成り立つ性質」を検証します。
検出できない環境 (ネットワーク未接続の CI 等) では 0 が返りますが、
それは安全な失敗なので OK と判定し、値の検証だけを SKIP します。

検証内容:

| 節 | 検証する事 |
|---|---|
| 5-1 | 初回が正の値である事 (0 を返さない) / 連続呼び出しが非減少である事 / int64 のオーバーフロー領域に入っていない事 |
| 5-2 | **200ms の `nanosleep` を基準時計と突き合わせ**、誤差 5ms 未満である事。ms/ns のスケール取り違えを確実に捕まえる (1000 倍ずれれば差は 200ms 級になる) |
| 5-3 | 20 万回呼んで**最小増分が 1ms 未満**である事 = `GetTickCount64` 相当の粗い時計ではない事 |
| 5-4 | 4 スレッド × 5 万回 = 20 万回で**一度も巻き戻らない**事 |
| 5-a-1 | 検出した IP に**実際に `bind()` できる**事 / `getifaddrs` の一覧に存在する事 / ループバックでない事 |
| 5-a-2 | 選ばれた NIC が仮想 NIC の接頭辞に一致しない事 / `IFF_LOOPBACK`・`IFF_POINTOPOINT` が立っていない事 / `IFF_UP` である事 |
| 5-a-3 | 自分自身を `/32` で除外すると同じ IP を返さない事 / `exclude_mask=0` が「除外なし」として扱われる事 / `192.168.99.0/24` の除外が無関係な NIC に影響しない事 |
| 5-a-4 | `name_out=NULL` で落ちない事 / `size=0` で**一切書き込まない**事 / 4 バイトバッファでも**前後の番兵を壊さず NUL 終端する**事 |
| 5-a-5 | **3000 回連続で呼んで結果が変わらない**事 = 内部の `socket()` の閉じ忘れ (fd リーク) が無い事。既定の fd 上限 1024 を余裕で超える回数にしてある |
| 5-5 | 両 hostroute ファイルが同時にリンクできた事 / IF 名が返る (= 常に 0 を返すスタブではない) 事 |

実行時には答え合わせ用に `getifaddrs()` の一覧も表示します。

```
このホストの IPv4 アドレス一覧 (答え合わせ用, 2 件)
   127.0.0.1        lo         flags=0x00010049 UP RUN LOOP
   169.254.0.21     eth0       flags=0x00011043 UP RUN
```

Step 5 の変更が既存の NAT / PPP を壊していない事も確認済みです
(`tests/test_nat.c` **92 項目成功**、`tests/test_linux_step34.c`
**53 項目成功**、`tests/test_linux_port.c` **30 項目成功**)。

### Step 3・4 の新規テスト

`tests/test_linux_step34.c` が Step 3・4 の実行時検証を行います
(**53 項目すべて成功 / 省略 0**)。

```bash
# 先に vmodem 本体をビルドしておく (「移植の進捗」節のコマンド)
gcc -O2 -std=c99 -Wall -Wextra -Iinclude tests/test_linux_step34.c \
    src/core/*.c -lpthread -o test_linux_step34
./test_linux_step34 ./vmodem
```

Step 3 はプロセス全体の振る舞い (シグナルの配送先・終了コード) なので
ライブラリ関数として呼び出せません。そのため**実際に vmodem を起動して
シグナルを送り、`/proc` とログを観測する**方式を取っています。
バイナリのパスは第 1 引数か `VMODEM_BIN` で指定し、見つからなければ
Step 3 の節を SKIP します (ビルド系の整備は Step 7 の範囲なので、
テストがビルド方法を仮定しないようにしています)。

検証内容:

| 節 | 検証する事 |
|---|---|
| 4-1 | 閾値以下が 1 バイトも出ない事 / INFO は簡潔形式 / ファイルに ANSI 色が混入しない事 |
| 4-2 | **1.2 秒間書き続けて秒の繰り上がりを必ず踏ませ**、570 行の時刻が一度も巻き戻らない事 |
| 4-3 | 6 スレッド × 400 行 = 2400 行が 1 行も分断・混在しない事 / TID が 6 種類記録され `1..pid_max` の値域に収まる事 |
| 4-4 | 4KB の本文・256 バイト全域の hexdump・端数 3 バイト・長さ 0・NULL で行の形が壊れない事 |
| 4-5 | 開けないパスで `VM_ERR_IO` / 二重 `shutdown` / `shutdown` 後の書き込み / `fmt=NULL` が安全な事 |
| 3 | SIGINT・SIGTERM・SIGHUP で終了コード 0 / **停止遅延が 100ms 未満**である事 |
| 3 | `/proc/<pid>/status` で `SigCgt` に 3 シグナル・`SigIgn` に SIGPIPE が立っている事 |
| 3 | `/proc/<pid>/task/*/status` で**メインスレッド以外の全スレッドが終了シグナルをブロックしている**事 |
| 3 | 2 回連続の SIGINT で必ず終了し、シグナルの既定動作で殺されていない事 |

テスト用 config は `audio_enable = true` にしています。ALSA デバイスは
CI にも VIM1 にも無いのですが、Step 2 の実装は null へフォールバックした
上でレンダー スレッドを生成するので、「シグナルをブロックしたまま走る
スレッド」を再現できます。`false` にするとスレッドが生えず、難所 2 の
検証が空振りします。

2 回連続 SIGINT の期待値は **0 と 130 の両方を正解**としています。1 発目の
処理が速ければ 2 発目が届く前に正常終了して 0 になり、間に合えば
`_exit(128 + SIGINT)` = 130 になります。ここで検証したいのは「必ず終わる」
事であって「必ず 130 になる」事ではありません (後者はタイミング依存で
CI が不安定になります)。

### Step 1・2 のテスト

`tests/test_linux_port.c` が Step 1・2 の実行時検証を行います
(**30 項目すべて成功**)。

```bash
gcc -O2 -std=c99 -Iinclude tests/test_linux_port.c \
    src/core/*.c src/dsp/*.c src/audio/*.c src/serial/*.c \
    -lm -lpthread -lasound -lutil -o test_linux_port
./test_linux_port
```

`/dev/ttyGS0` が無い環境でもテストできるよう、`openpty()` の疑似端末を
ttyGS0 の代役にしています (master 側が「Windows の RAS」、slave 側が
「`/dev/ttyGS0`」)。PTY も `gs_tty_ops` と同様に一部の制御線 ioctl が
使えないため、狙った分岐を実際に通せます。

検証内容:

- ALSA デバイスが無い時に null へフォールバックする事
- termios が raw モードになる事 (`ICANON` / `ECHO` / `OPOST` が落ちている)
- PPP フレーム相当のバイト列が無改変で往復する事
- 制御線が使えない時に `get_dtr()` が `true` を返す事 (誤ハングアップ防止)
- close で termios が復元される事
- 存在しないデバイスに対して有用なエラーメッセージが出る事

### ALSA バックエンドの実機相当プロファイル

3 通りのデバイス プロファイルで、いずれも underrun 0 を確認しています。

| プロファイル | 交渉結果 |
|---|---|
| デバイス無し | null へフォールバック |
| `null` プラグイン | FLOAT_LE / 8000Hz / mono / period 160 frames (20.0ms) / buffer 640 frames (80.0ms) |
| VIM1 HDMI 相当 (LD_PRELOAD でレート・形式を強制) | S16_LE (float から変換) / 48000Hz (8kHz からリサンプル) / stereo / period 960 frames (20.0ms) / buffer 3840 frames (80.0ms) |

### 既存テストへの回帰なし

上流のテストは以下の通りで、**移植前の上流ツリーと完全に同じ結果**です。

```
test_at  test_core  test_dsp  test_resample  test_sequence  test_ppp  test_nat   → 7/7 PASS
test_linux_port (Step 1・2)                                                       → 30/30 PASS
test_linux_step34 (Step 3・4)                                                     → 53/53 PASS
test_audio                                                                       → ★Step 7 でビルド可能になった (18/18 PASS)
```

Step 7 で `Makefile.linux` を用意した現在は、**全 13 本のテストが
`make -f Makefile.linux check` 一発でビルド・実行でき、
623 項目すべて成功します** (下記「Step 7」節を参照)。

`test_core` には既存のロガー検証が 24 項目含まれており、`vm_log.c` を
POSIX 向けに書き直した後も**全項目そのまま成功**します。`vm_log.h` の API を
一切変えていないため、上流のテストから見た振る舞いは同一です。

`test_nat` のビルドには `-Isrc/net` が必要です (`vm_nat_internal.h` が
`src/net/` にあるため)。上流の README に記載が無いだけで、移植による
変化ではありません。

`test_audio` は上流と 1 バイトも違わないファイルで、`-std=c99` 時に
`_POSIX_C_SOURCE` が無いため `nanosleep` / `struct timespec` を解決できない
という**既存の問題**でした (上流ツリーでも同一のエラーが再現します)。
**Step 7 で解決済み**です。上流ファイルは書き換えず、`Makefile.linux` の
`TEST_CFLAGS` にだけ `-D_POSIX_C_SOURCE=200809L` を足す形で通しました
(理由は「Step 7」節の罠 (5) を参照)。

---

## Step 6: `vm_nat_slirp.c` の Linux 対応

Step 6 の要求は 3 つ (6-a / 6-b / 6-c) で、**すべて `src/net/vm_nat_slirp.c`
1 ファイルの中で完結します**。新規ファイルは作っていません。

| 小項目 | 指示書の要求 | 実際に何が問題だったか |
|---|---|---|
| 6-a | poll 抽象の確認。「WSAPoll 専用フィルタが Linux 側に影響しないか確認」 | **影響していた**。`slirp_to_native()` の `#ifdef _WIN32` が PRI の 1 行にしか掛かっておらず、ERR/HUP を落とす Windows のための制約が POSIX にも適用されていた |
| 6-b | `GetProcAddress` の Linux 対応 | **Linux でリンクが通らなかった**。`slirp_pollfds_fill_socket` を静的参照していたため、apt の libslirp 4.8 に対して `undefined reference` |
| 6-c | Winsock 型の除去 | 型の隔離自体は概ね出来ていたが、**EINTR 未対応**と**`native == 0` の Windows 用細工が POSIX に漏れていた** |

### 6-a: WSAPoll 専用フィルタが Linux 側に漏れていた

移植前の `slirp_to_native()` はこうなっていました。

```c
if (ev & SLIRP_POLL_IN)  r |= POLLIN;
if (ev & SLIRP_POLL_OUT) r |= POLLOUT;
#ifdef _WIN32
if (ev & SLIRP_POLL_PRI) r |= POLLRDBAND;   /* ← ここだけ分岐 */
#else
if (ev & SLIRP_POLL_PRI) r |= POLLPRI;
#endif
/* SLIRP_POLL_ERR / SLIRP_POLL_HUP は意図的に無視する */
```

最後の「ERR / HUP を無視する」は **Windows の WSAPoll が
`events` にこれらを立てると `WSAEINVAL` で即死するから**という
Windows 固有の理由です。ところが `#ifdef` の外にあるため、
Linux ビルドでも ERR / HUP が落ちていました。

POSIX の `poll(2)` はこれらを `events` で単に無視するだけで
エラーにしないので、落とす必要がありません。落とすと 2 つ損をします。

1. **`events` が空になる場合の意味が変わる。** libslirp が
   `SLIRP_POLL_ERR` 単独を要求すると変換結果が 0 になり、
   後述の `native == 0` の細工で **`POLLIN` にすり替えられていました**。
   「接続失敗の検出」が「読み取り可能待ち」に化けるという致命的な誤りです。
2. **デバッグ時に libslirp の意図が消える。** poll をダンプしても
   何を待っているのか分からず、「繋がらない」の調査時間が伸びます。

そこで **関数ごと Windows 版と POSIX 版に完全に分割**しました。
POSIX 版は libslirp が要求した通り全ビットを素通しします。これは
QEMU の `net/slirp.c` (`slirp_poll_to_gio`) が IN/OUT/PRI/ERR/HUP を
すべて通しているのと同じ方針で、libslirp が想定している唯一の正しい
使い方です。

`poll(2)` が本当に `POLLERR|POLLHUP` を拒否しないことは
`tests/test_linux_step6.c` で実測しています (憶測にしていません)。

```
[ OK ] events=POLLIN|POLLERR|POLLHUP|POLLPRI で poll が成功する (rc=1 errno=0)
[ OK ] ERR 単独が POLLERR になる (0 に潰れない = WSAPoll フィルタ非適用)
```

### 6-b: `GetProcAddress` → `dlsym`。Linux ではリンクすら通っていなかった

移植前は「ヘッダのバージョンで静的に分岐」していました。

```c
#ifdef _WIN32
    /* GetModuleHandleA + GetProcAddress */
#elif VM_SLIRP_HAVE_SOCKET_API              /* = SLIRP_CHECK_VERSION(4,9,0) */
    g_slirp_abi.fill_socket = slirp_pollfds_fill_socket;   /* ★静的参照★ */
#endif
```

`SLIRP_CHECK_VERSION` が見るのは **ヘッダ**のバージョンです。本
リポジトリは `include/libslirp.h` に **4.9.3** を同梱しているので常に真に
なりますが、Debian/Ubuntu が提供する `libslirp.so.0` は **4.8.0** で、
そこに `slirp_pollfds_fill_socket` は存在しません。結果:

```
/usr/bin/ld: /tmp/cc3UPCy1.o: in function `slirp_be_open':
vm_nat_slirp.c:(.text+0x7b3): undefined reference to `slirp_pollfds_fill_socket'
collect2: error: ld returned 1 exit status
```

移植前のコメントには「POSIX では ELF の遅延束縛のおかげで起動時全滅は
起きない」と書かれていましたが、**これは事実誤認**です。遅延束縛は
実行時の解決を遅らせるだけで、**リンク時の未定義参照は静的リンカが
その場で弾きます**。

Windows は DLL を同梱できるのでヘッダと DLL が必ず一致しますが、Linux は
ディストリのライブラリを使うのでずれる方が普通です。そこで
`dlsym(RTLD_DEFAULT, ...)` による実行時解決に置き換えました。

```c
union { void *ptr; vm_fill_socket_fn fn; } u;
u.ptr = dlsym(RTLD_DEFAULT, "slirp_pollfds_fill_socket");
g_slirp_abi.fill_socket = u.fn;   /* 無ければ NULL = 旧 API へ */
```

| 選択 | 理由 |
|---|---|
| `dlsym` (静的分岐でなく) | ヘッダとライブラリのバージョンがずれても動く。Linux では常態 |
| `RTLD_DEFAULT` (`dlopen` でなく) | `dlopen("libslirp.so.0", RTLD_NOLOAD)` は libslirp が未参照だと NULL を返す。`RTLD_DEFAULT` はプロセスのグローバルスコープを検索するので確実 |
| `union` 経由のキャスト | ISO C は `void*` ↔ 関数ポインタの直接変換を未定義とする。POSIX.1-2008 は `dlsym` の RATIONALE でこの回避策を明示的に要求している |
| `_GNU_SOURCE` を先頭で定義 | `RTLD_DEFAULT` / `RTLD_NOLOAD` は GNU 拡張。`-std=c99` (`__STRICT_ANSI__`) だと `<dlfcn.h>` から見えなくなる |

**リンク時に `-ldl` が必要です** (指示書の要求どおり)。glibc 2.34 以降は
libdl が libc に統合されたので実質 no-op ですが、それ以前や musl では
必須なので必ず付けてください。

これで **同梱ヘッダ 4.9.3 + apt のライブラリ 4.8.0** という
ずれた組み合わせでもリンクが通り、実行時に `NULL` を得て
`slirp_pollfds_fill()` (旧 API) へフォールバックします。

```
INF nat(slirp): libslirp 4.8.0 を検出 (共有ライブラリ) cfg.version 上限=5,
    fill=slirp_pollfds_fill (旧API・fd は int に切り詰められる)
INF nat(slirp): 旧 API を使うが Linux の fd は int なので
    切り詰めの問題は起きない
```

旧 API の `int` 切り詰めは **Win64 (LLP64, `SOCKET` = 8 バイト) 固有の
問題**で、Linux では `fd` がそもそも `int` なので情報が落ちません。
つまり 4.8 環境で旧 API へ落ちるのは実害のない正常な経路です。

### 6-c: Winsock 型の隔離 (機械的に検証)

指示書の「`WSAPOLLFD` / `SOCKET` / `INVALID_SOCKET` が
`#ifdef _WIN32` で正しく隔離されているか確認」は、目視ではなく
**プリプロセッサ出力を検査**して確定させました。

```bash
$ gcc -std=c99 -Iinclude -Isrc/net -DVMODEM_HAVE_LIBSLIRP \
      -E src/net/vm_nat_slirp.c |
  grep -E 'WSAPOLLFD|INVALID_SOCKET|WSAPoll|UINT_PTR|SOCKET_ERROR|GetProcAddress|GetModuleHandle|closesocket|WSAGetLastError|\bSOCKET\b'
$          # 一致 0 件
```

Linux ビルドの翻訳単位から Winsock 由来の識別子が完全に消えています。
隔離箇所は 4 つだけです。

1. poll 抽象ブロック (`WSAPOLLFD` / `WSAPoll` / `POLL*` 定数の補完)
2. `VM_SLIRP_HAVE_SOCKET_API == 0` 時の `vm_slirp_socket_t` 補完
3. `slirp_to_native()` (6-a で完全分離)
4. `cb_add_poll_fd()` の `UINT_PTR` キャスト (`_WIN32` 内のみ)

### 6-c: `EINTR` — Linux でだけ起きる、追いにくい通信破壊

`src/main.c` の `install_signal_handlers()` は **意図的に `SA_RESTART` を
付けていません** (付けるとシリアル読取の `read()` が自動再開し、Ctrl-C の
反応がタイムアウト 200ms 分遅れるため)。その代償として、シグナル配送中に
走っていた `poll()` は `-1` / `EINTR` で戻ります。

Windows の `WSAPoll` に `EINTR` は無いので、移植前のコードには
この処理がありません。放置すると:

```
rc < 0  →  slirp_pollfds_poll(slirp, select_error = 1, ...)
        →  libslirp 側は if (!select_error) { ...受信処理... }
        →  その周回のソケット I/O が丸ごとスキップされる
```

送信 (`slirp_input` 経由) だけは `poll` と無関係に動くので、症状は
**「時々パケットを落とす / 転送が固まる」**という極めて追いにくい形に
なります。`SIGWINCH` / `SIGCHLD` / プロファイラの `SIGPROF` /
デバッガの停止再開でも普通に起きます。

対策として `EINTR` を **タイムアウトと同一視** (`rc = 0`) し、
`revents` をゼロクリアしてから libslirp に渡します。
ループで再試行しない理由は、**終了シグナルを受けた時にそこから
抜け出せなくなり、`SA_RESTART` を付けないという `main.c` の設計と
矛盾する**からです。呼び出し元 (`vm_modem.c`) は 20ms 周期の
ポーリングループなので、待ち時間が短くなるのは無害です。

`EINTR` 以外の `poll` 失敗 (`EFAULT` / `EINVAL` / `ENOMEM`) は復旧
不能なので、`select_error` として伝えつつ**最初の 1 回だけログ**を出します
(無音で止まるのを防ぎ、かつログを溢れさせない)。

`EINTR` が実際に起きることもテストで実測しています。

```
[ OK ] poll が EINTR で戻った (50 ms 経過, 要求 1000 ms)
[ OK ] タイムアウト前に戻っている -> 自動再開していない
```

### 6-c: `native == 0` の Windows 用細工を POSIX から外した

`add_poll_common()` には OS 共通でこう書かれていました。

```c
if (native == 0)
    native = POLLIN;
```

Windows では `WSAPoll` に `events == 0` を渡すと実装によって
`WSAEINVAL` を返すため、ダミーが必要です。しかし POSIX では
`poll(events == 0)` は正常に動き、`POLLERR` / `POLLHUP` / `POLLNVAL` は
`revents` に載ります。ここで勝手に `POLLIN` を立てると、

- libslirp の意図と違う「読み取り可能待ち」にすり替わる
- 読めるデータがある fd で `poll` が即戻りして **busy loop** になる

という二重の劣化を招きます。よって POSIX では**何もしない**のが正しく、
`#ifdef _WIN32` で囲みました。ただし `events == 0` が来た事実は
「繋がらない」時の手掛かりになるので、**一度だけ** 警告を出します。

### 6-c: `outbound_addr` の Linux での再検証

`outbound_addr` はもともと **Windows の RAS がダイアルアップ確立時に
デフォルト経路を奪う**問題への対策です (自分のプロセスが
`sendto(8.8.8.8)` すると自分の仮想回線に送り返され `WSAENETUNREACH`)。
Linux では前提が違うので、同じコードで害が無いかを実測しました。

| 項目 | Windows | Linux |
|---|---|---|
| デフォルト経路を奪う主体 | RAS が既定で奪う | pppd の `defaultroute` だが**本実装は pppd を使わない** → 経路表は無変更 |
| `bind()` の意味 | strong host model (Vista 以降) なので**出力 IF も固定される** | weak host model なので**送信元 IP の固定のみ**。出力 IF は経路表で決まる |
| 必要性 | **必須** | 不要。ただし有害でもない (eth0 / wlan0 併存時に送信元が安定する) ので有効のまま |

実測で確認した 3 点:

1. 実 NIC の IP への `bind()` は成功する。
2. 存在しない IP (`192.168.77.55`) への `bind()` は
   **`errno 99` = `EADDRNOTAVAIL`** で失敗する。
   → DHCP で NIC の IP が変わった後は libslirp が古い `outbound_addr` を
   持ち続けるため、**全ての新規接続が即エラー**になります。
   切り分けは `strace -e trace=bind` か起動ログの bind 先の確認。
3. NIC の IP に bind した UDP ソケットから `127.0.0.1` へ送っても
   応答が返る。

> **【訂正】上記 3 の結論は誤りでした**
>
> 「NIC の IP に bind したソケットから `127.0.0.1` へ送っても応答が返る」
> という実測自体は正しいのですが、そこから
> 「よって systemd-resolved 相手でも DNS 中継は壊れない」と結論したのが
> 誤りです。この実測は**自作の素朴な UDP echo サーバ**に対して行ったもので、
> **systemd-resolved の stub listener を相手にしていませんでした**。
>
> systemd-resolved の stub listener (`127.0.0.53` / `127.0.0.54`) は
> **ローカルループバック インタフェース宛の問い合わせにしか応答しません**。
> `outbound_addr` によって送信元が実 NIC のアドレスに固定されると、
>
> ```
> src = 実 NIC のアドレス  →  dst = 127.0.0.53:53
> ```
>
> となり、stub listener は応答を返しません。結果として PPP は確立し、
> IP も配られ、NLA の HTTP チェックも通って「IPv4 接続: インターネット」と
> 表示されるのに、ブラウザは `ERR_NAME_NOT_RESOLVED` になります。
>
> **これが「Linux 移植で最も壊れそうだった箇所」の、実際に壊れていた形**です。
> 対策は「DNS 解決の環境依存」節を参照してください。

Linux で「繋がらない」時に見る順番:

1. 起動ログに bind 先 IP が出ているか
2. `ip route get 8.8.8.8` の `src` がその IP と一致するか
3. 一致しないなら `vm_hostroute_pick_outbound_ip()` の
   `connect(2)` プローブが別 IF を選んでいる

### `timer_fires == 0` は正常。異常と誤認しないこと

指示書は「インターネットに出られない時は `timer_fires` を確認せよ」と
書いていますが、**この構成では 0 が正常**です。libslirp が `timer_new` を
呼ぶ箇所は upstream 全体で 1 つだけです。

```c
/* src/ip6_icmp.c  icmp6_post_init() */
if (!slirp->in6_enabled) {
    return;                                       /* ← ここで即 return */
}
slirp->ra_timer = slirp_timer_new(slirp, SLIRP_TIMER_RA, NULL);
```

つまり **IPv6 Router Advertisement 専用**です。本実装は IPV6CP を
Reject して `cfg.in6_enabled = false` にしているので、タイマは 1 つも
作られません (4.8.0 / master 双方のソースで確認済み)。

切り分けはこう読みます。

| 観測 | 意味 |
|---|---|
| `timer_fires == 0` かつ `poll > 0` | **正常**。別の原因を疑う |
| `poll == 0` | poll ループまで到達していない |
| `eintr` が `poll` と同オーダーで増える | シグナル嵐。6-c の罠を踏んでいる |

終了ログにこの 3 つを出すようにしました。

```
INF nat(slirp): 終了 poll=10 timer_fires=0 eintr=0
    (timer_fires=0 は IPv6 無効時の正常値)
```

### Step 6 の新規テスト

`tests/test_linux_step6.c` が Step 6-a / 6-b / 6-c の実行時検証を行います
(**42 項目すべて成功 / 失敗 0 / 省略 0**)。

```bash
gcc -O2 -std=c99 -Wall -Wextra -Iinclude -Isrc/net \
    -DVMODEM_HAVE_LIBSLIRP \
    tests/test_linux_step6.c \
    src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c \
    src/net/vm_eth.c src/net/vm_hostroute_linux.c \
    src/core/vm_log.c src/core/vm_types.c \
    -lslirp -lpthread -ldl -o test_linux_step6
./test_linux_step6
```

**このテストがリンクできること自体が 6-b の検証です。** 移植前は
同じコマンドが `undefined reference to 'slirp_pollfds_fill_socket'` で
失敗しました。

Step 6 の成果物は「`#ifdef` の掛け方」がほとんどで実行時に観測できる値が
少ないため、検証を 3 層に分けています。

| 層 | 何を検証するか |
|---|---|
| ビルド時に確定する事実 | テスト側が同じ `#ifdef` 規則を再現し、写像表を突き合わせる。実装を変えたらテストも落ちるべきなので**規則の複製が正しい** |
| ライブラリへ問い合わせる事実 | `dlsym` / `slirp_version_string()` で ABI を実測。バージョンと `fill_socket` の有無の**整合性**だけを判定 (値は環境依存なので assert しない) |
| OS の振る舞い | `poll(2)` が `events` の ERR/HUP を本当に無視するか、`EINTR` が本当に起きるか、`bind` の意味論。**憶測にせず実測** |

検証内容:

| 節 | 検証する事 |
|---|---|
| 6-a-1 | libslirp が実際に要求する 4 種の組み合わせが素通しされる事 / **ERR 単独・HUP 単独が 0 に潰れない**事 (= WSAPoll フィルタ非適用) / `POLLNVAL` は `events` に立たない事 |
| 6-a-2 | `events=POLLIN|POLLERR|POLLHUP|POLLPRI` で `poll` が**成功する**事 (WSAPoll は `WSAEINVAL`) / `events=0` でも失敗しない事 |
| 6-a-3 | `events=0` の fd でも切断が `revents` に載る事 = 補正しなくても取りこぼさない事 |
| 6-b-1 | `dlsym(RTLD_DEFAULT, ...)` が解決できる事 / **ライブラリのバージョンと `fill_socket` の有無が整合する**事 / `union` キャストが成立する事 |
| 6-b-2 | `dlopen(RTLD_NOLOAD)` ではなく `RTLD_DEFAULT` を選んだ根拠の記録 |
| 6-b-3 | glibc のバージョンから `-ldl` の必要性を判定 |
| 6-c-1 | `struct pollfd.fd` が `int` である事 / `slirp_os_socket` が Linux では `int` である事 = **旧 API の切り詰めが無害**である事 / fd が `int` に収まる小さな正整数である事 |
| 6-c-2 | 子プロセスから 50ms 後に `SIGUSR1` を送り、**`SA_RESTART` 無しの `poll` が実際に `EINTR` で戻る**事 / タイムアウト前に戻る事 |
| 6-c-3 | `poll` 失敗時の `revents` が信用できない事 = `EINTR` 時にゼロクリアが必要な事 |
| 6-c-4 | 外向き IP に `bind` できる事 / **存在しない IP への `bind` が `EADDRNOTAVAIL(99)`** になる事 |
| 6-c-5 | NIC bind 済みソケットから `127.0.0.1` へ**実際に届く**事 (**注**: これを「libslirp の DNS 中継が壊れない事」の根拠としたのは誤りでした。相手が自作 echo サーバだったためです。systemd-resolved の stub listener はループバック IF 宛以外に応答しません → 後述の「DNS 解決の環境依存」節) |
| 6-c-6 | `vm_nat_create(SLIRP)` が成功する事 (= `slirp_new` が `cfg.version` を受け付けた = ABI 整合) / 20ms × 10 回 `poll` を回せる事 / **`timers_active == 0`** である事 |
| 6-c-7 | 20 回の `poll` に 1 回あたり 0.5ms 以上掛かる事 = **timeout の下限クランプが効いていて busy loop になっていない**事 |
| 範囲 | `Makefile.linux` / `scripts/setup-gadget-linux.sh` / `windows/vim1modem.inf` が**存在する**事 + `Makefile.linux` が `-ldl` / `-lslirp` を渡す事 (Step 7/8 実装に伴い、元の「存在しない事」から意味を反転させた) |

権限が足りない項目は FAIL ではなく SKIP にします (sandbox では ICMP
ソケットが作れない等)。環境依存の値 (NIC の IP、libslirp のバージョン) は
assert せず、「どの環境でも成り立つ性質」だけを判定します。

Step 6 の変更が既存のテストを壊していない事も確認済みです
(`tests/test_nat.c` **92 項目成功**、`tests/test_linux_step5.c`
**29 項目成功**)。

### Step 6 の次

Step 7 / 8 / 8-a は**実装済み**です。詳細は後述の
[Step 7: `Makefile.linux`](#step-7-makefilelinux) と
[Step 8: USB ガジェットと Windows 側 INF](#step-8-usb-ガジェットと-windows-側-inf)
を参照してください。

| 成果物 | Step | 状態 |
|---|---|---|
| `Makefile.linux` | 7 | ✅ 完了 |
| `scripts/setup-gadget-linux.sh` | 8 | ✅ 完了 |
| `scripts/99-vmodem.rules` / `scripts/vmodem.service` | 8 | ✅ 完了 |
| `windows/vim1modem.inf` | 8-a | ✅ 完了 |

`tests/test_linux_step6.c` の最後の節は、これらが**存在する**事と
`Makefile.linux` が Step 6 の `dlsym` のために `-ldl` を渡す事を
検証する形に置き換えました。

---

## Step 7: `Makefile.linux`

指示書に載っていた雛形をそのまま置くのではなく、**実際にビルドして動かし、
壊れていた点を直した上で**採用しました。以下は「雛形との差分」と、その根拠です。

### 雛形をそのまま使うと壊れる点

| # | 雛形 | 実際に起きる事 | 対処 |
|---|---|---|---|
| 1 | `LDFLAGS = -lm -lslirp -lpthread -lasound -ldl` | `src/serial/vm_serial.c` の `openpty(3)` が未定義参照になる | `-lutil` を追加 |
| 2 | `CC = gcc` (または `CC ?= gcc`) | `?=` だと **効かない** | `$(origin CC)` で判定 |
| 3 | ライブラリを `LDFLAGS` に置く | 一部の環境 (`--as-needed` + リンク順) で取りこぼす | `LDLIBS` に分離 |
| 4 | 依存関係の記述が無い | ヘッダを直しても再ビルドされない | `-MMD -MP` + `-include $(DEPS)` |

#### 罠 (2): `CC ?= gcc` が効かない理由

GNU make は `CC` に**組み込みの既定値 `cc`** を持っています。`?=` は
「未定義なら代入」なので、既定値が入っている `CC` には代入されません。
結果、`CC ?= gcc` と書いても `cc` が使われ、クロスコンパイル時に
`CROSS_COMPILE` が無視されます。

```makefile
CROSS_COMPILE ?=
ifeq ($(origin CC),default)      # ← "default" = make の組み込み既定値
CC = $(CROSS_COMPILE)gcc
endif
```

`$(origin CC)` は、コマンドラインで `make CC=clang` と指定された場合は
`command line`、環境変数なら `environment` を返すので、
**ユーザ指定は上書きせず、組み込み既定値だけを置き換える**動作になります。

#### 罠 (1): `-lutil` (`openpty`)

`src/serial/vm_serial.c` は `--port pty` 相当の動作で `openpty(3)` を使います。
`openpty` は歴史的に libutil にあり、glibc 2.34 以降で libc に統合されました。
**手元の検証環境 (glibc 2.41) では `-lutil` 無しでも通ってしまう**ため、
「動いたから大丈夫」と判断すると Armbian Noble (glibc 2.39) 側で初めて
リンクエラーになります。雛形が `-ldl` を「古い glibc のために」入れているのと
全く同じ理由なので、`-lutil` も明示しました
(新しい glibc では空のスタブなので害はありません)。

### `NO_ALSA` を用意しなかった理由

`NO_SLIRP=1` (libslirp 無しでビルド) は用意しましたが、
**`NO_ALSA` は意図的に用意していません**。

当初 `-lasound` を外すスイッチを実装しましたが、実測すると
`src/audio/vm_audio.c` がバックエンド振り分けで `vm_audio_alsa.c` を
無条件に参照しており、`-lasound` を外すと必ず未定義参照になります。
「指定すると必ず失敗するオプション」は、あるほうが有害なので削除し、
`libasound2-dev` を必須依存としてドキュメント化しました
(`vm_audio_null.c` はビルド時ではなく**実行時**の ALSA オープン失敗に対する
フォールバックであり、リンク依存を消すものではありません)。

### テストのビルド規則 —— `.SECONDEXPANSION:` は使えなかった

テストごとにリンクするオブジェクトが違うため、当初
`.SECONDEXPANSION:` + `$$(DEPS_$$*)` で書きましたが動きませんでした。
パターン規則の**ステム (`$*`) 置換は二次展開より前に起きる**ため、
`$$(DEPS_$$*)` が意図した変数名に解決されず、
`*** multiple target patterns` で停止します。

`$(foreach)` + `$(eval)` で**明示規則を生成**する方式に変更しました。

```makefile
define VM_TEST_RULE
$(BUILD)/$(1): tests/$(1).c $(patsubst %.c,$(BUILD)/t/%.o,$(DEPS_$(1)))
	$(Q)$$(CC) $$(TEST_CFLAGS) $$< ... -o $$@
endef
$(foreach t,$(TESTS),$(eval $(call VM_TEST_RULE,$(t))))
```

### 罠 (5): テストだけ `-D_POSIX_C_SOURCE` が必要だった

`-std=c99` は `__STRICT_ANSI__` を定義するので、glibc の `<time.h>` は
`nanosleep()` と `struct timespec` を隠します。実際にビルドすると:

```
tests/test_audio.c:40:21: error: storage size of 'ts' isn't known
tests/test_audio.c:43:5:  error: implicit declaration of function 'nanosleep'
make: *** [Makefile.linux:348: build/test_audio] Error 1
```

全ソースを調べたところ、

* `src/*.c` … `vm_log.c` は `_POSIX_C_SOURCE`、`vm_nat_slirp.c` は
  `_GNU_SOURCE` を**それぞれ自前で定義**している
* `tests/test_linux_*.c` … 同じく自前で定義している (すべて `#ifndef` ガード付き)
* **`tests/test_audio.c` だけ何も定義していない** (上流由来)

という状態でした。対処として:

* `CFLAGS` 本体には足さない —— `src/*.c` は自衛済みで、
  グローバルに定義すると「どのファイルが何を必要としているか」が消える
* `-std=gnu99` に緩めない —— 指示書の `-std=c99` を守る
* 上流の `tests/test_audio.c` は書き換えない

の 3 点を満たすため、**`TEST_CFLAGS` にだけ** `-D_POSIX_C_SOURCE=200809L`
を追加しました。各テストの `#ifndef` ガードのおかげで二重定義にもなりません。

### 使えるターゲット

```bash
make -f Makefile.linux              # vmodem をビルド
make -f Makefile.linux tests        # テストバイナリをビルド
make -f Makefile.linux check        # 全テストをビルドして実行
make -f Makefile.linux check-deps   # ヘッダ/ライブラリが揃っているか事前確認
make -f Makefile.linux print-config # 実際に使われる CC/CFLAGS/LDLIBS を表示
make -f Makefile.linux install      # PREFIX (既定 /usr/local) へ配置
make -f Makefile.linux gadget-install  # udev ルールと systemd unit を配置
make -f Makefile.linux clean / distclean
make -f Makefile.linux help
```

主な変数: `CROSS_COMPILE=` / `NO_SLIRP=1` / `PREFIX=` / `DESTDIR=` / `V=1`。

`install` は `vmodem` に加えて `scripts/setup-gadget-linux.sh` を
`$(PREFIX)/bin/vmodem-setup-gadget` として入れ、`/etc/vmodem/config.ini` は
**既存があれば上書きしません**。

---

## Step 8: USB ガジェットと Windows 側 INF

### 成果物

| ファイル | 役割 |
|---|---|
| `scripts/setup-gadget-linux.sh` | configfs で CDC-ACM ガジェットを構成 (冪等) |
| `scripts/99-vmodem.rules` | `/dev/ttyGS0` 出現を契機に systemd unit を起動 |
| `scripts/vmodem.service` | 実際に `vmodem` を動かす unit |
| `windows/vim1modem.inf` | Windows XP 側で COM ポートとして認識させる INF (Step 8-a) |
| `windows/vim1modem-modem.inf` | その COM ポート上にモデムを登録する INF。DCB の `fOutxCtsFlow=0` で RAS 692 を回避する (Step 10-c) |

### `bDeviceClass = 0xEF` —— Windows で COM ポートとして見えるかの分かれ目

CDC-ACM は「制御インタフェース + データインタフェース」の 2 本組で、
これを 1 つの機能として束ねるのが **IAD (Interface Association Descriptor)** です。
Windows は IAD を見て初めて子デバイス
`USB\VID_1209&PID_0001&MI_00` を生成し、そこに `usbser.sys` が当たります。
IAD を有効にするには、デバイス記述子を次にしておく必要があります。

```sh
write_attr "$GADGET_DIR/bDeviceClass"    "0xef"   # Miscellaneous
write_attr "$GADGET_DIR/bDeviceSubClass" "0x02"   # Common Class
write_attr "$GADGET_DIR/bDeviceProtocol" "0x01"   # Interface Association
```

ここを既定 (`0x00`) のままにすると、Windows からは
「複合デバイスのうち何かよく分からないもの」に見え、INF を当てても
COM ポートになりません。**IAD を解釈できるのは XP SP2 以降**で、
SP1 や Windows 2000 では別の手当てが要ります (README 末尾の制約参照)。

### 冪等性 —— 「既に設定済みなら触らない」

指示書の要求どおり、2 回目以降の実行で構成を壊さない設計にしました。
これは単なる親切ではなく**必須**です。UDC にバインド済みのガジェットに
属性を書くと `EBUSY` で失敗するためです。

```sh
write_attr() {                      # 値が同じなら書かない
    [ "$(cat "$1" 2>/dev/null)" = "$2" ] && return 0
    printf '%s' "$2" > "$1"
}
```

`setup` / `status` / `teardown` / `--force` の 4 モードを持ち、
`teardown` は configfs の制約に従って**逆順で `rmdir` のみ**を使います
(`rm -rf` は効きません)。

### 実機なしで冪等性を検証した方法

検証環境には configfs も UDC もありません
(`/sys/kernel/config` 無し、`/proc/filesystems` に `configfs` 無し、
`/sys/class/udc` 無し、`modprobe` 無し)。
「冪等です」と書くだけでは検証になっていないので、
環境変数 `VM_TEST_ROOT` で configfs の位置を差し替えられるようにし、
**スクリプトのロジック自体をテストから実行**しています。

```
VM_TEST_ROOT=/tmp/gr ./scripts/setup-gadget-linux.sh setup
```

`tests/test_linux_step78.c` はこれを使って
「1 回目の setup → ツリーを記録 → 2 回目の setup → ツリーが完全に一致」
「teardown で全消去 → 二重 teardown が無害 → 再 setup が成功」
「UDC が無ければ失敗する」「別名 UDC へフォールバックする」
を実際に走らせて確認します。

この方式で**実バグを 1 件検出しました**。実 configfs は
カーネル生成の属性ファイルが残っていても `rmdir` が成功しますが、
ただのディレクトリでは `ENOTEMPTY` になります。
シミュレーション側で通常ファイルだけを消す `vm_rmdir()` を用意して
挙動を合わせました (サブディレクトリとシンボリックリンクは消さないので、
**削除順序の検証は意味を保ったまま**です)。

なお、これで検証できるのは**スクリプトの論理**だけで、
USB のエニュメレーション自体は実機でしか確認できません。

### VID/PID は `0x1209` / `0x0001` —— ただし出荷不可

指示書どおり pid.codes の `1209:0001` を既定にしましたが、これは
**Test PID** です。pid.codes のポリシー上、社内試験専用で
**配布物に載せてはいけません**。スクリプトは既定値のまま実行すると
その旨を警告し、`VM_VID` / `VM_PID` で上書きできます
(上書きすると警告も消えます)。同じ値が `windows/vim1modem.inf` の
ハードウェア ID とも一致している事をテストで突き合わせています。

### 指示書の udev ルールをそのまま採用しなかった理由

指示書の例は次のとおりでした。

```
ACTION=="add", KERNEL=="ttyGS0", RUN+="/usr/local/bin/vmodem-start.sh"
```

しかし `systemd-udev(7)` の `RUN` の仕様は
**「このコマンドから fork された全てのプロセスは、イベント処理の完了後に
無条件に kill される」**です。`vmodem` は常駐デーモンなので、
この書き方だと起動直後に殺されます。正しい書き方は、udev から
service unit を **pull-in** する形です。

```
ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyGS0", \
    TAG+="systemd", ENV{SYSTEMD_WANTS}+="vmodem.service"
```

対になる `scripts/vmodem.service` は `BindsTo=dev-ttyGS0.device` を持ち、
ガジェットが外れると自動停止します。`ExecStartPre` に
`-/usr/local/bin/vmodem-setup-gadget` を置いてあるので
(先頭の `-` は失敗許容)、ガジェット未設定でも自力で復帰します。

### Step 8-a: `windows/vim1modem.inf`

```ini
[Version]
Signature   = "$Windows NT$"
Class=Ports
ClassGuid   = {4D36E978-E325-11CE-BFC1-08002BE10318}
DriverVer   = 04/01/2009,5.1.2600.0
; CatalogFile is deliberately omitted: no .cat is shipped (unsigned).

[DeviceList.NT]
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001&MI_00
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001&MI_02

[DriverInstall.NT]
include     = mdmcpq.inf          ; usbser.sys の共通処理を再利用
CopyFiles   = DriverCopyFiles.NT
```

ポイント:

* **`&MI_00` 付きのハードウェア ID が本命**。IAD によって生成される
  子デバイスに一致させる必要があります。`&MI_02` と素の ID も
  保険として並べてあります。
* **`include = mdmcpq.inf`** で `usbser.sys` 用の定型処理を借ります
  (自前で `AddService` を全部書くより、XP の実装差に強い)。
* **`CatalogFile` は書きません**。`.cat` を同梱しないので、
  存在しないカタログを指すと署名検証で失敗します。未署名 INF は
  XP では「続行」で入りますが、**64bit Vista 以降はカーネルモード
  ドライバ署名の強制があり、この INF のままでは入りません**。

#### INF は ASCII + CRLF でなければならない

最初に書いた版は日本語コメント入り (非 ASCII 4215 バイト) でしたが、
**自分で書いたテストが落として気付きました**。XP の setupapi は INF を
システムのコードページで **ANSI として読む**ため、UTF-8 のマルチバイトは
化けるだけでなくパース自体を壊し得ます。全文を ASCII に書き直し、
改行も CRLF に統一しました (`tests/test_linux_step78.c` が
「非 ASCII バイト 0」「LF 単独 0 / CRLF 211」を assert します)。
日本語の設計メモはこの README 側に置いています。

同じテストで `Class       = Ports` (余分な空白) も検出したので
`Class=Ports` に直しています。

### `/dev/ttyGS0` では DTR / DCD が読めない

指示書が調査対象に挙げていた点です。Linux 6.12 のソースを確認しました。

* `drivers/usb/gadget/function/u_serial.c` の `gs_tty_ops` には
  **`.tiocmget` / `.tiocmset` が無い** → `/dev/ttyGS0` への `TIOCMGET` は
  `ENOTTY` を返します
* `f_acm.c` はホストから来た DTR を `port_handshake_bits` に保持していますが、
  **ユーザ空間には公開していません**

つまり「ホストが DTR を落としたら切断する」といった実装は
`/dev/ttyGS0` 越しには不可能です。`vmodem` 側は DTR/DCD を
AT コマンド (`ATH` / `+++`) とタイムアウトで代替する設計なので実害はありませんが、
制約として明記しておきます。テストではこの点を `SKIP` として記録しています。

---

## Step 9: DNS 解決の環境依存と ICMP 権限の恒久化

### 症状

PPP は確立し、IP も配られ、NLA の HTTP チェックも通って Windows の
ネットワーク アイコンは「**IPv4 接続: インターネット**」と表示されるのに、

* ブラウザは `ERR_NAME_NOT_RESOLVED`
* `ping 8.8.8.8` はタイムアウト

VIM1 自身の通信は正常 (`curl https://google.com` → 200)。

### 9-a: DNS が引けない理由

**憶測ではなく libslirp の master ソースを直接読んで確定させました。**

#### (1) DNS 代理が発動する条件

```c
/* libslirp  src/socket.c  sotranslate_out4() */
if (addr == slirp->vnameserver_addr.s_addr) {
    if (get_dns_addr(&addr) < 0) { ... }
    ...
}
```

宛先が `vnameserver_addr` と **完全一致し、かつポートが 53 の時だけ**
代理が働きます。逆に言えば、ゲストに `8.8.8.8` をそのまま教えた場合
libslirp はそれを「ただの外部宛 UDP」として扱います
(NAT 経由で本当に 8.8.8.8 へ出て行くので、経路があれば動きます)。

#### (2) 代理後の宛先は `/etc/resolv.conf` の **1 番目**

```c
/* libslirp  src/slirp.c  get_dns_addr_resolv_conf() */
if (!strncmp(buff, "nameserver", 10)) {
    ...                       /* 最初に見つかった 1 件で return */
}
/* nameserver が 1 件も無ければ 127.0.0.1 / ::1 にフォールバック */
```

systemd-resolved を使う環境では `/etc/resolv.conf` の 1 番目は
`127.0.0.53` (stub listener) です。

#### (3) `outbound_addr` と stub listener の衝突 ← **これが根本原因**

Step 5/6 で入れた `SlirpConfig.outbound_addr` は、libslirp の
**全ての外向きソケットをホストの実 NIC アドレスに `bind()`** させます。
その結果、DNS 代理の送信は

```
src = 実 NIC のアドレス  →  dst = 127.0.0.53:53
```

という組み合わせになります。ところが **systemd-resolved の stub listener は
ローカルループバック インタフェース宛の問い合わせにしか応答しません**
(送信元が非ローカル アドレスの問い合わせは無視される)。
よって応答が返らず `ERR_NAME_NOT_RESOLVED` になります。

> 前掲の Step 6-c-5 で「NIC bind 済みソケットから `127.0.0.1` へ届いたので
> DNS 中継は壊れない」と書きましたが、これは**自作の素朴な echo サーバ**を
> 相手にした実測でした。echo サーバは送信元を問わず返すので通り、
> stub listener は返さないので通らない — 実測の対象が違っていました。
> 該当箇所には訂正を追記しています。

#### 診断を誤らせる 2 つの罠

指示書は「`nslookup google.com 192.168.99.3` が無応答」を根拠に
「libslirp の DNS プロキシが壊れている」としていましたが、これは
**そもそも成立しない診断**です。

| 罠 | 実態 |
|---|---|
| ホストから `nslookup ... 192.168.99.3` が無応答 | `192.168.99.3` は libslirp が**仮想 NIC に入ってきた Ethernet フレームの中の IP** として解釈するアドレスであり、ホストの UDP ソケットからは到達しない。**無応答が正常**。プロキシの健全性の証拠にならない |
| `sendto()` が成功する | UDP の `sendto` はローカル キューへの投入が成功しただけ。宛先が応答しないことは検出できない |

#### 対策: 環境に応じて配る DNS を選ぶ (`vm_nat_pick_guest_dns`)

指示書の「Option A: 常に `8.8.8.8` を配る」を**そのまま採らなかった**理由は、
代理が正しく機能する環境 (VPN / 社内 DNS / `dnsmasq` を実アドレスで
使っている等) で**名前解決できるはずのホスト名が引けなくなる**ためです。

そこで起動時にホストの resolver を実測し、環境ごとに選びます。

```c
/* include/vmodem/vm_nat.h */
bool vm_nat_pick_guest_dns(const vm_nat_t *n,
                           uint32_t fallback1, uint32_t fallback2,
                           uint32_t *out1, uint32_t *out2);
```

| ホストの 1 番目の nameserver | ゲストに配る DNS | 戻り値 |
|---|---|---|
| 実アドレス (`192.168.1.1` 等) | libslirp の代理 (`192.168.99.3`) | `true` |
| ループバック (`127.0.0.53` 等) | 設定値 `dns1`/`dns2` (既定 `8.8.8.8`/`8.8.4.4`) | `false` |
| nameserver が 0 件 | 同上 (libslirp が `127.0.0.1` にフォールバックするため) | `false` |
| `mode != slirp` | 設定値そのまま | `false` |

`src/modem/vm_modem.c` の `build_ppp_cfg()` から呼び、
**どちらを選んだかを必ずログに出します**。

```
ppp: DNS に実アドレス 8.8.8.8 / 8.8.4.4 を配る (ホストの resolver がループバックのため slirp の DNS 代理は使わない)
ppp: DNS に slirp の代理 192.168.99.3 を配る (ホストの resolver が実アドレスなので代理が働く)
```

なお設定値が空 (`0`) の場合、`0` を配ると IPCP で Config-Reject を招き
33.6 kbps では往復が高くつくため、`8.8.8.8` / `8.8.4.4` を補完します。

### 9-b: ping が通らない理由

```c
/* libslirp  src/ip_icmp.c  icmp_send() */
so->s = slirp_socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
if (so->s == -1 && (errno == EAFNOSUPPORT
                 || errno == EPROTONOSUPPORT
                 || errno == EACCES)) {
    /* Linux 特有: ping ソケットが使えなければ raw に落ちる */
    so->s = slirp_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
}
if (so->s == -1) { ... return -1; }        /* ← 無言で捨てられる */
```

2 段構えで、**両方失敗するとゲストの ICMP は無言で捨てられます**。

| 段 | ソケット | 必要な権限 |
|---|---|---|
| 1 | `SOCK_DGRAM` + `IPPROTO_ICMP` | プロセスの GID が `net.ipv4.ping_group_range` の範囲内 |
| 2 | `SOCK_RAW` + `IPPROTO_ICMP` | `CAP_NET_RAW` |

#### `1 0` は「1 から 0」ではなく空集合

Debian / Ubuntu の既定値は

```
$ cat /proc/sys/net/ipv4/ping_group_range
1	0
```

これは `lo=1 > hi=0` すなわち**どの GID も許可しない**という意味です。
`lo <= gid` だけを見る素朴な判定は GID 1000 を誤って「許可」と判断します。
本実装は `(lo <= hi) && (gid >= lo) && (gid <= hi)` で判定します。

#### 恒久化 (揮発性 sysctl なので再起動で戻る)

`/proc/sys/...` への直接書き込みは再起動で失われます。
`/etc/sysctl.d/*.conf` は `systemd-sysctl.service` が起動時に読むので、
ここに置くのが正攻法です。3 重で確保しています。

| 手段 | ファイル |
|---|---|
| sysctl の恒久設定 | `scripts/99-vmodem.conf` → `/etc/sysctl.d/99-vmodem.conf` |
| セットアップ時の即時適用 | `scripts/setup-gadget-linux.sh` の `setup_ping_sysctl()` |
| サービス側の保険 | `scripts/vmodem.service` の `AmbientCapabilities=CAP_NET_RAW` + `ExecStartPre` |

```ini
# /etc/sysctl.d/99-vmodem.conf
net.ipv4.ping_group_range = 0 2147483647
```

```ini
# scripts/vmodem.service
AmbientCapabilities=CAP_NET_RAW
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_NET_BIND_SERVICE
ExecStartPre=-/usr/bin/env sh -c 'echo "0 2147483647" > /proc/sys/net/ipv4/ping_group_range'
```

`ExecStartPre` の先頭の `-` は「失敗しても起動を続ける」という意味です
(読み取り専用 `/proc` やコンテナ内でサービス自体が起動不能になるのを避ける)。

### 9-c: 新規モジュール `src/net/vm_netdiag.c`

原因が**環境依存で、かつログに何も出ない**種類の障害だったため、
起動時に自己診断して原因を名指しするモジュールを追加しました。

```c
/* include/vmodem/vm_netdiag.h */
bool vm_netdiag_host_resolver(vm_netdiag_resolver_t *out);
bool vm_netdiag_icmp_probe(vm_netdiag_icmp_t *out);
void vm_netdiag_report(uint32_t nat_network, uint32_t nat_netmask);
```

* `vm_netdiag_host_resolver()` — `/etc/resolv.conf` を
  **libslirp と同じ規則で**解析する (1 番目だけ / `%ifname` を落とす /
  IPv6 を飛ばす / `#` `;` をコメント扱い / 0 件なら `127.0.0.1`)
* `vm_netdiag_icmp_probe()` — libslirp の `icmp_send()` と
  **同じ順序・同じ型**でソケットを試し、`ping_group_range` も読む
* `vm_netdiag_report()` — 上記を `slirp_be_open()` の最後で出力。
  ホストの resolver が NAT ネットワーク内 (自己参照) の場合も警告する

IPv4 の解析に `inet_addr` / `inet_pton` を使わず 4 オクテットを
自前で厳格に解析しているのは、`inet_addr("1.2.3")` が**成功してしまう**ため
(`1.2.0.3` と解釈される) で、これを許すと判定を誤ります。

テスト容易性のため環境変数で差し替えられます
(`VM_RESOLV_CONF` / `VM_PING_GROUP_RANGE`)。

### 9-d: 運用時の切り分け

```sh
sudo ./scripts/setup-gadget-linux.sh status
```

`show_net_status()` が次を報告します。

* ホストの 1 番目の nameserver は何か
* libslirp の DNS 代理を使う構成になるか
* ping ソケットが使える状態か (`ping_group_range` の実値)

Windows 側からの最終確認:

```
nslookup google.com     … 名前が引ける
ping 8.8.8.8            … 応答が返る
```

### 9-e: Step 9 の新規テスト

`tests/test_dnsfix.c` (**26 項目成功 / 失敗 0 / 省略 1**)。
省略 1 件は sandbox で ICMP ソケットが作れないためで、権限のある実機では実行されます。

```bash
make -f Makefile.linux build/test_dnsfix && ./build/test_dnsfix
```

| 節 | 検証する事 |
|---|---|
| 1 | `resolv.conf` 解析 12 ケース (`127.0.0.53` / 実アドレス / 複数行は 1 番目 / `%eth0` 付き / IPv6 は飛ばす / 0 件は `127.0.0.1` 扱い / コメント / `nameserverfoo` は不一致 / `1.2.3` と `300.1.1.1` は不正 / タブ区切り / ファイル欠損) |
| 2 | `vm_nat_pick_guest_dns()` の両分岐 (代理を使う / 使わない) と NULL 安全性、設定値が空でも `0` を配らない事 |
| 3 | ICMP プローブの整合性 (`ok == (dgram_ok \|\| raw_ok)`) |
| 4 | `ping_group_range` の判定 (`"1 0"` は空集合 / `"0 2147483647"` は許可 / 読めない場合) |
| 5 | `vm_netdiag_report()` のスモーク テスト (自己参照ケースを含む) |

環境依存の値は assert せず、「どの環境でも成り立つ性質」だけを判定する方針は
Step 6 と同じです。

---

## Step 10: Windows XP で INF が自動で当たらず、ダイアルすると 692

### 症状

1. Windows XP で **INF が自動で当たらない**。手動で当てるしかない
2. 手動で当ててモデムとして設定し、ダイアルアップすると **RAS エラー 692**
   (「ポートまたは接続されているデバイスでハードウェア障害が発生しました」)
3. その時 **VIM1 側のログには通話開始も通信も一切出ない**。待機のまま
4. **Windows 11 では同じ構成で正常にダイアルできる**

「Win11 では動くのだから、おま環では」と片付けたくなりますが、
調べた結果 **XP 側の環境問題ではなく、本プロジェクトの Windows 側
デバイス定義の欠陥**でした。以下、一次資料で確定させた事実だけを書きます。

### 10-a: なぜ INF が自動で当たらなかったか

#### 事実: 互換 ID を 1 つも書いていなかった

Microsoft の "USB Serial Driver (Usbser.sys)" が明記している通り、CDC-ACM
デバイスは**互換 ID** `USB\Class_02&SubClass_02` で `usbser.sys` に結び付きます。
「Standard USB Identifiers」の規定では、複合デバイスの各インタフェースに対して
Windows は次を生成します。

| 種別 | 生成される ID |
|---|---|
| ハードウェア ID | `USB\VID_v(4)&PID_d(4)&MI_z(2)` |
| 互換 ID | `USB\CLASS_d(2)&SUBCLASS_s(2)&PROT_p(2)` |
| 互換 ID | `USB\CLASS_d(2)&SUBCLASS_s(2)` |
| 互換 ID | `USB\CLASS_d(2)` |
| 互換 ID | `USB\COMPOSITE` |

修正前の `vim1modem.inf` には **ハードウェア ID しか書いていません**でした。
ハードウェア ID は VID/PID に完全一致した時にしか当たらないため、
「その INF を手で指定した時だけ入る」= 自動では当たらない、という
そのままの結果になっていました。互換 ID を追加して解決しています。

```ini
[DeviceList.NT]
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001&MI_00
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001
%DESCRIPTION% = DriverInstall.NT, USB\VID_1209&PID_0001&MI_02
%DESCRIPTION% = DriverInstall.NT, USB\Class_02&SubClass_02&Prot_01   ; ← 追加
%DESCRIPTION% = DriverInstall.NT, USB\Class_02&SubClass_02           ; ← 追加
```

#### 訂正: `&Cdc_02` は XP の経路ではない

調査の途中で「XP は `USB\VID_xxxx&PID_xxxx&Cdc_02` の形で来るのでは」と
考えましたが、**これは誤り**でした。Microsoft "Support for Interface
Collections" によれば、`&Cdc_02` 形式の ID を `usbccgp.sys` が生成するのは
**そのソフトウェアキーに `EnumeratorClass` (`02,00,00`) が設定されている時だけ**で、
既定では無効です。さらに WMCDC のサポート自体が **Windows Vista 以降**です。
本プロジェクトは `EnumeratorClass` をどこにも設定していないので、
XP は通常の `&MI_00` 形式で列挙します。

INF には `&Cdc_02` の行も残してありますが、これは **XP では単に一致しない
不活性な行**であり、将来 `EnumeratorClass` を使う構成にした場合の保険です。
「これが効いている」という説明をしてはいけません。

#### もう 1 つの必要条件: INF の置き場所

Windows がドライバを自動で探すのは `%SystemRoot%\inf` と、
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\DevicePath` に登録された
ディレクトリだけです。プロジェクトフォルダに置いた INF は、
**互換 ID が正しくても永久に自動では見つかりません**。
`windows\vim1modem.inf` を `C:\WINDOWS\inf\` にコピーしてください。

### 10-b: 692 の真因 — CTS が永久に上がらない

症状 2 と 3 は同じ 1 つの原因から来ています。**「AT コマンドが 1 バイトも
送信されていない」**ため、VIM1 側のログが無音になり、XP は応答が無いので
692 を返していました。なぜ送信されないかを 4 つの一次資料で確定させました。

#### 事実 1: CDC-ACM の通知に CTS ビットは存在しない

Linux v6.12 の `include/uapi/linux/usb/cdc.h` の SerialState 通知 (0x20) の定義:

```c
#define USB_CDC_SERIAL_STATE_DCD        (1 << 0)
#define USB_CDC_SERIAL_STATE_DSR        (1 << 1)
#define USB_CDC_SERIAL_STATE_BREAK      (1 << 2)
#define USB_CDC_SERIAL_STATE_RING_SIGNAL (1 << 3)
#define USB_CDC_SERIAL_STATE_FRAMING    (1 << 4)
#define USB_CDC_SERIAL_STATE_PARITY     (1 << 5)
#define USB_CDC_SERIAL_STATE_OVERRUN    (1 << 6)
```

DCD / DSR / BREAK / RING / FRAMING / PARITY / OVERRUN の 7 つだけで、
**CTS は定義そのものが存在しません**。USB PSTN サブクラス仕様 v1.2 に
CTS の規定が無いためで、これは実装の手抜きではなく**プロトコルの仕様**です。

#### 事実 2: `f_acm.c` は DSR と DCD しか上げない

```c
/* drivers/usb/gadget/function/f_acm.c  acm_connect()  (v6.12) */
acm->serial_state |= ACM_CTRL_DSR | ACM_CTRL_DCD;
```

事実 1 の通り上げようが無いので当然です。

#### 事実 3: `usbser.sys` は RTS/CTS を扱わない

Keil の "USBSER.SYS quirks explained" が挙げる既知の癖のうち (c):

> **RTS changes just with DTR setting.**

Microsoft Q&A の "Virtual serial port (USBSER.SYS) is not sending RTS/CTS"
でも同じ挙動が報告されています。

#### 事実 4: Windows 内蔵「標準モデム」は CTS を待つ設定になっている ★

これが決め手でした。Microsoft WDK の "Locking the Port Speed (DCB)" が
`MdmHayes.inf` の DCB を引用しています。

```ini
HKR,, DCB, 1, 1C,00,00,00, 80,25,00,00, 15,20,00,00, ...
```

3 番目の DWORD がビットフィールドで、リトルエンディアンなので
`15,20,00,00` = **`0x00002015`**。Win32 の `DCB` 構造体のビット配置に
当てはめて展開します。

| ビット | メンバ | 値 |
|---|---|---|
| 0 | `fBinary` | 1 |
| 1 | `fParity` | 0 |
| **2** | **`fOutxCtsFlow`** | **1** |
| 3 | `fOutxDsrFlow` | 0 |
| 4-5 | `fDtrControl` | 1 (`DTR_CONTROL_ENABLE`) |
| 12-13 | `fRtsControl` | 2 (`RTS_CONTROL_HANDSHAKE`) |

`fOutxCtsFlow` について Microsoft は次のように規定しています。

> If this member is TRUE and CTS is turned off, output is suspended until
> CTS is sent again.

#### 因果の連鎖

```
Windows 内蔵「通信ケーブル経由の標準モデム」を選ぶ
  -> DCB の fOutxCtsFlow = 1  (事実 4)
  -> シリアルドライバは CTS が上がるまで送信を保留する
  -> しかし CDC-ACM に CTS を上げる手段は存在しない  (事実 1・2・3)
  -> unimodem の初期化文字列もダイアル文字列も 1 バイトも送信されない
  -> VIM1 は 1 バイトも受信しない = ログが待機のまま  (症状 3)
  -> XP はモデムが応答しないと判断し RAS 692 を返す  (症状 2)
```

Windows 11 の `usbser.sys` は KMDF で書き直されており、この挙動を示しません。
**症状 4 (Win11 では動く) はこの差で説明が付きます。**
つまり「おま環」ではなく、XP を含む構成では必ず起きる設計上の欠陥です。

### 10-c: 対策 — `windows/vim1modem-modem.inf`

内蔵の「標準モデム」を使う限り DCB は `MdmHayes.inf` 由来のままなので、
**自前のモデム INF を用意して DCB を上書き**します。

```ini
[Version]
Class     = Modem
ClassGuid = {4D36E96D-E325-11CE-BFC1-08002BE10318}

; DCBlength=0x1C, BaudRate=115200, ビットマスク=0x00001011
;   fBinary=1, fOutxCtsFlow=0 (★CTS を待たない), fDtrControl=1 (ENABLE),
;   fRtsControl=1 (RTS_CONTROL_ENABLE ... HANDSHAKE ではない)
HKR,, DCB, 1, 1C,00,00,00, 00,c2,01,00, 11,10,00,00, ...

; Properties の 6 番目の DWORD = ModemOptions
;   0x10 = ハードウェアフロー制御 / 0x20 = ソフトウェアフロー制御
;   0x20 だけを立てる (0x10 は意図的に落とす)
HKR,, Properties, 1, 00,00,00,00, 3c,00,00,00, 00,00,00,00, 07,00,00,00,
                     0f,00,00,00, 20,00,00,00, 00,c2,01,00, 40,83,00,00
```

設計上の判断:

* **`FlowControl_Hard` を定義しない**。定義するとプロパティ画面に
  「ハードウェア」が選択肢として現れ、選ばれた瞬間に 692 が再発します。
  Microsoft も「サポートしない設定は書かない」「`ModemOptions` と
  一致させる」と規定しています。
* **初期化文字列は `vm_at.c` が実装している AT だけ**にしました
  (`AT&F E0 Q0 V1` / `AT&C1 &D2 &K0 S0=0`)。未実装の AT に `ERROR` を返すと、
  unimodem はモデム全体を「応答しない」と見なしてダイアルを中止します。
  `&K0` (フロー制御なし) を送る事で DCE 側の設定も揃えています。
* **`Responses` は `vm_at.c` の `vm_at_result_text()` が実際に返す文字列と
  1 対 1 で対応**させました。verbose と numeric の両方を登録しています。
  知らない応答が来ると unimodem は待ち続けてタイムアウトします。
* `Class=Modem` なので、COM ポートとして入っている `vim1modem.inf` を
  **置き換えるのではなく、その上に載る**形になります。両方必要です。

### 10-d: 沈黙したまま失敗させない (VIM1 側)

この不具合が厄介なのは、**VIM1 側から見ると「何も起きていない」としか
見えない**点です。エラーも出ず、ただログが止まっているだけなので、
原因が Windows 側の DCB にあると気付くまでに時間がかかります。

そこで `src/modem/vm_modem.c` に、**COMMAND 状態のまま 20 秒間 1 バイトも
受信しなかったら、原因と対処を名指しした警告を 1 度だけ出す**処理を
追加しました。

```
WRN modem: /dev/ttyGS0 を開いてから 20 秒、DTE から 1 バイトも受信していない
WRN modem:   ホスト側が AT を送っていない可能性が高い (こちらの受信経路の問題ではない)
WRN modem:   最有力: Windows のモデム定義がハードウェアフロー制御 (RTS/CTS) を要求している
WRN modem:   CDC-ACM の SerialState 通知に CTS ビットは無く (uapi/linux/usb/cdc.h)、
WRN modem:   f_acm は DSR|DCD しか上げない
WRN modem:   -> DCB の fOutxCtsFlow=1 だと送信が永久に保留され、AT が 1 バイトも出ない
WRN modem:   対処: windows/vim1modem-modem.inf を入れる (fOutxCtsFlow=0)
```

20 秒という値は「RAS のモデム応答待ちより長く、利用者が反応が無いと感じるより
短い」ところに置いています。短すぎると、接続したまま放置している正常な
ケースで誤警告になります。警告は 1 度きりで、毎周期は出しません。

### 10-e: Step 10 のテスト

`tests/test_linux_step78.c` に `test_step8c_modem_inf()` を追加しました
(20 項目)。**「効く条件」だけを検査する**方針です。

| 検査 | 落ちると何が起きるか |
|---|---|
| `Class=Modem` / Modem クラス GUID / `unimdm.tsp` | TAPI から見えず、ダイアルアップの相手として選べない |
| DCB に `11,10,00,00` がある | `fOutxCtsFlow` が 0 でない = **692 が再発** |
| 有効行に `15,20,00,00` が無い | `MdmHayes.inf` の値を使ってしまっている |
| `ModemOptions` = `20,00,00,00` | DCB と食い違い、プロパティ画面を開くと元に戻る |
| `FlowControl_Hard` が**無い** | UI で選べてしまい、選ばれた瞬間に 692 が再発 |
| `&K0` / `&C1` / `&D2` を送る | DCE 側のフロー制御設定が揃わない |
| 応答 `NO CARRIER` / `NO DIALTONE` / `CONNECT 33600` | unimodem が応答を解釈できず待ち続ける |
| 全体が ASCII / 改行が CRLF | XP の setupapi は ANSI 読みなので化ける |
| `.cat` が無いのに `CatalogFile` を書いていない | 署名検証で失敗する |

`vim1modem.inf` 側にも `Class_02&SubClass_02` の存在を assert する項目を
追加しています (これが無いと 10-a の症状に戻ります)。

---


## ビルドに必要なもの

| 項目 | 内容 |
|---|---|
| ボード | Khadas VIM1 (Amlogic S905X) |
| OS | Armbian (Ubuntu Noble ベース) / Linux 6.12 |
| コンパイラ | gcc 13 以降 (Armbian Noble の既定は gcc 13.2 / 13.3) |
| PTY | `libutil` (`openpty`; glibc 2.34 以降は libc に統合) |
| 音声 | `libasound2-dev` (alsa-lib) |
| NAT | `libslirp-dev` (`--net slirp` 用。Step 6 で有効化済み) |

```bash
sudo apt install build-essential libasound2-dev libslirp-dev
```

ビルドは `Makefile.linux` (Step 7 の成果物) で行います。

```bash
make -f Makefile.linux            # vmodem をビルド
make -f Makefile.linux check      # 全テストをビルドして実行
make -f Makefile.linux check-deps # 依存が揃っているか確認
make -f Makefile.linux help       # 使えるターゲット一覧
```

`libslirp-dev` を入れて `-DVMODEM_HAVE_LIBSLIRP` を付ければ
`--net slirp` が使えます。Step 6 で ABI 差を実行時に吸収したので、
**Debian/Ubuntu の libslirp 4.8 系でもリンクが通ります**
(同梱ヘッダは 4.9.3 ですが、`slirp_pollfds_fill_socket` は
`dlsym` で実行時に探すため静的参照が残っていません)。
`dlsym` を使うので **リンク時に `-ldl` を付けてください**
(glibc 2.34 以降では no-op ですが、それ以前や musl では必須)。

`/dev/ttyGS0` を開くには USB Gadget の設定が必要ですが、その設定スクリプト
(`scripts/setup-gadget-linux.sh`) は Step 8 の成果物です。実行時に権限で
失敗した場合は `dialout` グループへの追加、また `serial-getty@ttyGS0` が
ポートを掴んでいる場合は無効化が必要で、エラーメッセージがその旨を案内します。

---

## ディレクトリ構成

```
Makefile.linux           ★Linux 版ビルド (Step 7)
include/vmodem/          公開ヘッダ
src/core/
  vm_config.c            config.ini のパーサ
  vm_log.c               ★ロガー (Step 4 で POSIX 対応)
  vm_ringbuf.c           リングバッファ
  vm_types.c             エラーコード → 文字列
src/audio/
  vm_audio.c             共通処理・バックエンド振り分け
  vm_audio_wasapi.c      Windows (上流)
  vm_audio_alsa.c        ★Linux / ALSA (Step 2)
  vm_audio_null.c        無音 (ALSA 失敗時のフォールバック先)
src/dsp/                 トーン合成・V.8 / V.34 ハンドシェイク音・リサンプラ
src/modem/               AT コマンド・接続シーケンス・全体制御
src/net/                 PPP / HDLC / 疑似 Ethernet / NAT
  vm_nat.c               ★NAT 共通・時計 (Step 5 で CLOCK_MONOTONIC 化)
  vm_nat_slirp.c         ★libslirp バックエンド (Step 6 で POSIX 対応)
  vm_hostroute.c         外向きアドレス検出 Windows 版
                         ★Step 5 で翻訳単位ごと _WIN32 で囲み Linux から除外
  vm_hostroute_linux.c   ★Linux / getifaddrs 版 (Step 5-a)
  vm_netdiag.c           ★DNS / ICMP の自己診断 (Step 9。libslirp と同じ規則で
                           resolv.conf を解析し ping_group_range も実測する)
src/serial/
  vm_serial.c            共通処理・バックエンド振り分け
  vm_serial_win32.c      Windows (上流)
  vm_serial_linux.c      ★Linux 実 tty / CDC-ACM (Step 1)
src/main.c               ★エントリポイント (Step 3 でシグナル処理を POSIX 化)
scripts/
  setup-gadget-linux.sh  ★configfs で CDC-ACM ガジェットを構成 (Step 8・冪等)
  99-vmodem.rules        ★udev ルール (Step 8。RUN+= ではなく SYSTEMD_WANTS)
  vmodem.service         ★systemd unit (Step 8。BindsTo=dev-ttyGS0.device。
                           Step 9 で CAP_NET_RAW と ping_group_range を追加)
  99-vmodem.conf         ★sysctl 恒久設定 (Step 9。ping_group_range)
windows/
  vim1modem.inf          ★Windows XP 用 INF (Step 8-a。ASCII + CRLF 必須。
                           Step 10-a で互換 ID Class_02&SubClass_02 を追加)
  vim1modem-modem.inf    ★Windows 用モデム INF (Step 10-c。RAS 692 の対策。
                           DCB の fOutxCtsFlow=0 が本体)
tests/
  test_linux_port.c      ★Step 1・2 の検証
  test_linux_step34.c    ★Step 3・4 の検証
  test_linux_step5.c     ★Step 5・5-a の検証
  test_linux_step6.c     ★Step 6-a・6-b・6-c の検証
  test_linux_step78.c    ★Step 7・8・8-a・8-c (Step 10) の検証
                           (ガジェットスクリプトを VM_TEST_ROOT で実行して
                            冪等性・teardown 順序まで実測する)
  test_dnsfix.c          ★Step 9 の検証 (resolv.conf 解析 12 ケース /
                            配る DNS の両分岐 / ping_group_range の空集合)
docs/README-windows.md   上流 Windows 版の README (libslirp の罠など)
```

### `.gitignore` の `core` が `src/core/` を飲み込んでいた

本 PR で `src/core/*.c` が「新規追加」になっているのは、上流から移植した
ファイルだからではありません。`.gitignore` にクラッシュダンプ除け目的で
`core` と書かれていたため、gitignore のパターン規則により
**`src/core/` ディレクトリごと無視され、4 ファイルすべてがコミットから
漏れていた**のが原因です。

```
core        → 任意の階層の core という名前のファイル/ディレクトリに一致
/core       → リポジトリ直下の core だけに一致  ← 本来の意図
```

`/core` / `/core.*` に修正しました。これに気付かないと `vm_log.c` が
リポジトリに存在せず、Step 4 の対象ファイルそのものが編集できません
(clone 直後はリンクも通りません)。

Windows 版の詳細 (libslirp のバージョン差異、Winsock の罠、com0com の設定など)
は [docs/README-windows.md](docs/README-windows.md) を参照してください。

---

## ライセンス

本プロジェクトのソースコードは **BSD-2-Clause** です。上流 WinDialupEmu の
ライセンスをそのまま継承し、本移植で追加したファイルにも同じ
`SPDX-License-Identifier: BSD-2-Clause` を付けています。

サードパーティ コンポーネント (alsa-lib: LGPL-2.1+ を動的リンク、
libslirp: LGPL-2.1+ を動的リンク、com0com: GPL-3.0 を別プロセスとして利用)
の扱いは [LICENSE](LICENSE) を参照してください。いずれも動的リンクまたは
プロセス境界越しの利用に留めているため、本プロジェクト自身のライセンスは
BSD-2-Clause を維持できます。

---

## 免責

本ソフトウェアは学習・懐古・検証を目的としたエミュレータです。
実在の電話網には一切接続しません。設定する電話番号には**実在しない番号**を
使用してください。
