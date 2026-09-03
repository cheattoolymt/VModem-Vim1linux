/*
 * vm_hostroute_linux.c - ホスト側の実インターネット経路の検出 (Linux 版)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Windows 版 src/net/vm_hostroute.c と **同じインタフェース**
 * (include/vmodem/vm_hostroute.h の vm_hostroute_pick_outbound_ip) を
 * Linux 向けに実装する。ビルド時にどちらか一方だけをリンクする
 * (Makefile.linux は本ファイル、Windows 側は vm_hostroute.c)。
 *
 * ===========================================================================
 * 目的: libslirp の SlirpConfig.outbound_addr に入れる IPv4 を決める
 * ===========================================================================
 * Windows 版の背景 (RAS がデフォルト経路を奪い、libslirp の sendto が
 * 自分の PPP に吸い込まれて WSAENETUNREACH になる) は
 * include/vmodem/vm_hostroute.h の冒頭に詳しく書かれている。
 *
 * ★Linux ではその「自己参照ループ」は起きない★
 *
 *   1. Linux の PPP は VModem の内部実装であって、**カーネルの
 *      ネットワークスタックには一切現れない**。
 *      我々がやっているのは
 *        /dev/ttyGS0 から HDLC フレームを読み、vm_ppp.c で解釈し、
 *        取り出した生 IP を libslirp に渡す
 *      という完全にユーザ空間の処理である。pppd も ppp0 も無い。
 *      したがってカーネルの経路表に 0.0.0.0/0 → 192.168.99.1 が
 *      入ることは原理的にない。Windows は RAS (カーネルのドライバ) が
 *      本物のインタフェースを作るので事情が全く違う。
 *   2. Linux は既定で weak host model (rp_filter を除く) なので、
 *      bind していないソケットの出力インタフェースは経路表だけで決まる。
 *
 * ではなぜ Linux でも outbound_addr を設定するのか。
 * 指示書 Step 5 は「Linux 版では同様の問題は発生しないが、
 * outbound_addr の設定は維持する」と明示している。実利も 3 つある。
 *
 *   (a) VIM1 は LAN と Wi-Fi の両方を持ち、しかも Tailscale が入っている
 *       (実機で確認済み)。tailscale0 は 100.64.0.0/10 で、経路を
 *       広く奪う設定もありうる。送信元を実 NIC に固定しておけば、
 *       ゲストの通信が意図せず VPN に入る事故を防げる。
 *   (b) 送信元アドレスが毎回同じになるので、tcpdump / 上流ルータの
 *       ログでゲストの通信を追跡しやすい。障害解析の速度が変わる。
 *   (c) Windows 版と Linux 版で libslirp に渡す設定を揃えられる。
 *       「Windows では動くが Linux では動かない」時の差分を
 *       コード側で減らしておく方が原因究明が早い。
 *
 * ===========================================================================
 * 検出方式: connect(2) で「カーネルに聞く」→ getifaddrs(3) で検証する
 * ===========================================================================
 * Windows 版は GetBestRoute() + GetAdaptersAddresses() の 2 段構えだった。
 * Linux での対応物を選ぶ際、以下を比較した。
 *
 *   [案 1] /proc/net/route を読む
 *       × IPv4 専用の 16 進テキストで、metric や multipath (nexthop group)
 *         を正しく解釈するのが難しい。policy routing (ip rule) を
 *         完全に無視するので、複数テーブルがある環境で誤る。
 *   [案 2] rtnetlink (RTM_GETROUTE) を自前で組む
 *       × 正しいが 300 行以上になり、libmnl も無い環境で書くのは
 *         バグを埋め込むだけ。しかも下の案 3 と同じ答えしか出ない。
 *   [案 3] UDP ソケットを connect(2) して getsockname(2) を読む ← 採用
 *       ○ カーネルの経路検索そのものを 1 回呼ぶだけ。policy routing・
 *         metric・multipath・IPv6 も含めてカーネルの判断と 100% 一致する。
 *       ○ **UDP の connect は 1 バイトもパケットを出さない**。
 *         (Linux では宛先の経路を引いて送信元を決めるだけ。
 *          man 2 connect: SOCK_DGRAM では相手を記録するのみ)
 *         だからネットワークが未接続でも副作用がなく、実行も速い。
 *       ○ 本サンドボックスで実測し、8.8.8.8 に connect すると
 *         source=169.254.0.21 (eth0 の実アドレス) が取れる事を確認した。
 *
 * ただし案 3 だけでは足りない。getsockname が返すのは「アドレス」であって
 * 「どのインタフェースのアドレスか」は分からない。tun/tap/tailscale が
 * デフォルト経路を持っていると、それらのアドレスが返る。
 * よって取得したアドレスを getifaddrs(3) で **必ず検証**し、
 * 仮想インタフェースのものだったら採用しない。
 *
 *   段 1: connect(8.8.8.8) + getsockname  → 候補アドレス
 *   段 2: getifaddrs で候補の持ち主を特定し、実 NIC か検証
 *   段 3: 検証に落ちたら getifaddrs を総当たりして実 NIC を 1 つ選ぶ
 *   段 4: それでも見つからなければ 0 を返す (outbound_addr を設定しない)
 *
 * 段 4 が「安全な失敗」である理由: outbound_addr が NULL なら libslirp は
 * bind() を省略し、カーネルの経路表任せの従来動作になる (src/misc.c
 * slirp_bind_outbound は addr == NULL なら何もせず 0 を返す)。
 * Linux ではそれで正常に通信できるので、無理に設定するより安全である。
 *
 * ===========================================================================
 * 除外条件 (指示書 Step 5-a の要求 + 実装上必要になった分)
 * ===========================================================================
 * 指示書が挙げた 4 つ:
 *   - ループバック            127.0.0.0/8
 *   - APIPA                   169.254.0.0/16
 *   - PPP インタフェース      IFF_POINTOPOINT
 *   - トンネル・仮想 NIC      tun / tap / ppp / docker / veth 等の名前
 * これに加えて実装した分:
 *   - IFF_UP / IFF_RUNNING が立っていないもの (リンクダウン中の NIC)
 *   - マルチキャスト 224.0.0.0/4 / ブロードキャスト 255.255.255.255 / 0.0.0.0
 *   - 呼び出し側が渡す仮想ネットワーク (既定 192.168.99.0/24)
 *
 * ★IFF_POINTOPOINT を除外する理由と、その副作用★
 * ppp0 / tun0 は IFF_POINTOPOINT が立つので、名前による判定を
 * すり抜けた独自命名 (wg-home, vpn-office 等) も拾える。
 * 副作用として、PPPoE で直接インターネットに出ている環境では
 * WAN 側が ppp0 になっており、それが唯一の外向き NIC でありうる。
 * その場合は本関数が 0 を返し、libslirp は bind せず従来動作になる。
 * Linux ではそれで正しく通信できるので問題にならない
 * (指示書の除外条件を優先する)。
 *
 * ★APIPA (169.254/16) を除外する理由と例外★
 * 通常 169.254 は DHCP 失敗時の自動設定で、経路が無い。
 * ところが本開発サンドボックスの eth0 は 169.254.0.21/30 で、
 * そこが唯一の外向き経路である (default via 169.254.0.22)。
 * つまり「APIPA だが実際には使える」環境が現実に存在する。
 * そこで
 *   - 段 1〜3 では APIPA を除外する (指示書通り)
 *   - 全部落ちた時に限り、段 1 の候補が APIPA でも
 *     「カーネルがデフォルト経路として選んだ実 NIC のアドレス」で
 *     あれば最後の候補として採用する
 * という二段構えにした。指示書の意図 (経路の無いアドレスに bind して
 * 通信を壊さない) を守りつつ、実在する構成で動かなくなる事を防ぐ。
 *
 * ===========================================================================
 * getifaddrs(3) の注意点 (manpage より)
 * ===========================================================================
 *   - 返るリストは freeifaddrs() で解放する。要素だけの free は不可。
 *   - ifa_addr は **NULL になりうる** (アドレスを持たない ifb など)。
 *     参照する前に必ず NULL と sa_family を確認する。
 *   - 同じ ifa_name のエントリが複数回現れる (AF_INET / AF_INET6 /
 *     AF_PACKET、および 1 NIC 複数アドレス)。
 *   - glibc では _GNU_SOURCE / _DEFAULT_SOURCE が必要
 *     (-std=c99 は __STRICT_ANSI__ を定義するので、#include より前に
 *      機能テストマクロを定義しなければ宣言が見えない)。
 * ===========================================================================
 */

