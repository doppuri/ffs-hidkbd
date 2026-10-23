# Android USB HID keyboard (FunctionFS)

root化したAndroid端末を **USB HIDキーボード** にして、PCにあるテキストファイルを、USBでつないだ別の機器（PC・ゲーム機など）にキー入力として流し込むツールです。

- カーネルの `CONFIG_USB_CONFIGFS_F_HID`（`/dev/hidg0`）は**不要**です。多くのAndroidカーネルで有効になっている **FunctionFS**（adbと同じ仕組み）を使い、HIDキーボードをユーザー空間で実装しています
- 端末側のプログラムはlibcを使わない静的バイナリです。**Android NDKは不要**で、clangとlldがあればビルドできます
- 長いファイル向けの機能: 進捗表示、途中終了と再開、自動インデントするエディタへの対応、大文字小文字の反転、入力補完のあるエディタへの対応

```
 [操作用PC] --(adb over Wi-Fi)--> [root化Android] --(USBケーブル: HIDキーボード)--> [入力先の機器]
  hidtype.py                       hidkbd
```

## 必要な機材
| 機材 | 内容 |
|---|---|
| root化したAndroid端末 | arm64 (aarch64)。USBデバイスとして動作できること（ふつうのスマートフォンなら可） |
| USBケーブル | 端末と入力先の機器をつなぐ。入力先側の端子に合わせる（Type-C / micro-B ⇔ Type-A など） |
| 入力先の機器 | USBキーボードを受け付ける機器（Windows / macOS / Linux PC、USBキーボード対応のゲーム機など） |
| 操作用PC | ビルドとテキスト送信に使う。端末と**同じネットワーク**にあること（入力先と同じPCでもよい） |

端末のUSB端子は入力先とつなぐのに使うので、操作用PCとの接続には **adb over Wi-Fi** を使います。

## 前提条件

### Android端末
- **root化済み**で、adbから `su` を使えること（`adb shell su -c id` で `uid=0(root)` と表示されればよい）
- カーネルで次の2つが有効なこと
  - `CONFIG_USB_CONFIGFS=y`
  - `CONFIG_USB_CONFIGFS_F_FS=y`（FunctionFS）
- USBガジェットがconfigfsで構成されていること（`/config/usb_gadget/g1`、設定名 `b.1`、adbの機能名 `ffs.adb`）。Android 8以降のconfigfs方式の端末では一般的な構成です
- adb over Wi-Fiで接続できること

確認するコマンドです（`/proc/config.gz` がない端末もあります）。
```sh
adb shell su -c "zcat /proc/config.gz | grep -E 'USB_CONFIGFS=|USB_CONFIGFS_F_FS='"
adb shell su -c "ls /config/usb_gadget/g1/functions /sys/class/udc"
```

### ビルド・操作用PC
| ソフトウェア | 用途 | 備考 |
|---|---|---|
| clang | 端末側プログラムのコンパイル | `--target=aarch64-linux-gnu` に対応したもの（通常のLLVM clangなら対応） |
| lld（`ld.lld`）または `rust-lld` | リンク | Rustがrustupで入っていれば、付属の `rust-lld` を自動で使う |
| POSIX sh | `build.sh` の実行 | WindowsではGit BashやMSYS2 |
| adb | 端末への転送・操作 | Android SDK Platform-Tools |
| Python 3.7以上 | `pc/hidtype.py` | 標準ライブラリだけで動く。Escキーでの中断はWindowsのみ |

clangやlldがPATHにない場合は、環境変数か `build.local.sh`（git管理外）で場所を指定します。
```sh
# build.local.sh の例
CLANG=/path/to/clang
LLD=/path/to/ld.lld
```

## セットアップ

### 1. adbをWi-Fi経由にする
端末を操作用PCにUSBでつないだ状態で、次を実行します。
```sh
adb tcpip 5555
adb connect <端末のIPアドレス>:5555
```
接続できたら、USBケーブルを入力先の機器につなぎ替えます（入力先が操作用PCと同じなら、そのままでかまいません）。以降の例では、`-s <端末のIPアドレス>:5555` で端末を指定します。環境変数 `ANDROID_SERIAL` を設定しておけば `-s` は省略できます。

### 2. ビルドと転送
```sh
sh build.sh                                   # out/hidkbd ができる
python pc/hidtype.py -s <IP>:5555 --install   # 端末の /data/local/tmp/hidkbd/ へ転送（実行権限も付ける）
```

### 3. HIDモードの開始と終了
```sh
python pc/hidtype.py -s <IP>:5555 --start     # USBを「HIDキーボード + adb」に切り替える
python pc/hidtype.py -s <IP>:5555 --stop      # 元のUSB構成に戻す
```
`--start` はUSB構成を書き換え、元の構成を端末に保存します。`--stop` でその構成に戻します。端末を再起動しても元に戻ります。

## 使い方
```sh
python pc/hidtype.py -s <IP>:5555 -w 5 input.txt              # 5秒後に input.txt を入力
python pc/hidtype.py -s <IP>:5555 -t "hello"                  # 文字列を入力
python pc/hidtype.py -s <IP>:5555 input.txt --autoindent --hold 20
```

