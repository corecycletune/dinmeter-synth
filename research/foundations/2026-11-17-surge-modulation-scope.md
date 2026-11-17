# 第8回｜Surge XTのMod Matrix：Voice / Scene / Globalの境界

調査日: 2026-11-17 JST。テーマ: モジュレーションの**対象範囲（scope）**と状態の所有権。前回のMono/Voice所有権とは別の問い。  
確認対象: [Surge XT公開ソース](https://github.com/surge-synthesizer/surge) のmain、取得時点のblob SHA: `ModulationSource.h` = `1430e210a397616464f32c1cc4c6af33a2e6a026`、`SurgePatch.cpp` = `4cfceafa8241330a6fd1f3d00faa803dcaa00873`、`SurgeSynthesizer.cpp` = `ecd725e6f16a82f0ca2dfbb699cb247a3dedce50`。ビルド・実機録音なし。GPL-3.0-or-laterのためコードの移植・転載なし。

## 新しく確認した原理

**変調の入力源（ENV/LFO/velocityなど）・変調の宛先（OSC pitch/VCF cutoff/FXなど）・変調状態の所有範囲は、独立して考える必要がある。** 同じLFOという名前でも、各ノートで位相が異なるVoice LFOと、シーン全体で共有するScene LFOは、和音に対して違う音響結果を生む。MIDIチャンネル別制御は、このVoice単位の独立性を自動的には保証しない。

### 一次ソースで確認した事実

1. [`ModulationSource.h` 40–85行](https://github.com/surge-synthesizer/surge/blob/main/src/common/ModulationSource.h#L40-L85) は `ms_ampeg` / `ms_filtereg`、`ms_lfo1..6`、`ms_slfo1..6`、velocity/keytrack/aftertouch/MIDIコントローラを異なるmodsourceとして列挙。
2. [同211–260行](https://github.com/surge-synthesizer/surge/blob/main/src/common/ModulationSource.h#L211-L260) の `isScenelevel()` は変調源をScene levelかどうか分類する。velocity/keytrack/polyaftertouch/releasevelocityはSceneレベルから除外され、Scene LFO群はScene level。これは**名前の類似ではなく、明示的なスコープ判定**。
3. 同ファイル `isModulatorDistinctPerScene()` のコメントと実装は、LFOとENVなどがSceneごとに別インスタンスである一方、マクロやMIDIコントローラは両Sceneで共有される場合があると説明。**Sceneごとに別**と**Voiceごとに別**は違う。
4. [`SurgePatch.cpp` 2191–2235行](https://github.com/surge-synthesizer/surge/blob/main/src/common/SurgePatch.cpp#L2191-L2235) のパッチ読込は `sceneId` と `isScenelevel()` に基づき、変調ルーティングを `scene[].modulation_scene`、`scene[].modulation_voice`、`modulation_global` に格納。これは**3つのルーティング格納先が実在する**根拠。
5. [同4140–4178行](https://github.com/surge-synthesizer/surge/blob/main/src/common/SurgePatch.cpp#L4140-L4178) の保存経路もScene/Voice/Globalのルーティングを分け、Globalルーティングでは `source_scene` を保存する。
6. [`SurgeSynthesizer.cpp` 176–203行](https://github.com/surge-synthesizer/surge/blob/main/src/common/SurgeSynthesizer.cpp#L176-L203) でScene LFOに `setIsVoice(false)` を与え、マクロのオブジェクトを両Sceneで共有している。

**未読・未確認**: VoiceのDSP実行ループでルーティングがいつどの順番で適用されるか、Global宛てでVoice由来変調源をどのように扱うか、音声ブロックの更新周期、モジュレーションのスムージング、MPEの全経路。**ルーティング保存コードだけで「どのソースでもGlobal FXを自由に変調できる」とは断定しない**。

## 信号経路を分ける

- **音声経路（概念）**: 各VoiceのOSC → 各VoiceのFilter/AMP → Scene mix → Global FX → Audio output。Surge XTの一般的な構造としての概略で、今回その全DSP実装を読了したわけではない。
- **制御経路（今回の主対象）**: 各ノートのVelocity / Keytrack / Voice ENV / Voice LFO、Scene LFO / Sceneコントローラ、共有Macro → ルーティング深度 → OSC/Filter/Scene/FXパラメータ。
- **音楽的意味**: 各VoiceのFilter ENVが独立なら、和音をずらして弾くと各音の明るさが別々に変化する。Scene LFOなら、和音全体に共通位相の周期変化を与えられる。後段のGlobal FXへのLFOはミックス後の音全体を揺らすため、各Voiceのフィルターを揺らす場合と等価ではない。
- **設計理由**: コードから確実なのはスコープが異なる変調経路を分けていること。開発者がそれを採用した歴史的動機は今回一次資料で確認できず、断定しない。

## DinMeter / SAM2695との比較

| 概念 | 現行DinMeter（11/10レビューに基づく） | 判断 |
|---|---|---|
| 変調元LFO/ENVをESP32で計算 | `SynthApp.cpp`に実装済み、出力最小間隔32ms | **実装済み**。ただし最新mainの差分は今回未照合 |
| 3OSCの独立MIDIチャンネルへCutoff等を送る | GS SysExによるパート制御 | **MIDIで近似可能**。各OSCチャンネルごとには変えられる |
| 1つのMIDIチャンネル内の同時和音で、各ノートが別位相のLFO/ENVを持つ | SAM内部のVoiceにアクセス不可 | **現行制御で実現できる根拠なし**。別チャンネルへ1音ずつ割り振る方式は構想可能だがチャンネル/発音数/MIDI帯域を消費し、SAM内のDSP独立性と同義ではない |
| 音声後段の共通FX変調 | SAM内蔵FXのMIDI制御はあり | **可動範囲はデータシートと実機で要検証**。DSPのGlobal FXと同一視不可 |
| 各Voiceの独立した高速Filter ENV/PWM | SAM内部処理に直接アクセスできない | **自前DSPまたは対応音源が必要** |
| Mod Matrixのスコープ概念 | OSCチャンネル/グローバル設定の区別はある | **設計原理として導入可能**。ただし新機能実装は別指示待ち |

## 反証と境界条件

- **反証1**: 「3つのOSCチャンネルがあるから3Voice独立Mod Matrixがある」は誤り。OSCは3つのGM音色レイヤーであり、各レイヤーが同じ和音全体を鳴らす。**チャンネル別**と**ノート別**を混同してはいけない。
- **反証2**: 「Scene LFOを使うと必ず同期した音になる」も強すぎる。音声側のVoice ENV、ピッチ、フィルター状態、LFOの開始位相/リセット条件で結果は変わる。
- **反証3**: 「Voice単位の変調が必ず優れている」わけではない。DUBの一括ワブルや演奏中の大きなフィルタースイープでは共通変調が意図に合う場合もある。
- **負荷**: 現行SAM2695のUART 31,250bpsで各ノート独立の高速変調をSysEx多発で再現する案は帯域不足になりやすい（既存[002 MIDI負荷カード](../knowledge/002-midi-output-budget.md)参照）。実際の破綻は未計測。
- **ライセンス**: Surgeの実装をコピーするのではなく、scope分類と信号の意味を独自に再設計する。

## 反証可能な実験（未実施）

1. C4を押し、0.5秒後にE4を押す。**各ノート独立のENV**と**共通のフィルター変調**を録音し、明るさの立上がりが各ノートで独立かをスペクトログラムで比較。
2. 1音・3和音・3OSCレイヤーで、LFO深度と周期を固定し、和音中の音ごとに位相がずれるか聴感と録音で比較。
3. ESP32のMOD更新を現行32msのまま、MIDI送信量・NoteOff到達時間を測定。各ノート独立SysExを増やす前に**通信の余裕**を確かめる。
4. DSP候補（AMY等）では「各Voiceで独立したLFO/ENV状態を持つか」「Global FXへ変調をどう接続するか」をソースから比較する。

## 再利用する設計判断

**パラメータに「対象（Voice/Scene/Global）」「変調源」「更新頻度」「発音状態の所有者」を別々に定義する。** これは現行SAM2695でも将来のDSP音源でも、できること・できないことを誤認しないための整理軸になる。今回は基礎研究のみであり、実装候補の追加やファームウェア編集はしていない。
