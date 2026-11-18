# 2026-11-18｜Mutable Instruments Warps：音声同士を掛け合わせるRing Modと、OSCを重ねるだけの違い

分類: 音声レート変調 / クロスモジュレーション / DSP・音作り基礎  
確認: Mutable Instruments公式マニュアルと、`pichenettes/eurorack`のWarpsソース（`modulator.cc`, `modulator.h`, `parameters.h`）の該当関数を実読。**実機試験・性能測定なし**。その他のWarps DSP全体（vocoder、oscillator、sample rate converter内部）は未読。

## 1. 新しい設計原理：2音を「加算」するのと「乗算」するのは違う

- 普通のOSCミックス: `y(t) = a(t) + b(t)`。音量・位相の干渉はあるが、理想的な線形ミックスそのものは新しい周波数成分を生成しない。
- **理想的なリング変調**: `y(t) = a(t) * b(t)`。2つの正弦波なら `sin(2πf₁t)sin(2πf₂t) = 1/2[cos(2π(f₁-f₂)t)-cos(2π(f₁+f₂)t)]`。元の2周波数そのものではなく、和・差周波数の側帯波が現れる。非整数比では金属的・ベル的な響きが出やすい。
- **振幅変調AMとの違い**: `y(t)=[1+m sin(2πf₂t)]sin(2πf₁t)` のようにキャリアを残すAMはキャリア成分も持つ。理想的なバランスド・リング変調ではキャリア抑圧。DCオフセットやクリッピングがあると抑圧が崩れる。
- 低周波のLFOでVCAを揺らす「トレモロ」と、可聴帯域の別OSCで音声サンプルを乗算する「リング変調」は制御レートと音声レートの違いが本質。低周波乗算をトレモロとして使うことは可能だが、一般のMIDI CC更新を音声レートの掛け算と同一視できない。

## 2. Warpsで実際に確認した信号経路と制御

**音声経路**: 2つの音声入力（または片方を内部OSCで置換）→ `SaturatingAmplifier::Process()` で入力ゲイン/ソフトクリップ → `Modulator::Process()` で選択したクロス変調処理 → 出力。通常の減算合成型 `OSC→Mixer→VCF→VCA` に対して、Warpsは**OSCや外部音源の後に挿入する音声レートの処理器**であり、標準のVCFやADSRを丸ごと置き換えるものではない。

**制御経路**: `Parameters` の `channel_drive[2]`, `modulation_algorithm`, `modulation_parameter`, `carrier_shape` などで、入力ゲイン、アルゴリズムの選択/補間、各アルゴリズム固有の音色、内部OSCを設定する。ソースでは `ParameterInterpolator` や `ProcessXmod<...>` の処理でブロック内の値を変化させる。CV/ノブ値が直接「音声」になるわけではない。

**スコープ**: Warpsモジュールは入力された**混合音声全体**を処理する。ポリシンセの各Voiceに別々のリング変調を掛けるなら、Voiceごとに別の乗算器/変調OSC/状態が必要。1台のWarpsを和音の最終出力に挿せば、音同士の相互変調も起こりうる。

## 3. ソースで確認したアルゴリズムの違い

- `Modulator::Xmod<ALGORITHM_DIGITAL_RING_MODULATION>` は入力2音を乗算し、パラメータ依存のゲインを掛け、`ring / (1 + abs(ring))` で非線形に圧縮。**純粋な乗算だけではなくソフト飽和が入る**。したがって理想的な和・差周波数のみになるとは限らない。
- `Modulator::Xmod<ALGORITHM_ANALOG_RING_MODULATION>` は `Diode(modulator + carrier) + Diode(modulator - carrier)` と `SoftLimit` を使う。**デジタル上でダイオード回路風の非線形性を模倣**している。上記のデジタルリングと同じ周波数スペクトルとは限らない。
- `Modulator::Xmod<ALGORITHM_XOR>` は入力を16bit整数にして排他的論理和を取り、元の加算波形と補間する。音声サンプルのビット操作であり、リング変調の単なる別名ではない。
- `Modulator::Xmod<ALGORITHM_FOLD>` は入力の和に乗算成分を少し混ぜ、波形折り畳みLUTを通す。倍音を新しく作るが、リング変調とは異なる非線形処理。
- `Modulator::xmod_table_` は隣接するアルゴリズム間の補間用関数表。`ProcessXmod<algorithm_1,algorithm_2>` は両方式の出力をバランスで補間する。これは1つのモジュレーション・ノブで複数方式を横断するUI設計例。
- `modulator.h` に `kOversampling=6`、`src_up_` / `src_down_` があり、`modulator.cc` の `Process` にリサンプル経路がある。**オーバーサンプリングを用いる設計**は確認。ただし実際の全経路のレート、アンチエイリアス効果やCPU消費は今回未測定。公式の製品説明にあるレートをESP32上での性能と同一視しない。

## 4. なぜこの設計が必要か／音作りで何に効くか

