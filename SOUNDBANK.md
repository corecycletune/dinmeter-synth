# Artist Soundbank v1 (B1–B9)

DinMeter Synth v1.10.17 の初期音色セット案。アーティストの**特定の音響的特徴を手がかりにした近似音色**で、曲中で使われた実機や録音から採譜・計測した完全再現ではありません。

## バンク構成（8音色 × 9バンク）

| Bank | Theme | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 |
|---|---|---|---|---|---|---|---|---|---|
| B1 | Funk / Bass | HH Chameleon | Bernie Mothership | Stevie Reggae | Prince Funk | Bernie Atomic | HH Headhunters | Parliament | HH Wah |
| B2 | Lead / Solo | George Duke | Jan Hammer | Chick Corea | Zawinul Brass | George Voice | Hammer Scream | Cory Henry | Herbie Glide |
| B3 | Jazz / Fusion | Zawinul Atmos | Chick Crystal | Herbie Butterfly | Duke Fusion | Lyle Mays | Hiromi | Zawinul Pulse | Chick Soft |
| B4 | Neo Soul / Keys | Kiefer | Glasper | D'Angelo | Erykah Badu | Terrace Martin | Moonchild | Jordan Rakei | Tom Misch |
| B5 | Modern Synth | BIGYUKI | Cory Henry | Louis Cole | DOMi | Flying Lotus | Thundercat | Tame Impala | Hiatus Kaiyote |
| B6 | Dub / Reggae | Sumac Dub | Mad Professor | King Tubby | Lee Perry | Augustus Pablo | Scientist | Rhythm & Sound | Zion Train |
| B7 | Dubstep | Mala Deep Sub | DMZ Dark Reese | Skream Wobble | Benga Hollow | Coki Metallic | Burial Ghost Pad | Rusko Dirty Bass | Skrillex Aggro |
| B8 | Experimental | Floating Points | Boards of Canada | Aphex Twin | Four Tet | Jon Hopkins | Nils Frahm | Rival Consoles | Oneohtrix Point Never |
| B9 | GM Standard | Piano | EP1 | EP2 | Drawbar Organ | Clavinet | Vibraphone | Acoustic Bass | String Ensemble |

B9 の設定はGM規格の**1始まりの番号**で順に 1, 5, 6, 17, 8, 12, 33, 49。内部の Program Change は 0, 4, 5, 16, 7, 11, 32, 48。

## 音作りの実装

- 既存のGM/GS素材（SQR、SAW、Ocarina、Whistle、Flute、Recorder、Clarinet、Harmonics）をOSC1〜3へ重ね、音量、トランスポーズ、デチューンを音色ごとに変更。
- 各パッチに音色別のアタック／ディケイ／リリース、フィルター初期値、レゾナンスを指定。
- **MOD ENV1 → CUTOFF** を設定して発音ごとのフィルター変化を付与。音色に応じてアタック、ディケイ、サステイン、リリース、深さを設定。
- **LFO1 → CUTOFF / PITCH** を音色に合わせて設定。Dubstep B7 P3は三角波LFOでWobbleを近似。
- **MOD WHEEL → CUTOFF** を音色共通の軽い操作用に設定。
- B6や一部B5/B8はSAM2695側のDelay/Pan Delayを使用。リバーブ／ディレイはSAM2695の同じグローバルFXエンジンを共有するため同時独立運用ではない。
- この音色集は**音楽的な方向性を決める設計値**。実音からのスペクトル分析や実機でのA/B試聴は未実施。

## 注意・制限

- MOD ENV / LFOの状態は**演奏全体で共有**し、SAM2695のポリフォニック各声に独立して適用されない。和音中に次の鍵盤を押した場合もフィルターENVが再トリガされ得る。
- リアルタイム・フィルター更新は現行コードで約31Hz（MIDI GS SysEx）。音声レートのフィルターではない。高速Wobbleや激しいFM/歪みは完全再現できず、段階感が出る可能性がある。
- フィルター更新のMIDI伝送量と押鍵・ポルタメントの同時使用は実機未検証。発音異常があればまず高変調のB7音色で検証する。
- PERFORMANCEモードの物理GLIDEノブがMONO/GLIDEを上書きする現行仕様は維持する。モノベースを演奏するときはノブ位置も確認する。
- 工場出荷設定の再生成はPRESET TOOLS → DELETE CURRENTで現在のライブラリの当該スロットのみ。ただし保存済み音色は復旧しないため、削除前に内容を確認すること。
- プリセット名はアーティストの音作りを参照したもので、実際に使用した機材・録音チェーンの断定や公認を意味しない。

## 旧プリセットの保護

- 初期選択は**ARTIST**。SYSTEM MENU → **LIBRARY: ARTIST / LEGACY**を選択すると切り替わる。
- 旧ライブラリのNVSキーは **pXX / gXX / oXX / fXX** のまま。
- 新ライブラリでは **aXX / bXX / cXX / dXX** を使用する。ARTISTの保存・削除はLEGACYのデータを変更しない。
- ボタンのバンク番号・スロット番号は維持。ARTIST初期値のB1〜B8は64音色、B9はGMの8音色。
- 切替操作時に未保存の編集内容は消えるため、必要なら先にSAVE PRESETを実行する。

## 実機での確認チェックリスト

1. OTA更新後にB1〜B9の全72スロットが呼べるか確認。
2. B9のピアノ、エレピ、オルガン、クラビ、ビブラフォン、ウッドベース、ストリングスの音色を確認。
3. B7 P1の低域、P3のWobble、P6の空間パッドを聞き比べる。
4. B6のディレイとB7のフィルターENVを試し、押鍵、和音、連打、GLIDE、PANICの同時操作で異常が出ないか確認。
5. ARTISTに変更を保存 → LEGACYへ切替 → 旧保存音が残っていることを確認 → ARTISTへ戻して変更が残っていることを確認。
6. 未保存状態でのライブラリ切替に関するUI挙動を確認。

実機結果を反映して、必要なパッチを1音色ずつ調整する。