| オプション | 内容 |
|---|---|
| `-w SEC` | 入力開始まで待つ（その間に入力先をクリックする。Escでキャンセル） |
| `--layout jp\|us` | 入力先のキーボード配列の設定（既定は `jp`） |
| `--autoindent` | 入力先が改行時に前の行のインデントを引き継ぐ場合に使う。インデントの差分だけをBackspaceと空白で調整する |
| `--case normal\|invert\|auto` | 英字のShiftを反転しない／常に反転する／入力先のCapsLockのLEDが点いている間だけ反転する |
| `--hold MS` | キーを押した状態と離した状態を、それぞれ最低MSミリ秒保つ。フレーム単位で入力を読む相手では20以上を推奨 |
| `--delay MS` | 1キーごとの追加の待ち時間 |
| `--from-line N` | N行目から再開する |
| `--show` | 送るキー列を表示するだけで、入力はしない（Backspaceは `<BS>` と表示） |

入力速度の目安は次のとおりです。
- `--hold 0`: 約500文字/秒
- `--hold 20`: 約25文字/秒

## 中断と再開
入力中は、次のどれかで中断できます。どれも入力先のフォーカスを動かさずに使えます。
- 操作用PCの **Escキー**（どのウィンドウにフォーカスがあっても検出する。Windowsのみ）
- 端末の **音量ダウンボタン**
- `hidtype.py` を実行しているターミナルでCtrl+C

中断すると `aborted ... at line N` と表示されます。再開の手順は次のとおりです。
- `--autoindent` を付けている場合: N行目の途中まで入った文字を、N行目を始めた改行も含めて消し、カーソルを **N-1行目の末尾** に置いてから `--from-line N` で実行します（最初に改行を送ります）
- `--autoindent` を付けていない場合: N行目を空にしてカーソルを行頭に置き、`--from-line N` で実行します

## トラブルシューティング

### 文字がときどき抜ける（特に入力補完のあるエディタ）
入力補完のポップアップを出すエディタでは、ポップアップが開く瞬間に届いたキーが捨てられることがあります。`--delay` を大きくしても直らず、かえって悪化します（入力の合間にポップアップが開きやすくなるため）。

`--hold 20` を使ってください。それでも抜ける場合は `--hold 30` 以上にします。

### 記号が違う文字になる
入力先のキーボード配列の設定に合わせて `--layout` を指定してください。日本語配列では、`\` `_` `|` を日本語キーボード固有のキー（「ろ」「￥」）で送ります。

### 大文字と小文字が逆になる
入力先でCapsLockがONになっています。`--case auto` を使うか、入力先のCapsLockをOFFにしてください。

### `already running` と表示される
HIDモードはすでに有効です。そのまま入力を始められます。やり直したいときは、`--stop` のあとに `--start` を実行します。

## 仕組み
- `--start` でUSBガジェットの構成を、FunctionFSのHID機能（`ffs.hid`）とadb（`ffs.adb`）の複合デバイスに切り替えます
- `hidkbd` がFunctionFSにHIDのディスクリプタを書き込み、ホストからの制御要求（レポートディスクリプタの要求、LED状態の通知など）に応えます。入力内容は8バイトのキーボードレポートとして送ります
- `hidtype.py` はテキストを前処理（改行の統一、自動インデントの補正など）してから端末に転送し、制御用のFIFOで入力ジョブを開始・中断します

## ファイル構成
| ファイル | 内容 |
|---|---|
| `device/hidkbd.c` | 端末側のHIDキーボード（FunctionFS、libc不使用、aarch64） |
| `device/hid_start.sh` | USB構成をHIDモードに切り替え、`hidkbd` を起動する |
| `device/hid_stop.sh` | `hidkbd` を止め、元のUSB構成に戻す |
| `pc/hidtype.py` | 操作用PCのツール（転送・起動停止・入力・進捗表示・中断） |
| `build.sh` | ビルドスクリプト |

## 制限と注意
- 入力できるのはASCII（英数字・記号・改行・タブ）だけです。それ以外の文字は警告を出してスキップします
- 入力先で日本語IMEがONになっていると、英字がローマ字入力として変換されてしまいます。IMEはOFFにしてください
- USBのVID:PIDには、Linux Foundationのガジェット用ID `1d6b:0104` を使っています。個人での利用を想定しています
- ガジェット構成の名前（`g1` / `b.1` / `ffs.adb`）が違う端末では、`device/hid_start.sh` と `hid_stop.sh` を修正する必要があります
- 音量ダウンボタンによる中断は、`gpio-keys` 系の入力デバイスを自動で探して使います。見つからない端末では使えません（ほかの中断方法は使えます）
- USB構成をroot権限で書き換えるので、自己責任で使ってください。うまく戻らなくなったときは、端末を再起動すれば元に戻ります

## 動作確認環境
- 端末: Android 9、Linux kernel 4.9（arm64、`CONFIG_USB_CONFIGFS_F_HID` なし）
- 入力先: Windows 11、USBキーボード対応のゲーム機上のエディタ
- 操作用PC: Windows 11（Git Bash、clang + rust-lld、Python 3.13）

## ライセンス
[WTFPL](http://www.wtfpl.net/)（Do What The Fuck You Want To Public License, Version 2）です。詳しくは [LICENSE](LICENSE) を見てください。

このソフトウェアは無保証です。
