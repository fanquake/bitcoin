package=libxkbcommon
$(package)_version=1.12.3
$(package)_download_path=https://github.com/xkbcommon/$(package)/archive/refs/tags/
$(package)_file_name=xkbcommon-$($(package)_version).tar.gz
$(package)_sha256_hash=b08bbd1ac6faef2b80774fbe22a0dda5563ef77480ad86677b51798bf0afef6d
$(package)_dependencies=libxcb libXau

define $(package)_config_cmds
  meson setup build \
    -Denable-docs=false \
    -Denable-wayland=false \
    -Denable-x11=true \
    -Denable-xkbregistry=false
endef

define $(package)_build_cmds
  meson compile -C build
endef

define $(package)_stage_cmds
  DESTDIR=$($(package)_staging_dir) meson install -C build
endef
