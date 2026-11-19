# 2026-11-19｜AMYの残響ネットワーク：初期反射、4本のフィードバック遅延、メモリと音の関係

分類: エフェクト / DSP実装 / 安価なMCUでの実現可能性  
成熟度: **AMY公開Cソースの関数精読＋計算上の評価**。実機試験・ベンチマークなし。  
対象: shorepine/amy の2026-11-19取得時点のデフォルトブランチ。参照時の検索結果コミット例 `9cdf615da9ed8c7de85b9c71ea96f8307be5fd70`（実際の取得が同一SHAかは未照合）。`src/delay.c`、`src/delay.h`、`src/amy.h`、`src/amy_fixedpoint.h`を読んだ。

## 今回の新しい原理：コーラスの「一本の揺れる遅延」とリバーブの「多本数・相互帰還」は違う

第3回のDaisySPコーラスは短い可変遅延をLFOで揺らす構成だった。今回は**長い残響を生成する遅延ネットワーク**を扱う。AMYの`stereo_reverb()`は、(a)6本の初期反射用遅延、(b)4本の循環遅延、(c)各循環路の1極LPFとフィードバック混合行列を持つ。

音声経路（概念）:
```
OSC/PCM → 各voiceのfilter・amp → stereo dry bus ───────────────→ 出力
                                   └→ early reflection ×6
                                        → feedback delay ×4
                                        → LPF + 4×4加減算matrix ──→ wetをdryへ加算
```
制御経路: `config_stereo_reverb(rev,liveness,crossover_hz,damping)` が帰還保持量・LPF係数・ダンピングを設定。`stereo_reverb(...,level)` のlevelがwetの混合量。これは**ボイスごとのフィルター/ENVではなく、複数ボイスの合成後に作用する共通のFX状態**である（呼び出し側の実際のバス配線は今回未読）。

### ソースで確認した事実

1. `new_delay_line(len,...)` はリングバッファ長を2の冪に限定。`DL_READ/DL_WRITE`は`(index)&(len-1)`で折り返す。割り算剰余を使わず読み書きする設計。
2. `init_stereo_reverb()` は循環遅延4本を各4096 sampleで確保、初期反射は4096,2048,2048,1024,1024,1024 sampleで確保。**総数27648 sample**。
3. `src/amy_fixedpoint.h`は`SAMPLE`を32bit整数のs8.23固定小数点として説明する。したがって遅延バッファの生サンプル領域だけで **27648×4=110592 bytes（108 KiB）**。構造体、状態、ヒープ管理、他のOSC/FX/USB/UI用RAMは含まない。
4. `stereo_reverb()` は6本の初期反射遅延を経由し、4本の帰還遅延の出力にLPFをかけて加減算行列で次の入力へ戻す。`liveness`が保持量、`damping`が高域減衰の度合いに関与。
5. 既定遅延サンプル数は2586,3062,3286,3798。ソースのmsコメントは44.1kHz換算に対応（2586/44100≈58.64ms）。**Daisyターゲットでは`AMY_SAMPLE_RATE=48000`なので、同じサンプル数なら約53.88ms**。つまりこの版では固定サンプル長を使う限り、サンプルレート変更で残響の時間構造が変わる。実機の音への影響は未測定。
6. `stereo_reverb()` はdry信号にwetを足す設計。最終出力段の飽和・クリップ保護はこの関数だけでは判断できない。

