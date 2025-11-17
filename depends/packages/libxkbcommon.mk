package=libxkbcommon
$(package)_version=1.3.1
$(package)_download_path=https://github.com/xkbcommon/$(package)/archive/refs/tags/
$(package)_file_name=xkbcommon-$($(package)_version).tar.gz
$(package)_sha256_hash=8eda6782c6ed4b83296521f2f7e6bea88aba76d49c39fb4fce0f8d355a9181ce
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

define $(package)_postprocess_cmds
  rm lib/*.la
endef

