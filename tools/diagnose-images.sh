#!/usr/bin/env bash
# Read-only diagnosis for mounted ISO/SFS disk images on the user's machine.
set -u
OUT="${1:-$HOME/rox-qt-imagenes-$(date +%Y%m%d-%H%M%S).txt}"
{
  printf 'ROX-Filer2-QT — diagnóstico de imágenes (solo lectura)\n'
  date -Is
  printf '\n--- Sesion y entorno ---\n'
  printf 'Usuario: %s UID: %s\n' "$(id -un)" "$(id -u)"
  printf 'DISPLAY=%s XDG_SESSION_TYPE=%s\n' "${DISPLAY:-}" "${XDG_SESSION_TYPE:-}"
  printf '\n--- Loop devices ---\n'
  if command -v losetup >/dev/null; then losetup -l -O NAME,BACK-FILE,RO 2>&1 || true; else echo 'Falta losetup'; fi
  printf '\n--- findmnt mountpoints ---\n'
  if command -v findmnt >/dev/null; then findmnt -rn -o SOURCE,TARGET,FSTYPE,OPTIONS 2>&1 || true; else echo 'Falta findmnt'; fi
  printf '\n--- /proc/self/mountinfo (ISO/SFS/FUSE/loop) ---\n'
  grep -Ei 'loop[0-9]+|squashfs|iso9660|udf|fuse|\.sfs|\.iso|\.img' /proc/self/mountinfo || true
  printf '\n--- lsblk ---\n'
  if command -v lsblk >/dev/null; then lsblk -o NAME,TYPE,FSTYPE,MOUNTPOINTS,BACK-FILE 2>&1 || lsblk -f 2>&1 || true; fi
  printf '\n--- ROX Qt image detector ---\n'
  if command -v rox-filer2-qt >/dev/null; then rox-filer2-qt --list-images 2>&1 || true; fi
  printf '\n--- ROX Qt version ---\n'
  if command -v rox-filer2-qt >/dev/null; then rox-filer2-qt --version 2>&1 || true; else echo 'rox-filer2-qt no esta en PATH'; fi
  printf '\n--- Opciones de imagen Qt ---\n'
  if command -v kreadconfig6 >/dev/null; then
    kreadconfig6 --file rox-filer2-qt.conf --group devices --key showImages 2>&1 || true
  fi
  for path in "${XDG_CONFIG_HOME:-$HOME/.config}/ROX-Filer2/rox-filer2-qt.conf" "$HOME/.config/ROX-Filer2/rox-filer2-qt.conf"; do
    if [[ -f "$path" ]]; then
      printf 'Config: %s\n' "$path"
      sed -n '/^\[devices\]/,/^\[/p; /^\[desktop\]/,/^\[/p' "$path" | grep -E '(^\[|Images=|showImages=|drivesEnabled=)' || true
      break
    fi
  done
} > "$OUT" 2>&1
printf 'Informe guardado: %s\n' "$OUT"
printf 'Revisalo antes de compartirlo: contiene nombres de archivos y rutas locales.\n'
