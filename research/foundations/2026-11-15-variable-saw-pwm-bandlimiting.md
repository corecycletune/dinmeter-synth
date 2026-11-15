# 第6回｜波形を変形するOSC：Plaits Variable SawとDaisySP PolyBLEP

調査日: 2026-11-15 JST  
分類: 発振器 / 波形・PWM / エイリアシング / DSP移植判断  
確認レベル: **公開実装本体を精読**（下記の関数）。実機試験・音響測定・CPU負荷測定なし。対象は公開ソースの現行取得版であり、オリジナルアナログ名機の回路と同一とは主張しない。

## 今回の新しい知見

「Saw」「Square」等の**波形名だけでは、音質・PWMの自由度・高音域の挙動は決まらない**。サンプル値を素朴に生成するOSCと、波形の不連続点を補正するOSCを分けて比較すべき。さらにPWM幅は単に0–100%を許すのではなく、音程（1サンプル当たりの位相進行）に応じて制約する実装がある。

## 原典で確認した実装

### 1. DaisySP Oscillator

- [oscillator.h](https://github.com/daisyaudio/DaisySP/blob/master/Source/Synthesis/oscillator.h): `WAVE_SAW`/`WAVE_SQUARE`と`WAVE_POLYBLEP_SAW`/`WAVE_POLYBLEP_SQUARE`を別の列挙値で定義。`SetPw`は0〜1へclampする。`Process()`は1サンプルごとに呼ぶ。`PhaseAdd()`と`Reset()`は位相を直接操作できる。
- [oscillator.cpp](https://github.com/daisyaudio/DaisySP/blob/master/Source/Synthesis/oscillator.cpp): `Oscillator::Process()`の素朴なSAW/SQUAREは位相から直接生成。PolyBLEP SAWは周期境界の補正、PolyBLEP SQUAREは位相0とパルス幅位置の**2つのエッジ**に補正を加える。`WAVE_POLYBLEP_TRI`は2エッジの補正後、leaky integratorを使う。`Polyblep()`はエッジ近傍でのみ非ゼロ。
- **推論**: 素朴な不連続波形は理想波形の高次倍音がNyquistを超えると折り返し成分を生む。PolyBLEPはエッジ近傍を補正してこれを低減するが、全条件で完全に除去するわけではない。
- **注意**: この実装の`SetPw`は端値0/1も許容する。高音での可聴特性、DC、エッジが重なる場合の品質は別途検証が必要。

### 2. Mutable Instruments Plaits VariableSawOscillator

- [variable_saw_oscillator.h](https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/oscillator/variable_saw_oscillator.h): `VariableSawOscillator::Render(frequency,pw,waveshape,out,size)`を精読。`frequency`はサンプル当たりの位相進行量（正規化周波数）。`pw`は折れ点/パルス幅、`waveshape`はnotch-sawとtriangleの間の連続変形。
- 周波数が0.25以上なら`pw=0.5`に固定し、それ未満でも`frequency*2 <= pw <= 1-frequency*2`に制約。これは**高音ほど極端な幅を許さない**という実装事実。目的が常に「アンチエイリアスだけ」かは原典に説明がないため断定しない。
- `ParameterInterpolator`でfrequency、pw、waveshapeをブロック内のサンプル間で補間。急なつまみ変更でパラメータが飛ぶことを抑える設計。
- `ComputeNaiveSample`で折れ線形状を生成し、エッジと傾きの変化点に`ThisBlepSample`/`NextBlepSample`と積分BLEPを加える。`next_sample_`に次サンプル分の補正を持ち越す。
- [stmlib/dsp/polyblep.h](https://github.com/pichenettes/stmlib/blob/master/dsp/polyblep.h): `ThisBlepSample`/`NextBlepSample`と積分版の多項式実装を確認。元の音声経路ではなく**波形生成中のエッジ補正**である。

## 音声経路・制御経路・ボイス単位

音声経路（DSP構成例）: **OSC位相生成 → Naive波形 → エッジ/傾き補正 → OSC MIX → VCF → VCA → DAC**。

制御経路: **MIDI note/pitch bend → 周波数**、**ノブまたはLFO → pw/waveshape**、**ENV → VCF/VCA**。DaisySPのOscillatorやPlaitsのVariableSawは波形生成モジュールであり、VCF/VCAや音声ごとのENVを自動で提供するものではない。

OSCインスタンスが`phase_`等の内部状態を保持する。複数ボイスに独立した位相/PWMを持たせたい場合は、ボイスごとにインスタンスまたは等価な状態が必要。全ボイスで1つのLFOを共有する設計も可能だが、それは**OSCの仕様ではなく上位のvoice managerの選択**。

## 音楽的意味と反証

- PWMはパルス幅を時間変化させることで倍音構成を変える。複数OSCのデチューンとは異なる音色変化を、**単一OSCの波形変形**から得られる。
- VariableSawは単なるSquare PWMではなく、折れ点とnotch/triangleの比率を動かす。LFOで`pw`を動かしたとき、音色が変わるが**音量/DCや高音域の挙動**も変わりうる。
- **反証**: 「PolyBLEPだからアナログ音になる」は誤り。補正は主に不連続点のデジタル折り返しを抑える技術で、アナログVCFの飽和、VCOドリフト、音声ごとのENVは別問題。
- **反証**: 「PWMをSAM2695へMIDIで送れば同じ効果」は根拠なし。チップ内部のサンプル波形のパルス幅を直接変えられる公開機能を確認していない。

## 現行DinMeterとの対応

| 項目 | SAM2695 + ESP32現行 | このOSC方式 |
|---|---|---|
| 波形素材 | GM/ROM音色をOSCレイヤーに割り当てる | サンプル単位で位相と波形を計算 |
| PWM・可変Saw | **現行機能として未確認**（MIDIで音色変更は可能） | pw/waveshapeを連続制御可能 |
| 高音域の折り返し | SAM2695の内蔵波形実装は非公開・不明 | DaisySP PolyBLEP / Plaits BLEPで補正 |
| OSC内部状態 | チップ内部、ESP32から直接操作できない | OSCインスタンスのphase等を制御可能 |
| ENV/VCF | SAM内蔵処理 + ESP32 MOD制御 | DSPに別途実装が必要 |
| 追加ハード | 既存構成でGM制御可能 | ESP32-S3/Pico 2/Daisy等のDSP + DAC/Codecが必要 |

**設計判断（仮説）**: 新しいDSP音源を比較するときは「波形の種類」だけでなく、**高音域のalias、PWM端値制限、音程変更時の位相連続性、パラメータ補間、CPU負荷**を評価項目に加える。既存ESP32-S3に実装可能かはCPU/PSRAM/I2S/USB Host同時処理の検証前には結論を出さない。

## 検証計画（すべて未実施）

1. 48kHz等でnaive saw/square、DaisySP PolyBLEP、Plaits VariableSawを同じ基音・音量で録音し、低音/高音のスペクトルとNyquist折り返し成分を比較する。音量差を補正して聴感比較。
2. PWM幅を0.5→0.05→0.95へ変え、特に高音域で制限の有無による音量・DC・ノイズを比較する。
3. `pw`/frequency/waveshapeをブロックごとに急変更し、補間の有無でクリック/zipper noiseがどう変わるか比較する。
4. 1/4/8/16ボイス、1/3 OSCでCPU使用率、音声期限超過、RAM、発音遅延を測定する。**性能値は未取得**。
5. ライセンス: DaisySPはMIT、Plaits該当ファイルとstmlib/polyblep.hはMIT文面を確認。採用時は依存ファイル・通知義務・バージョンを再確認。

## 調査範囲の限界

今回はDaisySPの`oscillator.cpp/.h`、Plaitsの`variable_saw_oscillator.h`、stmlibの`polyblep.h`を実際に読んだ。Plaits全体の音声エンジン・実機ハード・公式マニュアルは今回未読。これらのコードをDinMeterに移植・ビルド・実機試験したわけではない。
