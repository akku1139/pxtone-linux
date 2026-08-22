# pxtone-linux

pxtoneの非公式なLinux移植です。オリジナルは [pxtone-play-sample (Windows / XAudio2)](https://github.com/akku1139/pxtone-source-code) 。

## 構成

- `libpxtn.so` — pxtoneコア(共有ライブラリ)。各アプリから共有される
- `pxtone-play` — CUI再生プレイヤー(ループ再生、Ctrl-Cで停止)
- `pxtone-visualizer` — GUIアプリ。ピアノロール風にノーツを流しながら再生(SDL2)
- `pxtone-editor` — GUIピアノロールエディタ(GTK4)。ノート編集+保存対応

## pxtone-editor (GTK4)

```sh
./build/pxtone-editor <file.ptcop>
```

### ノート編集
- 左クリック(空きセル): ノート設置+プレビュー音。そのままドラッグで長さ調整
- 既存ノートの本体ドラッグ: 移動(スナップ位置+音程行に追従)
- 既存ノートの右端ドラッグ: 長さ変更
- 右クリック/右ドラッグ: ノート削除(連続)
- Ctrl+C/V: コピー/ペースト、Ctrl+Z/Y: Undo/Redo(最大100ステップ、マスタ設定込み)
- リサイズ時に既存ノートと重なる場合は自動で切り詰め/削除(pxTone仕様)

### イベント編集(event... ダイアログ)
velocity / volume / pan volume / pan time / portament / voice no / group no / tuning を編集。
選択中ノートの位置(またはビュー左端)のスナップ位置に書き込まれます。

### 曲設定(song... ダイアログ)
tempo / beats per measure / clock per beat / measures / repeat measure / last measure。
小節線グリッドに反映されます。

### 音源・トラック
- **+unit**: トラック追加(自動でwoice割り当て)
- **rename**: ユニット名変更
- **sound...**: PTV波形(sine/saw/square/triangle/pulse)/ PTNノイズ作成→ユニット割り当て+プレビュー

### 操作
- ホイール: 縦スクロール / Shift+ホイール: 横スクロール / Ctrl+ホイール: ズーム
- Space / ▶■ボタン: 再生/停止、Ctrl+S: 上書き保存(.ptcop)
- キー 1-4: スナップ(4分/8分/16分/32分)

## 変更点

- `Main.cpp` / `SimpleXAudio2.*` (Win32 + XAudio2) を、SDL2オーディオ + コマンドライン引数の `src/main.cpp` に置き換え
- ファイル選択ダイアログ → 引数でパス指定、MessageBox → 標準出力
- `pxtone/` ライブラリ本体はほぼそのまま(OGG Vorbis対応 `pxINCLUDE_OGGVORBIS` を有効化、`pxtnPulse_Oggv.cpp` のgoto跨ぎ初期化を1箇所修正)
- ビルドはCMake(libvorbis, libogg, SDL2 を使用)
- コア部分は共有ライブラリ `libpxtn.so` として各アプリからリンク(rpath `$ORIGIN`)
- 保存は本家pxTone互換の .ptcop 形式のみ(b_tune=false)。独自拡張データは埋め込まない

## テスト

```sh
cmake --build build && ctest --test-dir build  # または個別に:
./build/edit_smoke     <sample.ptcop>   # 編集+保存ラウンドトリップ
./build/history_smoke                   # 移動/コピペ/Undo/Redo
./build/woice_smoke                     # PTV/PTN音源作成+Moo描画
./build/event_smoke                     # ベロシティ等イベントのラウンドトリップ
./build/song_smoke                      # テンポ/拍子等マスタパラメータのラウンドトリップ
./build/unit_smoke                      # ユニット名+voice/group/tuning/portament
./build/roundtrip_smoke                 # 全イベント種別+マスタの統合ラウンドトリップ
```

## ビルド

```sh
sudo apt install cmake g++ libsdl2-dev libvorbis-dev libogg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 使い方

```sh
./build/pxtone-play <file.ptcop|file.pttune|file.ptnoise>
```

ループ再生されます。停止は Ctrl-C。

## GUIビジュアライザ

```sh
./build/pxtone-visualizer <file.ptcop|file.pttune>
```

ピアノロール風にノートが流れるGUIで再生します。表示モードはキー 1-4 で切替(タイトルバーに現在のモードと速度を表示):

1. **Lanes** — ユニット毎のレーンにシンプルなブロック表示
2. **Lanes + PianoRoll** — 各レーン内に音程精度のピアノロールオーバーレイ(黒鍵ストライプ・オクターブ線付き)
3. **PianoRoll Vertical** — 全面ピアノロール、ノートが上から下へ流れる(縦流れ)
4. **PianoRoll Horizontal** — 全面ピアノロール、ノートが左から右へ流れる(横流れ)

ロールモードでは Up/Down キーでスクロール速度を変更できます。白線が再生位置、通過後のノートは暗く表示。ESC / ウィンドウを閉じる / Ctrl-C で停止。

音声デバイスが無い環境では `SDL_AUDIODRIVER=dummy`(GUI確認は `SDL_VIDEODRIVER=dummy` も)を付けると動作確認できます。

## 謝辞

pxtone 及びサンプルコードは (c) pixel 氏によるものです。
