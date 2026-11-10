# 引き出し 001｜GM ROM音色を「OSC素材」にする

作成: 2026-11-10  
分類: 音作り / アーキテクチャ  
段階: **実装済みの設計整理＋音響評価待ち**

## 何に使える？

「独自のDSPオシレーターを作らず、GMチップの既存サンプルをアナログシンセ風の素材として使う」方式を、音色改善や新しいプリセット検討の際に再利用する。

## 成立していること（コード確認済み）

- [DinMeter WAVE_TABLE](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L375) は既存bank/programの組み合わせから8種の素材を選ぶ。表示上のSQR/SAW等は音色素材の名前であり、リアルタイムで数式から生成する波形ではない。
- [applyOscStatic](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L1920) はチャンネルごとのバンク・プログラム、デチューン、パン、レベルなどを設定する。
- [sendLayeredNote](https://github.com/corecycletune/dinmeter-synth/blob/main/SynthApp.cpp#L2130) は3つのOSCチャンネルと独立したGMレイヤーにノートを出す。各OSCは独立してtranspose等を持てる。
- 公式の[M5-SAM2695ライブラリ](https://github.com/m5stack/M5-SAM2695/blob/main/src/M5_SAM2695.cpp)はProgram Change、TVF/ENV/vibratoなど基本的なチップ制御を提供する。DinMeterの「3OSCレイヤー」という構造はこの薄いラッパーより上位に実装される。

## 設計原理として残す

1. **素材選択**: 倍音が多く、音程ごとに音色の変化が少ない既存音色を選ぶ。名前にSAW/SQRがあるだけで理想的な鋸波・矩形波と判断しない。
2. **積層**: 同一ノートを別チャンネルに振り分け、音色・デチューン・オクターブ・レベルを変える。代償として発音数とMIDI送信量が増える。
3. **音色形成**: ROM由来の内蔵波形と内蔵ENVに対し、TVFと外部制御の変調を重ねる。完全なアナログVCF/VCAの自由度は期待できない。
4. **演奏可能性**: パラメータをリアルタイムで操作してもNoteOff/Mono/Pitch Bendに矛盾が出ないことが前提。音色機能の追加はノート管理と切り離して評価。

## 反証・制約

- GM楽器音はサンプルのループ・複数サンプルの切替・固有のエンベロープを含むことがあり、全鍵域で均質な「発振器」とは限らない。**これは一般的なROMplerの制約からの推論**で、採用8素材の個別実測は未実施。
- 3チャンネルを使ってもチップのサンプルそのものや合成方式を改造できるわけではない。
- 独自DSP波形発生方式（例: Daisy等）のソースを、そのままSAM2695に移植できるとは限らない。

## 将来の実験メニュー（今は実施しない）

- 各WAVE_TABLE素材を同じ音域・同じ音量・同じENVで単音録音し、FFT・倍音減衰・ループ・鍵域差を比較。
- OSCを一つずつ増やして厚み・位相感・音量差・発音数への影響を比較。
- Cutoffの変化量を素材別に比較し「実際にフィルターが効きやすい素材」をランキング化。

## 参考

- [公式M5-SAM2695](https://github.com/m5stack/M5-SAM2695)
- [DinMeter設計比較・詳細ノート](../2026-11-10-sam2695-design-review.md)
- [SAM2695データシート](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/unit/Unit-Synth/SAM2695.pdf)

**取り出す場面**: プリセット作成、音の太さの改善、OSC素材の追加、フィルターの効き方の診断。
