# 第7回｜Surge XTのノート所有権・発音優先順位・再利用

調査日: 2026-11-16 JST。対象: Surge XT公開リポジトリの現行main取得版（コミットSHA未固定）。実機・ビルド・音声測定なし。**コード読解で確認した部分と設計上の推論を分ける**。

## 今回の問い

MONO/LEGATO/GLIDEを単なる「音程の移動」と捉えると、押鍵中の複数ノート、音声の再利用、ENVの継続/再トリガ、NoteOff後の復帰で破綻する。成熟したソフトシンセが**押鍵状態**と**実際に鳴るVoice**をどう分離しているかを調べる。先行ADSRノートは曲線と時間定義、今回は**ノート所有権とvoice再割当**に焦点を絞る。

## 一次ソースと確認した関数

- [SurgeSynthesizer.cpp](https://github.com/surge-synthesizer/surge/blob/main/src/common/SurgeSynthesizer.cpp) の `playVoice()`（取得ソース763行付近）、`getNonUltrareleaseVoices()`（613行付近）、`softkillVoice()`呼び出し（806行付近）、`reclaimVoiceFor()`呼び出し（975行付近）、押鍵記録（488行付近）、ノート優先順位選択（1435、1502行付近）を確認。
- [SurgeSynthesizer.h](https://github.com/surge-synthesizer/surge/blob/main/src/common/SurgeSynthesizer.h) の `playVoice`、`softkillVoice`、`enforcePolyphonyLimit`、`reclaimVoiceFor`宣言を確認。
- Surge XTのライセンス表記は `SurgeSynthesizer.cpp`冒頭に **GPL-3.0-or-later**。直接コードを移植しない。
- **未読**: `SurgeVoice`本体、上記の関数全体（特に `softkillVoice`の選択規則）、MPE固有経路、各voice DSPの信号処理。コード中の関数名だけで具体的な音声フェード時間や音質を断定しない。

## コードで確認できたこと（事実）

1. NoteOn経路は `midiKeyPressedForScene[scene][key]` に単調増加の `orderedMidiKey` を記録してから `playVoice()` を呼ぶ。別に `channelState[channel].keyState[key]` に velocity、keyIsDown、lastdetune、host note IDを記録する。つまり**入力イベント状態と発音Voiceは同一データではない**。
2. `playVoice()` は `getNonUltrareleaseVoices(scene)` と `polylimit` を比較し、超過分に `softkillVoice(scene)` を呼ぶ。さらに `enforcePolyphonyLimit(scene,3)` を呼ぶ。これは**ポリフォニー上限が実際のvoice管理に結びついている**証拠。ただし具体的なsteal選択規則は未読。
3. Monoの一部の経路は `monoVoicePriorityMode` により `ALWAYS_HIGHEST`、`ALWAYS_LOWEST`、`ALWAYS_LATEST`等を区別する。NoteOff側でも押鍵集合を走査し、最高・最低・最新のキーを求める処理がある。**NoteOff後の復帰先は最後に受けたMIDIイベントだけでは決まらない**。
4. Monoで既存Voiceを再利用する経路に `reclaimVoiceFor()` があり、`monoVoiceEnvelopeMode == RESTART_FROM_ZERO` を渡す。さらに特定モードで `resetPortamentoFrom()` を呼ぶ。したがって**音程の遷移・ENV再起動・voice再利用は独立の設計判断**として実装されている。

## 音声経路と制御経路

Surge XTの一般的なDSP経路（個別のコードは今回未読）: **各voiceのOSC → フィルター等 → 音量/ミックス → シーン/全体FX → 出力**。上記で確認したコードは主に**音声そのものを計算するのではなく、そのvoiceをいつ生成・再利用・解放するか**の制御経路。

制御経路: MIDI NoteOn/Off、押鍵順序、チャンネル、Mono優先順位、Poly上限、ENV再起動モード、Glideモード → voice状態・音程・ENVの挙動。Voiceごとに発音状態が存在し、シーン共通のポリ上限や優先モードが割当判断に作用する。**実際のPer-voice ENV/DSPの更新頻度は今回未読で未確認**。

## なぜこの設計が要るか（コードに基づく推論）

- 押鍵と発音を分離しないと、押しっぱなしのキーと、Voiceを奪われて実際には鳴っていないキーを区別できない。
- MonoでC4を押したままE4を押し、E4を離したとき、C4に戻るか無音になるかは**押鍵集合と優先順位**で決まる。ENVを再トリガするか、音程を滑らせるかは別の判断。
- Polyで上限を超えた際、古い/リリース中のvoiceを奪う場合も、MIDI入力の押鍵情報は失ってはいけない。**ただしSurgeが具体的にどのvoiceを優先して奪うかは未読**。

## DinMeter/SAM2695との対応

| 要素 | 現行DinMeter（2026-11-10レビュー時点） | 今回の知見 |
|---|---|---|
| 入力押鍵と出力発音 | `heldInput`/`noteOrder`と`activeOscOutputNote`/`activeGmOutputNote`を分離。**実装済み** | Surgeの分離方針と共通。ただし全体の状態遷移の同等性は未確認 |
| Mono優先順位 | last-note priorityをソフト実装。**実装済み** | high/low/latestの切替は**将来の拡張候補**。現行GM音源に必須ではない |
| Glide | ESP32で5ms間隔のPitch Bend送信。**実装済み** | Surgeのvoice内部のpitch遷移と異なり、MIDI帯域と音源応答に制約。連続性の同等性は**未確認** |
| ENV retrigger | SAM内蔵ENV＋ESP32 MOD ENVが別。挙動詳細は**未確認** | 再トリガとvoice再利用を独立の選択肢として設計する価値 |
| Voice stealing | SAM2695内部の割当詳細は**不明** | MIDIチャンネルを増やすだけではDSP内部のvoice stealing規則を制御できない |
| Audio-rate modulation | SAMの内部信号に直接アクセス不可 | 独自DSP/音源変更が必要 |

## 反証・設計上の注意

- **「Surgeが同じノート管理を使うのでDinMeterも正しい」は誤り**。データを分離していても、PANIC、USB再接続、transpose変更、NoteOff欠落で状態不整合は起こり得る。
- **「voice stealingを実装すれば音切れが解消する」も誤り**。SAM内部のポリ割当はブラックボックス。現行DinMeterのGLIDEの階段状変化はMIDI帯域・音源内のPitch Bend補間・更新周期など複数要因がある。
- **「Monoでは1つのvoiceしか必要ない」とは限らない**。ENVの余韻を重ねる実装もあり得る。Surgeの具体的なvoice数はモード別の実測・コード追跡が必要。

## 実験案（未実施）

1. MIDIイベント列 `On(C4)→On(E4)→Off(E4)→Off(C4)` と逆順を入力し、held集合、優先音、実際の送信NoteOn/Offを時系列で比較。
2. `On(C4)→On(E4)→Octave変更→Off(E4)→Off(C4)` で送信先の実音程を記録。transposeを変更した後のNoteOffの音程を推測で計算し直さない。
3. MonoのENV継続/再起動、GLIDE on/offを分け、音声録音で立ち上がりと音程曲線を比較。
4. Polyの上限超過とPANIC/USB再接続を加え、入力押鍵数、出力NoteOn/Off、音源の実際の鳴りを独立に記録。

## 再利用する原理

**押鍵（入力の真実）・発音voice（音声の真実）・遷移ルール（優先順位/ENV/Glide）を別々に定義する。** これにより「音程を変えた」「押鍵が変わった」「同じvoiceを再利用した」を混同しない。今後はSurgeの`softkillVoice`/voice本体の精読と、DinMeterの最新コードとの照合を行う。

ライセンス: Surge GPL-3.0-or-later。**今回の記述は原理の独自要約で、コードを転載・移植していない**。
