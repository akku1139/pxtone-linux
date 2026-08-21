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

- 左クリック/ドラッグ : ノート追加(スナップ)+長さ変更
- 右クリック : ノート削除
- ホイール / Shift+ホイール / Ctrl+ホイール : 縦スクロール / 横スクロール / ズーム
- Space : 再生/停止、Ctrl+S : 上書き保存(.ptcop)
- キー 1-4 : スナップ(4分/8分/16分/32分)、ヘッダのドロップダウンでユニット選択

## 変更点

- `Main.cpp` / `SimpleXAudio2.*` (Win32 + XAudio2) を、SDL2オーディオ + コマンドライン引数の `src/main.cpp` に置き換え
- ファイル選択ダイアログ → 引数でパス指定、MessageBox → 標準出力
- `pxtone/` ライブラリ本体はほぼそのまま(OGG Vorbis対応 `pxINCLUDE_OGGVORBIS` を有効化、`pxtnPulse_Oggv.cpp` のgoto跨ぎ初期化を1箇所修正)
- ビルドはCMake(libvorbis, libogg, SDL2 を使用)
- コア部分は共有ライブラリ `libpxtn.so` として各アプリからリンク(rpath `$ORIGIN`)

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