/*
 * ★順序厳守★ 機能テストマクロは **どの #include より前**でなければ効かない。
 * getifaddrs / struct ifaddrs は POSIX ではなく BSD 由来の拡張なので
 * _DEFAULT_SOURCE (glibc 2.19+) が必要。古い glibc 用に _BSD_SOURCE も置く。
 * IFF_RUNNING (net/if.h) も同様に隠れる。
 */
#if !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif
#if !defined(_BSD_SOURCE)
#  define _BSD_SOURCE 1
#endif
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

/* --------------------------------------------------------------------------
 * ヘルパ
 * -------------------------------------------------------------------------- */
static void ip_to_str(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)(ip & 0xFFu));
}

/*
 * 「外に出るのに使えないアドレス」を弾く。
 * Windows 版 addr_is_usable() と同じ判定 + APIPA を別扱いにできるよう分離。
 */
static bool addr_is_apipa(uint32_t ip)
{
    return (ip & 0xFFFF0000u) == 0xA9FE0000u;   /* 169.254.0.0/16 */
}

static bool addr_is_usable(uint32_t ip, bool allow_apipa)
{
    if (ip == 0u || ip == 0xFFFFFFFFu)
        return false;
    if ((ip & 0xFF000000u) == 0x7F000000u)      /* 127.0.0.0/8  ループバック */
        return false;
    if ((ip & 0xF0000000u) == 0xE0000000u)      /* 224.0.0.0/4  マルチキャスト */
        return false;
    if (!allow_apipa && addr_is_apipa(ip))
        return false;
    return true;
}