公式マニュアルは、Warpsが2つの音声を組み合わせる多様なクロス変調方式を提供し、単純な波形を内部キャリアとして使えることを明記する。これは外部OSCが1つしかなくてもリング変調を使えるようにする設計（公式に明記された用途）。公式はクロス変調向けに正弦波等の倍音の少ない波形、ボコーダー向けに倍音豊かな波形を案内している。

- **ベル/金属音**: 例として正弦波440Hzと660Hzを理想的に乗算すると220Hzと1100Hzが現れる。実際のWarps出力は入力の倍音と非線形圧縮により追加成分を持つ。
- **ドローン/ノイズ系**: 2音の周波数を微妙に動かすと、和・差側帯波も移動し、フィルターだけでは得られないスペクトルの動きを作れる。
- **音声レート変調の注意**: 高域側帯波はNyquistを超えると折り返しノイズになる。6倍オーバーサンプリングは対策の一種だが、特定の品質を保証するものではない。

## 5. 現行DinMeter/SAM2695との対応

| 区分 | 内容 |
|---|---|
| 実装済み（既存レビューより） | SAM2695のROM音色を3つのMIDIチャンネルで重ねるOSC風レイヤー、ピッチ/レベル/TVF等のMIDI制御。**音声サンプルの乗算ではない** |
| MIDIで近似可能 | 2レイヤーのデチューンやピッチ変調で「動く音」を作ること。ただし**リング変調の和・差周波数生成とは非等価** |
| DSP/音源変更が必要 | 2つの独立音声波形をサンプル単位で乗算するリング変調、ダイオード非線形、XOR、wavefold。既存SAM音声を処理する場合は音声入力ADC/Codecを伴う外部DSP、または内部DSP音源を用いる |
| 未確認 | SAM2695内部の音色/エフェクトにリング変調相当の機能が存在するか（現行レビューには根拠なし）、DinMeterのI2S/CPU余力、音質/遅延、各音声独立処理の実装可能性 |

## 6. 反証・代替案・将来の最小実験

- **反証1**: 「GM音源では絶対にリング変調音が出ない」とは断言しない。ROM音色が元から金属的側帯波を含む可能性があるし、未確認の内部効果もある。ただし音声同士の自由な掛け算をMIDI制御だけで実装したとは言えない。
- **反証2**: 「リング変調には必ず高価なアナログICが必要」は誤り。安価なマイコンのDSPで乗算できる。ただし**入力2音の生成/取得、ADC/Codec、DAC、音声処理CPU、電源と配線**は必要。
- **反証3**: 「2OSCにすると必ず金属音になる」は誤り。単純加算なら周波数和差の新成分は作らない。非線形のゲイン/フィルター/クリッピングは別の現象。
- **検証案（未実施）**: 48kHzで440Hzと660Hzの正弦波を生成し、加算・純乗算・Warps風の飽和付き乗算を同レベルで録音/FFT比較。理想乗算は220Hz/1100Hzを予測。Warps風飽和は高次成分が増える可能性。周波数を上げて折り返しも比較。
- **ハードウェア案（未採用）**: ①別ESP32-S3/Pico 2で2OSCとリング演算を生成してI2S DACへ、②GM出力をADC/Codecで取り込みDSP処理、③アナログリングモジュール追加。価格・基板サイズ・レイテンシー・音質の評価前に購入しない。

## 7. 一次資料・読んだソース（2026-11-18確認）

- [Mutable Instruments Warps公式マニュアル](https://pichenettes.github.io/mutable-instruments-documentation/modules/warps/manual/) — 音声入力、内部OSC、アルゴリズム、パラメータ、設計意図。
- [Warps公式製品資料](https://pichenettes.github.io/mutable-instruments-documentation/modules/warps/) — ハードウェアSTM32F405/WM8731、公式公称レート。製品版の仕様であり、他の移植版には適用不可。
- [`warps/dsp/modulator.cc`](https://github.com/pichenettes/eurorack/blob/master/warps/dsp/modulator.cc) — `Modulator::Process`, `Xmod<ALGORITHM_DIGITAL_RING_MODULATION>`, `Xmod<ALGORITHM_ANALOG_RING_MODULATION>`, `Xmod<ALGORITHM_XOR>`, `Xmod<ALGORITHM_FOLD>`, `xmod_table_`。
- [`warps/dsp/modulator.h`](https://github.com/pichenettes/eurorack/blob/master/warps/dsp/modulator.h) — `ProcessXmod`, `SaturatingAmplifier::Process`, `kOversampling`。
- [`warps/dsp/parameters.h`](https://github.com/pichenettes/eurorack/blob/master/warps/dsp/parameters.h) — アルゴリズム種別、内部OSC種別、各制御パラメータ。
- [DinMeter既存のGM音源・設計レビュー](../2026-11-10-sam2695-design-review.md)、[知識カード001](../knowledge/001-gm-rom-as-osc.md) — 比較の出発点。

**ライセンス**: Warpsの上記ソースヘッダにはMIT許諾文を確認。ただし再利用時は依存コード・リソース・ライセンスの全体確認と著作権表示が必要。本文は原理の説明に留め、コードの大量転載はしない。
