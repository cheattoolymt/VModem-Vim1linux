#!/bin/bash
# ===========================================================================
#  setup-gadget-linux.sh -- VModem 用 USB CDC-ACM ガジェット設定 (Step 8)
#
#  対象: Khadas VIM1 (Amlogic S905X) / Armbian 25.11.1 / Linux 6.12 meson64
#  役割: configfs 経由で「USB シリアル (CDC-ACM) デバイス」を生成し、
#        Windows 側から COM ポートとして見えるようにする。
#        生成されると Linux 側には /dev/ttyGS0 が現れ、これを
#          sudo vmodem --port /dev/ttyGS0 --net slirp -v
#        に渡す。
#
#  設計方針: **べき等 (idempotent)**
#        指示書 Step 8 の要求どおり、既に同じ設定で動いている場合は
#        何も壊さずに終了する。UDC へ bind 済みのガジェットに
#        書き込むと EBUSY で失敗するため、これは要求である以上に
#        技術的な必然でもある。
#
#  使い方:
#        sudo bash scripts/setup-gadget-linux.sh          # 設定
#        sudo bash scripts/setup-gadget-linux.sh status   # 状態表示
#        sudo bash scripts/setup-gadget-linux.sh teardown # 取り外し
#        sudo bash scripts/setup-gadget-linux.sh --force  # 作り直し
#
#  環境変数で上書き可能:
#        VM_UDC / VM_VID / VM_PID / VM_GADGET / VM_SERIAL /
#        VM_MANUFACTURER / VM_PRODUCT
# ===========================================================================

set -u

# ---------------------------------------------------------------------------
#  疑似ルート (テスト用)
#
#  VM_TEST_ROOT を指定すると、configfs / /sys/class/udc / /dev/ttyGS0 を
#  すべてその下の普通のディレクトリに読み替えて動く。
#  configfs も UDC も無い開発機 (CI, コンテナ, x86 の PC) で
#  「べき等になっているか」「teardown が正しい順序か」を
#  実際に走らせて確かめるために用意した。
#
#  本物の configfs は mkdir した瞬間にカーネルが属性ファイルを生やすが、
#  普通のディレクトリではそれが起きない。そこで疑似ルート時は
#  vm_mkdir() が同じ属性ファイルを自分で作って挙動を模倣する。
#
#  ★ 疑似ルートは「スクリプトのロジック」の検証用であって、
#    USB が本当に列挙されるかの検証にはならない ★
#    実機での確認は必ず別途行う事。
# ---------------------------------------------------------------------------
VM_TEST_ROOT="${VM_TEST_ROOT:-}"

# --- 既定値 ----------------------------------------------------------------
GADGET_NAME="${VM_GADGET:-vmodem}"

if [ -n "$VM_TEST_ROOT" ]; then
    CONFIGFS="$VM_TEST_ROOT/sys/kernel/config"
    UDC_CLASS_DIR="$VM_TEST_ROOT/sys/class/udc"
    TTY_DEV="$VM_TEST_ROOT/dev/ttyGS0"
else
    CONFIGFS="/sys/kernel/config"
    UDC_CLASS_DIR="/sys/class/udc"
    TTY_DEV="/dev/ttyGS0"
fi
GADGET_DIR="$CONFIGFS/usb_gadget/$GADGET_NAME"

# UDC (USB Device Controller)
#   VIM1 は dwc2 が c9100000.usb として現れる (指示書指定)。
#   ただし DT/カーネルによって名前が変わり得るので、
#   指定が見つからなければ実在する UDC を自動採用する。
UDC_WANT="${VM_UDC:-c9100000.usb}"

# ★ VID/PID について必ず読むこと ★
#   1209:0001 は pid.codes の「Test PID」。
#   pid.codes の方針では *社内・手元での試験専用* であり、
#   配布・製品化する物に載せてはいけない。
#   趣味の実機検証には使えるが、他人に配る場合は
#   pid.codes で自分用の PID を取得して VM_PID で上書きすること。
VID="${VM_VID:-0x1209}"
PID="${VM_PID:-0x0001}"