/*
 * インタフェース名から「仮想 NIC / トンネル」を判定する。
 *
 * ★名前で判定するのは本質的に不完全である★
 * ユーザは ip link set name で好きな名前を付けられるので、
 * これは「よく使われる名前を拾う速い網」に過ぎない。
 * 本命は下の iff_is_real_nic() が見る IFF_POINTOPOINT で、
 * 名前判定はそれを補う二重の網として置いている。
 *
 * 一覧の根拠 (すべて実在する既定名):
 *   tun / tap      : OpenVPN, QEMU, WireGuard(旧設定), ip tuntap
 *   ppp            : pppd (PPPoE / 3G ドングル)
 *   docker / br-   : Docker のブリッジ (br-xxxxxxxx)
 *   veth           : コンテナのペアデバイス
 *   virbr / vnet   : libvirt
 *   wg             : WireGuard
 *   tailscale      : Tailscale (VIM1 実機で稼働中)
 *   zt             : ZeroTier
 *   gre / sit / ip6tnl / erspan : カーネルのトンネルドライバ
 *   dummy          : ip link add type dummy
 *   usb0 / rndis   : ★重要★ USB Gadget 側が作るネットワーク面。
 *                    我々自身の USB-C ケーブル。ここに bind したら
 *                    Windows 版と同じ自己参照ループになる。
 */
static bool name_is_virtual(const char *name)
{
    static const char *const prefixes[] = {
        "tun", "tap", "ppp", "docker", "veth", "br-", "virbr", "vnet",
        "wg", "tailscale", "zt", "gre", "sit", "ip6tnl", "erspan",
        "dummy", "usb", "rndis", "lo", NULL
    };
    int i;

    if (name == NULL)
        return true;

    for (i = 0; prefixes[i] != NULL; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], n) == 0)
            return true;
    }
    return false;
}

/*
 * ifa_flags から「稼働中の実 NIC か」を判定する。
 *
 *   IFF_UP        : 管理上有効 (ip link set up)
 *   IFF_RUNNING   : ★リンクが実際に上がっている★
 *                   ケーブルが抜けた eth0 は UP だが RUNNING は落ちる。
 *                   ここを見ないと「LAN ケーブルが抜けているのに
 *                   eth0 の IP に bind して全通信が失敗する」になる。
 *   IFF_LOOPBACK  : lo
 *   IFF_POINTOPOINT : ppp0 / tun0 / wg0 など。指示書の除外条件。
 */
static bool iff_is_real_nic(unsigned int flags)
{
    if ((flags & IFF_UP) == 0)
        return false;
    if ((flags & IFF_RUNNING) == 0)
        return false;
    if ((flags & IFF_LOOPBACK) != 0)
        return false;
    if ((flags & IFF_POINTOPOINT) != 0)
        return false;
    return true;
}

