# pxtone-linux

pxtoneの非公式なLinux移植です。オリジナルは [pxtone-play-sample (Windows / XAudio2)](../pxtone-source-code) 。

## 変更点

- `Main.cpp` / `SimpleXAudio2.*` (Win32 + XAudio2) を、SDL2オーディオ + コマンドライン引数の `src/main.cpp` に置き換え
- ファイル選択ダイアログ → 引数でパス指定、MessageBox → 標準出力
- `pxtone/` ライブラリ本体はほぼそのまま(OGG Vorbis対応 `pxINCLUDE_OGGVORBIS` を有効化、`pxtnPulse_Oggv.cpp` のgoto跨ぎ初期化を1箇所修正)
- ビルドはCMake(libvorbis, libogg, SDL2 を使用)

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

音声デバイスが無い環境では `SDL_AUDIODRIVER=dummy` を付けると動作確認できます。

## 謝辞

pxtone 及びサンプルコードは (c) pixel 氏によるものです。
