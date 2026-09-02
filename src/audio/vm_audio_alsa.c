/*
 * vm_audio_alsa.c - Linux ALSA (alsa-lib) 再生バックエンド
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Windows 版 vm_audio_wasapi.c と同じインタフェース
 * (vm_audio_internal.h の vm_audio_run_alsa) を実装する。
 * スタブではなく完全な実装である。
 *
 * ===========================================================================
 * WASAPI 版との対応関係
 * ===========================================================================
 * 両者は驚くほど綺麗に対応する。設計を写し替えれば良い。
 *
 *   WASAPI                          ALSA
 *   ------------------------------  ------------------------------------
 *   CoInitializeEx                  (不要)
 *   IMMDeviceEnumerator             snd_device_name_hint()
 *   GetMixFormat                    snd_pcm_hw_params_set_*_near で交渉
 *   Initialize(shared, EVENTCALLBACK) snd_pcm_hw_params + sw_params
 *   SetEventHandle + WaitForSingleObject snd_pcm_wait()
 *   GetCurrentPadding               snd_pcm_avail_update()
 *   GetBuffer/ReleaseBuffer         snd_pcm_writei()
 *   AvSetMmThreadCharacteristics    (SCHED_FIFO は使わない: 後述)
 *   AUDCLNT_E_DEVICE_INVALIDATED    -ENODEV
 *   (プリロール)                    snd_pcm_writei で無音を先詰め
 *
 * ===========================================================================
 * 実装上の難所 (1): VIM1 で音が出ない可能性への対処
 * ===========================================================================
 * Khadas フォーラムには「VIM1 / VIM1S で HDMI 音声が出ない」報告が
 * 複数あり、公式 Ubuntu ROM でも未解決の事例が多い。
 * これはハードウェア/ドライバ側の問題であってコードの問題ではない。
 *
 * 従って本実装の方針は
 *
 *   ★ ビルド時には絶対に除外しない (常にコンパイル・リンクする)
 *   ★ 実行時に snd_pcm_open() が失敗したら null バックエンドへ
 *     自動フォールバックし、モデム動作は続行する
 *
 * である。音が出なくてもダイアルアップは成立しなければならない。
 * (ハンドシェイク音は「懐かしさ」のための演出であり、
 *  PPP の成立には一切関与しない)
 *
 * フォールバックは run_alsa() の中で vm_audio_run_null() を
 * 直接呼ぶ形で行う。こうすると
 *   - a->ready_ev はフォールバック先が set するので呼び出し元は待つだけ
 *   - vm_audio_open() は VM_OK を返す (音は出ないが正常起動)
 *   - a->backend は ALSA のままだが device_name が "null device" になる
 * という素直な挙動になる。
 *
 * ===========================================================================
 * 実装上の難所 (2): デバイス選択と 8000Hz
 * ===========================================================================
 * 電話網は 8kHz だが、HDMI/I2S コーデックは 8000Hz を拒否する事が多い
 * (HDMI 音声の規格上の最低は 32kHz)。
 *
 * WASAPI 共有モードと同じ発想で、
 *   1. デバイスに 8000Hz を要求してみる
 *   2. 通らなければ snd_pcm_hw_params_set_rate_near() に任せる
 *   3. 実際に確定したレートを a->device_rate に入れる
 *   4. vm_audio_backend_resampler_init() が 8k -> 確定レートの
 *      リサンプラを用意する
 * とする。共通リサンプラ (vm_resample.c) が変換を担うので、
 * このファイルはレートを「交渉して報告する」だけで良い。
 *
 * デバイス名は params.device_match で部分一致指定できる
 * (WASAPI 版と同じ意味論)。マッチしない/未指定なら "default" を使う。
 * 明示的に "hw:0,1" 等と書かれた場合はそのまま渡す
 * (ALSA のデバイス名として解釈させる)。
 *
 * ===========================================================================
 * 実装上の難所 (3): float か int16 か
 * ===========================================================================
 * DSP は float32 で出力する。ALSA が SND_PCM_FORMAT_FLOAT_LE を
 * 受け付けるならゼロコピーに近い形で渡せるが、組み込み系の I2S
 * コーデックは S16_LE しか受けない事が多い。
 *
 * そこで
 *   1. FLOAT_LE を試す
 *   2. 駄目なら S16_LE にして自前で変換する
 * とする。変換はレンダースレッド内で行うが、バッファは
 * ループ開始前に確保するので malloc は発生しない
 * (WASAPI 版の store_samples() と同じ規律)。
 *
 * ===========================================================================
 * 実装上の難所 (4): グリッチを出さないための規律
 * ===========================================================================
 * WASAPI 版と同じ鉄則をそのまま適用する。レンダーループ内では
 *   - malloc / free をしない        (ヒープロックで数 ms 止まる)
 *   - ミューテックスを取らない      (プライオリティ インバージョン)
 *   - ファイル IO / printf をしない
 * PCM は vm_frb (ロックフリー SPSC リング) 経由で受け取り、
 * 足りなければ無音を埋める (vm_audio_backend_render が面倒を見る)。
 *
 * ★ SCHED_FIFO について ★
 *   WASAPI 版は MMCSS でスレッド優先度を上げていた。Linux の等価物は
 *   pthread_setschedparam(SCHED_FIFO) だが、これは
 *     - root 権限 (または RLIMIT_RTPRIO の設定) が必要
 *     - 暴走した時にシステム全体を巻き込んで固める
 *   という危険がある。本用途は 8kHz モノラルの軽い処理で、
 *   バッファも 4 周期 (約 80ms) 取るので通常の優先度で十分間に合う。
 *   従って意図的に優先度操作は【行わない】。
 *
 * ===========================================================================
 * 実装上の難所 (5): XRUN からの復帰
 * ===========================================================================
 * ALSA は再生が追いつかないと XRUN (-EPIPE) 状態になり、
 * 以降の書き込みが全部失敗するようになる。放置すると無音のままになる。
 *
 * snd_pcm_recover(pcm, err, silent) が
 *   -EPIPE   (XRUN)     -> snd_pcm_prepare()
 *   -ESTRPIPE (suspend) -> snd_pcm_resume() をリトライ
 * を適切に処理してくれるので、これを使うのが定石。
 * silent=1 にして alsa-lib が stderr に直接吐くのを抑え、
 * ログは我々の VM_LOG* に統一する。
 *
 * XRUN 自体はアンダーラン (耳に聞こえるグリッチ) なので回数を数える。
 */
