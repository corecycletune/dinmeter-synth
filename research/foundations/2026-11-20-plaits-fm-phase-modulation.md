# 第11回｜Plaitsの2-op FM：位相変調、4倍オーバーサンプリング、高音域の変調量制限

調査日: 2026-11-20。分類: FM/位相変調・DSP/音質。成熟度: **公開ソースの該当関数を精読。実機・CPU負荷・音質未測定**。

## 既存調査との差分
先行10回のOSC帯域制限、リング変調、ENV、Voice管理、FXを確認した。今回は**乗算リング変調とは異なる「音声レートの位相変調FM」**と、FM固有のエイリアシング対策を調査する。前回のAMYリバーブや第6回のPWM/PolyBLEPと重複しない。

## 原典（2026-11-20にGitHubで内容確認）
- [Mutable Instruments eurorack / Plaits FM実装](https://github.com/pichenettes/eurorack/blob/main/plaits/dsp/engine/fm_engine.cc) `FMEngine::Init`、`FMEngine::Render`。
- [Plaits FM状態定義](https://github.com/pichenettes/eurorack/blob/main/plaits/dsp/engine/fm_engine.h) `FMEngine`。
- [Plaitsテスト](https://github.com/pichenettes/eurorack/blob/main/plaits/test/plaits_test.cc) テストハーネスにFMエンジンが含まれるが、この調査では**FMテスト関数本体の全範囲は未読**。
- [AMY FMアルゴリズム](https://github.com/shorepine/amy/blob/main/src/algorithms.c) `FmAlgorithm algorithms[33]` の存在は検索結果で確認、内部の処理順序は未読。

**ライセンス**: Plaitsの `fm_engine.cc/.h` 冒頭にはMIT許諾文が記載されている。リポジトリの他のファイル（例：テストコード）はGPL表記があり、依存ファイルを含む再利用には個別確認が必要。ここではコード移植せず原理を学ぶ。

## 1. 信号の流れと音作り
- **音声経路**: 正弦波モジュレーター → キャリア正弦波の位相に加算 → 音声出力。FMエンジンはOSC相当の音源で、音量ENV、VCF、VCAなどを統合した完全なシンセ音声経路そのものではない。Plaits Voice側の接続・ENV/音量処理は今回未読。
- **制御経路**: `harmonics` はFM比率（音程差をルックアップ表で量子化）、`timbre` は変調量、`morph` はフィードバック量を制御。これらは `ParameterInterpolator` でブロック内に補間。
- **1音/全体**: `FMEngine` は `carrier_phase_`、`modulator_phase_`、`sub_phase_`、`previous_sample_` 等をインスタンス状態として保持。**各発音に独立インスタンスを割り当てれば**位相・帰還状態は独立できるが、実機PlaitsのポリフォニックVoice配列や実発音数はこのファイルからは断定しない。
- **代表的な音**: 小さい変調量で柔らかな正弦波系、大きい変調量で多くの側帯波、非整数比率で非調和倍音の金属音、フィードバックで複雑な音色を得られるという一般的なFM/PM理論。Plaitsの実音は未試聴。

## 2. コードで確認できた設計上の工夫

### (a) 実装上は位相変調
`float modulator = SinePM(modulator_phase_, modulator_fb * previous_sample_)` と `float carrier = SinePM(carrier_phase_, amount * modulator)` があり、**モジュレーターをキャリア位相へ入力**する。周波数を毎サンプル積分して変調する方式と同一のコードではない。音楽機材で「FM」と呼ばれる位相変調系の実例。リング変調の `carrier * modulator` とは別の演算。

### (b) 4倍オーバーサンプリング
`for (size_t j = 0; j < kOversampling; ++j)` で高レート合成し、`Downsampler` で本来の出力レートへ戻す。ソース冒頭に `// 4x oversampling` のコメント。**高周波の折り返しを減らす狙いと推論**できるが、抑圧量はフィルタ係数と試験条件で変わる。内部計算回数も増えるため、低コストMCUでは負荷検証が必要。

### (c) 高音域でFM indexを抑える
`hf_taming = 1 - (modulator_note - 72) * 0.025` を0〜1へ制限して二乗。変調量は `2 * timbre² * hf_taming`。ソースコメントは「高音ノートの最大FM indexを下げ、aliasingを防ぐ」。つまり**同じTIMBRE値でも高音域では明るさが抑えられる**。音色の鍵域均一性と折り返し低減のトレードオフ。

### (d) 帰還は符号で動作が分かれる
`feedback < 0` では位相増分側、`feedback > 0` ではモジュレーターの `SinePM` 位相側に前回出力を使う。符号で**異なる帰還経路**を切り替える。前回サンプルには `ONE_POLE(previous_sample_, carrier, 0.05f)` の平滑化がある。単純な「feedbackの強弱」ノブではない点が設計上重要。

### (e) AUXは独立サブ出力
`sub_phase_` はキャリア位相増分の半分で進み、`sub = SinePM(sub_phase_, amount * carrier * 0.25f)` を別のダウンサンプラーで処理。FMエンジンはキャリア本線とAUXに異なる信号を出す。**2-opの「2」は音声出力数ではなく主なキャリア/モジュレーター構造**。

## 3. なぜこの構造か（事実と推論の分離）
- **ソースに明記**: 高音域のFM index制限はaliasingを防ぐ目的。
- **コードからの推論**: 4倍oversampling+downsamplingは高域のaliasing対策。コストは合成処理を複数回実行すること。
- **一般理論**: 位相変調の側帯波は指数と周波数比で変化。高音で変調量を一律維持するとナイキストを超える側帯波が増えやすい。
- **未確認**: 具体的なPlaitsハードウェアのCPU負荷、oversampling前後のaliasing抑圧dB、サンプルレート依存性、実際の高音域音色差、利用者が感じるノブ操作感。

## 4. DinMeter/SAM2695との対応

| 項目 | 現行DinMeter | Plaits FM / 将来のDSP |
|---|---|---|
| OSC素材 | GM ROM音色をMIDIチャンネルで積層（既存知識001） | 正弦波の位相を音声レートで演算し側帯波を生成 |
| 音色変化 | 内蔵TVF/ENV等をMIDIで制御（近似可能な範囲） | FM index/ratio/feedbackを音声レートで変える |
| 各音独立 | MIDIチャンネル設定と個々の内部Voice状態は非等価 | 各VoiceがFMEngineの位相・帰還状態を所有すれば独立 |
| 音質対策 | ROMサンプル内部生成は直接変更不可 | oversampling、高音域index制限、downsamplingを設計可能 |
| 実現可能性 | **外部GMレイヤーの加算だけで位相変調FMは再現できない** | ESP32-S3/別DSP MCU+DACで候補。性能未検証 |

SAM2695にFM波形が全く存在しないと断定していない。GMプリセットの一部にFM由来のサンプルがあっても、**キャリア位相へのリアルタイム演算**を外部MIDIから直接実現することとは異なる。

## 5. 反証・比較実験（未実施）
1. **本当に4倍が必要か**: 1倍/2倍/4倍で同じ音量・比率・index・音程を固定し、FFTでナイキスト周辺の折り返しとCPU負荷を比較。高音のindex制限が既に強ければ4倍の利得は小さい可能性。
2. **音作りの代償**: 高音域でhf_taming ON/OFFを比較し、音色の明るさと折り返しノイズの両方を測定。「高音で鈍る」という副作用を確認。
3. **CPU予算**: ESP32-S3でUSB Host/UI/OSC/Filter/FXと共存できるか、別DSP基板との同一プリセット・発音数・遅延を測る。現時点で数値なし。
4. **制御スムージング**: TIMBRE/MORPHを連続操作した時のクリック・ジッタ・更新レートを測定。MIDI 31.25kbps制御と内部DSP制御の違いを定量化。
5. **ライセンス**: 直接移植するなら `fm_engine.cc/.h` 以外の `stmlib`、downsampler、lookup tablesのライセンスを個別確認。

## 再利用できる設計原理
**音声レートで相互変調する方式は、GMチャンネルの加算とは別の音作りの自由度を持つ。** 安価なDSP音源を比較する際は「FMあり」だけでなく、位相/周波数のどちらを変調するか、変調比、帰還経路、aliasing対策、CPU負荷、各音独立の状態管理まで評価する。現時点では部品採用やDinMeterのコード変更を推奨しない。
