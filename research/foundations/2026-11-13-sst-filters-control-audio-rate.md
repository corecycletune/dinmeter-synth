# 2026-11-13｜Surge SST Filters：音声レートと制御レートを分離する理由

分類：VCF / フィルター設計 / ボイス独立性 / DSP移植性
確認レベル：**sst-filters の README と公開ヘッダ `include/sst/filters++/api.h` を実際に読んだ**。内部DSPの個々のフィルター実装・音響特性・DinMeter実機は未検証。

## 何を学ぶか

前回のコーラスは音声合成後の可変遅延だった。今回は**音声経路の途中に入るVCFを、サンプル処理と係数更新に分離する設計**を扱う。フィルターは単にcutoffとresonanceを持つノブではない。

## 1. 確認できた実装上の事実

一次資料：[SST Filters README](https://github.com/surge-synthesizer/sst-filters/blob/main/README.md)、[フィルターAPIヘッダ](https://github.com/surge-synthesizer/sst-filters/blob/main/include/sst/filters%2B%2B/api.h)。

- SST FiltersはSurge由来のLPF/HPF/BPF/Notch、Legacy/Vintage Ladder、OB-Xd、K35、Diode Ladder等の**異なるモデル**を公開。これは「すべて同じ音」との主張ではなく、複数の異なるフィルター設計を選択可能という事実。
- APIは`FilterModel`に加えてpassband、slope、drive、submodel等を設定できる。**cutoff/resonanceだけではモデルの個性を記述しきれない**。
- ヘッダの例は`setSampleRateAndBlockSize(48000,16)`を使い、16サンプルごとに各voiceの`makeCoefficients(voice,cutoff,resonance)`→`prepareBlock()`、各サンプルで`processSample()`、末尾で`concludeBlock()`という二段階処理。**16は例示値であり普遍的な推奨ブロック長ではない**。
- `freezeCoefficientsFor(voice)`は変更のないvoiceの係数再計算を避けるためのAPI。ヘッダには、同じ係数を使うvoice間で`copyCoefficientsFromVoiceToVoice(from,to)`しても、各voiceの内部レジスタ（フィルター状態）は独立と明記される。
- 4-wide SIMDで複数voiceをまとめて処理する設計。READMEは**raw SSE SIMD intrinsics**の利用と、SSEのない環境では`simde`を併用する推奨を明記。
- ライセンスは**GPL-3**。コードの直接移植や静的組み込みを検討する場合、配布形態との整合を個別確認する。

## 2. 音声経路と制御経路

音声経路（1voice）：

`OSC → [Filter(model, cutoff, resonance, drive; 内部状態を保持)] → VCA → Voice mix`

制御経路：

`Key pitch + Filter ENV + LFO + Knob + Velocity → cutoff/resonance/driveの目標値 → 係数更新（制御ブロック）`

ここで**係数**はフィルターの特性を決める値、**内部状態**は過去のサンプルの記憶。2voiceのcutoffが同じでも、過去に違う波形を処理していれば内部状態は違う。係数の共有と音声状態の共有を混同しない。

## 3. 数値で理解する制御レート

48 kHz、16サンプル/ブロックの例なら係数更新の機会は最大**3,000回/秒**（48,000÷16）。音声サンプルは依然として**48,000回/秒**処理される。ブロック間で係数を補間する方式なら急激なパラメータ変化を緩和できるが、具体的な補間方式・滑らかさはモデル実装の精読が必要。

**重要な反証**：3,000 Hzの制御レートが必要だという意味ではない。音楽的なcutoffノブはより低い更新頻度でも十分なことがある。一方、フィルターをオーディオレートで変調する場合はブロック更新だけでは同じ効果を得られない可能性がある。

## 4. DinMeterとの対応と違い

| 論点 | 現行DinMeter（既存SAMレビュー） | SST Filtersから得る設計原理 |
|---|---|---|
| 音声処理 | SAM2695内部でTVFを処理。ESP32はMIDI/GS/NRPNを送る | 自前DSPなら**毎サンプル**音声を処理し、制御係数更新は別レートにできる |
| フィルター種類 | SAM内蔵TVFの制御を実装。任意のフィルター形式を選べる証拠はない | モデル、傾斜、drive、非線形性を**明示的に区別**して選定 |
| ENV/LFO | ESP32側のMOD出力は最短32ms間隔（最大約31回/秒） | SST例のブロック更新は3,000回/秒。**用途と計算場所が違うので単純な優劣比較は不可** |
| 3OSCとポリ | 3つのMIDIチャンネルで音色を重ねる | 1音ごとのフィルター内部状態は独立。複数OSCを同じvoiceのフィルターに混ぜるかも設計事項 |
| 移植性 | ESP32-S3 | SSTはSSE/4-wide SIMD前提の部分あり。**そのまま軽量ESP32へ移植できると考えない** |
| ライセンス | DinMeterの公開条件は別途確認 | SSTはGPL-3。直接組み込みは法的・技術的な別評価が必要 |

## 5. 音楽的な意味・設計仮説

- **同じSaw波でも、フィルターのモデル・drive・resonanceが違えば音色形成が変わり得る**。ただしSST各モデルの聴感優位性は今回測っていない。
- **各voiceの独立フィルター状態**を保つと、異なる時刻に弾いた音のFilter ENVを別々に走らせられる。和音の各音が独自に明るくなったり暗くなったりする設計につながる。
- フィルターのモデル選択を単なる名前ではなく、周波数応答・レゾナンス・飽和・CPUコストで比較するべき。
- **DinMeterでの直接再現は不明**。SAMのTVFに外部から自由な係数やフィルター状態を注入できる根拠はない。新規DSP音源か、SAM音声出力をADC/Codecに取り込む後段DSPが候補。ただし後段DSPは**すでに混ざった全音に一括処理**となり、voiceごとのVCFとは異なる。

## 6. 反証・制約・今後の実験

1. SSTの多数のフィルターモデルがあることは、DinMeterに多種類のフィルターが必要という証明ではない。まず必要な音色を定義する。
2. 4-wide SIMDはx86では有効でも、ESP32-S3では互換層の負荷が高くなる可能性がある。**simdeでビルド可能＝十分高速、ではない**。
3. `makeCoefficients`等のAPIが存在することと、内部フィルターがどのような数値積分・飽和・補間を行うかは別。個別実装のコード精読は次回以降。
4. 検証案（未実施）：同一Saw/同一音量でLPFモデルを比較。cutoff sweep、共振、drive、CPU負荷、折り返しノイズ、発音数、パラメータ更新時のクリックを録音・測定する。
5. SAM2695の内蔵TVFと比較する場合は**音量一致**、同じ入力素材、同じ音域で評価し、「音が大きい＝太い」を避ける。
6. ライセンスを含め、現時点で**SSTのコードをDinMeterへ採用する推奨はしない**。設計原理と評価軸を引き出しに保存する。

## 再利用する判断

**DSPを選ぶときは、音声サンプル処理・制御係数更新・voiceごとのフィルター状態・CPU最適化・ライセンスを分けて評価する。** 特に「ESP32で動く」だけでは音作りの品質・演奏可能な発音数は分からない。

関連：[名機設計比較](2026-11-10-classic-synth-architectures.md) / [AMY DSP設計](2026-11-11-amy-dsp-architecture.md) / [SAM2695現行比較](../2026-11-10-sam2695-design-review.md)
