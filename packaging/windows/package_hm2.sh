#!/usr/bin/env bash
# Builds dist/hm2-windows.zip: "Hotline Miami 2.exe" (launcher frozen with PyInstaller, no Python
# needed by players), bin/gow3-probe.exe with the MSYS2 CLANG64 DLLs it needs, run.py, scripts, patches.
# No game files and nothing a player made may ship. Run from an MSYS2 CLANG64 shell after `bash build.sh`.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/gow3-probe.exe && -f out/gow3-gpu-capabilities.exe && -f launcher.py ]] || { echo 'Build first (bash build.sh) and put launcher.py here.' >&2; exit 1; }
# VERSION is the tag of the GitHub release this zip goes to (v0.2): the launcher shows it and
# compares it with the latest release to offer updates.
version=$(tr -d '\r\n' < VERSION 2>/dev/null || true)
[[ $version =~ ^v[0-9]+(\.[0-9]+)*$ ]] || { echo 'VERSION must hold the release tag, e.g. v0.2' >&2; exit 1; }

# A Windows Python (python.org, not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org): winget install -e --id Python.Python.3.12  (then open a NEW shell) or WINPYTHON=path/to/python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller iced-x86==1.21.0

# run.py and its scripts import many standard modules that PyInstaller cannot see from the launcher:
# read their imports and bundle every standard module they use.
hidden=()
while IFS= read -r module; do
    [[ -n $module ]] && hidden+=(--hidden-import "$module")
done < <(out/pyenv/Scripts/python.exe - run.py scripts <<'PY' | tr -d '\r'
import ast, importlib.util, pathlib, sys
files = []
for a in sys.argv[1:]:
    p = pathlib.Path(a)
    files += [p] if p.is_file() else sorted(p.rglob("*.py"))
local = {f.stem for f in files}
mods = {"runpy", "ctypes.wintypes", "iced_x86"}
for f in files:
    try:
        tree = ast.parse(f.read_text(encoding="utf-8"))
    except Exception:
        continue
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            mods.update(x.name for x in n.names)
        elif isinstance(n, ast.ImportFrom) and n.level == 0 and n.module:
            mods.add(n.module)
            for x in n.names:
                try:
                    if importlib.util.find_spec(n.module + "." + x.name):
                        mods.add(n.module + "." + x.name)
                except Exception:
                    pass
for m in sorted(mods):
    top = m.split(".")[0]
    if top not in local and (top in sys.stdlib_module_names or top == "iced_x86"):
        print(m)
PY
)
echo "Modulos extras incluidos: $(( ${#hidden[@]} / 2 ))"
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name "Hotline Miami 2" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher.py")"

dest=out/stage/hm2-windows
rm -rf -- out/stage
mkdir -p "$dest/bin"
cp -r "out/pyi-dist/Hotline Miami 2/." "$dest/"
for exe in gow3-probe.exe gow3-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/gow3-probe.exe" "$dest/bin/gow3-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
cp run.py LICENSE README.md VERSION "$dest/"
cat > "$dest/LEIA-ME.txt" <<EOF
Hotline Miami 2: Wrong Number - port nativo para PC (nao oficial)
=================================================================
O jogo NAO esta incluido. Voce precisa da sua propria copia do jogo (pasta com eboot.bin).

1. Extraia este zip numa pasta qualquer (nao execute de dentro do zip).
2. Abra "Hotline Miami 2.exe", clique em Procurar e escolha a pasta do SEU jogo.
3. Clique em JOGAR.

Se o jogo fechar sozinho, a opcao "Abrir de novo automaticamente" tenta outra vez.
Feche programas pesados (Discord, navegador) antes de jogar. Placa de video com Vulkan e necessaria.

Licenca: GPL-2.0 (arquivo LICENSE). Codigo-fonte: ${HM2_SOURCE_URL:-COLOQUE-AQUI-O-LINK-DO-SEU-REPOSITORIO}
Baseado em gow3_pc (FelipePrado0), que e derivado de bloodborne_pc / shadPS4.
EOF
find "$dest" -name __pycache__ -prune -exec rm -r {} +
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname gow3.ini -o -iname settings.json \
    -o -iname mods.json -o -iname patches.json -o -iname 'nvngx_dlss*.dll' \
    -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.wad' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
mkdir -p dist
rm -f dist/hm2-windows.zip
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path hm2-windows -DestinationPath ../../dist/hm2-windows.zip")
du -sh "$dest" dist/hm2-windows.zip
echo "Pronto: dist/hm2-windows.zip (versao $version: publique no Release com a tag $version)"
