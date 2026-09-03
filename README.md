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

本リポジトリは移植手順書 (`linux_port_instructions.md`) の **Step 0〜5 のみ**を
実装した段階です。**Step 6 以降は意図的に未着手**です。

| Step | 内容 | 状態 |
|---|---|---|
| 0 | 上流コードの取り込みと調査 | ✅ 完了 |
| 1 | `src/serial/vm_serial_linux.c` (USB Gadget CDC-ACM シリアル) | ✅ 完了 |
| 2 | `src/audio/vm_audio_alsa.c` (ALSA 音声出力) | ✅ 完了 |
| 3 | `src/main.c` のシグナル処理 | ✅ 完了 |
| 4 | `src/core/vm_log.c` の POSIX 対応 | ✅ 完了 |
| 5 | `src/net/vm_nat.c` の時計 / `vm_hostroute_linux.c` (新規) | ✅ 完了 |
| 6 | `src/net/vm_nat_slirp.c` (libslirp の POSIX 対応) | ⬜ 未着手 |
| 7 | `Makefile.linux` | ⬜ 未着手 |
| 8 | `scripts/setup-gadget-linux.sh` / `windows/vim1modem.inf` | ⬜ 未着手 |

Step 3・4 が入ったので、**libslirp を使わない構成 (`--net none` /
`--net loopback`) なら Linux 上で実際に起動・常駐・正常終了できます**。
Step 5 で `src/net/` の Windows 依存のうち **時計 (`GetTickCount64`) と
外向きアドレスの検出 (`GetAdaptersAddresses`)** が解消されました。
`--net slirp` にはまだ `GetProcAddress` / `WSAPOLLFD` 前提のコードが
`vm_nat_slirp.c` に残っているため、**Step 6 が必要**です。

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

Step 5 の成果物は 3 つです。**`vm_nat_slirp.c` 本体の移植 (Step 6) には
手を付けていません。**

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
test_audio                                                                       → ビルド不可
```

`test_core` には既存のロガー検証が 24 項目含まれており、`vm_log.c` を
POSIX 向けに書き直した後も**全項目そのまま成功**します。`vm_log.h` の API を
一切変えていないため、上流のテストから見た振る舞いは同一です。

`test_nat` のビルドには `-Isrc/net` が必要です (`vm_nat_internal.h` が
`src/net/` にあるため)。上流の README に記載が無いだけで、移植による
変化ではありません。

`test_audio` は上流と 1 バイトも違わないファイルで、`-std=c99` 時に
`_POSIX_C_SOURCE` が無いため `nanosleep` / `struct timespec` を解決できない
という**既存の問題**です (上流ツリーでも同一のエラーが再現します)。
テストとビルド系の整備は Step 7 以降の範囲なので本 PR では触っていません。

---

## ビルドに必要なもの

| 項目 | 内容 |
|---|---|
| ボード | Khadas VIM1 (Amlogic S905X) |
| OS | Armbian (Ubuntu Noble ベース) / Linux 6.12 |
| コンパイラ | gcc 13 以降 (Armbian Noble の既定は gcc 13.2 / 13.3) |
| 音声 | `libasound2-dev` (alsa-lib) |
| NAT | `libslirp-dev` (`--net slirp` 用。有効化は Step 6) |

```bash
sudo apt install build-essential libasound2-dev libslirp-dev
```

`Makefile.linux` は Step 7 の成果物なのでまだありません。現時点では
上記の `gcc` コマンドで個別にコンパイル・テストしてください。

なお `libslirp-dev` を入れて `-DVMODEM_HAVE_LIBSLIRP` を付けても、現時点では
`vm_nat_slirp.c` が `slirp_pollfds_fill_socket` (libslirp 4.9 以降の API) を
直接参照しているため 4.8 系ではリンクが通りません。この ABI 差の吸収は
**Step 6 の範囲**なので本段階では手を付けていません。

`/dev/ttyGS0` を開くには USB Gadget の設定が必要ですが、その設定スクリプト
(`scripts/setup-gadget-linux.sh`) は Step 8 の成果物です。実行時に権限で
失敗した場合は `dialout` グループへの追加、また `serial-getty@ttyGS0` が
ポートを掴んでいる場合は無効化が必要で、エラーメッセージがその旨を案内します。

---

## ディレクトリ構成

```
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
  vm_nat_slirp.c         libslirp バックエンド (Step 6 で移植予定・未着手)
  vm_hostroute.c         外向きアドレス検出 Windows 版
                         ★Step 5 で翻訳単位ごと _WIN32 で囲み Linux から除外
  vm_hostroute_linux.c   ★Linux / getifaddrs 版 (Step 5-a)
src/serial/
  vm_serial.c            共通処理・バックエンド振り分け
  vm_serial_win32.c      Windows (上流)
  vm_serial_linux.c      ★Linux 実 tty / CDC-ACM (Step 1)
src/main.c               ★エントリポイント (Step 3 でシグナル処理を POSIX 化)
tests/
  test_linux_port.c      ★Step 1・2 の検証
  test_linux_step34.c    ★Step 3・4 の検証
  test_linux_step5.c     ★Step 5・5-a の検証
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
