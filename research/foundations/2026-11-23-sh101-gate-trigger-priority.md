# 2026-11-23｜Roland SH-101：Gate/Triggerとノート優先順位の結合

分類: 名機の減算合成 / Mono発音制御。対象は**1982–88年のオリジナルSH-101**。PLUG-OUTソフト版と混同しない。原典はRoland公式のオリジナル取扱説明書と公式仕様。実機試験なし、サービス回路・ファームウェア未読。

## 重要な一次資料上の発見
Rolandオリジナル取扱説明書の「Gate/Trigger selector switch」節では、**GATEモードは押鍵重複時に低音優先**、**GATE+TRIGモードは後着音優先**と説明する。つまりSH-101ではENV再トリガの選択と音程優先規則が、操作上は完全に独立していない。LFOモードはENVをLFO周期で繰り返し、重複押鍵は低音優先と記載。これを「Monoなら常にlast-note priority」と一般化するのは誤り。

根拠:
- Roland公式オリジナル日本語取扱説明書 https://cdn.roland.com/assets/media/pdf/SH-101_OM.pdf （「ゲート・トリガー選択スイッチ」節、PDFページ位置は未照合）
- Roland公式 SH-101 Technical Specifications https://support.roland.com/hc/en-us/articles/201921519-SH-101-Technical-Specifications （Gate/Trigger選択、VCA ENV/GATE、SOURCE MIXER、Portamento）
- https://cdn.roland.com/assets/media/pdf/SH-101_PlugoutSoftSynth_OM.pdf は**ソフト版**資料であり、本ノートのオリジナル実機の根拠には使わない。

## 信号経路と制御の分離
**音声**: 1 VCOのsaw/pulse、同期したsub-oscillator、noiseをSOURCE MIXERで加算 → 共通のVCF（LPF、共振・自己発振可）→ 共通VCA → 出力。Subは別鍵盤ボイスではなく、同じ演奏音程に従う低音素材。各波形レベルは独立だが、フィルターとVCAは共通。オリジナルSH-101は単音楽器。

**制御**: Keyboard CVがVCO音程とVCF key-followへ、ENVがVCF cutoffおよびVCA（ENV選択時）へ、LFOがPWM/VCF等へ、Gate/TriggerスイッチがENV発火規則へ影響。VCAはENV/GATE選択で、音量の時間形状を切替可能。ポルタメントは音程制御の経路であり、音声をディレイする効果ではない。

**設計理由**: 公式説明から「レガートでも各音の音量・音色を再形成するためにGATE+TRIGを用意した」ことは直接確認できる。一方、優先順位を切り替えと結合した回路上の必然性・コスト理由は原典未確認で推測しない。

## 音楽的な意味
- 低音を保持して上で短い音を重ねる演奏は、GATEでは低音優先が働く。GATE+TRIGでは新しい音に切り替わり、ENVが再開する。
- ベースの短いアタックを繰り返すにはGATE+TRIG、滑らかな音色変化を残すにはGATEという方向性。ただし押鍵優先も変わるため、ENV差だけの比較にならない。
- サブOSCを加えて太くしても、独立した低音鍵盤ボイスを追加したことにはならない。共通フィルターで両方の成分が同時に変わる。

## DinMeterとの対応（2026-11-10レビューを基準、最新SynthApp.cppは未読）
| 要素 | 分類 | 理由 |
|---|---|---|
| 3つのGM素材を重ねる、オクターブ差、デチューン | 実装済み | 複数MIDIチャンネルによるレイヤー。SH-101の位相同期subとは非等価 |
| Monoのlast-note優先、Pitch Bend Glide | 実装済み（レビュー時点） | SH-101のGATE時のlow-note優先は再現されない |
| ENV再トリガと音程優先の組合せ | MIDI制御で近似可能な部分あり／詳細未確認 | ESP32側の押鍵集合とENV状態を分離できる可能性。SAM内蔵ENVの発火挙動は別 |
| 真の位相同期sub、同一ミキサー→共通VCFの波形処理 | DSP/音源変更が必要 | SAM2695内部音声サンプルへのアクセスなし |
| SH-101型VCA ENV/GATE切替 | 未確認 | 音量CCによる近似は原機のVCAと同一ではない |

## 反証・制約・未検証
- SH-101のGATE/low-note関係は公式説明で確認したが、すべての製造リビジョン・外部CV/GATE入力で同じかは未確認。
- GATE+TRIGでのENV再トリガは明記されるが、**内部アナログENVの厳密な曲線**や各キー移行時の挙動は測定していない。
- DinMeterのlast-note設計はSH-101の誤実装ではない。楽器としての仕様選択が異なるだけ。
- 実験案（未実施）：低音を保持→高音を押す→高音を離す、の3操作を各モードで行い、音程優先・ENVの立ち上がり・戻り音の挙動を別々に記録。SAM2695では受信/送信MIDIと音声録音を同時観測。
- 将来の設計案：Mono Priority (last/low/high) と ENV Retrigger (always/legato-only/never) は**内部設計では独立**させ、SH-101風の操作プリセットで組み合わせる。これは推論・候補であり、実装指示ではない。

ソースコード転載なし。ファームウェア・設定・プリセット・ビルド変更なし。
