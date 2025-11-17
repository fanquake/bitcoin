package=libxkbcommon
$(package)_version=1.13.1
$(package)_download_path=https://github.com/xkbcommon/$(package)/archive/refs/tags/
$(package)_file_name=xkbcommon-$($(package)_version).tar.gz
$(package)_sha256_hash=aeb951964c2f7ecc08174cb5517962d157595e9e3f38fc4a130b91dc2f9fec18
$(package)_dependencies=libxcb libXau
$(package)_patches=meson_empty_vars.patch

define $(package)_config_cmds
  meson setup build \
    -Denable-docs=false \
    -Denable-wayland=false \
    -Denable-x11=true \
    -Denable-xkbregistry=false
endef

define $(package)_preprocess_cmds
  patch -p1 < $($(package)_patch_dir)/meson_empty_vars.patch
endef

define $(package)_build_cmds
  meson compile -C build
endef

define $(package)_stage_cmds
  DESTDIR=$($(package)_staging_dir) meson install -C build
endef
