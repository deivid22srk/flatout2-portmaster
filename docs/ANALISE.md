# Análise do projeto e plano do port para Android

> Documento gerado durante o trabalho de port (fork `deivid22srk/flatout2-portmaster`).
> Data da análise: 2026-11-09, sobre o upstream `badcats72/flatout2-portmaster` v1.0.0 (commit `309425a`).

## 1. O que o projeto realmente é

Este **não** é um port PortMaster comum (binário Linux de terceiros + scripts). O FlatOut 2
(fechado, x86 Windows) foi **estaticamente recompilado** para AArch64 — o mesmo modelo dos ports
"N64 recomp": o código x86 do `FlatOut2.exe` e da `d3dx9_30.dll` foi levantado (lift) para C e
recompilado como código ARM64 nativo, ligado a um runtime próprio que emula o ambiente Win32 que
o jogo espera. O binário final (`fo2.aarch64`, ~95 MB com debug info) é distribuído na *Release*
do GitHub; o repositório **não** contém código gerado nem arquivos do jogo.

O runtime do port vem do projeto **NFSU2 recompiled** (MIT), construído com **pcrecomp** (MIT),
**MojoShader** (zlib, para traduzir shaders D3D9 → GLSL ES) e **stb** (domínio público/MIT).

## 2. Arquitetura atual (consoles ARM64 Linux)

```
port/FlatOut 2.sh                  ← script PortMaster (Knulli/dArkOS/ROCKNIX)
flatout2/fo2.aarch64               ← binário ET_EXEC AArch64, base fixa 0x1000000, BIND_NOW
flatout2/gamedata/                 ← ARQUIVOS DO USUÁRIO (FlatOut2.exe, d3dx9_30.dll,
                                     filesystem, data.bfs, langpack.bfs, Savegame/)
```

Camadas do runtime (código-fonte em `runtime/`):

| Camada | Arquivos | Função |
|---|---|---|
| **Host** | `runtime/host/main.cpp`, `sdl_gamepad.h` | ciclo de vida, janela SDL2 + contexto GLES2, eventos de gamepad, linha de comando (`--cap=N`, caminho do jogo via `argv[1]` ou `gamedata/` no CWD) |
| **Guest** | `runtime/guest/*.cpp/.inc/.h` | ambiente Win32 recriado: memória do hóspede (`guest_memory.h`), threads, arquivos, kernel, MSVCRT (`fo2_msvcrt.inc`), áudio (`guest_audio.inc` → `SDL_OpenAudioDevice`), entrada, rede |
| **D3D9→GLES** | `runtime/d3d9_gles/` | traduz chamadas e shaders D3D9 para **OpenGL ES 2/3** (MojoShader). O jogo **já renderiza em GLES nativamente** |
| **Nativo** | `runtime/native/` | substituições nativas: FMV via FFmpeg (dlopen de `libavcodec.so.56–61`), vídeo, widescreen |
| **Compat glibc** | `runtime/compat/glibc_compat.c`, `compat_start.S` | pinos de versão (`@GLIBC_2.17`) e trampolins para rodar em glibc 2.17–2.36+ |
| **SDK** | `runtime/sdk/include/` | cabeçalhos (SDL2, GLES2, EGL, dxvk D3D, recomp32, win32hle) |

### O que o binário importa (verificado com readelf no `fo2.aarch64` v1.0.0)

- **ELF 64 AArch64, `ET_EXEC` (não-PIE), imagem em 0x1000000, entry 0x1260f00, BIND_NOW.**
- `DT_NEEDED`: `libSDL2-2.0.so.0`, `libGLESv2.so.2`, `libm.so.6`, `libc.so.6`,
  `libpthread.so.0`, `libdl.so.2`, `librt.so.1`.
- **374 importações PLT**: 8 `GLOB_DAT` + 370 `JUMP_SLOT`. **Sem** `R_AARCH64_RELATIVE`,
  `IRELATIVE` ou `COPY` — relocação trivial de resolver manualmente.
