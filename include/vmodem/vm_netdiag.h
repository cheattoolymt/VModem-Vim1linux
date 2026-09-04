/*
 * vm_netdiag.h - ホスト側ネットワーク環境の実測 (Linux 向け)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * なぜこのモジュールが必要になったのか
 * ===========================================================================
 * 症状: 「PPP は確立し IP も配られ NLA の HTTP チェックも通る。
 *        しかしブラウザは ERR_NAME_NOT_RESOLVED、ping 8.8.8.8 も無応答」
 *
 * この 2 つは別々の原因を持つ。どちらも **ホスト (VIM1) 側の環境** に
 * 依存するため、コンパイル時には決められない。実行時に測るしかない。
 *
 * ---------------------------------------------------------------------------
 * (A) 名前解決が出来ない理由 -- libslirp の DNS 代理と systemd-resolved の衝突
 * ---------------------------------------------------------------------------
 * まず、切り分けで必ず引っかかる罠を先に潰しておく。
 *
 *   ★ VIM1 自身から `nslookup google.com 192.168.99.3` が
 *     タイムアウトするのは「正常」である ★
 *
 * libslirp は純粋なユーザ空間ライブラリで、カーネルの経路表にも
 * netfilter にも一切フックしない。192.168.99.3 という仮想アドレスは
 * libslirp のプロセス内部にしか存在しない。DNS 代理が働くのは
 *
 *     slirp_input(ゲストの Ethernet フレーム)
 *       -> ip_input -> udp_input -> sosendto
 *       -> sotranslate_out4()   ← ここで宛先を書き換える
 *
 * という **ゲスト由来のパケットだけ** が通る経路である
 * (libslirp master src/socket.c sotranslate_out4)。
 *
 *     if (!s->disable_dns && so->so_faddr.s_addr == s->vnameserver_addr.s_addr) {
 *         return (so->so_fport == htons(53) &&
 *                 get_dns_addr(&sin->sin_addr, &sin->sin_port) >= 0);
 *     }
 *
 * ホストの IP スタックから 192.168.99.3:53 へ UDP を投げても、その
 * パケットは libslirp に届かない (誰も listen していないアドレスなので
 * カーネルが捨てる)。よって nslookup のタイムアウトは代理の故障を
 * 意味しない。**この切り分けは無効** であり、ここで数時間を失う。
 *
 * では本当の原因は何か。上のコードで宛先は get_dns_addr() の値に
 * 書き換えられる。Linux 版の get_dns_addr() は /etc/resolv.conf の
 * 最初の nameserver を返す (libslirp master src/slirp.c
 * get_dns_addr_resolv_conf)。
 *
 * Armbian / Debian / Ubuntu で systemd-resolved が動いていると、
 * resolv.conf は次のようになっている:
 *
 *     nameserver 127.0.0.53      <- systemd-resolved の stub listener
 *
 * ここで本実装が SlirpConfig.outbound_addr を設定している事が効いてくる。
 * libslirp は全ての外向きソケットに対し slirp_bind_outbound() で
 * bind(2) を行うので、DNS 代理のソケットも **実 NIC の IP** に固定される。
 * つまり
 *
 *     送信元 192.168.x.y:随意ポート  ->  宛先 127.0.0.53:53
 *
 * という UDP が飛ぶ。ここが致命的で、systemd-resolved の stub listener は
 * ループバック上のローカル発信のみを想定しており、この様な非ローカル
 * 送信元のクエリには応答しない (freedesktop の systemd-resolved.service
 * マニュアル: "provides a local DNS stub listener on ... the local
 * loopback interface")。応答が来ないので、
 *
 *   - ゲストの DNS クエリは無応答のまま SO_EXPIREFAST (10 秒) で回収
 *   - Windows は名前解決に失敗し ERR_NAME_NOT_RESOLVED
 *
 * となる。sendto(2) 自体は成功する (実測: 送信は成功し、素の echo
 * サーバ相手なら応答も返る) ため、strace でも「送れている」ように見え、
 * 極めて切り分けにくい。
 *
 * ★ 従来のコードのコメントは「実測で問題無しと確認できた」と書いていたが、
 *   その実測は素の UDP echo に対するものであり、systemd-resolved の
 *   stub listener に対する検証ではなかった。ここが誤りだった ★
 *
 * したがって判定に必要な事実は 1 つ:
 *   「/etc/resolv.conf の nameserver がループバック (127.0.0.0/8) か否か」
 * これを vm_netdiag_host_resolver() で測る。
 *
 * ---------------------------------------------------------------------------
 * (B) ping が通らない理由 -- ICMP ソケットの権限
 * ---------------------------------------------------------------------------
 * libslirp の ICMP は次の 2 段構えである
 * (libslirp master src/ip_icmp.c icmp_send):
 *
 *     so->s = slirp_socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
 *     if (not_valid_socket(so->s)) {
 *         if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT
 *          || errno == EACCES) {
 *             so->so_type = IPPROTO_IP;
 *             so->s = slirp_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
 *         }
 *     }
 *     if (not_valid_socket(so->s)) {
 *         return -1;                 ← ここに来ると ping は永久に無応答
 *     }
 *
 *   1 段目 SOCK_DGRAM+IPPROTO_ICMP ("ping ソケット")
 *        net.ipv4.ping_group_range に自分の gid が含まれていないと EACCES。
 *        Debian 系の既定は `1 0` (lo > hi = 空集合) で **誰も使えない**。
 *   2 段目 SOCK_RAW+IPPROTO_ICMP
 *        CAP_NET_RAW が必要。無いと EPERM。
 *
 * 両方失敗すると icmp_send は -1 を返し、ゲストの ICMP Echo は
 * 静かに捨てられる。ログにも何も出ない。これが
 * 「ping 8.8.8.8 だけタイムアウトする」の正体である。
 *
 * さらに ping_group_range は sysctl なので **再起動でリセットされる**。
 * 手で echo した人は「昨日は動いたのに」となる。
 *
 * よって起動時に実際に socket(2) を試し、駄目なら直し方を含めて
 * 警告する。これを vm_netdiag_icmp_probe() で行う。
 *
 * ---------------------------------------------------------------------------
 * 設計方針
 * ---------------------------------------------------------------------------
 *  - 副作用を持たない。読むだけ。sysctl を勝手に書き換えたりしない
 *    (root で動く事が多いので、黙って書くと運用者が把握できなくなる)。
 *  - 環境変数でパスを差し替えられるようにし、テストから検証できる。
 *      VM_RESOLV_CONF        /etc/resolv.conf の代わりに読むファイル
 *      VM_PING_GROUP_RANGE   ping_group_range の代わりに読むファイル
 *  - Windows では常に「不明」を返す。Windows 版の get_dns_addr() は
 *    GetNetworkParams() を使い resolv.conf を読まないので、
 *    (A) の問題は原理的に起きない。
 * ===========================================================================
 */
