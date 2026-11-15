# Клиент v5 на Windows

Сервер остаётся на Linux (Selectel). На Windows собираем только клиент.

## 1. Компилятор: w64devkit

w64devkit — это g++, make и командная строка в стиле Linux в одной папке, без установки.

1. Открой https://github.com/skeeto/w64devkit/releases
2. Скачай `w64devkit-x64-<версия>.7z.exe` (это самораспаковывающийся архив).
3. Запусти его и распакуй в `C:\` → появится `C:\w64devkit`.
4. Запускай `C:\w64devkit\w64devkit.exe` — откроется консоль, в ней работают `g++`, `make`, `ls`, `cd`.

## 2. Библиотека raylib

1. Открой https://github.com/raysan5/raylib/releases
2. Скачай `raylib-<версия>_win64_mingw-w64.zip` (именно **mingw-w64**, не msvc).
3. Распакуй и переименуй папку в `C:\raylib`, чтобы внутри были `C:\raylib\include\raylib.h` и `C:\raylib\lib\libraylib.a`.

Если положил в другое место — укажи при сборке: `make client-v5 RAYLIB_DIR=D:/libs/raylib ...`

## 3. Код проекта

Скопируй на ПК папку проекта целиком (нужны как минимум `Makefile` и `v5_prediction/`),
например в `C:\games\OnlineGameCpp`.

## 4. Сборка и запуск

В консоли w64devkit:

```
cd C:/games/OnlineGameCpp
make client-v5 HOST=135.106.163.230
```

Получится `build/v5_client.exe`. При первом запуске Windows может спросить про брандмауэр — разреши.

Готовый `build/v5_client.exe` можно запускать и без w64devkit (двойным кликом подключится к 127.0.0.1,
поэтому лучше из консоли: `build\v5_client.exe 135.106.163.230 7777`).