- **TLS**: PT_TLS de ~18,3 KB (memsz 0x47d8), acessos por offset estático a partir de `TPIDR_EL0`
  (layout glibc: bloco TLS imediatamente antes do TCB).
- `INIT_ARRAY` de 32 bytes (4 construtores) a chamar após o carregamento.
- Importações por grupo: **~247 glibc** (stdio, stdlib, pthread, sockets, dirent, locale/`*_l`,
  wide chars, `__xstat`, `__libc_start_main`, `stdout/stderr` como dados), **40 SDL2**
  (`SDL_CreateWindow`, `SDL_GL_*`, `SDL_PollEvent`, `SDL_GameController*`, `SDL_OpenAudioDevice`,
  etc.), **~80 GLES2** (`gl*`, encaminháveis 1:1) e `dlopen/dlsym` (o presenter procura
  `libEGL.so.1`/`libEGL.so`/`libGLESv2.so*` por `RTLD_NOLOAD`).

### Dependências e o que é opcional

- **SDL2** (vídeo/janela, GL context, eventos, áudio, gamepad) — única lib dinâmica "grande".
- **EGL/GLES2** — obtidas via `SDL_GL_GetProcAddress` + fallback `dlopen`.
- **FFmpeg** — **opcional**: sem ele, FMVs não tocam ("no FFmpeg, no videos").
- **Dados do jogo** — fornecidos pelo usuário (GOG/Zoom Platform v2.1.0.9,
  `FlatOut2.exe` MD5 `1f8cae2b47276ca134750ba438aa35ca`), em `gamedata/` ao lado do binário
  (ou via `argv[1]`). `gamecontrollerdb.txt` é opcional e procurado em caminhos fixos.

## 3. Por que Android não é "só executar"

O binário é um executável **Linux/glibc**: precisa do `ld-linux-aarch64.so.1`, do glibc e de um
SDL2 Linux (X11/KMS/ALSA) — nada disso existe no Android (bionic, `ANativeWindow`, AAudio).
Recompilar o hóspede não é possível sem o `FlatOut2.exe` de cada usuário + cadeia pcrecomp no
Windows. As alternativas clássicas (Termux/proot, X11 server) exigem apps extras e não entregam
um APK autônomo.

**Decisão de arquitetura (registrada):** carregar o `fo2.aarch64` **dentro do processo do app**,
com um **carregador ELF próprio** (o binário é ET_EXEC com apenas 378 relocações de GOT — caso
ideal) e **shims glibc→bionic + SDL2→Android**:

1. **Loader** (`app/src/main/cpp/loader/`): mapeia os PT_LOAD nos endereços fixos, copia TLS,
   monta TCB glibc (com o bloco TLS do exe imediatamente antes), resolve as 378 relocações
   contra a tabela de shims (+ `dlsym` como fallback), chama `INIT_ARRAY` e transfere o controle
   para a thread principal do hóspede numa thread `clone()` dedicada com stack próprio.
2. **Shims glibc** (`compat/`): os ~247 símbolos glibc encaminhados para bionic/syscall;
   casos especiais implementados à mão: `__libc_start_main`, `__ctype_b_loc`/`__ctype_*_loc`
   (tabelas no formato glibc geradas), funções `*_l` (locale C), `__xstat`, `timer_create`
   (fallback por thread), `dlopen` (mapeia `libEGL.so.1`→`libEGL.so`, `libavcodec.so.NN` →
   libs do hóspede), `stdout/stderr` (FILE* reais), `copy_file_range`/`sendfile` (syscall).
3. **Shim SDL2** (`sdl_shim/`): **43 funções** — vídeo via `ANativeWindow` + EGL direto
   (janela da SurfaceView do app), áudio via **OpenSL ES** (callback em thread hóspede),
   eventos sintetizados (`SDL_PollEvent`) alimentados por JNI (toque + gamepad Android),
   gamepad mapeado para `SDL_GameController`. O presenter do port se satisfaz com
   `SDL_GL_GetProcAddress` (o caminho EGL "surfaceless" é desligado por padrão no Android).