#if !defined(__linux__)
/* 非 Linux では空翻訳単位にする (ビルドシステムが常に渡しても安全) */
typedef int vm_audio_alsa_dummy_t;
#else

/*
 * ---------------------------------------------------------------------------
 * ★ 機能テストマクロは alsa/asoundlib.h より前に定義する事 ★
 * ---------------------------------------------------------------------------
 * -std=c99 は __STRICT_ANSI__ を定義するため、glibc は既定で
 * POSIX の宣言を出さない。この状態で alsa/global.h を読むと
 *
 *     struct timespec { ... };   <- alsa/global.h が自前で定義する
 *
 * が pthread.h 経由で既に入っている glibc の struct timespec と
 * 衝突して「redefinition of 'struct timespec'」になる。
 * alsa/global.h は __USE_POSIX199309 が立っていれば自前定義を
 * 抑制するので、_POSIX_C_SOURCE を先に定義しておけば解決する。
 *
 * また snd_pcm_hw_params_alloca() は alloca(3) に展開されるので
 * <alloca.h> が必要になる (これも POSIX 宣言が要る)。
 *
 * ★ この順序は動作の前提である ★
 *   vm_audio_internal.h は pthread.h を引くので、
 *   このブロックより後に include しなければならない。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "vm_audio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <alloca.h>

#include <alsa/asoundlib.h>

/* ---------------------------------------------------------------------------
 * 定数
 * -------------------------------------------------------------------------*/