出典:
- [AMY src/delay.c](https://github.com/shorepine/amy/blob/main/src/delay.c): `new_delay_line`, `init_stereo_reverb`, `config_stereo_reverb`, `stereo_reverb`
- [AMY src/delay.h](https://github.com/shorepine/amy/blob/main/src/delay.h): API
- [AMY src/amy.h](https://github.com/shorepine/amy/blob/main/src/amy.h): `AMY_SAMPLE_RATE`, `reverb_params_t`
- [AMY src/amy_fixedpoint.h](https://github.com/shorepine/amy/blob/main/src/amy_fixedpoint.h): s8.23 sample形式
- 実装コメントの参照先: [Stautner–Puckette 1982](https://www.ee.columbia.edu/~dpwe/e4896/papers/StautP82-reverb.pdf) / [Pure Data rev2~](https://github.com/duvtedudug/Pure-Data/blob/master/extra/rev2%7E.pd)。**原論文・Pure Data実装は今回未読**。名称だけで同等性を保証しない。

## なぜこの方式が音楽的に意味を持つか

**推論**: 一本のディレイを単純帰還させると周期的な反復音が聞こえやすい。異なる遅延時間を持つ複数経路を相互に混ぜると反射が密になり、明瞭なエコーより空間的な尾を作りやすい。LPFが循環ごとに高域を弱めると残響が暗くなる。これはダブのテープエコー（拍に同期した明瞭な反復）と、空間残響（密な反射）の役割の違いとして使える。

**反証**: 複数遅延を用いれば必ず自然な残響になるわけではない。モード共振、周期性、音の濁り、ステレオ相関、固定小数点の飽和、サンプルレート依存などがあり、測定と聴感が必要。AMYのコードだけで「JUNOらしい」「テープらしい」「高音質」とは断定できない。

## DinMeter/SAM2695との対応

| 原理 | 現行でできること | 限界と将来の手段 |
|---|---|---|
| 複数OSCの合成音に空間残響 | SAM2695内蔵ReverbのMIDI制御（パラメータの実装詳細は要再確認） | 内部残響の4本帰還行列やフィードバックLPFをESP32から直接変更できる根拠はない |
| 各OSC/各voice独立の残響 | MIDIチャンネル別の送信設定は可能 | 同一チャンネル内のvoiceごとに独立したFX遅延バッファを持つこととは異なる |
| 独自リバーブDSP | SAM2695のMIDIコマンドだけではAMYの演算を移植できない | 独立DSP音源の音声出力に適用するか、SAM2695のアナログ出力をADC/Codec経由で取り込む |
| メモリ費用 | SAM2695内蔵FXならESP32のRAMを大きく消費しない | 独自DSPで同種の残響を実装するとバッファだけで108KiB程度（今回のAMY版）＋他の作業領域が必要 |

**設計判断（まだ採用しない）**: 将来の低価格音源比較では「OSCの演算量」だけでなく、**共通FXの長い遅延バッファをどこに置くか**がPSRAM・外部RAMの必要性を左右する。既存DinMeter ESP32-S3のメモリ余力・音声入出力ピン・外部RAMアクセス遅延は未確認。FXを各voiceへ複製する前にglobal busを考える価値がある。

## 反証・制約・検証メニュー（未実施）

- 同じリバーブの有無でDSP CPU使用率、バッファメモリ、音切れ、ノート遅延を比較。44.1/48kHzで残響の時間特性が変わるかインパルス応答を録音して検証。
- wet 0/100%と高feedbackでクリップ/振幅成長を測定。乾いたGM音色を含む複数素材で尾の濁りを比較。
- 独立DSP構成ならAMYのFXバッファを内部RAMとPSRAMに置いた場合の処理時間・キャッシュミスを比較。
- SAM2695内蔵ReverbのMIDI設定で同等の残響が得られる可能性は残る。**自由度が低いことと音質が劣ることは同義ではない**。
- 参照コードのライセンスを確認してから移植可否を判断。今回はコードの移植・改変なし。

## 第1期の進捗

今回: エフェクト分野で「短い可変遅延コーラス」と別に、**多本数のフィードバック残響ネットワーク・メモリ予算・サンプルレート依存**をソースに基づいて説明できた。残りの重要課題は操作ノブの音楽的マッピング、測定方法、MIDI/デジタル制御、安価なDSP候補の現行基板適合性。既存knowledgeカードは2件であり、目安10〜15件には未達。第1期完了とは判定しない。