4. **App Android** (`android/`): Gradle (Kotlin DSL) + CMake, `minSdk 26`,
   `targetSdk 34` (o hóspede roda *no processo do app* via mmap/PROT_EXEC anônimo —
   permitido a apps modernos, sem `exec()`; armazenamento usa o diretório do app,
   sem permissões), paisagem fixa, tela imersiva.

Fluxo do APK: `BootActivity` (verifica `gamedata/`, MD5 do exe, instruções) → `GameActivity`
(SurfaceView) → ponte JNI → loader carrega `fo2.aarch64` de `files/port/` → hóspede roda na
thread própria → SDL shim renderiza na Surface e injeta eventos.

## 4. Arquivos do jogo no Android

- Caminho: `Android/data/com.deivid22srk.flatout2/files/gamedata/` (mesma estrutura do PortMaster:
  `FlatOut2.exe`, `d3dx9_30.dll`, `filesystem`, `data.bfs`, `langpack.bfs` + `zoom_platform.bfs`
  se existir). Acessível pelo USB/gerenciador de arquivos, sem root.
- O binário `fo2.aarch64` **não fica no repositório** (regra do projeto) e vem:
  (a) embutido no APK pelo CI, baixado da Release pública do upstream no momento do build;
  ou (b) copiado manualmente pelo usuário para `Android/data/.../files/port/fo2.aarch64`.
- **Nenhum arquivo comercial do jogo entra no repositório** (`.gitignore` cobre `*.bfs`,
  `FlatOut2.exe`, `d3dx9_30.dll`, o RAR auxiliar e artefatos de build).

## 5. Plano de trabalho (executado neste fork)

1. `docs/ANALISE.md` (este arquivo) — análise e plano.
2. Projeto Android completo em `android/` com Gradle (Kotlin DSL) + CMake:
   loader ELF, shims, SDL shim, atividades Java, controles de toque, gamepad, imersivo.
3. `.github/workflows/build.yml`: JDK 17, SDK/NDK, cache Gradle, `assembleDebug`/`assembleRelease`,
   download do binário da release upstream no build, upload do APK como artifact.
4. README atualizado (compilar, instalar, arquivos do jogo, limitações).

## 6. Riscos e limitações conhecidas do port Android

- **Não validado em hardware real** durante a geração deste fork (sem aparelho/emulador no
  ambiente de build): o carregador e os shims foram escritos contra a superfície real do
  binário (readelf acima) e compilam no CI, mas a primeira execução em aparelho pode exigir
  ajustes (log de diagnóstico integrado via `adb logcat -s FO2`).
- Mapeamento do hóspede em 0x1000000: se algum driver mapear essa região, o loader reporta
  erro claro no log.
- FMVs: desligados por padrão (sem FFmpeg no APK). Pode ser habilitado depois com libs FFmpeg
  bionic carregadas via shim `dlopen` (nomes `.so.NN` mapeados).
- LAN/multiplayer: já não funciona no upstream; segue igual.
- `targetSdk 34`: instalação manual (sideload); a Play Store exigiria revisão adicional —
  decisão registrada acima.

## 7. Arquivo auxiliar (buzzheavier) — verificação

O arquivo `fla2.rar` (2,8 GB) apontado como material auxiliar **não é um pacote de
dependências**: é o jogo FlatOut 2 completo, repack do site "SteamGG.net" (GOG repackeado,
fonte não-oficial/pirata). Dois pontos:

1. **Inutilizável para este port**: o `FlatOut2.exe` do pacote tem MD5 `40078c35de1366488d7c3dc761008cd4`,
   mas o port exige a versão DRM-free GOG/Zoom v2.1.0.9 (MD5 `1f8cae2b47276ca134750ba438aa35ca`);
   o launcher do port recusa o executável. Os `.bfs` (`fo2a/fo2b/fo2c/patch1`) seguem um
   layout diferente do esperado (`data.bfs`/`langpack.bfs`).
2. **Licença**: por regra do projeto (e do fork), nenhum arquivo comercial entra no
   repositório. O pacote foi apenas inspecionado localmente e descartado; os dados do jogo
   devem vir da sua própria cópia GOG/Zoom.