#ifndef VMODEM_VM_NETDIAG_H
#define VMODEM_VM_NETDIAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * ホストのリゾルバ (/etc/resolv.conf) の観測結果
 * -------------------------------------------------------------------------- */
typedef struct {
    /* resolv.conf を読めて nameserver が 1 つ以上あったか */
    bool     available;

    /*
     * 最初の nameserver。ホストバイトオーダ。
     * libslirp の get_dns_addr() は「最初の 1 つ」を代理先に使うので、
     * 我々が見るべきものも最初の 1 つである。
     */
    uint32_t first;

    /* nameserver 行の数 (参考情報。ログに出すだけ) */
    int      count;

    /*
     * first が 127.0.0.0/8 か。
     * これが true の時 libslirp の DNS 代理は
     * systemd-resolved 等の stub listener 宛になり、
     * outbound_addr と併用すると応答が返らない。
     */
    bool     loopback;
} vm_netdiag_resolver_t;

/*
 * /etc/resolv.conf を読む。
 * 戻り値は out->available と同じ (呼び出し側の書き方の好みに合わせる)。
 * out は必ず初期化される (失敗時は全 0 = available:false)。
 *
 * パースは libslirp の get_dns_addr_resolv_conf() と同じ規則にする:
 *   - "nameserver" の後に空白、その後にアドレス
 *   - "%" 以降はインタフェース指定なので切り落とす
 *   - IPv6 は無視する (IPv4 の代理先しか関係しないため)
 * ここが libslirp とずれると判定が無意味になるので、
 * 変更する時は必ず libslirp 側の実装と突き合わせる事。
 */
bool vm_netdiag_host_resolver(vm_netdiag_resolver_t *out);

/* --------------------------------------------------------------------------
 * ICMP ソケットの可用性
 * -------------------------------------------------------------------------- */
typedef struct {
    /* SOCK_DGRAM + IPPROTO_ICMP (ping ソケット) が開けたか */
    bool dgram_ok;
    int  dgram_err;      /* 開けなかった時の errno */

    /* SOCK_RAW + IPPROTO_ICMP が開けたか */
    bool raw_ok;
    int  raw_err;        /* 開けなかった時の errno */

    /* ping_group_range を読めたか、及びその値 */
    bool     range_known;
    uint32_t range_lo;
    uint32_t range_hi;

    /* 実行中の実 gid が range に含まれるか (range_known 時のみ有効) */
    bool gid_in_range;
} vm_netdiag_icmp_t;

/*
 * ICMP ソケットを実際に開いてみて、すぐ閉じる。
 * 「開ける」事の確認なので副作用は無い。
 *
 * 戻り値: どちらか一方でも開けたら true (= ゲストの ping が通る見込み)。
 *
 * ★ 起動時に 1 度だけ呼ぶ事 ★
 *   毎パケット呼ぶと socket(2) の無駄打ちになる。
 */
bool vm_netdiag_icmp_probe(vm_netdiag_icmp_t *out);

/*
 * 上記 2 つを実行して、人間が読める診断をログ (VM_LOGI / VM_LOGW) に出す。
 * NAT バックエンドの起動直後に呼ぶ事を想定している。
 *
 * nat_network / nat_netmask はホストバイトオーダ。
 * 「リゾルバが我々の仮想ネットワークの中を指している」という
 * 自己参照の設定ミスも検出したいので受け取る。
 */
void vm_netdiag_report(uint32_t nat_network, uint32_t nat_netmask);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_NETDIAG_H */
