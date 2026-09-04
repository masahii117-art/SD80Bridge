[README_SD80_Bridge_Ver1_final.md](https://github.com/user-attachments/files/31670471/README_SD80_Bridge_Ver1_final.md)
# SD80 Bridge Ver1

EDIROL SD-80 USB / Virtual MIDI Bridge for Windows

------------------------------------------------------------------------

## 概要

古いMIDI音源のUSBドライバは、最新OSではメーカーによる
サポートが終了している場合があります。

Roland公式のWindows 11対応状況については、以下を参照してください。

https://www.roland.com/jp/support/support_news/25090811win1/

旧バージョンのUSBドライバを強制的に使用する方法もありますが、
最新のWindows環境ではセキュリティによってドライバが使用できない
場合があり、セキュリティ設定の変更が必要となることがあります。

https://note.com/lego_hasiri/n/n5ca83c2b4d83

もちろん、PCに接続したMIDI-IFを介してMIDIケーブルで
SD-80と接続することは可能です。

しかし、SD-80のように32パートを使用する音源では、
MIDIケーブルによる接続は煩わしいものになります。

**SD80 Bridge** は、EDIROL SD-80をWindows標準MIDIドライバ環境から
制御するためのBRIDGEアプリケーションです。

SD-80専用USBドライバを準備することなく、**DAWを起動する前に
SD80 Bridgeを起動することで、DAWからSD-80のMIDIポートを
利用できるようにします。**

------------------------------------------------------------------------

## 注意事項

本ソフトウェアは個人で開発・検証しているものです。

Windows MIDI Services / MIDISRVを含むWindows側の仕様変更や
不具合、またはSD80 Bridge自体の不具合によって正常に動作しない
場合があります。

**本ソフトウェアは動作を保証するものではなく、
WindowsやDAW、SD-80を含む環境についてサポートを保証するものでもありません。**

特にWindows MIDI Services / MIDISRV側の問題については、
Microsoftによる修正状況やWindowsの更新によって動作が変わる
可能性があります。

SD80 Bridgeには、Windows MIDI Services / MIDISRVの状態に応じて
MIDI接続を再初期化するためのリセット処理を実装しています。

このリセット処理は、MIDI接続が正常に確立できない場合などに
再初期化を行うためのものですが、すべての環境・症状について
改善を保証するものではありません。

本ソフトウェアを使用する場合は、使用者自身の判断と責任において
ご利用ください。

------------------------------------------------------------------------

## ファイル

### Ver.1で使用する主な実行ファイル

```text
SD80VirtualMidiGUI.exe : GUI
SD80VirtualMidi.exe    : Backend
```

`SD80VirtualMidiGUI.exe` と `SD80VirtualMidi.exe` が連携して
SD80 Bridgeとして動作します。

### リポジトリ内の主なフォルダ・ファイル

```text
SD80Common
```

SD80 Bridgeで共通して使用する処理をまとめた共通ライブラリです。

```text
SD80MidiSend
```

SD-80へのMIDI送信を検証するためのプロジェクトです。

```text
SD80UsbExplorer
```

SD-80 USB接続およびUSBインターフェースの調査・検証に使用した
プロジェクトです。

```text
SD80VirtualMidi
```

SD80 BridgeのBackendを構成するプロジェクトです。

Virtual MIDIポートの生成、MIDI処理、SD-80 USBとの通信など、
Bridgeの主要なバックエンド処理を担当します。

```text
SD80VirtualMidiGUI
```

SD80 BridgeのGUIを構成するプロジェクトです。

接続状態、MIDI状態、MODE、DRUM CH、INSTRUMENTなどの
表示および操作を担当します。

```text
SD80VirtualMidiMinimal
```

SD80 Bridgeの最小構成・動作確認用として使用したプロジェクトです。

```text
driver
```

SD-80 USBドライバに関する調査・検証用のファイルを収録しています。

通常のSD80 Bridgeの使用では必要ありません。

```text
SD80Bridge.slnx
```

SD80 BridgeのVisual Studioソリューションファイルです。

ソースコードから開発・ビルドする場合に使用します。

------------------------------------------------------------------------

## 起動方法

### 1. SD80VirtualMidiGUI.exeを起動

`SD80VirtualMidiGUI.exe` を右クリックし、

**「管理者として実行」**

を選択してください。

### 2. 接続状態を確認

SD-80 BRIDGEの状態表示が以下のように変化します。

```text
Virtual MIDI
    STARTING (橙)
         ↓
    CONNECTED (緑)

SD-80 USB
    STARTING (橙)
         ↓
    CONNECTED (緑)

Bridge System
    NOT READY (橙)
         ↓
    READY (緑)
```

**3つの表示がすべて緑色になるまで待機してください。**

正常に接続されると、

- Virtual MIDI → CONNECTED（緑）
- SD-80 USB → CONNECTED（緑）
- Bridge System → READY（緑）

となります。

### 接続に失敗する場合

Virtual MIDIまたはSD-80 USBが

**DISCONNECTED（赤）**

になる場合は異常です。

まず以下を確認してください。

1. `SD80VirtualMidiGUI.exe` を「管理者として実行」しているか確認する。
2. Windowsを再起動して、再度SD80 Bridgeを起動する。

上記を行っても改善しない場合は、Windows MIDI Services / MIDISRV側の
状態、Windows側の環境、またはSD80 Bridge側の問題である可能性があります。

ただし、本ソフトウェアは動作および問題解決を保証するものではなく、
Windows MIDI Services / MIDISRV、Windows、DAW等について
サポートを保証するものではありません。

不具合と思われる現象が発生した場合は、原因調査の参考として
必要に応じて以下の情報を記録してください。

```text
・Windows OSバージョン
・Windowsビルド番号
・使用しているDAW
・発生した症状
・発生するまでの操作
```

これらの情報は、原因調査の参考情報として使用できます。

------------------------------------------------------------------------

## 使用方法

### 1. DAWを起動

**必ずSD80 Bridgeを先に起動してください。**

SD80 Bridgeが正常に

```text
Virtual MIDI       CONNECTED
SD-80 USB          CONNECTED
Bridge System      READY
```

となっていることを確認してからDAWを起動してください。

### 2. DAWのMIDIポートを設定

DAWのMIDIポート設定では、以下の4ポートを選択できます。

それぞれSD-80の機能にブリッジされています。

```text
a) SD-80 Bridge Virtual MIDI
       ↓
   SD-80 PART A（内蔵音源）

b) SD-80 Bridge Virtual MIDI Gr2
       ↓
   SD-80 PART B（内蔵音源）

c) SD-80 Bridge Virtual MIDI Gr3
       ↓
   SD-80 MIDI OUT 1（外部音源）

d) SD-80 Bridge Virtual MIDI Gr4
       ↓
   SD-80 MIDI OUT 2（外部音源）
```

### 3. MIDIポートを使用する

a) と b) のポートをDAWのMIDI OUTに設定すると、
USBドライバ使用時と同様にSD-80の内蔵音源を使用できます。

