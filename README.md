# barcode_msc / HID Logger

バーコードリーダー（HID キーボードデバイス）のスキャン結果を USB ストレージに自動記録する中継デバイスのファームウェアと、Web ブラウザから操作するための管理 Web アプリです。

The firmware to save the scanning results from USB-HID Barcode reader to USB mass storage device. And the Web app for managing.

## 概要 / Overview

```
[バーコードリーダー] ──USB──> [HID Logger デバイス] ──USB──> [PC]
                                   |
                               [USB ストレージ]
                               (log.txt に記録)
```

- バーコードリーダーを USB ホストとして受け付け、スキャン結果を時刻付きで USB ストレージの `log.txt` に書き込みます。
- PC からは CDC（USB シリアル）デバイスとして見え、Web アプリまたはシリアルターミナルからログの取得・設定変更が可能です。
- OLED ディスプレイに接続状態とスキャン内容をリアルタイム表示します。

## ハードウェア / Hardware

| 項目 | 内容 |
|------|------|
| MCU | CH32V203（TinyUSB ホスト対応） |
| 基板 | オリジナル設計基板 |
| ディスプレイ | SSD1306 OLED 128×32（I2C アドレス `0x3C`） |
| ボタン | 3 個（BTN_1: PB12、 BTN_2: PB13、 BTN_3: PB14） |
| LED | 4 個（PB15、PA8、PA9、PA10） |
| USB ホスト | HID（キーボード）・MSC（ストレージ）対応 |

### ピン配置 / Pinout

| ピン | 機能 |
|------|------|
| PB12 | BTN_1（長押しで時計設定） |
| PB13 | BTN_2（時計設定：次の桁へ） |
| PB14 | BTN_3（時計設定：決定 / 起動時長押しでブートローダー） |

## ファームウェア / Firmware

### 依存ライブラリ / Dependencies

コアは verylowfreq/arduino_core_ch32_sz を利用します。

Arduino IDE または PlatformIO で以下のライブラリをインストールしてください。

- `Adafruit GFX Library`
- `Adafruit SSD1306`

### 書き込みの補足 / Note for Flash

事前にCAT Bootloaderを書き込んでおくと、BTN_3（右ボタン）を押したまま電源を入れたさいにブートローダーが立ち上がります。

### キーボードレイアウト / Keyboard Layout

`barcode_msc.ino` の以下の行で切り替えられます。

```cpp
KeyboardHost.setLayout(KeyboardLayout::JP_JIS);  // 日本語 JIS
// KeyboardHost.setLayout(KeyboardLayout::US_ASCII); // 英語 US
```

### 動作 / Behavior

- スキャンされた文字列は改行（`\n`）で確定し、`HH:MM:SS\t<バーコード>\r\n` の形式で USB ストレージの `/log.txt` に追記されます。
- OLED の下段に直近のスキャン内容が 3 秒間表示されます。
- ソフトウェアクロックを内蔵していますが、電源を切ると時刻はリセットされます。

### 時計設定（本体操作）/ Clock Setting (on-device)

1. **BTN_1 を 2 秒間長押し**するとプログレスバーが伸びて時計設定モードに入ります。
2. `[UP]`（BTN_1）で現在の桁を増やし、`[NEXT]`（BTN_2）で次の桁へ移動します。
3. `[OK]`（BTN_3）で確定します。

## Web アプリ / Web App

`docs/index.html` を WebSerial/WebUSB対応ブラウザで開くと、Web Serial API 経由でデバイスを操作できます。

https://verylowfreq.github.io/barcode_msc

### 対応ブラウザ / Supported Browsers

Web Serial API に対応したブラウザが必要です。

- Google Chrome
- Microsoft Edge

Safari は非対応です。

### 使い方 / Usage

1. HID Logger デバイスを PC に USB 接続する。
2. Web アプリをブラウザで開く。
3. 「**デバイスを選択して接続**」ボタンを押してポートを選択する。
4. 接続後、以下のタブを操作できます。

| タブ | 機能 |
|------|------|
| デバイス情報 | HID・MSC の接続状態とデバイス時刻を表示 |
| ログ | USB ストレージのログを取得・CSV ダウンロード・消去 |
| リアルタイム | スキャン結果をリアルタイムで表示（パススルーモード） |
| 時計設定 | PC の現在時刻を取得してデバイス時刻を設定 |

### デバッグパネル / Debug Panel

画面右下の「**Debug**」ボタンを押すと、シリアル通信のローレベルログを確認できます。

## シリアルコマンド / Serial Commands

シリアルターミナル（115200 baud、改行 `\n`）から直接操作することも可能です。

| コマンド | 説明 |
|----------|------|
| `status` | HID・MSC の接続状態と時刻を返す |
| `log get size` | ログファイルのバイト数を返す |
| `log read <offset>,<length>` | ログの一部を読み出す |
| `log clear` | ログファイルを消去する |
| `clock set HH:MM:SS` | 時刻を設定する |
| `dump set on` | パススルーモード有効（スキャン結果をシリアルに出力） |
| `dump set off` | パススルーモード無効 |

レスポンスは成功時 `+OK`、エラー時 `-ERR: <理由>` で始まります。

## ログ形式 / Log Format

USB ストレージ上の `/log.txt` はタブ区切りのテキストファイルです。

```
09:15:32	4901234567890
09:15:45	4901234567891
```

## ライセンス

MIT License (c) Mitsumine Suzu (verylowfreq)
