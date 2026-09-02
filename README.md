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

本リポジトリは移植手順書 (`linux_port_instructions.md`) の **Step 0〜2 のみ**を
実装した段階です。**Step 3 以降は意図的に未着手**です。

| Step | 内容 | 状態 |
|---|---|---|
| 0 | 上流コードの取り込みと調査 | ✅ 完了 |
| 1 | `src/serial/vm_serial_linux.c` (USB Gadget CDC-ACM シリアル) | ✅ 完了 |
| 2 | `src/audio/vm_audio_alsa.c` (ALSA 音声出力) | ✅ 完了 |
| 3 | `src/main.c` のシグナル処理 | ⬜ 未着手 |
| 4 | `src/core/vm_log.c` の POSIX 対応 | ⬜ 未着手 |
| 5 | `src/net/vm_nat.c` / `vm_hostroute_linux.c` / `vm_nat_slirp.c` | ⬜ 未着手 |
| 6 | `Makefile.linux` | ⬜ 未着手 |
| 7 | `scripts/setup-gadget-linux.sh` (USB Gadget 設定) | ⬜ 未着手 |
| 8 | `windows/vim1modem.inf` (XP 用 INF) | ⬜ 未着手 |

つまり**現時点では実行可能なバイナリはまだ作れません**。Step 1・2 の 2 つの
バックエンドは単体で完成しており、コンパイル・単体テストは通ります
(後述の「検証状況」参照)。エンドツーエンドの動作には Step 3 以降が必要です。

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

## 検証状況

Step 1・2 で追加・変更した翻訳単位は警告ゼロでコンパイルできます。

```bash
gcc -c -O2 -std=c99 -Wall -Wextra -Iinclude \
    src/serial/vm_serial.c src/serial/vm_serial_linux.c \
    src/audio/vm_audio.c src/audio/vm_audio_alsa.c src/audio/vm_audio_null.c
```

### 新規テスト

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
test_audio                                                                       → ビルド不可
```

`test_audio` は上流と 1 バイトも違わないファイルで、`-std=c99` 時に
`_POSIX_C_SOURCE` が無いため `nanosleep` / `struct timespec` を解決できない
という**既存の問題**です (上流ツリーでも同一のエラーが再現します)。
テストとビルド系の整備は Step 6 以降の範囲なので本 PR では触っていません。

---

## ビルドに必要なもの

| 項目 | 内容 |
|---|---|
| ボード | Khadas VIM1 (Amlogic S905X) |
| OS | Armbian (Ubuntu Noble ベース) / Linux 6.12 |
| コンパイラ | gcc 13 以降 (Armbian Noble の既定は gcc 13.2 / 13.3) |
| 音声 | `libasound2-dev` (alsa-lib) |
| NAT | `libslirp-dev` (Step 5 以降で使用) |

```bash
sudo apt install build-essential libasound2-dev libslirp-dev
```

`Makefile.linux` は Step 6 の成果物なのでまだありません。現時点では
上記の `gcc` コマンドで個別にコンパイル・テストしてください。

`/dev/ttyGS0` を開くには USB Gadget の設定が必要ですが、その設定スクリプト
(`scripts/setup-gadget-linux.sh`) も Step 7 の成果物です。実行時に権限で
失敗した場合は `dialout` グループへの追加、また `serial-getty@ttyGS0` が
ポートを掴んでいる場合は無効化が必要で、エラーメッセージがその旨を案内します。

---

## ディレクトリ構成

```
include/vmodem/          公開ヘッダ
src/core/                設定・ログ・リングバッファ
src/audio/
  vm_audio.c             共通処理・バックエンド振り分け
  vm_audio_wasapi.c      Windows (上流)
  vm_audio_alsa.c        ★Linux / ALSA (Step 2)
  vm_audio_null.c        無音 (ALSA 失敗時のフォールバック先)
src/dsp/                 トーン合成・V.8 / V.34 ハンドシェイク音・リサンプラ
src/modem/               AT コマンド・接続シーケンス・全体制御
src/net/                 PPP / HDLC / 疑似 Ethernet / NAT
src/serial/
  vm_serial.c            共通処理・バックエンド振り分け
  vm_serial_win32.c      Windows (上流)
  vm_serial_linux.c      ★Linux 実 tty / CDC-ACM (Step 1)
tests/
  test_linux_port.c      ★Step 1・2 の検証
docs/README-windows.md   上流 Windows 版の README (libslirp の罠など)
```

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
