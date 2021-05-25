package=native_musl_cross_make
$(package)_version=3635262e4524c991552789af6f36211a335a77b3
$(package)_download_path=https://github.com/richfelker/musl-cross-make/archive
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=musl-cross-make-$($(package)_version).tar.gz
$(package)_sha256_hash=60bed670d689d5c2164020960df36b80189dc5617e9763e023672f98a99d567e
$(package)_patches=config.mak

define $(package)_preprocess_cmds
  cp -f $($(package)_patch_dir)/config.mak config.mak && \
  sed -i.old "s/TARGET =/TARGET = $(host)/g" config.mak
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) install && \
  mkdir -p "$($(package)_staging_prefix_dir)"/ && \
  rm -rf output/share && \
  cp -rf output/* "$($(package)_staging_prefix_dir)"/
endef
