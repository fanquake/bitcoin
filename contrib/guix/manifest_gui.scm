(use-modules (gnu packages bison)
             ((gnu packages compression) #:select (xz zip))
             ((gnu packages installers) #:select (nsis-x86_64))
             (gnu packages ninja)
             (gnu packages pkg-config)
             ((gnu packages python-xyz) #:select (python-lief))
             (guix build-system python)
             (guix git-download)
             (guix packages)
             (toolchains))

(packages->manifest
 (append
  (list ;; Compression and archiving
        xz
        ;; Build tools
        ninja
        ;; Tests
        python-lief)
  (let ((target (getenv "HOST")))
    (cond ((string-suffix? "-mingw32" target)
           (list zip
                 nsis-x86_64))
          ((string-contains target "-linux-")
           (list bison
                 (make-bitcoin-cross-toolchain target) ;; glibc 2.31 based
                 pkg-config))
          ((string-contains target "darwin")
           (list zip))
          (else '())))))