enum {
    /*
     * 1 周期 (period) の長さ。ALSA は period 単位で割り込みを上げる。
     * WASAPI 版の 20ms ブロックと揃えて 20ms とする。
     * DSP も 20ms (VM_DSP_BLOCK_SAMPLES=160 @ 8kHz) 単位で生成するので
     * 位相が揃い、余分なバッファリングが発生しない。
     */
    ALSA_PERIOD_MS = 20,

    /*
     * バッファ全体の周期数。4 周期 = 80ms。
     * 大きいほど XRUN に強いが、停止要求への追従とレイテンシが悪化する。
     * 「懐かしい音を聞かせる」用途では 80ms の遅延は問題にならない。
     */
    ALSA_PERIODS = 4,

    /* snd_pcm_wait のタイムアウト。quit を確認するために有限にする */
    ALSA_WAIT_MS = 200,

    /* 中間バッファの上限 (フレーム数 x チャネル数) */
    ALSA_MAX_FRAMES = 8192,
    ALSA_MAX_CH     = 8
};

/* ---------------------------------------------------------------------------
 * デバイス名の決定
 * ---------------------------------------------------------------------------
 * params.device_match の解釈:
 *   NULL / ""          -> "default"
 *   "hw:..","plughw:.." -> そのまま (ALSA デバイス名と見なす)
 *   "default","sysdefault" 等 -> そのまま
 *   それ以外           -> 部分一致検索 (WASAPI 版と同じ意味論)
 *
 * 部分一致検索は snd_device_name_hint() で PCM デバイスを列挙し、
 * NAME または DESC に match が含まれる最初のものを採用する。
 * 例: device_match="HDMI" -> "hdmi:CARD=..." が見つかる
 * -------------------------------------------------------------------------*/
static void pick_device(const char *match, char *out, size_t out_size,
                        char *desc_out, size_t desc_size)
{
    void **hints = NULL;
    void **h;
    bool found = false;

    snprintf(out, out_size, "%s", "default");
    snprintf(desc_out, desc_size, "%s", "default");

    if (!match || !match[0]) return;

    /* ALSA デバイス名らしい綴りならそのまま使う */
    if (strncmp(match, "hw:", 3) == 0 ||
        strncmp(match, "plughw:", 7) == 0 ||
        strncmp(match, "default", 7) == 0 ||
        strncmp(match, "sysdefault", 10) == 0 ||
        strncmp(match, "dmix", 4) == 0 ||
        strchr(match, ':') != NULL) {
        snprintf(out, out_size, "%s", match);
        snprintf(desc_out, desc_size, "%s", match);
        return;
    }

    /* 部分一致検索 */
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        VM_LOGW("snd_device_name_hint に失敗: default を使います");
        return;
    }

    for (h = hints; *h != NULL && !found; h++) {
        char *name = snd_device_name_get_hint(*h, "NAME");
        char *desc = snd_device_name_get_hint(*h, "DESC");
        char *ioid = snd_device_name_get_hint(*h, "IOID");

        /*
         * IOID が "Input" のものは録音専用なので除外する。
         * NULL は「入出力両対応」を意味するので採用可。
         */
        bool is_output = (ioid == NULL) || (strcmp(ioid, "Input") != 0);

        if (name && is_output &&
            (strstr(name, match) != NULL ||
             (desc && strstr(desc, match) != NULL))) {
            snprintf(out, out_size, "%s", name);
            snprintf(desc_out, desc_size, "%s", desc ? desc : name);
            found = true;
        }

        free(name);
        free(desc);
        free(ioid);
    }
    snd_device_name_free_hint(hints);

    if (found) {
        VM_LOGI("ALSA デバイス \"%s\" に一致: %s", match, out);
    } else {
        VM_LOGW("ALSA デバイス \"%s\" に一致するものが無い: default を使います",
                match);
    }
}

/* ---------------------------------------------------------------------------
 * float -> S16_LE 変換
 * ---------------------------------------------------------------------------
 * レンダースレッドから呼ばれるので malloc / IO をしない。
 * クリップは WASAPI 版と同じく ±1.0 で飽和させる。
 * -------------------------------------------------------------------------*/
