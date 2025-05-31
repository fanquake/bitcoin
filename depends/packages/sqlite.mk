package=sqlite
$(package)_version=3500000
$(package)_download_path=https://sqlite.org/2025/
$(package)_file_name=sqlite-autoconf-$($(package)_version).tar.gz
$(package)_sha256_hash=3bc776a5f243897415f3b80fb74db3236501d45194c75c7f69012e4ec0128327

define $(package)_set_vars
$(package)_config_opts = --disable-shared --disable-readline --disable-rtree
$(package)_config_opts += --disable-fts4 --disable-fts5
$(package)_config_opts_debug += --debug
$(package)_cppflags += -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
$(package)_cppflags += -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_JSON -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
$(package)_cppflags += -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install-headers install-lib
endef

define $(package)_postprocess_cmds
  rm -rf lib/pkgconfig && \
  rm -rf bin share
endef
