# シンセ設計の基礎資料室（名機・方式横断）

初版: 2026-11-10  
目的: DinMeterで困ったときに検索するだけでなく、**過去の楽器がどう音を作ったか・なぜそう設計されたか**を継続的に学び、そこから新しい機能を発想できるようにする。

## この資料室の位置づけ

- `research/knowledge/` = **すぐ使う技術の引き出し**（DinMeterのコードや実験との接続が主）。
- `research/foundations/` = **原理の図書室**（まだDinMeterに関係しない設計でも保管する）。
- `research/2026-11-10-sam2695-design-review.md` = 現在のSAM2695実装と先行事例の詳細レビュー。

「今回すぐ使えない＝不要」ではない。一方で、楽器のカタログ情報や未読マニュアルを無限に列挙せず、資料ごとに独自の**理解・比較・再利用可能な設計原理**を一つ以上残す。

## まず押さえる系譜と論点

| 分類 | 主な調査対象 | 何を理解するか |
|---|---|---|
| 単音・減算合成 | Minimoog Model D、Roland SH-101 | VCO→Mixer→VCF→VCA、サブOSC、GLIDE、トリガ |
| セミモジュラー | KORG MS-20、ARP 2600 | 音声経路と制御経路を分ける、HPF/LPF、パッチング |
| ポリフォニック・アナログ | Sequential Prophet-5、Roland Juno、Oberheim系 | 1音ごとの発音/フィルター/ENV、ユニゾン、音声割当 |
| 波形・変調の発展 | PWM、Osc Sync、Ring Mod、FM、Wavetable | 波形そのものを変える方法と、音源後段で加工する方法の違い |
| 演奏表現 | Mono / Legato / Portamento / Aftertouch / Velocity | 音程の遷移、ENVの再トリガ、ノート優先順位 |
| モダンな設計 | Surge XT、VCV Rack、オープンソースDSP | Voice単位/Global単位の変調、柔軟なMOD行列、リアルタイムの責務分離 |
| エフェクト・空間 | BBDディレイ、アナログコーラス、テープエコー、リバーブ | モジュレーションの遅延・帯域・ノイズ・フィードバック |
| 音の分析 | 波形、倍音、周波数特性、録音実験 | 形容詞ではなく測定や聴感の条件をそろえ、仮説を反証する |

## 第1回の基礎比較

- [2026-11-10｜3つの名機＋Surge XTから学ぶ基本設計](2026-11-10-classic-synth-architectures.md)  
  Minimoog Model D / KORG MS-20 / Prophet-5 / Surge XT を、音声経路・変調・DinMeterへの応用可能性の軸で比較。

## 第5回：ENVの曲線・時間定義・再トリガ

- [2026-11-14｜ADSRのConstant Rate/Timeと指数曲線](2026-11-14-adsr-curve-rate-retrigger.md) — EarLevelの原理・公開APIを確認。DaisySP実装本体は未読、実機未検証。

## 第4回：フィルターの音声レートと制御レート

- [2026-11-13｜SST Filters：voiceごとの状態と係数更新の分離](2026-11-13-sst-filters-control-audio-rate.md) — 4-wide SIMD、制御ブロック、サンプル処理、GPL-3、ESP32への移植制約。ヘッダ/API確認、実機未検証。

## 第3回：コーラスと短い可変遅延

- [2026-11-12｜DaisySP実装から学ぶコーラスとJUNO系音作り](2026-11-12-chorus-modulated-delay.md) — 音声後段の短い可変遅延、LFO/フィードバック、ステレオ処理、DinMeter内蔵FXとの差。ヘッダの遅延範囲と実装の不一致も記録。

## 第2回：ソース精読で確認したDSP設計

- [2026-11-11｜AMYの帯域制限OSC・osc/voice/synth階層・音声スレッド設計](2026-11-11-amy-dsp-architecture.md) — `oscillators.c`、`filters.c`、`amy.c`の関数を確認。実機検証なし。

## 根拠になる一次・準一次資料

### メーカー公式資料

