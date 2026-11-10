![FlatOut 2](images/flatout2-banner.jpg)

# FlatOut 2 for ARM64 handhelds — + Android port

FlatOut 2 (PC) statically recompiled to native ARM64 Linux, as a PortMaster-style port. You provide the game files, the port does the rest: nothing to compile.

**Transparency:** this port was developed entirely with Claude, Anthropic's AI, which wrote the code. My own part was testing it on real consoles, analysing the results and proposing improvements.

---

## 🤖 Port Android (neste fork)

Este fork adiciona um **app Android** (`android/`) que carrega o **mesmo binário ARM64 recompilado** (`fo2.aarch64`) dentro do processo do app: um carregador ELF próprio mapeia o executável glibc em memória e uma camada de *shims* (glibc→bionic, SDL2→ANativeWindow/EGL/OpenSL, GLES2→libGLESv2 do sistema) satisfaz as 373 importações dele — sem `exec()`, sem glibc externa, sem X11.

### Como compilar

```bash
git clone https://github.com/deivid22srk/flatout2-portmaster.git
cd flatout2-portmaster/android
./gradlew assembleDebug        # requer JDK 17, Android SDK 34 + NDK r26.3
```

O binário `fo2.aarch64` **não está no repositório**: o APK o embute a partir da release
pública do upstream (o CI baixa automaticamente), ou você o coloca manualmente em
`android/app/src/main/assets/fo2/fo2.aarch64` antes de compilar.

**CI:** `.github/workflows/build.yml` gera os APKs (debug + release) a cada push/PR/
acionamento manual e publica como artifact `flatout2-android-apk`.

### Como instalar

1. Instale o APK (sideload — `minSdk 26`, alvo `arm64-v8a`).
2. Copie os arquivos **do seu jogo** (GOG/Zoom v2.1.0.9) para:
   `Android/data/com.deivid22srk.flatout2/files/gamedata/`
   (`FlatOut2.exe`, `d3dx9_30.dll`, `filesystem`, `data.bfs`, `langpack.bfs`).
3. Abra o app: a tela inicial valida os arquivos (inclusive o MD5 do exe) e mostra
   instruções se algo faltar.

### Controles

- **Gamepad físico** — mapeado direto para SDL GameController (botões e analógicos).
- **Toque** — volante na esquerda, acelerar/freio/freio de mão/nitro na direita,
  reset/câmera/pausa/marchas no topo. Botão ☰ oculta/mostra.
- Tela cheia imersiva, orientação paisagem.

### O que ainda não funciona / limitações

- ⚠️ **Não validado em aparelho real ainda** (sem dispositivo no ambiente de build):
  loader e shims compilam no CI e foram escritos contra a superfície real do binário,
  mas a primeira execução em hardware pode exigir ajustes — rode com
  `adb logcat -s FO2` e reporte o log.
- FMVs desligados (FFmpeg não é embutido; os arquivos de vídeo são ignorados).
- LAN/multiplayer: já não funciona no upstream; segue igual.
- Sem suporte a armeabi-v7a (o binário é arm64 only).

### Análise técnica

O documento [`docs/ANALISE.md`](docs/ANALISE.md) descreve a arquitetura do port,
o levantamento completo das importações do binário (readelf), as decisões de projeto
e os riscos conhecidos do port Android.

---

## Port original para handhelds (upstream, `badcats72`)

<!-- SCREENSHOTS: one per console, same place in the game, FPS counter visible -->

| RG34XX-SP | RG351P | GKD Pixel II |
|---|---|---|
| <a href="images/rg34xx-sp.png"><img src="images/rg34xx-sp.png" height="220" alt="RG34XX-SP"></a> | <a href="images/rg351p.png"><img src="images/rg351p.png" height="220" alt="RG351P"></a> | <a href="images/gkd-pixel2.png"><img src="images/gkd-pixel2.png" height="220" alt="GKD Pixel II"></a> |

Click an image to see it at its real resolution.

## Tested consoles

| Console | Chip | CPU | GPU | OS | Render size | Launcher | Average FPS | Min | 1% low |
|---|---|---|---|---|---|---|---|---|---|
| Anbernic RG34XX-SP (2 GB) | H700 | Cortex-A53 | Mali-G31 MP2 | Knulli Scarab | 648x432 | FPS Unlock | 39 | 28 | 23 |
| Anbernic RG351P | RK3326 | Cortex-A35 | Mali-G31 MP2 | dArkOS | 480x320 | 30 FPS | 26 | 15 | 15 |
| GKD Pixel II | RK3326S | Cortex-A35 | Mali-G31 2EE | ROCKNIX | 640x480 | 30 FPS | 25 | 12 | 12 |