c) と d) のMIDI OUTはポートとして存在しますが、
Ver.1では動作未確認です。

------------------------------------------------------------------------

## 表示倍率

GUIのメニュー **表示** から、GUIのサイズを変更できます。

```text
100%
80%
50%
30%
最小
```

各倍率から「最小」を選択すると、
1～16CHの表示が省略され、ディスプレイ表示のみになります。

最小表示では、MIDI信号に同期して **Bridge System** の文字が点滅します。

------------------------------------------------------------------------

## MODE

メニュー **MODE** から、SD-80本体の現在のモードに合わせて、

```text
GM2
NATIVE
GS
```

を選択してください。

この設定は、GUIの `INSTRUMENT` に表示される
楽器名テーブルを選択するためのものです。

**この設定はSD-80本体とは連動していません。**

そのため、本アプリのINSTRUMENT表示を使用しない場合は、
特に設定する必要はありません。

------------------------------------------------------------------------

## DRUM CH

メニュー **DRUM CH** から、DRUMとして使用しているMIDIチャンネルを
チェックしてください。

この設定も、`INSTRUMENT` に表示される楽器名テーブルを
選択するためのものです。

**この設定はSD-80本体とは連動していません。**

本アプリのINSTRUMENT表示を使用しない場合は、
特に設定する必要はありません。

------------------------------------------------------------------------

## 終了方法

SD80 Bridgeを終了するときは、

**必ずDAWを先に終了してください。**

推奨する終了順序は以下のとおりです。

```text
1. DAWを終了
       ↓
2. SD80 Bridgeを終了
```

SD80 Bridgeを先に終了すると、DAWがMIDIポートを保持したままとなり、
DAWがハングアップする場合があります。

この場合は、WindowsのタスクマネージャーからDAWのタスクを終了し、
SD80 Bridgeを再起動してから最初の手順に戻ってください。

------------------------------------------------------------------------

## Windows MIDI Services / MIDISRVについて

SD80 Bridgeは、WindowsのMIDI環境を利用してSD-80 USBとの接続を行っています。

そのため、Windows MIDI Services / MIDISRVの仕様や状態、
Windows Updateによる変更などの影響を受ける可能性があります。

Windows MIDI Services / MIDISRV側に問題が発生した場合、
SD80 Bridge側だけでは解決できない場合があります。

SD80 BridgeにはMIDI接続を再初期化するための
リセットルーチンを実装していますが、
これは接続状態を再構築するための処理であり、
Windows MIDI Services / MIDISRV側のすべての問題を
解決するものではありません。

------------------------------------------------------------------------

## 不具合について

本ソフトウェアは個人で開発・検証しているVer.1のソフトウェアです。

すべてのWindows環境、DAW、MIDI機器での動作を
保証するものではありません。

また、Windows MIDI Services / MIDISRVを含む
Windows側の問題について、修正やサポートを
保証するものではありません。

不具合が発生した場合は、原因調査の参考として
以下の情報を記録してください。

```text
・Windows OSバージョン
・Windowsビルド番号
・使用しているDAW
・SD-80の接続状態
・SD80 Bridgeの表示状態
・発生した症状
・発生するまでの操作
```

------------------------------------------------------------------------

## Disclaimer

SD80 Bridgeは現状の状態で提供されるソフトウェアです。

使用によって発生したデータの損失、DAWの停止、
システム上の問題、その他の損害について、開発者は責任を負いません。

本ソフトウェアを使用する場合は、
使用者自身の判断と責任においてご利用ください。

------------------------------------------------------------------------

## Version

**SD80 Bridge Ver.1**
