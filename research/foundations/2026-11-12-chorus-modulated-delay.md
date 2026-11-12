# 2026-11-12｜JUNO系コーラスを「時間変化する短い遅延」として理解する

分類：エフェクト / 音声経路 / 制御経路 / 実装比較
確認範囲：RolandのJUNO-60 CHORUS **ソフトウェア版**公式マニュアル、DaisySPの実コード（`daisyaudio/DaisySP` master、2026-11-12閲覧）、DinMeterの`SynthApp.cpp`。実機・聴感・CPU測定は**未実施**。

## 原典と確認レベル

- [Roland公式：JUNO-60 CHORUSソフトウェア版のマニュアル](https://www.roland.com/jp/support/by_product/rc_juno-60_chorus/owners_manuals/) — I/II/I+II、Noise Level、Dry/Wet Mix、Output Levelを説明。**1980年代の実機の回路・BBD段数・正確なLFO位相はこの資料では確認できない**。ソフト版の仕様をオリジナル実機へそのまま帰属しない。
- [DaisySP Chorus実装](https://github.com/daisyaudio/DaisySP/blob/master/Source/Effects/chorus.cpp) と [ヘッダ](https://github.com/daisyaudio/DaisySP/blob/master/Source/Effects/chorus.h) — `ChorusEngine::Process`、`ProcessLfo`、`Chorus::Process`、`SetPan`等をコードで確認。
- [DaisySP DelayLine実装](https://github.com/daisyaudio/DaisySP/blob/master/Source/Utility/delayline.h) — `SetDelay(float)`、`Read()`、`Write()`を確認。分数サンプルの遅延時間を**線形補間**で読む。
- [DaisySP Flanger](https://github.com/daisyaudio/DaisySP/blob/master/Source/Effects/flanger.cpp) — `Flanger::Process`は同様の短い可変遅延＋フィードバック。コーラス/フランジャーのアルゴリズムの近さを確認。
- [DaisySP LICENSE](https://github.com/daisyaudio/DaisySP/blob/master/LICENSE) — MIT。将来コードを流用するなら著作権表示等の条件を再確認。

## 1. 何が音を変えるか：OSCを増やすのとは別

典型的なコーラスの音声経路（DaisySPの実装に基づく）：

`OSC → MIXER → FILTER → VCA → [dry + (短い可変Delay → wet)] → stereo output`

- **音声経路**：入力音を短い遅延線に保存し、時間のずれた音と原音を混ぜる。
- **制御経路**：低周波LFOが遅延時間を連続的に動かす。遅延時間が変わることでwet側にわずかなピッチ変化が生じ、原音との干渉・広がりが変わる。
- **ボイス単位か全体か**：ここで読んだ`Chorus`クラスは**入力された合成済みの音声信号を処理**する。個々のノートを所有しないグローバルな後段エフェクトとして使用できる。DaisySP自体が全シンセのどこに挿入されるかを決めるわけではない。

**重要な区別**：3つのGM音色をデチューンして積層するDinMeterのOSCレイヤーは、コーラスの「時間変化する短い遅延」とは異なる。OSCのデチューンだけでは同じ処理にならない。後段エフェクトなら**OSC/発音数に比例してDSPインスタンスを増やさず**、合成済み信号をまとめて処理できる。

## 2. DaisySPの関数から確認した具体的な設計

`ChorusEngine::Process(in)`：
1. `ProcessLfo()`で三角波状の制御値を生成。
2. `del_.SetDelay(lfo_sig + delay_)`で遅延読み出し位置を変更。
3. `out = del_.Read()`で分数サンプル補間を含む遅延音を取得。
4. `del_.Write(in + out * feedback_)`でフィードバックを含めて遅延線へ書く。
5. `(in + out) * .5f`で原音と遅延音を混合。

`Chorus::Process(in)`は2つの`ChorusEngine`を処理し、`pan_[0], pan_[1]`の重みで左右に振り分け、`GetLeft/Right`で取り出せる。

- `SetDelay(float)`の**実コード**は`0.1 + 7.9*value`なので正規化入力0〜1を**0.1〜8ms**に対応させる。**ヘッダの説明「0.1〜50ms」は実装と不一致**。正規化パラメータの実際の範囲はコードを優先して判断する。
- ヘッダでは`kDelayLength=2400`（48kHzで50ms分のメモリ）と宣言。**50msのバッファがあることと、`SetDelay(float)`が50msまで使うことは別**。`SetDelayMs`を直接呼ぶ場合は上限を明示的に検査しておらず、`DelayLine::SetDelay(float)`の配列長クランプが最後の安全網になる。sample_rateが48kHz以外なら「50ms」の実時間も変わる。
- `SetLfoDepth`は0〜0.93に制限。LFO振幅は基本遅延時間に比例し、負の遅延位置を避ける設計意図が読み取れる。
- `SetFeedback`は0〜1の範囲。0以外なら単純な1回のwetコピーではなく過去の遅延出力が再循環する。フィードバックが大きい場合の音量・発振挙動は実測が必要。
- `Chorus::Init`は2エンジンを同じ初期LFO位相/速度で初期化し、パンを0.25/0.75にする。**デフォルトで左右のLFOが逆位相という根拠はない**。ステレオ差は少なくともパンの重みから生まれる。真の位相差をつけるなら追加実装・設定が必要。

## 3. 音楽的な意味

- 緩やかなLFO＋短い遅延＋Dry/Wet混合：持続音・パッドに広がりや動きを与える。
- LFOを止める：一定の短い遅延による干渉（comb filtering）が中心となり、通常の動くコーラスとは異なる。
- Dryをなくしてwetだけにする：時間変化する遅延によるピッチ揺れが目立ち、ビブラートに近づく（DaisySPの`ChorusEngine`はdry/wetを固定1:1で返すため、wet-onlyには外側のミキサー設計が必要）。
- フィードバックを増やす：共振やフランジャー的な音が強くなる可能性。ただし音色の評価は未実施。
- Rolandソフト版のI/II/I+IIはメーカーが異なる効果モードとして提供することを確認したが、**各モードの正確なLFO速度・遅延時間はこの資料では特定していない**。

## 4. DinMeter/SAM2695との違い

[現行SynthApp.cpp](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp) で確認：

- `configureLegacyEffects()`（約1433行）は`sendNRPN(...,0x37,0x5F,...)`で既存チップ側のchorus/spatialを設定。
- `applyReverbAll()`（約1826行）は`synth.setReverb()`をOSC1〜3に送る。
- `applyCurrentPresetToSynth()`（約2180行）は`legacyChorusSend > 0`で旧chorusルーティングを有効化。
- これらは**SAM2695内部エフェクトのMIDIパラメータ制御**であり、ESP32がオーディオサンプルを読み書きして可変遅延を計算するものではない。

| 目的 | 現行SAM2695 | DSP音源追加/音声後段DSP |
|---|---|---|
| コーラスを有効化する | 既存NRPN経由で制御あり | 実装可能 |
| delay timeを1サンプル単位で動かす | 外部から内部DSPバッファへアクセスできる根拠なし | 自前のdelay lineなら可能 |
| ステレオ位相・左右パン・dry/wetを任意に制御 | 現行コードの該当制御は限定的。チップ仕様の全能力は要確認 | DSPで設計可能 |
| 既存GM音色全体にDaisySP型コーラスをかける | SAM2695の内蔵FXなら既存音声経路で可能 | **SAMのアナログ音声をADC/Codecへ入力してDSP処理**が必要。DAC単体では不可 |
| 新DSPで合成した音だけにかける | SAM側からは不可 | 同じDSP内で音声合成→コーラス処理可能 |

## 5. 反証・未確認

1. **「JUNOの太さはコーラスだけで決まる」は誤り**。OSC/DCO、VCF/VCA、ENV、演奏、出力レベルなどの差もある。コーラスの寄与は同音量A/B比較で評価すべき。
2. **「ステレオだから必ず左右逆位相」は誤り**。今回のDaisySP初期化ではLFOは同相。パン重みとLFO位相は別の設計要素。
3. **「DaisySPのchorusを使えば実機JUNOのBBD音が再現できる」は未立証**。今回の実装は浮動小数点の可変delay lineであり、BBD段数、クロック漏れ、帯域制限、ノイズなどの実機特性を再現するコードは確認していない。
4. **「コーラスを増やすほど良い音になる」は未検証**。音量・位相干渉・モノ互換性の問題がある。
5. `ChorusEngine::Process`は音声サンプル単位の処理であり、SAM2695のMIDI更新間隔（約32ms）で同じアルゴリズムを外部から再現できない。
6. 実機でのCPU負荷・RAM・レイテンシー・ノイズ・効果音は未測定。DaisySPの実装がDinMeterのマイコンにそのまま載ることは未確認。

## 6. 将来の試験計画（未実施）

- 単音持続音でFX OFF / SAM内蔵Chorus ON / 外部DSP Chorus ONを録音。音量を合わせ、時間波形、左右相関、モノ合算、周波数スペクトル、CPU/レイテンシーを比較。
- DaisySPの正規化Delayと`SetDelayMs`を個別テストし、遅延範囲・クランプを確認。
- LFO同相と逆相、panだけの差、dry/wet比率、feedbackを一変数ずつ変えて聴感比較。
- SAMの音声を外部DSPへ入れる場合は、DACだけでなく**ADC/Codec**を含む基板構成・電源・配線・総額を見積もる。
- サンプルレート変更時の`kDelayLength`固定バッファの時間長を確認。

## 取り出すべき設計原理

**「音声の厚みを増す」には、OSCの複数化だけでなく、後段の時間変化する短い遅延という独立した手段がある。** 音声経路のどこにFXを挿入するかで、必要なハード（DACだけか、ADC/Codecも必要か）と演算負荷が変わる。

これは将来のDSP音源選定や、現行SAM2695の内蔵FXの使い方を判断する材料になる。今回の結果だけで部品購入・ファームウェア変更は推奨しない。
