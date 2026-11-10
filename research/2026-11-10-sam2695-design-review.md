# DinMeter Synth — SAM2695/GM自作シンセ先行実装調査・設計比較

調査開始: 2026-11-10（JST）  
対象: [DinMeter Synth](https://github.com/corecycletune/dinmeter-synth) / `SynthApp.cpp`（main、2026-11-10時点）  
性質: **調査メモ（ファームウェア変更なし）**。実機検証未実施の事項は「仮説」と記す。

## 1. 調査目的と評価軸

単なるリンク集ではなく、既存コードの**音作りの思想とMIDI状態管理**を読み、現行DinMeterとの差分・採用可能性・ハード制約・実験方法を記録する。

評価軸:
1. GM ROM音色の使い方（音色選択だけか、OSC素材として積層するか）
2. TVF/ENV/LFO/音量の制御方式（チップ内蔵機能か、ESP32側の時間更新か）
3. MONO/LEGATO/GLIDEのノート所有権・音程遷移
4. MIDI転送量と演奏イベント（特にNoteOff）の優先処理
5. M5Stack Din Meter + SAM2695での再利用可否、ライセンス

## 2. 確認した先行資料・コード

| 資料 | 確認範囲 | DinMeterへの用途 | 確認状況 |
|---|---|---|---|
| [Dream SAM2695データシート（M5Stack配布PDF）](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/unit/Unit-Synth/SAM2695.pdf) | MIDI CC/NRPN、GS SysEx、LFO共通レート、チップの機能制約 | 実装の真偽を判定する一次資料 | 表・脚注を確認 |
| [M5Stack M5-SAM2695](https://github.com/m5stack/M5-SAM2695) / [ソース](https://github.com/m5stack/M5-SAM2695/blob/main/src/M5_SAM2695.cpp) | `setTvf`/`setEnvelope`/`setVibrate`/`setPitchBend`/NoteOn/Off | 公式近縁の制御方式と差分 | 実際にソースを読んだ |
| [René Bohne ESP32-SAM2695-Synth](https://github.com/renebohne/esp32-SAM2695-synth) | README: BLE/USB MIDI入力、GM/MT-32 bank切替、CC/NRPN/SysEx | PCレスの接続設計、SAM2695制御機能 | README確認。実装本体の比較は**未実施** |
| [PHLXMSynth](https://github.com/echacont/PHLXMSynth) | README: Arduino/Fluxamasynth SAM2695、シーケンサ | GMチップ自作事例 | README確認。ソース未比較 |
| [M5UnitSynth MATLAB連携](https://github.com/amin45mh/M5UnitSynth) | [MATLABクラス](https://github.com/amin45mh/M5UnitSynth/blob/main/+arduinoioaddons/+M5Stack/M5UnitSynth.m) の命令インターフェース | パラメータ体系や制御APIの網羅性 | ソースの一部を確認 |
| [enudenki USB Host MIDI Library](https://github.com/enudenki/esp32-usb-host-midi-library) | README、`src/UsbMidi.cpp`、[Issue #4](https://github.com/enudenki/esp32-usb-host-midi-library/issues/4) | USB入力由来のNoteOff欠落・処理順序 | ソースとIssue確認 |
| [Roland SC-8820 MIDI資料](https://cdn.roland.com/assets/media/pdf/SC-8820_OM.pdf) | GS DT1の標準チェックサム | SAM2695実装との仕様比較 | 公式資料で照合 |

**制限**: 今回の先行事例群は網羅調査ではない。「同様の3OSC GMシンセが存在しない」とは言えない。公開コードを直接流用する前に各ライセンスを確認すること。DinMeter本体はPolyForm Noncommercial。

## 3. 現行DinMeterの音作りモデル（実コード確認済み）

参照: [`SynthApp.cpp`](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp)

- [WAVE_TABLE 8素材](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L375): GM/MT-32互換bankの既存音色を発振素材として選択する。独立したDSP波形生成ではなく**SAM2695のROMプレイヤーをOSC的に扱う**方式。
- [3 OSC + GMレイヤー](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L2038): 3チャンネルのOSCと独立GMレイヤーでNoteOn/Offを複製。各レイヤーでtranspose、音量、デチューンなどを扱う。
- [TVF](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L834): 内蔵TVFの静的NRPNを64（中立）に置き、GS SysExでCC1/CC2のcutoff深度を変化させる。両方のコントローラを127に固定し、パラメータ深度を書き換える方式。従来の`setTvf`一回設定との差は**発音中に変調しやすいこと**。
- [ENV](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L1008): SAM内蔵音色に対するADR設定に加えて、ESP32側のADSR的MOD ENVを別に計算。MOD ENVはチップの音量エンベロープと同一ではない。
- [LFO/MOD](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L1131): LFOを独自計算しルーティングへ渡す。動的MODの出力最小間隔は[32 ms](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L550)（最大約31回/秒）。
- [MONO/LEGATO/GLIDE](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L1874): チップの内部MonoとPortamentoはオフに保ち、ESP32側がlast-note priorityとPitch Bendでソフトウェア・ポルタメントを管理する。音程更新間隔は[5 ms](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L594)（理論最大200回/秒）、ベンド範囲は±24半音。
- [押鍵と出力の分離](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L2038): `heldInput`/`noteOrder`と`activeOscOutputNote`/`activeGmOutputNote`を分ける。発音中のtranspose変更でも実際に送った発音番号でNoteOffを送る設計。
- [USB MIDI入力](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L2523): CIN=8をNoteOff、CIN=9かつvelocity=0もNoteOffとして扱う。

### 既存のM5Stackライブラリとの比較

| 分野 | M5Stackライブラリで確認した方式 | DinMeterの独自設計 | 評価 |
|---|---|---|---|
| 波形 | Program Changeでチップ内蔵音色を直接選択 | ROM音色を3OSC相当に重ね、さらにGMレイヤー | 音作りの拡張だがチャンネル・発音数を消費 |
| Filter | `setTvf`でNRPN 0x0120/0x0121 | GS CC1/CC2深度による連続変調 | 連続制御の自由度と通信量を比較すべき |
| 内蔵ENV | `setEnvelope`でAttack/Decay/Release | 同じ内蔵ADR＋別系統のMOD ENV | どこが発音エンベロープか混同に注意 |
| LFO | `setVibrate`で内蔵vibrato制御 | ESP32側で複数LFOを計算し出力 | 内蔵音源との整合、レートと遅延を検証 |
| ポルタメント | MIDI Pitch Bend等の基本API | 音程変化を自前生成しMono状態を自分で保持 | 高機能だがNote状態同期が複雑 |
| Note管理 | MIDI命令を送る薄いラッパー | 入力押鍵・出力発音・レイヤーを個別追跡 | 故障箇所も多く、テストが重要 |

## 4. 検証で得た事実と反証

### 4-1. GS SysExのチェックサム固定0は、それだけではバグと断定できない

現行`sendGsPartParameter`は11バイトのSysExを送り、末尾直前（`msg[9]`）が`0x00`固定。標準的なRoland GS DT1ではアドレスとデータを使うチェックサムが必要。ただし**SAM2695の配布データシート29ページの脚注は「x or xx means don't care」**としている。該当一覧の`xx`がdon’t careであれば、本チップでは固定0で許される。よって**現時点の優先不具合候補から外す**。なお、機種互換性や動作の最終判断には実機テストが必要。

参照: [`sendGsPartParameter`](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L819) / [SAM2695 PDF](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/unit/Unit-Synth/SAM2695.pdf) / [Roland SC-8820](https://cdn.roland.com/assets/media/pdf/SC-8820_OM.pdf)

### 4-2. USBライブラリIssue #4は現行コードで最低限対処済み

[Issue #4](https://github.com/enudenki/esp32-usb-host-midi-library/issues/4): 「NoteOffがCIN=8でなく、NoteOn（CIN=9）/velocity 0として届く」という報告。DinMeterの`onMidiMessage`はvelocity=0をNoteOffに変換済み。したがって、このIssueだけでは現在の鳴りっぱなしの説明にならない。別のパケット欠落や接続異常がないか、入力ログで検証する。

## 5. 仮説・設計リスク（未検証、優先順位順）

### [P1] MIDIシリアル通信の帯域／イベント遅延

SAM2695は[MIDI_BAUD=31250](https://github.com/m5stack/M5-SAM2695/blob/main/src/M5_SAM2695.h)で接続。通常の8N1 UARTでは概算**最大3125バイト/秒**（実効帯域は別途測定）。現行コードはSysEx 11バイト/件を複数OSCへ送信し、Pitch Bendも最大約200回/秒送る設計。

**シナリオ例（上限寄りの概算）**: 3 OSCのfilter変調が毎32msに各2件のSysExを送信すれば `3 × 2 × 11 × (1000/32) = 約2063バイト/秒`。GLIDE時に4レイヤーへ3バイトPitch Bendを毎5ms送れば `4 × 3 × (1000/5) = 約2400バイト/秒`。両者の単純合計は約4463バイト/秒でリンク速度を超える。

これは**常に起きる負荷ではない**。実際はパラメータ変更時だけ送信し、各種キャッシュ・音の状態・GMレイヤーON/OFF等に左右される。送信バッファの詰まりがNoteOff遅延やPANICに関係するかは未確認。最優先で送信バイト数・最大キュー滞留・NoteOff遅延を計測したい。

### [P1] ノート所有権・PANICからの復元

`handleNoteOn/Off`、`rebuildHeldNotes`、`silenceSynthOnly`がそれぞれ状態を更新する。ソフトウェアGLIDE中のNoteOn再発行、オクターブ切替時に旧ピッチのNoteOffが欠落、外部電源・USB接触不良時の切断再接続などを組み合わせると破綻する可能性がある。**複数の故障原因を単一仮説で片付けない**。

### [P2] 音色素材とフィルタ変調の実際の効き方

SAM2695内蔵音色ごとの倍音・内蔵エンベロープが異なる。GM音色の一部は「アナログ波形」ではなくサンプル楽器で、TVF変化やピッチ変化の質も異なる可能性がある。素材ごとにCUTOFF最大最小・ENVを固定して録音・FFT比較を検討。

### [P2] チップ内蔵LFOとESP32独自LFOの使い分け

データシート上、内蔵LFOの一部rateはチャンネル共通。ESP32側で個別LFOを実装した理由は妥当だが、最終的にSysEx/CCで変調する限りシリアル帯域と可聴なステップを確認したい。

## 6. 鍵盤交換時の切り分け実験（Alesis Q25は未試験）

再現しなかったことと原因が分かったことは別。最低限、同じプリセット/演奏パターンで旧鍵盤とQ25を比較する。

| 条件 | 旧鍵盤 | Q25 | 見るもの |
|---|---|---|---|
| 単音、MONO/GLIDE OFF | 未測 | 未測 | NoteOn/Offの対 |
| 重ね弾き、MONO ON / GLIDE OFF | 未測 | 未測 | last-note priority |
| 重ね弾き、MONO/GLIDE ON | 未測 | 未測 | ノート取りこぼし、Pitch Bend |
| 押鍵中オクターブ変更 | 未測 | 未測 | 旧ピッチのNoteOff有無 |
| 押鍵中つまみ変更＋LFO最大負荷 | 未測 | 未測 | 音切れ・バッファ遅延 |
| USBケーブルと本体を動かさず→別ケーブル | 未測 | 未測 | 接触不良の切り分け |

入力側のUSB MIDI **受信パケット**とSAM2695へ**送信したMIDI bytes**を別々に記録すると、鍵盤原因・USBホスト原因・ノート管理原因・音源原因を分離できる。診断用ログ追加は将来の**別作業**とし、ここではファームウェアを触らない。

## 7. 次回以降の情報収集・設計比較テーマ

- SAM2695のGS SysExとNRPNをソース行・データシート行で突合し、音作り可能なパラメータとグローバル制約を整理。
- BLE/USB MIDI受信実装でイベントドロップ・キュー・コールバック優先度がどう扱われているか読み比べる。
- ソフトウェアMONO/LEGATO/GLIDEの公開シンセを探し、Note stack、発音所有権、glide curve、再トリガ方式を比較（**SAM2695以外も対象**）。
- アナログ風音色をGM ROM素材から作る方法（波形素材選択、トランスポーズ、デチューン、TVF、TVA、ステップノイズ）について実例・計測法を集める。
- MIDI帯域制約を守るイベントスケジューリング（NoteOff優先、CC/SysEx間引き、値差分送信）を調査。早期にコード変更せず、まず概念比較。
- 先行実装の採用は特許・ライセンス・利用条件も確認し、直接コピーではなく設計原理を理解する。

## 8. 更新ルール

- 今回の文書は調査の起点。追加情報は**「確認済み」「仮説」「反証」「実機試験待ち」**を混ぜない。
- 根拠は原典URL・ファイル・該当関数を付ける。前回未確認のものを確認済みに昇格させる際は実際に読む。
- 同じ情報は重複記載せず、結論が変わったときはその理由も履歴として残す。
- 実機テストやファームウェア変更はユーザーの指示があるまで行わない。
