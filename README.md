# ModernDisplay

Плагин для **GTA: San Andreas 1.0 US** и **SA-MP (0.3.7 R1-R5, 0.3.DL)**, который переводит вывод игры с устаревшего DirectX 9 на современный **D3D9Ex -> D3D11 DXGI Flip Model (`FLIP_DISCARD` + `Allow Tearing`)** и включает настоящий **NVIDIA Reflex (Low Latency + Boost)**.

Без ImGui, без конфигов и лишних настроек: один файл `.asi` на ~170 КБ с оптимальной конфигурацией из коробки.

## Что даёт

- **Мгновенный Alt-Tab и Win без сброса устройства и крашей** - на `D3D9Ex` при сворачивании не возникает `DeviceLost`, не пересоздаются шрифты чата, текстуры и ImGui других плагинов. Работает и на Windows 10, и в 4:3 без чёрного экрана.
- **Настоящий NVIDIA Reflex (Low Latency + Boost)** - динамическая загрузка `nvapi.dll` (x86) на `ID3D11Device`, нулевая очередь кадров CPU -> GPU, аппаратные маркеры кадра (`SIMULATION`, `INPUT_SAMPLE`, `RENDERSUBMIT`, `PRESENT`) и удержание частот GPU в режиме Boost.
- **Универсальный Low-Latency для AMD и Intel** - на видеокартах без NVAPI автоматически используется `DXGI Frame Latency Waitable Object` с очередью 1 кадр.
- **DXGI Independent Flip + Allow Tearing** - обход буфера рабочего стола Windows (DWM) в безрамочном режиме, поддержка 144-360 Гц и G-Sync / FreeSync.
- **Полная совместимость с SA-MP и модами**:
  - Работают `F8`-скриншоты (включая чат, ники, худ и `8x MSAA`), NVIDIA App / ShadowPlay, `ModernFpsBoost`, `ModernFpsLimit`, SAMPFUNCS, MoonLoader.
  - Автоматический подбор того же GPU по `LUID` на ноутбуках с гибридной графикой (Optimus / Switchable Graphics).
  - Авто-детект локального `d3d9.dll` (DXVK, ENB, ReShade) и Windows < 10 - плагин безопасно отходит в сторону без конфликтов.

## Требования и установка

- **ОС:** Windows 10 / Windows 11
- **Игра:** GTA:SA 1.0 US (одиночная игра или SA-MP 0.3.7 R1 / R2 / R3-1 / R4 / R5-1 / 0.3.DL-R1)
- **Установка:** скопировать `ModernDisplay.asi` в папку с `gta_sa.exe` (или в папку `scripts/`). Если в папке игры уже лежит `d3d9.dll` от DXVK, убрать/переименовать его.

## Сборка из исходников

Требуется Visual Studio 2022 с компонентом C++ x86 (Windows 10/11 SDK):

```bat
build.bat
```

Готовый файл появится в `build\ModernDisplay.asi`.

## Автор

**Softik Inquisitor** - https://vk.ru/miurawr
