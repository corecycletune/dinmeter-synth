# 2026-11-14｜ADSRの「時間」と「傾き」は別：指数カーブ、Constant Rate、再トリガ

分類：ENV / 音楽的ノブマッピング / ボイス管理  
確認レベル：**EarLevel Engineeringの原理解説・公開API・著者の補足を確認**。DaisySP ADSR本体の関数は未読（GitHubのファイル取得失敗）。DinMeter実機・録音未検証。  
対象：歴史的なRC式ENVの原理をデジタルで再構成する方法。特定のMinimoog/SH-101実機回路を検証したものではない。

## 1. 今日の新しい原理

**同じADSRのA/D/S/R数値でも、段のカーブ、時間の定義、再トリガ時の初期値が違えば別の音になる。** 「ADSRを実装した」だけではアナログシンセの音量変化やフィルター変化を再現できない。

一次的な設計資料：
- [Nigel Redmon, Envelope generators—ADSR Part 1](https://www.earlevel.com/main/2013/06/01/envelope-generators/)：RC回路の指数的な応答、gateとの関係。
- [Part 2](https://www.earlevel.com/main/2013/06/02/envelope-generators-adsr-part-2/)：constant rateとconstant time、target ratioによる曲線変更、係数の計算。
- [公開コードの説明/API](https://www.earlevel.com/main/2013/06/03/envelope-generators-adsr-code/)：`process`, `gate`, `setAttackRate`, `setDecayRate`, `setReleaseRate`, `setSustainLevel`, `setTargetRatioA`, `setTargetRatioDR`、状態列挙。実際のダウンロード済みソース本体を全行精読したわけではない。

## 2. 音声経路と制御経路

音声：`OSC → MIXER → VCF → VCA → OUTPUT`

制御：
- `Gate/NoteOn/NoteOff → AMP ENV → VCAゲイン`：音量の立上り・減衰・保持・余韻を作る。
- `Gate/NoteOn/NoteOff → FILTER ENV → VCF cutoff`：音量を一定に保ちながら音の明るさを時間変化させられる。
- 同じENV信号を両方に配ることもできるが、音色表現の自由度は減る。
- ポリシンセでは**音声ごとにENVの状態と開始時刻**を持つか、全体共通かが設計上の分岐。既存のProphet-5比較を参照。

## 3. 指数カーブとパラメータの定義

RC式に由来する近似は、現在値を目標へ少しずつ近づける更新で作れる。Redmonは、指数応答が理論上は目標に漸近して到達しない問題に対し、目標を少し行き過ぎた値に設定し、閾値到達で次段へ切り替える方式を解説している。

係数の一例：`coef = exp(-log((1+targetRatio)/targetRatio)/samples)`。この式は**選んだ正規化と定義**に対応するもので、すべてのアナログENVの普遍的な公式ではない。

- 小さいtarget ratio：指数的な曲線に近い。
- 大きいtarget ratio：線形に近づく。
- 係数計算をノブ変更時に行い、音声処理側では加減乗算中心にする。
- **Constant rate**：Decay/Releaseの傾きの速さを基準にする。Releaseを「最大レベル1から0へ落ちる時間」で指定した場合、実際に0.5から離すとより短くなる。
- **Constant time**：開始値にかかわらず指定時間で終点に着く設計。どちらが正しいかではなく、楽器の演奏挙動をどう定義するか。

**反証**：指数カーブが常に聴感上優れるとは言えない。音量VCAの応答（線形/指数）、元のPCM素材のENV、フィルターの周波数変換によって、同じ制御カーブでも音の変化は異なる。Attackは指数減衰を反転した形だけが正解ではない。

## 4. 音楽的に重要な「再トリガ」

鍵盤を重ねたり、LEGATO/GLIDE中に新しいノートを弾いた時に、
1. ENVをゼロから再スタートするか、
2. 現在レベルから再スタートするか、
3. LEGATOではENVを再トリガしないか、
という**別の仕様**がありうる。

どの方式が名機に採用されているかはモデル・版ごとにマニュアル/回路で確認する必要がある。再トリガを変えるだけで、短い連打のアタック感やポルタメント時のつながりが変わる可能性がある。

## 5. 現行DinMeterとの違いと制約

[現行SAM2695レビュー](../2026-11-10-sam2695-design-review.md)で確認済みの構成：
- SAM2695のROM音色は**チップ内部のENV**を持ち、DinMeterはADR設定を送る。
- ESP32上に**別のMOD ENV**があり、フィルター等のMIDIパラメータを変える。動的MOD出力は最短32ms間隔。両者は同一ではない。
- 3 OSCレイヤーはMIDIチャンネルを使う構成で、各押鍵に独立したDSP ENVがあるわけではない。
- MONO/LEGATO/GLIDEはESP32側でノート管理する。ENVの再トリガとノートの切替は設計上連動するが、**現行の具体的な再トリガ分岐は今回ソースを精読していないため断定しない**。

| 実現したいこと | 判定 |
|---|---|
| チップ内蔵音色のADRを変える | **実装済み**。ただし曲線形状やSustain等の自由度は要確認 |
| ESP32側のMOD ENVを32msごとに送る | **実装済み**。滑らかさ・帯域・音色への作用は実測なし |
| ノブに非線形時間マッピングを使う | **実装可能性あり**。現在のマッピングと実測範囲を先に調べる |
| 1音ごとに独立したAMP/FILTER ENVを持つ | **現行MIDI制御だけで同等の実現は未確認**。新規DSP音源なら設計可能 |
| SAM2695のPCM音声に音声レートで独自AMP ENVを掛ける | **直接操作する方法は未確認**。全体音声後段のDSPではボイスごとの処理と異なる |

## 6. 実験案と反証

- まず独立したPC上の小さなENVシミュレータで、linear / exponential、constant rate / constant time、再トリガ3方式を比較する（**未実施**）。
- 同じ波形・同じ最終音量・同じ鍵盤イベントで、ENV形状だけ変えた録音を比較する。PCM音色を使うなら元音色の内蔵ENVの影響を明記する。
- 同じRelease設定でも、Sustain=1.0と0.5でReleaseの実時間が変わるか測る。変わらなければその実装は上記constant-rate方式ではない可能性。
- レガート中に音量が再アタックするかを録音で確認。NoteOff欠落やPANICは別原因の可能性があるため混同しない。
- ENV更新を速くすれば必ず音が良くなるという仮説は誤り。必要な変調速度・MIDI帯域・音声チップ内部処理を分けて評価する。

## 7. 再利用する設計原理

**ENV仕様は「段の長さ」だけでなく、曲線、到達判定、constant rate/time、現在値からの再トリガ、ボイス所有権、適用先の応答をセットで決める。** とくに今後の別DSP音源では、AMP ENVとFILTER ENVを分け、各voiceの状態を独立にすることが重要な設計候補。

### 資料の限界
EarLevelは設計者の公開解説・コードであり、特定の歴史的機種の一次回路図ではない。DaisySPの`adsr.cpp`と`adsr.h`は今回GitHub connectorで取得できず、実装の中身を読んだとは記載しない。DaisySP全体がADSRを提供することは[公式README](https://github.com/daisyaudio/DaisySP)で確認。ライセンスはDaisySP本体とサブモジュールを別々に確認すること。
