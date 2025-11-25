package=libxkbcommon
$(package)_version=1.13.2
$(package)_download_path=https://github.com/xkbcommon/$(package)/archive/refs/tags/
$(package)_file_name=xkbcommon-$($(package)_version).tar.gz
$(package)_sha256_hash=acc4d5f7c3cbba5f9f8d08d8bdbeede84ecede46792f47929aa9321873385528
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
