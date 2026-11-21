# 2026-11-21｜VCV Rackのパラメータ表示曲線・実値・平滑化を分離する設計

分類: 操作UI / 音楽的ノブカーブ / デジタル制御
状態: **VCV Rack v2ブランチのParamQuantity.hpp / ParamQuantity.cppの該当関数を精読**。Engine側の平滑化アルゴリズム、DinMeter最新SynthApp.cppは今回未読。実機試験なし。

## 1. 原典と版
- [VCV Rack v2 ParamQuantity.hpp](https://github.com/VCVRack/Rack/blob/v2/include/engine/ParamQuantity.hpp) — displayBase/displayMultiplier/displayOffset、smoothEnabled、snapEnabledの定義。v2ブランチは可動参照でありコミット固定ではない。
- [VCV Rack v2 ParamQuantity.cpp](https://github.com/VCVRack/Rack/blob/v2/src/engine/ParamQuantity.cpp) — `getDisplayValue()`, `setDisplayValue()`, `setValue()`, `setImmediateValue()`, `getValue()`, `getImmediateValue()` を確認。
- ソース内コメントではRack 2.3.0以前と以降でsetValue()の意味が変わる。**Rackの全バージョンに同じ挙動を一般化しない**。
- VCV Rackのコードを直接移植せず、設計原理のみ参照。利用前に該当ソースのライセンスを個別確認。

## 2. 新しい設計原理：ノブ表示・内部値・時間変化は三つの別問題
**事実（コード）**: `ParamQuantity`はエンジンが保持する値と表示値の変換を分離する。`displayBase=0`なら線形、正なら指数、負なら対数として表示値を計算する。倍率とオフセットも表示側で適用する。逆変換`setDisplayValue()`もある。`snapEnabled`で整数化でき、`smoothEnabled`はエンジンのスムージング目標値への設定経路を選ぶ。

**重要な区別**: これは**表示値の変換**であり、ノブ位置からDSPカットオフをどの曲線で動かすかを自動的に規定しない。ノブ物理位置→内部パラメータ、内部パラメータ→画面表示、パラメータ目標値→実際の時間変化を分けて設計する。

音声信号: `OSC → MIXER → FILTER → VCA → FX`。ここで操作するのは主にFILTER cutoff、ENV時間、LFO速度などの**制御パラメータ**。ENV/LFO/鍵盤による変調は別の制御経路。VCVのParamQuantity自体は音声処理器ではない。音声ごと/全体共通かは対象モジュールのDSP構成で決まり、このAPIだけでは判断できない。

## 3. 音楽的な使い方と設計例（推論、VCVの仕様ではない）
20–20000 Hzのcutoffを物理ノブ位置`x∈[0,1]`から指数写像する例: `f(x)=20*(1000**x)` Hz。中央は約632 Hzで、線形写像の中央10010 Hzとは大きく違う。低域の微調整がしやすいが、目的によっては中域を狭く感じる。

ENV時間10 ms–10 sなら`t(x)=0.01*(1000**x)`秒、中央約316 ms。LFO速度0.05–20 Hzも比率を保つ写像が候補。これは**提案式**であり、DinMeterの現行仕様でもVCVの実際のノブ曲線でもない。

## 4. DinMeter / SAM2695との対応
- **実装済み（既存レビューで確認）**: ESP32からSAM2695のTVF/ENV/LFO関連パラメータをMIDIで更新する。3OSCは独立MIDIレイヤーであり、独立した音声DSPではない。
- **MIDI制御で近似可能（設計候補）**: 物理ノブの非線形写像、数値表示のHz/ms等への換算（ただしSAMのパラメータ値と物理量の対応を測定する必要）、送信値の差分抑制、操作時のデッドバンド。
- **DSP/音源変更が必要**: SAMの内部音声サンプル単位のカットオフ補間を自由に選ぶこと、音声ごとに独立した平滑化を設計すること。MIDI送信値の滑らかさは音源内部の音声平滑化を保証しない。
- **未確認**: 最新SynthApp.cppのノブマッピング、SAM2695のcutoff/ENVの物理単位換算、MIDI値変更時の内部補間と可聴段差。VCVのEngineスムージング実装本体も未読。

## 5. 反証・制約・実験案
- **反証**: 指数写像が常に良いわけではない。GM音源内蔵のcutoffが非線形なら、入力に指数を重ねると特定領域へ偏る。聴感上の均等性は素材とフィルター特性で変わる。
- 7bitのMIDI CCや有限分解能のSysEx/NRPNでは、表示だけ滑らかでも制御値は段階的。連続送信は31250bpsの帯域を圧迫し、NoteOff遅延を招く仮説がある（未実測）。
- **将来の実験（未実施）**: 同一GM素材でcutoffの入力値を段階走査し、録音スペクトルの変化量と可聴段差を測る。ENV時間はNoteOnから振幅包絡の実測値で較正。線形/指数/測定補正LUTを比較し、操作時間あたりの狙った音への到達性を評価。
- 表示上のHzやmsは、実測またはメーカー仕様による較正ができるまで**推定値**と明示する。

## 再利用できる判断
UI曲線の最適化はDSPの変更なしでも可能だが、**内部音源の物理応答が不明なままノブを「Hz」「ms」と表示するのは誤認を生む**。まずMIDI値と音響結果の対応を測り、物理ノブ写像・表示単位・時間平滑化を独立に評価する。