MANUFACTURER="${VM_MANUFACTURER:-VModem Project}"
PRODUCT="${VM_PRODUCT:-VIM1 Dialup Modem}"
SERIAL="${VM_SERIAL:-VMODEM0001}"

FUNC="acm.usb0"          # f_acm (CDC-ACM) のインスタンス名
CONF="c.1"               # コンフィグレーション 1
CONF_LABEL="CDC ACM"

# --- 表示ヘルパ ------------------------------------------------------------
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_WARN=$'\033[33m'
    C_INFO=$'\033[36m'; C_OFF=$'\033[0m'
else
    C_OK=''; C_NG=''; C_WARN=''; C_INFO=''; C_OFF=''
fi
info()  { printf '%s[*]%s %s\n' "$C_INFO" "$C_OFF" "$*"; }
ok()    { printf '%s[OK]%s %s\n' "$C_OK"   "$C_OFF" "$*"; }
warn()  { printf '%s[!]%s %s\n' "$C_WARN"  "$C_OFF" "$*" >&2; }
die()   { printf '%s[NG]%s %s\n' "$C_NG"   "$C_OFF" "$*" >&2; exit 1; }

# mkdir。疑似ルート時は、本物の configfs がカーネル側で自動生成する
# 属性ファイル群を自前で作って挙動を模倣する。
vm_mkdir() {
    local d="$1"
    mkdir -p "$d" || return 1
    [ -n "$VM_TEST_ROOT" ] || return 0

    local f
    case "$d" in
        */usb_gadget/*/functions/*|*/usb_gadget/*/configs/*/strings/*)
            : ;;  # 属性は個別に扱う
    esac
    if [ "$d" = "$GADGET_DIR" ]; then
        for f in idVendor idProduct bcdDevice bcdUSB \
                 bDeviceClass bDeviceSubClass bDeviceProtocol UDC; do
            [ -e "$d/$f" ] || printf '' > "$d/$f"
        done
        mkdir -p "$d/functions" "$d/configs" "$d/strings"
    elif [ "$d" = "$GADGET_DIR/strings/0x409" ]; then
        for f in manufacturer product serialnumber; do
            [ -e "$d/$f" ] || printf '' > "$d/$f"
        done
    elif [ "$d" = "$GADGET_DIR/configs/$CONF" ]; then
        [ -e "$d/MaxPower" ] || printf '' > "$d/MaxPower"
        mkdir -p "$d/strings"
    elif [ "$d" = "$GADGET_DIR/configs/$CONF/strings/0x409" ]; then
        [ -e "$d/configuration" ] || printf '' > "$d/configuration"
    fi
    return 0
}

# rmdir。configfs のディレクトリは中に属性ファイルが residing していても
# rmdir できる (属性はカーネルが生やした物で、実体のあるエントリではない)。
# 疑似ルートは普通のディレクトリなのでそれが成り立たず、rmdir が
# ENOTEMPTY で落ちる。疑似ルート時のみ属性ファイルを先に消して
# 本物の configfs と同じ結果になるようにする。
#   ★ 消すのは通常ファイルだけ ★
#     サブディレクトリや symlink を消してしまうと
#     「削除順序が正しいか」の検証にならないので触らない。
vm_rmdir() {
    local d="$1" f
    if [ -n "$VM_TEST_ROOT" ]; then
        for f in "$d"/*; do
            [ -f "$f" ] && [ ! -L "$f" ] && rm -f "$f"
        done
    fi
    rmdir "$d" 2>/dev/null
}

# 単一の属性へ書く。既に同じ値なら書かない (べき等 + bind 済み EBUSY 回避)。
write_attr() {
    local path="$1" val="$2" cur=""
    [ -e "$path" ] || die "属性が存在しません: $path"
    cur="$(cat "$path" 2>/dev/null || true)"
    if [ "$cur" = "$val" ]; then
        return 0
    fi
    if ! printf '%s' "$val" > "$path" 2>/dev/null; then
        die "書き込み失敗: $path <- '$val'
     (ガジェットが UDC に bind 済みだと EBUSY になります。
      '$0 teardown' で外してからやり直してください)"
    fi
}

# --- 事前確認 --------------------------------------------------------------
require_root() {
    # 疑似ルートは普通のディレクトリなので root は要らない
    [ -n "$VM_TEST_ROOT" ] && return 0
    [ "$(id -u)" -eq 0 ] || die "root 権限が必要です。sudo を付けて実行してください。"
}

# configfs をマウント (未マウントなら)。
#   Armbian では通常 systemd が /sys/kernel/config を自動マウントするが、
#   最小構成やコンテナでは無いことがある。
mount_configfs() {
    if [ -n "$VM_TEST_ROOT" ]; then
        mkdir -p "$CONFIGFS/usb_gadget"
        return 0
    fi
    if [ -d "$CONFIGFS/usb_gadget" ]; then
        return 0
    fi
    if ! grep -q ' configfs$' /proc/filesystems 2>/dev/null; then
        die "カーネルが configfs をサポートしていません (CONFIG_CONFIGFS_FS)。"
    fi
    info "configfs をマウントします: $CONFIGFS"
    mkdir -p "$CONFIGFS"
    mount -t configfs none "$CONFIGFS" 2>/dev/null || true
    [ -d "$CONFIGFS/usb_gadget" ] || \
        die "configfs をマウントしても usb_gadget が現れません。
     libcomposite が無い可能性があります: sudo modprobe libcomposite"
}

# libcomposite (USB ガジェットフレームワーク) を読み込む。
#   usb_gadget ディレクトリはこのモジュールが作る。
load_libcomposite() {
    if [ -n "$VM_TEST_ROOT" ]; then
        mkdir -p "$CONFIGFS/usb_gadget"
        return 0
    fi
    if [ -d "$CONFIGFS/usb_gadget" ]; then
        return 0
    fi
    info "libcomposite を読み込みます"
    modprobe libcomposite 2>/dev/null || \
        die "modprobe libcomposite に失敗しました。
     カーネルに CONFIG_USB_LIBCOMPOSITE / CONFIG_USB_CONFIGFS が必要です。"
    # sysfs に現れるまで少し待つ
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -d "$CONFIGFS/usb_gadget" ] && return 0
        sleep 0.1
    done
    die "libcomposite 読み込み後も $CONFIGFS/usb_gadget が現れません。"
}

# 使える UDC を決める。
pick_udc() {
    local avail
    if [ ! -d "$UDC_CLASS_DIR" ]; then
        die "$UDC_CLASS_DIR がありません。USB がデバイス(ペリフェラル)モードで
     動く設定になっていない可能性があります。
     VIM1 では USB-C 側が peripheral/otg である必要があります。"
    fi
    avail="$(ls -1 "$UDC_CLASS_DIR" 2>/dev/null)"
    if [ -z "$avail" ]; then
        die "利用可能な UDC がありません ($UDC_CLASS_DIR が空)。
     dwc2 がロードされているか、Device Tree で dr_mode = \"peripheral\"
     または \"otg\" になっているか確認してください。
       ls $UDC_CLASS_DIR
       dmesg | grep -i dwc2"
    fi
    if [ -e "$UDC_CLASS_DIR/$UDC_WANT" ]; then
        UDC="$UDC_WANT"
        return 0
    fi
    UDC="$(printf '%s\n' "$avail" | head -n1)"
    warn "指定の UDC '$UDC_WANT' が見つかりません。'$UDC' を使います。"
    warn "  (実在する UDC: $(printf '%s' "$avail" | tr '\n' ' '))"
}

# --- 状態取得 --------------------------------------------------------------
# 0: 望む構成で bind 済み / 1: 存在するが未 bind or 別構成 / 2: 未作成
gadget_state() {
    [ -d "$GADGET_DIR" ] || return 2
    local bound=""
    [ -f "$GADGET_DIR/UDC" ] && bound="$(cat "$GADGET_DIR/UDC" 2>/dev/null || true)"
    if [ -n "$bound" ]; then
        return 0
    fi
    return 1
}

cmd_status() {
    printf '\n=== VModem USB ガジェット状態 ===\n'
    printf '  ガジェット名  : %s\n' "$GADGET_NAME"
    printf '  configfs パス : %s\n' "$GADGET_DIR"
    if [ ! -d "$GADGET_DIR" ]; then
        printf '  状態          : %s未作成%s\n\n' "$C_WARN" "$C_OFF"
        printf '  作成するには: sudo bash %s\n\n' "$0"
        return 0
    fi
    printf '  VID:PID       : %s:%s\n' \
        "$(cat "$GADGET_DIR/idVendor" 2>/dev/null)" \
        "$(cat "$GADGET_DIR/idProduct" 2>/dev/null)"
    printf '  bDeviceClass  : %s (IAD 用に 0xef が期待値)\n' \
        "$(cat "$GADGET_DIR/bDeviceClass" 2>/dev/null)"
    local bound
    bound="$(cat "$GADGET_DIR/UDC" 2>/dev/null || true)"
    if [ -n "$bound" ]; then
        printf '  bind 先 UDC   : %s%s%s\n' "$C_OK" "$bound" "$C_OFF"
    else
        printf '  bind 先 UDC   : %s(未 bind)%s\n' "$C_WARN" "$C_OFF"
    fi
    printf '  機能          : %s\n' \
        "$(ls -1 "$GADGET_DIR/functions" 2>/dev/null | tr '\n' ' ')"
    if [ -e "$TTY_DEV" ]; then
        printf '  %-13s : %s存在%s\n' "$TTY_DEV" "$C_OK" "$C_OFF"
    else
        printf '  %-13s : %s無し%s\n' "$TTY_DEV" "$C_WARN" "$C_OFF"
    fi
    printf '\n'
}

# --- 取り外し --------------------------------------------------------------
#   configfs のガジェットは「作った順の逆」で消さないと EBUSY になる。
#     UDC を空にする -> config から function の symlink を消す ->
#     config を消す -> function を消す -> strings を消す -> gadget を消す
#   ※ ディレクトリの削除は rmdir のみ (rm -rf は効かない)
cmd_teardown() {
    require_root
    if [ ! -d "$GADGET_DIR" ]; then
        ok "ガジェットは存在しません (何もしません)"
        return 0
    fi
    info "ガジェット '$GADGET_NAME' を取り外します"

    # 1) unbind
    if [ -f "$GADGET_DIR/UDC" ] && [ -n "$(cat "$GADGET_DIR/UDC" 2>/dev/null)" ]; then
        printf '' > "$GADGET_DIR/UDC" 2>/dev/null || \
            warn "UDC の unbind に失敗しました (無視して続行)"
        sleep 0.2
    fi

    # 2) config 内の function symlink
    local cfg fn s
    for cfg in "$GADGET_DIR"/configs/*; do
        [ -d "$cfg" ] || continue
        for fn in "$cfg"/*; do
            [ -L "$fn" ] && rm -f "$fn"
        done
        # 3) config の strings と config 自身
        for s in "$cfg"/strings/*; do
            [ -d "$s" ] && vm_rmdir "$s"
        done
        [ -d "$cfg/strings" ] && vm_rmdir "$cfg/strings"
        vm_rmdir "$cfg" || warn "config の削除に失敗: $cfg"
    done

    # 4) function
    for fn in "$GADGET_DIR"/functions/*; do
        [ -d "$fn" ] && { vm_rmdir "$fn" || warn "function の削除に失敗: $fn"; }
    done

    # 5) gadget の strings
    for s in "$GADGET_DIR"/strings/*; do
        [ -d "$s" ] && vm_rmdir "$s"
    done
    # 疑似ルートでは configs/functions/strings の親も実体ディレクトリ
    if [ -n "$VM_TEST_ROOT" ]; then
        for s in configs functions strings; do
            [ -d "$GADGET_DIR/$s" ] && vm_rmdir "$GADGET_DIR/$s"
        done
    fi

    # 6) gadget 本体
    if [ -n "$VM_TEST_ROOT" ]; then
        rm -f "$TTY_DEV"
    fi

    if vm_rmdir "$GADGET_DIR"; then
        ok "取り外し完了"
    else
        die "ガジェットの削除に失敗しました: $GADGET_DIR
     (どこかが使用中です。lsof / dmesg を確認してください)"
    fi
}

# --- 本体 ------------------------------------------------------------------
cmd_setup() {
    require_root
    load_libcomposite
    mount_configfs

    # --- べき等の判定 ---
    if gadget_state; then
        local bound
        bound="$(cat "$GADGET_DIR/UDC" 2>/dev/null)"
        ok "ガジェット '$GADGET_NAME' は既に UDC '$bound' に bind 済みです。"
        info "設定を変更しません (べき等)。作り直すなら --force を付けてください。"
        if [ -e "$TTY_DEV" ]; then
            ok "$TTY_DEV も存在します。そのまま vmodem を起動できます。"
        else
            warn "$TTY_DEV が見当たりません。dmesg を確認してください。"
        fi
        show_next_steps
        return 0
    fi

    pick_udc

    if [ -d "$GADGET_DIR" ]; then
        info "既存の未 bind ガジェットを再利用します: $GADGET_DIR"
    else
        info "ガジェットを作成します: $GADGET_DIR"
        vm_mkdir "$GADGET_DIR" || die "mkdir に失敗: $GADGET_DIR"
    fi

    # --- デバイスディスクリプタ ---
    write_attr "$GADGET_DIR/idVendor"  "$VID"
    write_attr "$GADGET_DIR/idProduct" "$PID"
    [ -e "$GADGET_DIR/bcdDevice" ] && write_attr "$GADGET_DIR/bcdDevice" "0x0100"
    [ -e "$GADGET_DIR/bcdUSB" ]    && write_attr "$GADGET_DIR/bcdUSB"    "0x0200"

    # ★ ここが Windows で COM ポートとして見えるかの分かれ目 ★
    #   CDC-ACM は「通信インタフェース + データインタフェース」の
    #   2 本組で 1 つの機能を成す複合デバイス。どの 2 本が組なのかは
    #   IAD (Interface Association Descriptor) で示す。
    #   ホストに「IAD を読め」と伝えるには、デバイスディスクリプタを
    #     bDeviceClass    = 0xEF (Miscellaneous)
    #     bDeviceSubClass = 0x02 (Common Class)
    #     bDeviceProtocol = 0x01 (Interface Association Descriptor)
    #   にする必要がある。ここを 0 のままにすると Windows は
    #   インタフェース 0 だけを見て「不明なデバイス」にしがち。
    #   (Linux 側は IAD が無くても CDC を解釈できるので気づきにくい)
    #   XP は SP2 以降で IAD を解釈できる。SP1 以前は非対応。
    write_attr "$GADGET_DIR/bDeviceClass"    "0xef"
    write_attr "$GADGET_DIR/bDeviceSubClass" "0x02"
    write_attr "$GADGET_DIR/bDeviceProtocol" "0x01"

    # --- 文字列ディスクリプタ (0x409 = en-US) ---
    vm_mkdir "$GADGET_DIR/strings/0x409"
    write_attr "$GADGET_DIR/strings/0x409/manufacturer" "$MANUFACTURER"
    write_attr "$GADGET_DIR/strings/0x409/product"      "$PRODUCT"
    write_attr "$GADGET_DIR/strings/0x409/serialnumber" "$SERIAL"

    # --- 機能: CDC-ACM ---
    #   これが /dev/ttyGS0 を生む。f_acm は u_serial 経由で TTY を出す。
    vm_mkdir "$GADGET_DIR/functions/$FUNC" || \
        die "acm 機能の作成に失敗しました。
     CONFIG_USB_CONFIGFS_ACM が有効か確認してください。"

    # --- コンフィグレーション ---
    vm_mkdir "$GADGET_DIR/configs/$CONF"
    vm_mkdir "$GADGET_DIR/configs/$CONF/strings/0x409"
    write_attr "$GADGET_DIR/configs/$CONF/strings/0x409/configuration" "$CONF_LABEL"
    # バスパワー 250mA (=500mA/2 単位ではなく mA 表記), self-powered ではない
    [ -e "$GADGET_DIR/configs/$CONF/MaxPower" ] && \
        write_attr "$GADGET_DIR/configs/$CONF/MaxPower" "250"

    # --- 機能をコンフィグへ ---
    if [ ! -e "$GADGET_DIR/configs/$CONF/$FUNC" ]; then
        ln -s "$GADGET_DIR/functions/$FUNC" "$GADGET_DIR/configs/$CONF/$FUNC" || \
            die "機能のリンクに失敗しました"
    fi

    # --- UDC へ bind ---
    info "UDC '$UDC' へ bind します"
    if ! printf '%s' "$UDC" > "$GADGET_DIR/UDC" 2>/dev/null; then
        die "UDC への bind に失敗しました。
     ・他のガジェット (例: g_ether, dwc2 の既定 gadget) が
       同じ UDC を掴んでいる可能性があります:
         cat /sys/kernel/config/usb_gadget/*/UDC
     ・Armbian の usb-gadget サービスが動いている場合は止めてください。"
    fi

    ok "bind 完了 (UDC=$UDC, VID:PID=$VID:$PID)"

    # 疑似ルートでは f_acm が居ないので TTY を自分で置く
    if [ -n "$VM_TEST_ROOT" ]; then
        mkdir -p "$(dirname "$TTY_DEV")"
        [ -e "$TTY_DEV" ] || printf '' > "$TTY_DEV"
    fi

    # /dev/ttyGS0 が現れるのを待つ
    local i=0
    while [ "$i" -lt 20 ]; do
        [ -e "$TTY_DEV" ] && break
        sleep 0.1
        i=$((i + 1))
    done
    if [ -e "$TTY_DEV" ]; then
        ok "$TTY_DEV が使えます"
    else
        warn "$TTY_DEV が現れませんでした。dmesg | tail を確認してください。"
    fi

    # PID が Test PID のままなら警告する
    if [ "$PID" = "0x0001" ] && [ "$VID" = "0x1209" ]; then
        printf '\n'
        warn "VID:PID = 1209:0001 は pid.codes の *Test PID* です。"
        warn "手元での試験には使えますが、配布物に載せてはいけません。"
        warn "配布する場合は pid.codes で PID を取得し VM_PID=... で上書きを。"
    fi

    show_next_steps
}

