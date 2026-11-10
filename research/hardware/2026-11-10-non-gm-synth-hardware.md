# GM音源以外で「本物のシンセエンジン」を作る安価な部品・方式の候補

調査: 2026-11-10 (JST)  
状態: **候補・原典URL・概算価格を確認**。DinMeter実機のピン配置、CPU負荷、音質、発音数、レイテンシーは未検証。価格は販売店の確認時点で変動・送料別。

## 目的

現在のDinMeter Synthは「SAM2695のGM/MT-32互換ROM音色を複数MIDIチャンネルで重ね、フィルターや音量をMIDIで操作する」構成。これに対し、元の**波形自体をリアルタイム合成できる**安価な部品が手に入るか、将来の音源変更や別ユニット追加の選択肢を整理する。

**注意：PCM5102Aは音源チップではなくDAC。** 実際の発振器・フィルター・エンベロープ・エフェクトはESP32/RP2350/STM32などのマイコン内で計算する。デジタル音声をI2SでDACに送り、アナログ音声に変える構成。

## 1. 方式ごとの候補と価格帯

| 方式 / 候補 | 確認した価格・構成例 | 音作りの自由度 | 注意点 |
|---|---|---|---|
| **今のESP32-S3 + I2S DAC (PCM5102A)** | DAC単体: 530円（電子工作ステーション。一方は売切）、Adafruit基板1,210円（税込、確認時点）、DinMeter本体は既に所有 | PCMを自力生成できればOSC、各種VCFモデル、ENV/LFO、歪み、Delayまでソフト実装可能 | 現在のM5Stack Din Meter基板で空きI2Sピン・RAM・CPU・タスク処理余力を要確認。DACだけを挿しても音は出ない |
| **独立ESP32-S3 + PCM5102A** | MCU基板とDACを別途購入。具体的な合計は販売店・PSRAM有無で別見積もり | DSP専用マイコン化でき、既存DinMeterにコントローラ役を残せる | 基板・配線・電源・DinMeter側とのMIDI/UART/I2C通信が増える |
| **Raspberry Pi Pico 2 (RP2350) + PCM5102A** | KSY価格Pico 2=913円、DAC基板530〜1,210円の掲載例。**部品例の単純合計約1,443〜2,123円**（送料・配線・在庫・電源等除く） | 外部音源モジュールにできる。オシレーター、フィルター、ENVを実装する公開プロジェクトあり | CPU/メモリ、ボイス数、I2S実装、Daisy/Teensyより豊富でない音響ライブラリを検証 |
| **Daisy Seed3（オーディオDSP）** | メーカーが2026-11-01から1MB版$29.99、65MB版$34.99と掲載（送料/税別） | ステレオ音声CodecとDSP用途の環境を備え、発振・VCF・エフェクトの実装を学びやすい | 円換算/国内流通と供給を要確認。旧SeedとSeed3は版を分けて記録 |
| **Teensy 4.1 + Audio Shield** | 千石電商掲載Teensy 4.1=5,200円、Audio Shield=1,980円、合計**7,180円**（基板例） | PJRC Audio Libraryが波形合成・エフェクト・ミキサー等を提供 | 他より高い。専用基板をもう一つ載せることになる |
| **アナログVCO IC（例 SSI2130）+ VCF/VCA** | SSI2130部品単体$5.95という海外販売例。ただしVCO単体であり、VCF/VCA、電源、制御用DAC、基板が追加必須 | 実際にアナログ発振/フィルターを実装可能 | **ICの値段 ≠ 1音声分の完成費用**。複数OSC/ポリ化で基板・調整・電源コスト増。日本での入手性・合計費用は未確認 |

### 価格・仕様の原典