/* --------------------------------------------------------------------------
 * 段 1: カーネルに「グローバルへ出るならどの送信元か」を聞く
 * --------------------------------------------------------------------------
 * UDP ソケットを 8.8.8.8:53 へ connect して getsockname する。
 *
 * ★パケットは 1 バイトも出ない★
 *   SOCK_DGRAM の connect(2) は「既定の宛先を記録する」だけの操作で、
 *   その副作用としてカーネルが経路検索を行い送信元アドレスを確定する。
 *   実際の送信は sendto/send の時に起きる。よってこの関数は
 *   ネットワークが切れていても安全に呼べる (経路が無ければ
 *   connect が ENETUNREACH で失敗し、0 を返すだけ)。
 *
 * ★8.8.8.8 を使う意味★
 *   そこへ通信するのではなく「グローバル IPv4 へ出る経路」の代表として
 *   使うだけ。Windows 版が GetBestRoute(8.8.8.8) を使っているのと
 *   全く同じ発想で、両実装の挙動を揃える意味もある。
 */
static uint32_t probe_source_ip_via_connect(void)
{
    int                fd;
    struct sockaddr_in dst;
    struct sockaddr_in me;
    socklen_t          mlen = sizeof(me);
    uint32_t           ip;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        VM_LOGD("hostroute: socket() が失敗 (%s)", strerror(errno));
        return 0;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(53);
    dst.sin_addr.s_addr = htonl(0x08080808u);   /* 8.8.8.8 */

    if (connect(fd, (const struct sockaddr *)&dst, sizeof(dst)) != 0) {
        /*
         * ENETUNREACH = デフォルト経路が無い。
         * Wi-Fi 接続前に起動した場合などに起きる。異常ではない。
         */
        VM_LOGD("hostroute: connect(8.8.8.8) が失敗 (%s) "
                "-> 経路表からは決められない", strerror(errno));
        (void)close(fd);
        return 0;
    }

    memset(&me, 0, sizeof(me));
    if (getsockname(fd, (struct sockaddr *)&me, &mlen) != 0 ||
        me.sin_family != AF_INET) {
        VM_LOGD("hostroute: getsockname が失敗 (%s)", strerror(errno));
        (void)close(fd);
        return 0;
    }

    (void)close(fd);

    ip = ntohl(me.sin_addr.s_addr);
    {
        char a[16];
        ip_to_str(ip, a, sizeof(a));
        VM_LOGD("hostroute: カーネルの経路検索によるグローバル向け送信元 = %s",
                a);
    }
    return ip;
}

/* --------------------------------------------------------------------------
 * 段 2 / 段 3: getifaddrs でアドレスの持ち主を検証・探索する
 * --------------------------------------------------------------------------
 * want_ip != 0 : そのアドレスが実 NIC のものか検証する (段 2)
 * want_ip == 0 : 使える実 NIC のアドレスを 1 つ選ぶ    (段 3)
 *
 * Windows 版 scan_adapters() と同じ役割・同じ引数構成にしてある。
 */
static uint32_t scan_ifaddrs(uint32_t want_ip,
                             uint32_t exclude_net, uint32_t exclude_mask,
                             bool allow_apipa,
                             char *name_out, size_t name_size)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *ifa;
    uint32_t        found = 0;

    if (getifaddrs(&list) != 0) {
        VM_LOGW("hostroute: getifaddrs が失敗 (%s)", strerror(errno));
        return 0;
    }

    for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        const struct sockaddr_in *sin;
        uint32_t                  ip;

        /*
         * ★ifa_addr は NULL になりうる★ (manpage 明記)
         * アドレス未設定のインタフェースで NULL 参照して落ちるのは
         * getifaddrs の典型的な誤用。
         */
        if (ifa->ifa_addr == NULL)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;               /* AF_INET6 / AF_PACKET は対象外 */

        if (!iff_is_real_nic(ifa->ifa_flags))
            continue;

        if (name_is_virtual(ifa->ifa_name)) {
            VM_LOGT("hostroute: %s は仮想/トンネルと判断して除外",
                    ifa->ifa_name);
            continue;
        }

        sin = (const struct sockaddr_in *)(const void *)ifa->ifa_addr;
        ip  = ntohl(sin->sin_addr.s_addr);

        if (!addr_is_usable(ip, allow_apipa))
            continue;

        /* 仮想ネットワーク (既定 192.168.99.0/24) は我々自身の PPP 側 */
        if (exclude_mask != 0u &&
            (ip & exclude_mask) == (exclude_net & exclude_mask)) {
            VM_LOGT("hostroute: %s のアドレスは仮想ネットワーク内なので除外",
                    ifa->ifa_name);
            continue;
        }

        if (want_ip != 0u && ip != want_ip)
            continue;

        found = ip;
        if (name_out != NULL && name_size > 0)
            snprintf(name_out, name_size, "%s", ifa->ifa_name);
        break;
    }

    freeifaddrs(list);
    return found;
}