static void store_s16(int16_t *dst, const float *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        float s = src[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        dst[i] = (int16_t)(s * 32767.0f);
    }
}

/* ---------------------------------------------------------------------------
 * ハードウェア パラメータの交渉
 * ---------------------------------------------------------------------------
 * 成功時 0。format / rate / channels / period_frames / buffer_frames を
 * 確定値で返す。
 * -------------------------------------------------------------------------*/
static int negotiate(snd_pcm_t *pcm, int want_rate,
                     snd_pcm_format_t *out_fmt, unsigned *out_rate,
                     unsigned *out_ch, snd_pcm_uframes_t *out_period,
                     snd_pcm_uframes_t *out_buffer)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_format_t fmt;
    unsigned rate, ch;
    snd_pcm_uframes_t period, buffer;
    unsigned period_time, buffer_time;
    int dir, err;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) {
        VM_LOGE("snd_pcm_hw_params_any 失敗: %s", snd_strerror(err));
        return err;
    }

    /*
     * ---- アクセス方式 ----
     * RW_INTERLEAVED = snd_pcm_writei() で書く方式。
     * mmap 方式の方が理論上速いが、writei はドライバの対応が広く
     * plug プラグイン経由でも確実に動くのでこちらを選ぶ。
     * 8kHz モノラルの帯域では性能差は無意味。
     */
    if ((err = snd_pcm_hw_params_set_access(pcm, hw,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        VM_LOGE("set_access(RW_INTERLEAVED) 失敗: %s", snd_strerror(err));
        return err;
    }

    /*
     * ---- フォーマット ----
     * FLOAT_LE が通れば変換不要。組み込みコーデックは S16_LE だけの
     * 事が多いのでフォールバックする。
     */
    fmt = SND_PCM_FORMAT_FLOAT_LE;
    if (snd_pcm_hw_params_set_format(pcm, hw, fmt) < 0) {
        fmt = SND_PCM_FORMAT_S16_LE;
        if ((err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0) {
            VM_LOGE("FLOAT_LE / S16_LE いずれも拒否されました: %s",
                    snd_strerror(err));
            return err;
        }
        VM_LOGI("ALSA フォーマット: S16_LE (float から変換します)");
    } else {
        VM_LOGI("ALSA フォーマット: FLOAT_LE (変換不要)");
    }

    /*
     * ---- チャネル数 ----
     * モノラルを要求する。HDMI はステレオ以上しか受けない事が多いので
     * set_channels_near に任せて確定値を受け取る。
     * 共通の vm_audio_backend_render() が device_channels 分だけ
     * 複製してくれるので、2ch でも 8ch でも正しく鳴る。
     */
    ch = 1;
    if ((err = snd_pcm_hw_params_set_channels_near(pcm, hw, &ch)) < 0) {
        VM_LOGE("set_channels_near 失敗: %s", snd_strerror(err));
        return err;
    }
    if (ch < 1 || ch > ALSA_MAX_CH) {
        VM_LOGE("対応外のチャネル数: %u", ch);
        return -EINVAL;
    }

    /*
     * ---- サンプルレート ----
     * まず 8000Hz をそのまま要求する (通れば変換不要で最高品質)。
     * HDMI は 32kHz 未満を拒否するので、その場合は near に任せる。
     */
    rate = (unsigned)want_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0) {
        VM_LOGE("set_rate_near(%d) 失敗: %s", want_rate, snd_strerror(err));
        return err;
    }
    if (rate == (unsigned)want_rate) {
        VM_LOGI("ALSA レート: %u Hz (DSP レートと一致: リサンプル不要)", rate);
    } else {
        VM_LOGI("ALSA レート: %u Hz (%d Hz は拒否されたのでリサンプルします)",
                rate, want_rate);
    }

    /*
     * ---- period / buffer サイズ ----
     * 時間指定 (set_period_time_near) にするのがレート非依存で安全。
     * フレーム数で指定すると、レートが変わった時に時間長が変わってしまう。
     */
    period_time = ALSA_PERIOD_MS * 1000;                  /* usec */
    dir = 0;
    if ((err = snd_pcm_hw_params_set_period_time_near(pcm, hw,
                    &period_time, &dir)) < 0) {
        VM_LOGW("set_period_time_near 失敗: %s (既定値を使います)",
                snd_strerror(err));
    }

    buffer_time = ALSA_PERIOD_MS * ALSA_PERIODS * 1000;   /* usec */
    dir = 0;
    if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw,
                    &buffer_time, &dir)) < 0) {
        VM_LOGW("set_buffer_time_near 失敗: %s (既定値を使います)",
                snd_strerror(err));
    }

    /* ---- 確定 ---- */
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        VM_LOGE("snd_pcm_hw_params 適用に失敗: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_get_period_size(hw, &period, &dir)) < 0) {
        VM_LOGE("get_period_size 失敗: %s", snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_hw_params_get_buffer_size(hw, &buffer)) < 0) {
        VM_LOGE("get_buffer_size 失敗: %s", snd_strerror(err));
        return err;
    }

    if (period == 0 || period > ALSA_MAX_FRAMES) {
        VM_LOGE("period サイズ %lu が中間バッファを超えています",
                (unsigned long)period);
        return -EINVAL;
    }

    /*
     * ---- ソフトウェア パラメータ ----
     * start_threshold : この量が溜まったら自動で再生を開始する。
     *                   buffer 全体にすると「満タンまで鳴らない」。
     *                   period にして、1 周期書いたら鳴り始めるようにする。
     * avail_min       : snd_pcm_wait() が「書ける」と判断する閾値。
     *                   period にすると 1 周期分の空きが出た時に起きる。
     *                   WASAPI の EVENTCALLBACK と同じ挙動になる。
     */
    if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0) {
        VM_LOGE("sw_params_current 失敗: %s", snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw, period)) < 0)
        VM_LOGW("set_start_threshold 失敗: %s", snd_strerror(err));
    if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0)
        VM_LOGW("set_avail_min 失敗: %s", snd_strerror(err));
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
        VM_LOGE("snd_pcm_sw_params 適用に失敗: %s", snd_strerror(err));
        return err;
    }

    *out_fmt    = fmt;
    *out_rate   = rate;
    *out_ch     = ch;
    *out_period = period;
    *out_buffer = buffer;
    return 0;
}