Measured with the port's own FPS counter by playing one lap on the same track with the same car, in similar driving, with busy moments and quiet ones. These are the figures shown on the screenshots above: expect some variation between races.

Other consoles are not tested. The launcher tries to adapt to the screen and video driver of an unknown console, but it may need work: please open an issue with your console, your OS and `ports/flatout2/log.txt`.

## What you need

You must own the game. This port does not contain any of its data.

You need the **GOG / Zoom Platform** version of the game (v2.1.0.9). Its `FlatOut2.exe` must have this MD5: `1f8cae2b47276ca134750ba438aa35ca`. The launcher checks it and tells you if your executable is not the right one.

## Install

1. Copy both launchers and the `flatout2` folder from the release zip into your `ports` folder.
2. Copy everything from your game's folder into `ports/flatout2/gamedata/`. About 3 GB are needed.
3. Start one of the two launchers from the Ports menu:

| Launcher | Frame rate |
|---|---|
| `FlatOut 2 [RK3326 - 30 FPS].sh` | limited to 30 FPS: steadier on small chips |
| `FlatOut 2 [H700 - FPS Unlock].sh` | up to 60 FPS |

The names are only a guide: either one runs on any console.

## Controls

| Button | In a race |
|---|---|
| South button | Accelerate, confirm |
| West button | Brake, reverse |
| East button | Handbrake, back |
| North button | Reset the car |
| R1 | Nitro |
| R2 / L2 | Shift up / down |
| Select | Change camera |
| Start | Pause |

| Combination | Action |
|---|---|
| **Select + L1** | Swaps A/B and X/Y, remembered. Use it once if your buttons are the wrong way round |
| **L2 + R2** | Shows or hides the FPS counter, which counts from that moment |
| **L1 + R2** | Screenshot, saved as `ports/flatout2/screenshot_N.ppm` |
| **Select + Start** | Quit |

## What this port adds

- A 100% save in profile slot 8, named **TEST**: every car and every event unlocked, to try the whole game at once.
- A fourth chase camera, close to the car.
- Native FMV decoding through the console's FFmpeg (no FFmpeg, no videos).
- Sparks and particles filtered to hide a rendering glitch.

## Known problems

- **LAN play does not work.** The menu is there, but the feature is not finished.
- Online play is not available.
- Small graphical glitches can appear with the sparks when cars touch.
- At random, the sound of your own car can be missing from the start of a race. Most races are fine.
- When that happens on a car with jet engines, their flames are missing too.
- On RK3326 consoles, big crashes can drop the frame rate under 20 FPS for a moment.

## Roadmap

- **Performance:** lighter rendering (fewer draw calls, simpler shaders) and faster physics on small chips, where big crashes cost the most.
- **Audio:** fix the missing car sound.
- **Graphics:** fix the spark glitches.
- **LAN multiplayer** between consoles on the same Wi-Fi network.
- More consoles and screen formats, with your reports.
- A way to change the controls from the port.

## Version

Current version: **1.0.0**.

## Advanced settings

Optional file `ports/flatout2/fo2.env`, one `VARIABLE=value` per line:

| Variable | Effect |
|---|---|
| `FO2_CAP=60` | Frame rate limit |
| `FO2_RENDER=640x480` | Render size |
| `FO2_SWAP_ABXY=1` | Start with A/B and X/Y swapped |
| `FO2_KEEP_SETTINGS=1` | Keep the game's own graphics settings (the launcher otherwise forces the lowest) |
| `NFSU2_SHOW_FPS=1` | FPS counter on at launch |

## Source code

The code of the port is in this repository, under the MIT license (see `LICENSE`). To change it and rebuild it, see [BUILD.md](BUILD.md). The repository itself holds no game files or generated code; the binary in the Release is built from the game's executable.

## Credits

Bugbear Entertainment and Empire Interactive made the game. The recompilation runtime comes from the [NFSU2 recompiled port](https://github.com/manuelpllull/nfsu2-recompiled-r36s) (MIT), built with [pcrecomp](https://github.com/sp00nznet/pcrecomp) (MIT), with MojoShader (zlib) and stb (public domain or MIT). FFmpeg is the console's own. See `THIRD_PARTY_NOTICES.md`.

This project is not affiliated with or endorsed by the game's rights holders. FlatOut is a trademark of its respective owner.
