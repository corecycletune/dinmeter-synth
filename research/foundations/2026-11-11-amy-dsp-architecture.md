# 2026-11-11｜AMYソース精読：帯域制限OSCと「osc→voice→synth」の分離

調査対象：[shorepine/amy](https://github.com/shorepine/amy) の公開ソース（2026-11-11閲覧）。調査は**コード読解のみ**、ビルド・実機試験・音響測定は未実施。README紹介にとどまった前回の調査から、今回は関数単位の確認へ進めた。

## 1. 確認した設計上の事実

### A. 波形の種類だけでなく「音域ごとの倍音制限」が発振器の品質を決める

[oscillators.c の choose_from_lutset()](https://github.com/shorepine/amy/blob/main/src/oscillators.c) は、目標の波形周期 `period` と、テーブルに含まれる最高次の倍音 `highest_harmonic`、テーブル長を使って、折り返しノイズ（aliasing）を起こさないLUTを選ぶ。コードでは `interp_bandwidth < 0.9f` を条件としている。別の `render_lut*` 関数群は、線形/三次補間やFM/feedbackの有無に応じてサンプルループを分ける。

**意味**：単純なSAW波形の1周期をどの音高でも同じサンプル列で高速再生すると、高音域ではナイキスト周波数を超える倍音が折り返す。AMYは周波数に応じて使用可能な倍音数を制限する設計。単に「波形生成可能」という仕様より重要な音質上の差。

**留保**：コード上の閾値は実装確認済みだが、全設定で完全にaliasingを除去できることを証明したわけではない。FM等の非線形変調が新たな倍音を生む場合は別評価が必要。

### B. 「3 OSC」と「3声ポリフォニック」は違う

[docs/synth.md](https://github.com/shorepine/amy/blob/main/docs/synth.md) は明確に `osc`（波形生成・個別パラメータ）→ `voice`（複数oscをまとめた1音）→ `synth`（複数voiceをノートへ割り当てる管理単位）を分けている。例として、Juno風パッチは**1 voiceを5 oscillatorsで構成**し、6 voicesのsynthを作る。synthはボイス割当・voice stealing・一括パラメータ変更を担う。文書例では、3 voicesへ4音目を弾くと最古のvoiceが奪われる。

**意味**：複数OSCを1つの鍵盤音に束ねる「音色構成」と、複数の鍵盤音を同時発音する「発音数・所有権」を別階層にする。フィルターENVもvoice単位に置くかsynth共通に置くかを明示できる。

**留保**：この段落は開発者ドキュメントに基づく。今回、実際のvoice stealing関数のコード追跡は未実施。**「実コードで確認済み」とはしない**。

### C. 音声スレッドとイベント入力のロック範囲を分ける

[amy.c のrender lock / queue lockの設計コメントと実装](https://github.com/shorepine/amy/blob/main/src/amy.c) は、レンダリングブロックを保護するロックと、イベント差分キューを短時間保護するロックを区別している。パッチロードはESP32-S3で数十msかかり得るため、レンダリング中に読み出す構造体の解放・再初期化が衝突しないよう設計されている。通常のnote-on入力は短いqueue lockのみで済む意図がコードコメントに明記されている。

**意味**：DSPへ移行すると、今までのMIDI UART帯域とは別に、音声処理の**締切（audio deadline）**とイベントの受け渡しが新しい制約になる。MIDIノートの受信処理から音声エンジンを直接操作する設計は慎重に評価すべき。

**留保**：コメントとロック宣言の確認であり、全イベント経路が必ず非ブロッキングであることは未検証。

### D. フィルターはカットオフ/共振だけでなく数値精度と安定性が重要

[filters.c の dsps_biquad_gen_lpf_f32() / dsps_biquad_gen_hpf_f32() / dsps_biquad_gen_bpf_f32() / dsps_biquad_gen_notch_f32()](https://github.com/shorepine/amy/blob/main/src/filters.c) は2次IIRフィルターの係数を計算し、カットオフの上限を正規化周波数0.45に制限する等のガードを持つ。固定小数点演算について、低いcutoffで丸め誤差が共振・クリップ・LPF分子ゼロ化に影響するという実装コメントがある。

**意味**：ESP32で自由なVCFを実装できるとしても、ノブを広い範囲で動かした時に不安定化しないことが必要。特に高Q・低cutoffをテスト対象にする。

**留保**：コメントにある不具合・対策はAMYの特定の演算経路の話であり、DinMeterで発生した事実ではない。

## 2. DinMeterとの比較と採用条件

| 論点 | 現行DinMeter（既存レビュー） | AMYから得られる原理 | 現時点の判断 |
|---|---|---|---|
| OSC | SAM2695のGM ROM音色をMIDIチャンネルで3レイヤー化 | 波形LUTを音域に応じて選び、直接PCM合成 | **現行SAM2695へは移植不可**。別DSP音源の評価基準にする |
| 発音階層 | OSCレイヤー別出力ノートを追跡 | osc / voice / synthを明確に分ける | **概念は再利用可能**。今すぐ全面改修はしない |
| MOD | ESP32が31Hz程度でMIDI制御値を更新 | voice内でaudio/control-rate処理を分担 | SAM2695ではaudio-rate波形処理は直接不可 |
| Filter | SAM2695内蔵TVF/GS制御 | 独立DSPフィルター係数・数値安定性 | DSP基板を選ぶ場合の検証項目 |
| イベント | USB MIDI入力→状態管理→MIDI UART送信 | レンダリング/イベントキューを分離 | **別DSP基板方式の設計指針**になる |

## 3. 今回の新しい設計判断

**重要**：「DACを追加すればGM音源より自由に音作りできる」という前回の一般論だけでは不十分。自作DSPを評価する際は、最低でも **(a) 波形のaliasing対策、(b) voice/osc階層とnote stealing、(c) オーディオdeadlineとイベントキュー、(d) フィルター数値安定性** をチェックする。

特に音色の豊かさを求めてOSC数だけ増やすと、`osc数 × 発音数`でDSP負荷が増える。AMYのドキュメント上の「最大osc数」は、実機でその数を高品質に同時発音できる保証ではない。

## 4. 反証・未解決

- AMYが良い構造を持っていても、**DinMeterのESP32-S3に空きI2Sピン/CPU/PSRAMがあることは未確認**。そのため現行機に載せると決める根拠はない。
- `choose_from_lutset()` が高音域のaliasingを抑える一方、低音域の波形補間や非線形フィルターの音質優位性を証明するものではない。
- 独立DSP基板でもUART/USBでのノート到着遅延や、2台のマイコン間の同期問題は残る。
- AMYのライセンス、依存ライブラリ、音色パッチの配布条件は**今回未確認**。直接コピーや組み込み前に確認する。
- 具体的なCPU負荷、レイテンシー、DAC出力のS/N、発音数は実機で測定していない。

## 5. 次回の実験・調査（まだ実施しない）

1. AMYの`synth`/voice allocation実装を関数単位で追い、レガートとnote stealingをDinMeterのノート所有権と比較する。
2. `oscillators.c`のLUT生成元を調べ、Saw/Square/triangleの帯域制限方式を整理する。
3. AMYのESP32-S3オーディオ出力経路とM5Stack Din Meter回路図を突合し、USBホストとI2Sの共存可否を評価する。
4. 1OSC/1voice、3OSC/1voice、3OSC/4voiceを同一条件で録音し、aliasing・CPU・音切れ・レイテンシーを比較する。

関連：[安価な非GM音源の比較](../hardware/2026-11-10-non-gm-synth-hardware.md) / [名機設計の基礎](2026-11-10-classic-synth-architectures.md) / [DinMeter SAM2695レビュー](../2026-11-10-sam2695-design-review.md)
