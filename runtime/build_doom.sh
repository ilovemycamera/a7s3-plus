#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CLANG="${CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang || command -v clang)}"
LLD="${LLD:-$(command -v /opt/homebrew/opt/lld/bin/ld.lld || command -v /opt/homebrew/bin/ld.lld || command -v ld.lld)}"
OBJCOPY="${OBJCOPY:-$(command -v /opt/homebrew/opt/llvm/bin/llvm-objcopy || command -v llvm-objcopy || true)}"

mkdir -p doom_build
rm -f doom_build/*.o

DOOM_SRCS=(
  dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c
  d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c
  hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c
  i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c
  m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c
  p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c
  p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c
  p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c
  r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c
  s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c
  w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c
)

for f in "${DOOM_SRCS[@]}"; do
  "$CLANG" --target=aarch64-linux-gnu -mno-outline-atomics -fPIC -O2 \
    -isystem linux_inc \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -Dexit=a7s3_doom_exit \
    -Dprintf=a7s3_printf -Dfprintf=a7s3_fprintf -Dvfprintf=a7s3_vfprintf \
    -Dputs=a7s3_puts -Dputchar=a7s3_putchar -Dfflush=a7s3_fflush \
    -fno-asynchronous-unwind-tables -Wno-deprecated-non-prototype -Wno-unused-result \
    -c "doomgeneric/doomgeneric/$f" -o "doom_build/${f%.c}.o"
done

"$CLANG" --target=aarch64-linux-gnu -mno-outline-atomics -fPIC -O2 \
  -isystem linux_inc \
  -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
  -c doomgeneric_a7s3.cpp -o doom_build/doomgeneric_a7s3.o

"$LLD" -shared -Bsymbolic --hash-style=both -soname libDoomA7s3v17.so -init _init \
  doom_build/*.o \
  -o doom_a7s3.so

"$CLANG" --target=aarch64-linux-gnu -mno-outline-atomics -fPIC -O2 -nostdlib \
  -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
  -c doom_loader.cpp -o doom_build/doom_loader.o

"$LLD" -shared --hash-style=both -soname libJit.so -init _init \
  doom_build/doom_loader.o \
  -o doom_loader.so

if [ -n "$OBJCOPY" ]; then
  "$OBJCOPY" --remove-section=.comment doom_a7s3.so
  "$OBJCOPY" --remove-section=.comment doom_loader.so
fi

gzip -9 -n -c doom_a7s3.so > doom_a7s3.so.gz
rm -rf doom_build doom_a7s3.so

for wad in ../DOOM1.WAD ../doom1.wad ../freedoom1.wad DOOM1.WAD doom1.wad freedoom1.wad; do
  if [ -f "$wad" ]; then
    echo "Compressing $wad -> doom1.wad.gz"
    gzip -9 -c "$wad" > doom1.wad.gz
    break
  fi
done

ls -lh doom_a7s3.so.gz doom_loader.so
