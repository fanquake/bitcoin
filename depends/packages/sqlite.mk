package=sqlite
$(package)_version=3500300
$(package)_download_path=https://sqlite.org/2025/
$(package)_file_name=sqlite-autoconf-$($(package)_version).tar.gz
$(package)_sha256_hash=ec5496cdffbc2a4adb59317fd2bf0e582bf0e6acd8f4aae7e97bc723ddba7233

define $(package)_set_vars
$(package)_config_opts = --disable-shared --disable-readline --disable-rtree
$(package)_config_opts += --disable-fts4 --disable-fts5
$(package)_config_opts_debug += --debug
$(package)_cppflags += -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
$(package)_cppflags += -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_JSON -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
$(package)_cppflags += -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT
$(package)_cppflags += -DSQLITE_OMIT_LOAD_EXTENSION
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE) libsqlite3.a
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install-headers install-lib
endef
