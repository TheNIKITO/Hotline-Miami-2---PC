# Hotline Miami 2: Wrong Number — PC (port nativo, não oficial)

Port experimental de **Hotline Miami 2: Wrong Number** (PS4) para **Windows 10/11 (64 bits)**.
O executável original do jogo roda nativamente no processador (o PS4 é x86-64), as bibliotecas do
sistema são reimplementadas pelo projeto e a parte gráfica é traduzida para **Vulkan** com o
núcleo de vídeo do shadPS4. É um fork do `gow3_pc`.

> **Aviso:** projeto de estudo, sem ligação com os criadores ou a publicadora do jogo.
> **Nenhum arquivo do jogo está incluído.** Você precisa da sua própria cópia, já extraída.

## Estado atual

Testado em Windows 11 com uma RTX 5060 Ti, com esta versão do jogo:

| | |
| --- | --- |
| Título | Hotline Miami 2: Wrong Number |
| ID do título | `CUSA00368` (EUA) |
| ID do conteúdo | `UP3643-CUSA00368_00-HOTLINEMIAMI2000` |
| Versão do jogo (`APP_VER`) | `01.00` |
| Versão master (`VERSION`) | `01.01` |
| SHA-256 do `eboot.bin` | `044f0cbb2ca62e7782b31004b40c9f8a71e1d2e8fd981e255c3399743bf313c4` |

Para conferir a sua cópia, rode no PowerShell, dentro da pasta do jogo:
`Get-FileHash .\eboot.bin -Algorithm SHA256`. Se o resultado for igual ao da tabela (o PowerShell mostra
em maiúsculas), é a mesma versão.

Funciona:
- O jogo abre, renderiza e responde ao controle.
- Efeitos sonoros e **música** (ATRAC9, decodificada em tempo real, com loop).
- Launcher com seletor de pasta e botão JOGAR.

Limitações conhecidas:
- O jogo às vezes fecha sozinho logo ao abrir (o launcher tenta de novo automaticamente).
- Volume e balanço (pan) dos sons ainda são ignorados.
- Ainda não há opções de tela cheia, janela e resolução.
- Só foi testado numa máquina e numa versão do jogo; outras versões podem não funcionar.

## Como usar (pacote pronto)

1. Em **Releases**, baixe `hm2-windows.zip` e **extraia** numa pasta (não abra de dentro do zip).
2. Abra `Hotline Miami 2.exe`, clique em **Procurar** e escolha a pasta do **seu** jogo
   (a que contém `eboot.bin`, `sce_module`, `sce_sys` e os arquivos `.wad`).
3. Clique em **JOGAR**.

Requisitos: Windows 10 (1903 ou mais novo) ou 11, placa de vídeo com **Vulkan 1.3** e driver
atualizado, processador recente com AVX.

Se algo der errado, feche programas pesados (Discord, navegador) e veja o arquivo
`launcher_last_run.log` na pasta do programa; ele mostra o que o jogo escreveu na última execução.
Para testar só o som, rode com a variável `NGS2_BEEP=1` (toca um bipe de 440 Hz no início).

## O que este fork adiciona

| Arquivo | Para quê |
| --- | --- |
| `src/runtime_ngs2.c` | Áudio (libSceNgs2): efeitos PCM16 e música ATRAC9 em streaming (LibAtrac9), com loop |
| `src/runtime_pngdec.c` | Decodificador de PNG próprio (`scePngDec*`) |
| `src/runtime_stubs.c` | Respostas para funções de sistema que o jogo importa (rede, diálogos, threads) |
| `src/import_names.inc` | Nomes das importações do jogo |
| `launcher.py` | Janela com seletor de pasta, botão JOGAR e reabertura automática |
| `packaging/windows/package_hm2.sh` | Gera o `hm2-windows.zip` distribuível |

## Compilar a partir do código

Instale o [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), abra o shell **MSYS2 CLANG64**:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}

git submodule update --init --recursive
bash build.sh
GOW3_GAME_DIR=C:/caminho/da/pasta/do/jogo python run.py     # executa o jogo
python launcher.py                                          # ou use a janela
```

Para gerar o zip (precisa do Python do python.org: `winget install -e --id Python.Python.3.12`):

```bash
export HM2_SOURCE_URL="https://github.com/TheNIKITO/Hotline-Miami-2---PC"
bash packaging/windows/package_hm2.sh
```

A documentação técnica do projeto base (como o loader, o runtime e a GPU funcionam) está em
`docs/` e em `packaging/windows/README.md`.

## Créditos e licença

Este projeto é derivado de **gow3_pc** (FelipePrado0), que se baseia no port para Windows de
**Supermedo** e no **bloodborne_pc** de **deadinside28**, e usa o núcleo de vídeo do
**shadPS4**. Veja `docs/README-gow3-original.md` para a lista completa de créditos e bibliotecas.
Decodificação ATRAC9: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT).

Licença: **GPL-2.0 ou posterior** (arquivo `LICENSE`).

---

## English summary

Unofficial, experimental native PC port of *Hotline Miami 2: Wrong Number* (PS4 build `CUSA00368`,
USA, app version 01.00, `eboot.bin` SHA-256 `044f0cbb…bf313c4`) for 64-bit Windows 10/11. The game's x86-64 executable runs natively; system libraries are
reimplemented and graphics go through Vulkan (shadPS4 video core). It is a fork of `gow3_pc`.
**No game files are included**: you need your own copy. Working: rendering, controller, sound
effects and ATRAC9 music, launcher. Known issues: occasional crash at startup (the launcher
retries), no volume/pan, no window/resolution options yet. GPL-2.0-or-later.
