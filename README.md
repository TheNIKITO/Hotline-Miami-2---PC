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
- Volume da música e dos efeitos nas opções do jogo, inclusive enquanto o som toca.
- Launcher com seletor de pasta, botão JOGAR e atualização automática pelos Releases do GitHub.

Limitações conhecidas:
- O jogo ainda pode fechar sozinho logo ao abrir, bem mais raramente desde a v0.2 (nos testes,
  13 de 17 aberturas seguidas foram de primeira); o launcher tenta de novo automaticamente.
- O balanço (pan) dos sons ainda é ignorado.
- Ainda não há opções de tela cheia, janela e resolução.
- Só foi testado numa máquina e numa versão do jogo; outras versões podem não funcionar.

## Como usar (pacote pronto)

1. Em **Releases**, baixe `hm2-windows.zip` e **extraia** numa pasta (não abra de dentro do zip).
2. Abra `Hotline Miami 2.exe`, clique em **Procurar** e escolha a pasta do **seu** jogo
   (a que contém `eboot.bin`, `sce_module`, `sce_sys` e os arquivos `.wad`).
3. Clique em **JOGAR**.

**Atualizações:** ao abrir, o launcher mostra a versão instalada e confere o último Release aqui no
GitHub (barra de progresso). Se houver uma versão nova, o **JOGAR** fica bloqueado e o botão
**Atualizar** baixa o zip, instala por cima e reabre o launcher; seus saves, a pasta do jogo e as
configurações continuam. Sem internet, o launcher avisa e libera o **JOGAR** mesmo assim.
Quem tem a v0.1 precisa baixar a v0.2 uma vez à mão (a v0.1 não tinha o atualizador).

Requisitos: Windows 10 (1903 ou mais novo) ou 11, placa de vídeo com **Vulkan 1.3** e driver
atualizado, processador recente com AVX.

Se algo der errado, feche programas pesados (Discord, navegador) e veja o arquivo
`launcher_last_run.log` na pasta do programa; ele mostra o que o jogo escreveu na última execução.
Para testar só o som, rode com a variável `NGS2_BEEP=1` (toca um bipe de 440 Hz no início).

## O que este fork adiciona

| Arquivo | Para quê |
| --- | --- |
| `src/runtime_ngs2.c` | Áudio (libSceNgs2): efeitos PCM16 e música ATRAC9 em streaming (LibAtrac9), com loop; volume por voz e saídas separadas de música e efeitos |
| `src/runtime_pngdec.c` | Decodificador de PNG próprio (`scePngDec*`) |
| `src/runtime_stubs.c` | Respostas para funções de sistema que o jogo importa (rede, diálogos, threads) |
| `src/import_names.inc` | Nomes das importações do jogo |
| `launcher.py` | Janela com seletor de pasta, botão JOGAR, reabertura automática, versão e atualização pelo GitHub |
| `VERSION` | Versão do pacote (a tag do Release, ex.: `v0.2`), mostrada e comparada pelo launcher |
| `patches/Hotline_Miami_2_Wrong_Number.xml` | Correção de uma corrida no próprio jogo: uma thread de trabalho começava antes da sua fila existir e fazia o jogo fechar ao abrir (aplicada sozinha) |
| `src/runtime_memory.c` (base) | A memória do jogo é reservada e só confirmada quando usada, em vez de 6 GB de uma vez na abertura |
| `src/runtime_mutex.c` (base) | Mutexes e condições destruídos ficam um tempo em quarentena antes de liberados (o jogo às vezes ainda os usava) |
| `packaging/windows/package_hm2.sh` | Gera o `hm2-windows.zip` distribuível, com o `VERSION` |

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

Para lançar uma versão: ponha a tag nova em `VERSION` (ex.: `v0.3`), gere o zip e publique um
Release com **essa mesma tag** e o `hm2-windows.zip` anexado. É o que o launcher dos jogadores
procura para oferecer a atualização.

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
effects and ATRAC9 music, in-game music/SFX volume, launcher with updates from GitHub Releases.
Known issues: a rarer crash at startup (the launcher retries), no sound panning, no
window/resolution options yet. GPL-2.0-or-later.
