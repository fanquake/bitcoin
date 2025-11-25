package=libxkbcommon
$(package)_version=1.14.0-beta1
$(package)_download_path=https://github.com/xkbcommon/$(package)/archive/refs/tags/
$(package)_file_name=xkbcommon-$($(package)_version).tar.gz
$(package)_sha256_hash=7297aa400484216fd8af96531c6109a70327610d74b97ac44f22d49faaddbae5
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