1. [Moog Minimoog Model D公式マニュアル（PDF・検索取得。後日アクセスとページの再検証が必要）](https://api.moogmusic.com/sites/default/files/2022-11/Minimoog_Model_D_Manual.pdf) — 3 OSC、ミキサー、フィルター、フィルターENV、ラウドネスENV、Glide。公開URLが直近の取得時に404だったため**リンク切れの可能性を明記**。
2. [KORG MS-20 オリジナル取扱説明書の公式配布ページ](https://www.korg.com/jp/support/download/product/1/67/) — 日本語のブロック図、VCO MIX→HPF→LPF→VCA、EG1/2、MG、パッチパネル。
3. [Sequential Prophet-5公式資料ページ](https://sequential.com/support/download/prophet-5-10-documentation/) / [User's Guide 1.3（PDF）](https://www.sequential.com/wp-content/uploads/2021/02/Prophet-5-Users-Guide-1.3.pdf) — Poly Mod、各音声の変調、ミキサー、フィルター、Vintage特性。
4. [Roland SH-101 ソフトウェア版・公式説明](https://www.roland.com/jp/products/rc_sh-101/) — オリジナル構造のモデル化と追加機能。**ソフトウェア版の仕様とオリジナル機の仕様を混同しない**。

### ソースコードと公開設計資料

5. [Surge XTユーザーマニュアル](https://surge-synthesizer.github.io/manual-xt/) — Voice LFO / Scene LFO、モジュレーションマトリクス、複数フィルター方式。
6. [Surge XT開発者向けArchitecture](https://github.com/surge-synthesizer/surge/blob/main/doc/Surge%20Architecture.md) / [ソース](https://github.com/surge-synthesizer/surge) — voice/DSP構造、パラメータ管理。GPL-3.0なので安易なコード移植をしない。
7. [VCV Rack Fundamental公式モジュール一覧](https://library.vcvrack.com/Fundamental) / [VCV Rackコード](https://github.com/VCVRack/Rack) — VCO、VCF、VCA、LFO、ADSRの機能分離。ホスト/各モジュールのライセンスは個別確認。

*収集原則*: メーカー原典→オープンソース実装→二次解説・動画の順に評価。映像や音色サンプルは有用だが、回路や実装の根拠にはしない。説明とコードが食い違う場合、資料の版・製造リビジョン・ソフトと実機の違いを調べる。

## 追跡する問い

- アナログらしい「太さ」はどこで生まれるのか：波形の倍音、複数OSCのデチューン、フィルター非線形、飽和、エンベロープ、揺らぎ、エフェクトの寄与を分ける。
- 同じ`ENV`でも、**音量そのもの**を作るENV、**フィルター**のcutoffを動かすENV、自由な**変調**ENVはどう違うか。
- ポリフォニックの「各音に独立したVCO/VCF/VCA/ENV」が、SAM2695の**MIDIチャンネル別の設定**とどう違うか。
- OSCを重ねることと、ミキサー段で歪ませること、フィルターを自己発振させることの音響的な差は何か。
- MIDIメッセージで近似できる制御と、チップの内部音声信号にアクセスできないため再現できない処理は何か。
- UI制約の中で、名機の「触れば音が分かる」操作感だけを抽出できるか。

## 分析のテンプレート

各楽器・方式で「出典と版」「音声信号の流れ」「変調の流れ」「各音声 vs 全体」「代表的な音作り」「その方式が必要だった理由（根拠付き）」「DinMeterとの違い」「SAM2695で可能/近似/不可/不明」「反証・未検証」「後で取り出せる設計原理」を記録する。

未検証の説明を事実として書かない。元マニュアルや第三者コードを大量転載しない。回路図やコードの複製ではなく、**自分の言葉で原理を整理**する。

## 調査の優先度

1. 減算合成の基本：OSC / VCF / VCA / ENV / LFOと音声経路
2. モジュレーション設計：VCF ENV、PWM、Key tracking、Per-voice変調
3. Mono/Legato/Glide、発音割当、ユニゾン・デチューン
4. フィルター方式と非線形・ゲインステージ
5. ディレイ/コーラス/テープ由来の音作り
6. Wavetable/FM/PCM/GMと減算合成の境界

調査結果からDinMeterに使える仮説が見つかれば`research/knowledge/`へ移す。未確定のまま実装へ入らない。
