# 引き出し 002｜MIDI送信負荷を制御し、NoteOffを遅らせない

作成: 2026-11-10  
分類: 安定性 / 通信 / リアルタイム処理  
段階: **コードからの設計リスク（未計測）**

## 何に使える？

演奏中に音が鳴りっぱなしになったり、ポルタメントとLFOを同時使用すると制御が乱れたりする場合の原因切り分け。音作り機能を増やす前の「送信量の予算」にも使う。

## 確認済みの実装事実

- [DinMeterの送信先](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L778): `SynthSerial`へMIDIの生バイトを出力。
- [SAM2695のMIDI_BAUD=31250](https://github.com/m5stack/M5-SAM2695/blob/main/src/M5_SAM2695.h): UART標準8N1として最大約3125バイト/秒（1バイト10ビットで計算）。
- [GLIDE_UPDATE_MS=5](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L594): GlideのPitch Bend更新は最大約200回/秒。
- [MOD_OUTPUT_INTERVAL_MS=32](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L550): ENV/LFO等の動的MOD出力は最大約31回/秒。
- [sendLiveCutoff](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L985): OSCごとに値差分キャッシュを用い、同じ値なら再送しない。変更時はGS SysExを2件出す。
- [sendGsPartParameter](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L819): 1メッセージ11バイト。
- [onMidiMessage](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L2523): NoteOn velocity=0をNoteOffとして受ける。このため[USB MIDI Issue #4](https://github.com/enudenki/esp32-usb-host-midi-library/issues/4)の例は**単独では原因にならない**。

## 理論上の送信負荷（音源側シリアル、概算）

- TVF: 3 OSC × (2件×11バイト) × (1000/32) ≈ **2063バイト/秒**。
- Glide: 最大4レイヤー × 3バイト × (1000/5) ≈ **2400バイト/秒**。
- 単純合計 ≈ **4463バイト/秒** > 3125バイト/秒。

**重要**: これは全OSCが毎回変化し、GMレイヤーが有効で、Glideも頻繁にベンド更新する等の厳しい仮想ケース。実負荷ではない。Pitch Bend更新やSysEx送信の一部はキャッシュ・条件分岐で抑制される。負荷超過による音の鳴りっぱなしが確認されたわけではない。

## 採用候補の考え方（設計案・まだ実装しない）

1. NoteOff / All Sound Off / 接続切断の安全イベントを最優先する。NoteOnも演奏上重要。**演奏イベント優先**。
2. フィルター等の連続値は、前回送信値との差が小さければ間引き、最終値は保持する（coalescing）。
3. MIDIリンクに「最大バイト/秒」「現在の送信待ち」「イベント遅延」の指標を作る。更新間隔だけでなくバイト数で制御。
4. 送信キューとデバイスの実際の受信能力は別なので、書き込み完了の意味を誤解しない。
5. 重要イベントの優先順位付けには、現在の生バイト送信をどう直列化するか設計が必要。**SysExや3バイトメッセージの途中に別のMIDIメッセージを挿入して壊さない**こと。

## 反証・他の可能性

- 鳴りっぱなしはUSB鍵盤側のNoteOff欠落、USBケーブル・本体接触不良、ノート所有権の同期、チップ状態に起因する可能性もある。
- 低負荷の単音でも再現すれば「送信帯域の飽和だけが原因」という仮説は弱まる。
- 同じ音色・負荷でQ25に替えるだけで再現性が変わる場合、USB入力側の差を疑う。ただし完全なソフト無罪にはならない。

## 実機での最小検証（未実施）

- USB受信のNoteOn/Off数と、SAMへ出したNoteOn/Off数を**別々に計測**する。
- 1秒あたりの書き込みバイト数、連続ピーク、NoteOff到達遅延を記録。
- GLIDE ON/OFF × MOD ON/OFF × OSC数1/3 × 鍵盤旧/Q25 の条件で再現性を比較。
- NoteOff遅延が確認されれば優先送信・パラメータ間引きを段階的に検討。ファームウェア変更は別途指示を受けてから。

## 参考

- [enudenki USB MIDI Host](https://github.com/enudenki/esp32-usb-host-midi-library)
- [公式M5-SAM2695](https://github.com/m5stack/M5-SAM2695)
- [詳細レビュー](../2026-11-10-sam2695-design-review.md)

**取り出す場面**: 音の鳴りっぱなし、長時間演奏の安定性、GLIDE＋LFOの競合、新機能追加時の通信設計。