show_next_steps() {
    cat <<'EOS'

--- 次の手順 ---------------------------------------------------------------
  1) VModem を起動 (ホストに繋ぐ前でも可)
       sudo vmodem --config /etc/vmodem/config.ini \
                   --port /dev/ttyGS0 --net slirp -v
     ソースツリーから直接動かす場合:
       sudo ./vmodem --port /dev/ttyGS0 --net slirp -v

  2) VIM1 の USB-C を Windows 機に接続

  3) Windows 側
       デバイスマネージャに "VIM1 Dialup Modem" が出る。
       ドライバを聞かれたら windows/vim1modem.inf を指定
       (XP では署名が無いので「続行」を選ぶ)。
       COM ポート番号を確認し、ダイヤルアップ接続のモデムに指定する。

  --- 補足: DTR/DCD について ------------------------------------------------
  Linux 6.12 の CDC-ACM ガジェット (drivers/usb/gadget/function/u_serial.c)
  の gs_tty_ops には .tiocmget / .tiocmset が無い。つまり
  /dev/ttyGS0 に対する TIOCMGET は ENOTTY で失敗し、
  ホストが送った DTR の状態をユーザ空間から読む事はできない。
  VModem 側はこれを前提に「ポートを開いている間は接続中」として扱う。
---------------------------------------------------------------------------
EOS
}

# --- 引数処理 --------------------------------------------------------------
case "${1:-setup}" in
    setup|"")            cmd_setup ;;
    -f|--force|force)    cmd_teardown; cmd_setup ;;
    status|-s|--status)  cmd_status ;;
    teardown|down|stop)  cmd_teardown ;;
    -h|--help|help)
        sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        die "不明な引数: $1  ('$0 --help' を参照)"
        ;;
esac