/* ===========================================================================
 * レンダー ループ本体
 * ===========================================================================
 * vm_audio_internal.h の契約に従う:
 *   1. 自前の初期化
 *   2. a->device_rate / device_channels / device_name を埋める
 *   3. vm_audio_backend_resampler_init(a) を呼ぶ
 *   4. a->ready_result に VM_OK を入れて vm_ev_set(a->ready_ev)
 *   5. a->quit が立つまでレンダー
 *   6. 後片付け
 * 失敗時は null バックエンドへフォールバックする (ready_ev は
 * フォールバック先が set するので、ここでは set しない)。
 * =========================================================================*/
void vm_audio_run_alsa(vm_audio_t *a)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_S16_LE;
    snd_pcm_uframes_t period = 0, buffer = 0;
    unsigned rate = 0, ch = 0;
    char dev[128], desc[VM_AUDIO_NAME_MAX];
    int err;
    vm_err_t rc;

    /*
     * 中間バッファ。レンダーループ内で malloc しないため
     * ループ開始前に確保する (WASAPI 版と同じ規律)。
     * mix  : 共通レンダラが書く float (interleaved, ch 分)
     * cvt  : S16_LE 変換先 (FLOAT_LE の場合は未使用)
     */
    float   *mix = NULL;
    int16_t *cvt = NULL;

    if (!a) return;

    pick_device(a->params.device_match, dev, sizeof(dev),
                desc, sizeof(desc));

    /*
     * ------------------------------------------------------------------
     * デバイスを開く
     * ------------------------------------------------------------------
     * SND_PCM_NONBLOCK は使わない。ブロッキングで開き、
     * snd_pcm_wait() で待つ方が制御が素直になる
     * (WASAPI の WaitForSingleObject(event) と同じ構図)。
     * quit への追従は snd_pcm_wait のタイムアウトで確保する。
     */
    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        /*
         * ★ ここが VIM1 の HDMI 音声問題に対する防衛線 ★
         * 「サウンドカードが無い」(-ENODEV / -ENOENT) は
         * VIM1 + Armbian では十分あり得る。
         * エラーログを出して null バックエンドへ自動フォールバックし、
         * モデム動作は続行させる。
         */
        VM_LOGE("ALSA デバイス \"%s\" を開けません: %s",
                dev, snd_strerror(err));
        VM_LOGW("音声を無効化して続行します (null バックエンドへフォールバック)");
        VM_LOGW("VIM1 では HDMI 音声が動作しない事例が報告されています。"
                "aplay -l でデバイスの有無を確認してください");
        vm_audio_run_null(a);
        return;
    }

    /* ---- ハードウェア パラメータの交渉 ---- */
    err = negotiate(pcm, a->params.dsp_rate,
                    &fmt, &rate, &ch, &period, &buffer);
    if (err < 0) {
        VM_LOGE("ALSA パラメータの交渉に失敗しました");
        VM_LOGW("音声を無効化して続行します (null バックエンドへフォールバック)");
        snd_pcm_close(pcm);
        vm_audio_run_null(a);
        return;
    }

    /* ---- 共通層へ確定値を報告 ---- */
    a->device_rate     = (int)rate;
    a->device_channels = (int)ch;
    snprintf(a->device_name, sizeof(a->device_name), "%s", desc);

    VM_LOGI("ALSA: %s / %u Hz %u ch / period=%lu frames (%.1f ms) "
            "/ buffer=%lu frames (%.1f ms)",
            dev, rate, ch,
            (unsigned long)period, 1000.0 * (double)period / rate,
            (unsigned long)buffer, 1000.0 * (double)buffer / rate);

    /* ---- リサンプラ初期化 (デバイスレート確定後) ---- */
    rc = vm_audio_backend_resampler_init(a);
    if (rc != VM_OK) {
        VM_LOGE("リサンプラ初期化に失敗しました");
        snd_pcm_close(pcm);
        vm_audio_run_null(a);
        return;
    }

    /* ---- 中間バッファ確保 ---- */
    mix = (float *)malloc((size_t)period * (size_t)ch * sizeof(float));
    if (fmt == SND_PCM_FORMAT_S16_LE)
        cvt = (int16_t *)malloc((size_t)period * (size_t)ch * sizeof(int16_t));

    if (!mix || (fmt == SND_PCM_FORMAT_S16_LE && !cvt)) {
        free(mix); free(cvt);
        snd_pcm_close(pcm);
        a->ready_result = (int)VM_ERR_NOMEM;
        vm_ev_set(a->ready_ev);
        return;
    }

    /*
     * ------------------------------------------------------------------
     * プリロール
     * ------------------------------------------------------------------
     * WASAPI 版と同じく、開始前にバッファを無音で埋める。
     * これをやらないと開始直後に必ずグリッチが出る。
     *
     * start_threshold=period にしてあるので、1 周期書いた時点で
     * 自動的に再生が始まる。明示的な snd_pcm_start() は不要
     * (むしろ start_threshold と競合するので呼ばない)。
     */
    if ((err = snd_pcm_prepare(pcm)) < 0)
        VM_LOGW("snd_pcm_prepare 失敗: %s", snd_strerror(err));

    {
        snd_pcm_uframes_t pre;
        memset(mix, 0, (size_t)period * (size_t)ch * sizeof(float));
        if (cvt) memset(cvt, 0, (size_t)period * (size_t)ch * sizeof(int16_t));

        for (pre = 0; pre + period <= buffer; pre += period) {
            const void *src = cvt ? (const void *)cvt : (const void *)mix;
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, src, period);
            if (w < 0) {
                if (snd_pcm_recover(pcm, (int)w, 1) < 0) break;
            }
        }
    }

    /* ---- 初期化成功を通知 ---- */
    a->ready_result = (int)VM_OK;
    vm_ev_set(a->ready_ev);

    /* ================================================================= */
    /* レンダー ループ                                                    */
    /* ================================================================= */
    while (!a->quit) {
        snd_pcm_sframes_t avail;
        snd_pcm_sframes_t wrote;
        const void *src;

        /*
         * デバイスが「1 周期分の空きが出た」と教えてくれるのを待つ。
         * avail_min=period にしてあるので、WASAPI の
         * EVENTCALLBACK と同じタイミングで起きる。
         *
         * タイムアウトを付けるのは、デバイス取り外し等で
         * イベントが来なくなった場合に quit を確認するため。
         */
        err = snd_pcm_wait(pcm, ALSA_WAIT_MS);
        if (err == 0) continue;              /* タイムアウト: quit を確認 */
        if (err < 0) {
            if (err == -EINTR) continue;
            /*
             * XRUN / suspend からの復帰を試みる。
             * 復帰できなければループを抜けるが、モデム動作は
             * 継続させたいのでプロセスは終了させない。
             */
            if (snd_pcm_recover(pcm, err, 1) < 0) {
                VM_LOGW("snd_pcm_wait が回復不能なエラー (%s): "
                        "レンダーを停止します", snd_strerror(err));
                break;
            }
            a->ring.underruns++;
            continue;
        }

        if (a->quit) break;

        /* flush 要求の処理 (消費者スレッドなので安全に reset できる) */
        vm_audio_backend_handle_flush(a);

        avail = snd_pcm_avail_update(pcm);
        if (avail < 0) {
            if (snd_pcm_recover(pcm, (int)avail, 1) < 0) {
                VM_LOGW("snd_pcm_avail_update が回復不能なエラー (%s): "
                        "レンダーを停止します", snd_strerror((int)avail));
                break;
            }
            a->ring.underruns++;
            continue;
        }

        /*
         * 1 周期分溜まっていなければ次のイベントを待つ。
         * 中途半端な量を書くと period 境界がずれて XRUN しやすくなる。
         */
        if ((snd_pcm_uframes_t)avail < period) continue;

        /* リング -> リサンプル -> 音量 -> チャネル複製 */
        vm_audio_backend_render(a, mix, (uint32_t)period);

        if (cvt) {
            store_s16(cvt, mix, (uint32_t)period * (uint32_t)ch);
            src = cvt;
        } else {
            src = mix;
        }

        wrote = snd_pcm_writei(pcm, src, period);
        if (wrote < 0) {
            /*
             * -EPIPE = XRUN。耳に聞こえるグリッチなので計上する。
             * snd_pcm_recover が prepare/resume を適切に呼んでくれる。
             */
            if (wrote == -EPIPE) a->ring.underruns++;

            if (snd_pcm_recover(pcm, (int)wrote, 1) < 0) {
                VM_LOGW("snd_pcm_writei が回復不能なエラー (%s): "
                        "レンダーを停止します", snd_strerror((int)wrote));
                break;
            }
            continue;
        }

        if ((snd_pcm_uframes_t)wrote < period) {
            /*
             * 部分書き込み。ブロッキングモードでは通常起こらないが、
             * 起きた場合は次の周期で続きが書かれるので放置して良い
             * (波形は連続する)。
             */
            VM_LOGD("ALSA 部分書き込み: %ld/%lu frames",
                    (long)wrote, (unsigned long)period);
        }
    }

    /* ---- 停止 ---- */
    /*
     * snd_pcm_drain() は「溜まっている音を鳴らし切ってから止める」。
     * 停止要求に素早く応えたいので drop (即座に破棄) を使う。
     * 鳴らし切りたい場合は上位層が vm_audio_drain() を先に呼ぶ。
     */
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    free(mix);
    free(cvt);

    VM_LOGI("ALSA レンダー終了 (underruns=%u)",
            (unsigned)a->ring.underruns);
}

#endif /* __linux__ */