/* --------------------------------------------------------------------------
 * 本体
 * --------------------------------------------------------------------------
 * 戻り値: ホストオーダの IPv4。決められなければ 0
 *         (呼び出し側は outbound_addr を設定せず従来動作にする)
 */
uint32_t vm_hostroute_pick_outbound_ip(uint32_t exclude_net,
                                       uint32_t exclude_mask,
                                       char *name_out, size_t name_size)
{
    uint32_t probed;
    uint32_t picked;
    char     a[16];

    if (name_out != NULL && name_size > 0)
        name_out[0] = '\0';

    /* ---- 段 1: カーネルの経路検索に聞く ---- */
    probed = probe_source_ip_via_connect();

    /* ---- 段 2: その候補が実 NIC のものか検証する ---- */
    if (probed != 0u) {
        picked = scan_ifaddrs(probed, exclude_net, exclude_mask,
                              /* allow_apipa = */ false,
                              name_out, name_size);
        if (picked != 0u) {
            ip_to_str(picked, a, sizeof(a));
            VM_LOGI("hostroute: 外向きアドレスに %s (%s) を採用 "
                    "(カーネルの経路検索と実 NIC 検証の両方が一致)",
                    a, (name_out != NULL && name_out[0] != '\0')
                           ? name_out : "?");
            return picked;
        }

        ip_to_str(probed, a, sizeof(a));
        VM_LOGD("hostroute: 経路が指す %s は実 NIC のアドレスとして "
                "確認できなかった (VPN / トンネル / リンクダウンの可能性)",
                a);
    }

    /* ---- 段 3: 実 NIC を総当たりで 1 つ選ぶ ---- */
    picked = scan_ifaddrs(0, exclude_net, exclude_mask,
                          /* allow_apipa = */ false,
                          name_out, name_size);
    if (picked != 0u) {
        ip_to_str(picked, a, sizeof(a));
        VM_LOGI("hostroute: 外向きアドレスに %s (%s) を採用 "
                "(経路表からは決められなかったのでインタフェース列挙で選択)",
                a, (name_out != NULL && name_out[0] != '\0')
                       ? name_out : "?");
        return picked;
    }

    /*
     * ---- 段 3.5: APIPA を許した上で、段 1 の候補をもう一度検証する ----
     *
     * ★なぜ APIPA を条件付きで許すのか★
     * 169.254/16 は通常「DHCP に失敗して経路が無いアドレス」であり、
     * bind すると通信が全滅するので除外するのが正しい。
     * しかし本プロジェクトの開発サンドボックスの eth0 は
     *
     *     2: eth0  inet 169.254.0.21/30
     *     default via 169.254.0.22 dev eth0
     *
     * であり、**APIPA だがそこが唯一の外向き経路**である。
     * 段 1 でカーネル自身が「グローバルへ出る送信元はこれ」と答えた
     * アドレスなら、経路がある事はカーネルが保証している。
     * よってこの場合に限り採用する。
     *
     * 「指示書は APIPA を除外せよと書いている」に反しないのは、
     * 除外の目的が『経路の無いアドレスに bind して通信を壊さない事』で
     * あり、ここでは経路の存在が確認できているためである。
     */
    if (probed != 0u && addr_is_apipa(probed)) {
        picked = scan_ifaddrs(probed, exclude_net, exclude_mask,
                              /* allow_apipa = */ true,
                              name_out, name_size);
        if (picked != 0u) {
            ip_to_str(picked, a, sizeof(a));
            VM_LOGI("hostroute: 外向きアドレスに %s (%s) を採用 "
                    "(169.254/16 だがカーネルがグローバル向け経路として "
                    "選んだ実 NIC なので使用可能と判断)",
                    a, (name_out != NULL && name_out[0] != '\0')
                           ? name_out : "?");
            return picked;
        }
    }

    /*
     * ---- 段 4: 安全な失敗 ----
     * 0 を返すと呼び出し側 (vm_nat_slirp.c) は outbound_addr を設定せず、
     * libslirp は bind を省略してカーネルの経路表任せになる。
     * Linux ではそれで正常に通信できるので、これは「劣化」ではない。
     * Windows 版と違い警告のトーンを落としてあるのはそのためである。
     */
    VM_LOGI("hostroute: 外向きに使う実 NIC のアドレスを特定できなかった。"
            "outbound_addr を設定せず、カーネルの経路表に任せて続行する "
            "(Linux では通常これで問題ない)");
    return 0;
}