- [電子工作ステーション PCM5102A基板](https://electronicwork.shop/items/67e3865de37c5b014309a327) — 530円掲載。標準版は売り切れと表示されているため在庫と品種を再確認。
- [スイッチサイエンス Adafruit PCM5102A DAC](https://www.switch-science.com/products/10382) — 確認時税込1,210円掲載。I2S入力・ライン出力。
- [KSY Raspberry Pi Pico 2](https://raspberry-pi.ksyic.com/main/index/pdp.category/21/) — 税込913円の掲載例。メーカー[公式](https://www.raspberrypi.com/products/raspberry-pi-pico-2/)は$5から。
- [Daisy 2026-11-01価格改定案内](https://daisy.audio/blogs/seeds-n-circuits/an-update-on-pricing) / [Seed3公式仕様](https://docs.daisy.audio/hardware/Seed3/)。
- [千石電商 Teensy価格表](https://www.sengoku.co.jp/mod/sgk_cart/search.php?cid=0&code=&k3=3&list=4&multi=teensy) / [PJRC Audio Shield](https://www.pjrc.com/store/teensy3_audio.html)。
- [SSI2130 VCO販売例](https://www.amplifiedparts.com/products/integrated-circuit-ssi2130-vco-sound-semiconductor)。**国内調達価格の代用にはしない**。

## 2. 先行プロジェクト（実際に何が作られているか）

| 先行実装 | READMEで確認した内容 | 次にソースで調べること |
|---|---|---|
| [AMY](https://github.com/shorepine/amy) | 小型のC製音声合成エンジン。ESP32-S3 / Pico 2 / Teensy / Daisyなどに対応を明記。アナログ風、FM、PCM、Wavetable、フィルター、LFO、効果、MIDIのvoice stealingまで対象 | 発振器/フィルター実装、実ボイス数・CPU/メモリ実測、演奏のレイテンシー |
| [GrooveS3](https://github.com/MikeAymeric/GrooveS3) | ESP32-S3 + PCM5102A + AMY、MIDIイベントとAudio処理を異なるcore/taskで分離する設計をREADMEに記述 | 実際のソースコードがREADMEどおりに動くか、I2S/FreeRTOSキュー、音質、部品表 |
| [ESP32 Analog Style MIDI Synth](https://github.com/arizzi74/ES32-Analog-Style-Midi-Synth) | ESP32 + PCM5102A、PolyBLEP OSC、Sub/Unison、ADSR×2、LPF/HPF、driveなどをREADMEに記載 | 波形のアンチエイリアス処理、フィルター係数、共有/ボイス別ENVの違い |
| [ESP32 SoundFont SF2 Sampler](https://github.com/copych/ESP32_SF2_Sampler_Synthesizer) | ESP32-S3/P4 + PSRAM + PCM5102A、SF2読み込み、オプションの各音声LPF、FX、GM互換MIDIをREADMEに記載 | GMチップ代替としてSoundFontを使う場合のメモリ/ストレージ/ライセンス |
| [Pico SYNTH Platform](https://trial4you.eu/pico-synth/) | Pico系のI2S合成やフィルター、poly、chorusの教材とPico 2でのSoundFont試作を紹介 | README・配布ソースを読んでCPU消費とDAC配線を評価 |

**確認レベル**：上の5件はREADME/公開ページを確認。**音声処理関数の精読・実機動作の確認はまだしていない**。宣伝上の「低遅延」「高品質」「高ポリ数」はそのまま採用判断に使わない。出典のソフトウェアライセンスは各リポジトリで個別確認。

## 3. 乗り換え／共存の3案

### A. 既存DinMeterのESP32-S3で音声合成（追加DACのみ）

`USB MIDI → DinMeter ESP32-S3 [DSP + UI + Presets] → I2S → PCM5102A → 音声出力`

**利点**: 新規部品費が最少となる可能性。OSC/Filter/ENV/エフェクトを細かく自作できる。  
**懸念**: 既存本体にはUSBホスト、Angle8/ByteButton、画面、設定/OTA、演奏管理があり、DSPのリアルタイム処理と競合するかもしれない。Din Meter基板の空きGPIO/PSRAMが不明。**実機検証前に購入・実装を決めない**。

### B. 音声エンジン専用の別マイコンを追加（研究の本命候補）

`USB MIDI → 既存DinMeter（操作/UI/ノート管理） → UART等 → Pico 2 or ESP32-S3 or Daisy [DSP] → DAC → 出力`

**利点**: 現行UIやノート制御を保ちつつ、音声合成の計算を分離できる。USB Hostの負荷からDSPを隔離できる。  
**懸念**: 別基板・通信プロトコル・電源・遅延・筐体配線が必要。どの制御処理をどちらのマイコンが担当するかを明確化する。

### C. SAM2695とDSP音源のハイブリッド

`DinMeter → SAM2695 GMレイヤー + DSPシンセレイヤー → オーディオミキサー → 出力`

**利点**: GMの音色を残しつつ、DSP側で本物のPCMサンプル前段の波形・VCF・モジュレーションを追加できる。  
**懸念**: アナログ音声のミックス、出力レベル、ノイズ、レイヤー間遅延、電源が増える。**PCM5102Aは基本的に出力DACなので、既存SAM2695音声へDSPエフェクトを掛けたいなら音声入力用ADC/Codecも必要**。単純なオーディオ合算だけならミキサーで実現可能。

## 4. DinMeterへの評価軸（価格だけで結論を出さない）

| 軸 | 判定方法 |
|---|---|
| 合計費用 | 本体+DAC/Codec+入出力+電源+配線/基板+送料/税。IC単価だけは不可 |
| 音作り | 真のDSP波形/VCF/ENV/音声ごとのMOD/ディレイ等が動くか |
| 演奏性能 | USB MIDI→発音までのレイテンシー、発音数、和音、ピッチ移動の滑らかさ |
| 既存資産 | DinMeter UI/プリセット/ノート管理/USBホスト/OTAを何割保持できるか |
| 実装難度 | I2Sピン/PSRAM/電源、2CPU間の通信、録音/試験系、保守性 |
| 調達 | 日本国内での購入可否、再入手性、モジュールの大きさ、ケースへの搭載 |
| 自由度 | ソース公開の範囲、商用利用可能性、コミュニティ実績と更新状況 |

## 5. 次の調査（コードを直す前に）

1. **現行DinMeterの余力調査**：M5Stack Din Meter v1.1の回路図/ピンアウト、RAM/PSRAM、空きI2S、電源。I2SとUSB Host同時運用の先行実装。
2. **AMYとESP32-S3**：発振器・フィルター/Envelopeの実装コード、音声処理タスク、I2S DMA、発音数/CPU負荷の資料。
3. **Pico 2とDaisy Seed3**：同じテスト用1OSC+LPF+ENV+PORTAMENTOで比較できるか。最低コストと音質のトレードオフ。
4. **アナログ実装**：SSI2130等VCO、VCF/VCA、±電源、MIDI-CV DACを含む**1音分の完成費用**を計算。IC単価だけで安いと判断しない。
5. **将来の完成形**：単独ESP32-S3、別DSP基板、GM+DSP混成を、価格・サイズ・消費電流・開発負荷で比較。

## 暫定結論（根拠と不確実性）

- **GMチップ以外の安価な音源構成は実在**。特に「マイコン + I2S DAC + OSSシンセエンジン」は、SAM2695より波形合成やfilterを自由に設計できる。
- **最安部品 ≠ 最良のDinMeter改造案**。現行本体でI2Sが使えるか/処理余力があるかは不明。音声合成専用基板方式の方が、トータルで安定する可能性がある。
- 比較・学習を優先し、実装や買い物を先走らない。次回はAMYソースとDinMeterのアーキテクチャの比較を進める。
