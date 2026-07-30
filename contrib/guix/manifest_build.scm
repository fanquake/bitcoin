(use-modules (gnu packages)
             ((gnu packages bash) #:select (bash-minimal))
             ((gnu packages cmake) #:select (cmake-minimal-4))
             (gnu packages commencement)
             ((gnu packages compression) #:select (gzip))
             (gnu packages cross-base)
             (gnu packages gcc)
             (gnu packages llvm)
             ((gnu packages python) #:select (python-minimal))
             ((gnu packages version-control) #:select (git-minimal))
             (guix download)
             (guix gexp)
             (guix packages)
             (toolchains))

(packages->manifest
 (append
  (list ;; The Basics
        bash-minimal
        which
        coreutils-minimal
        ;; File(system) inspection
        grep
        findutils
        ;; File transformation
        patch
        sed
        ;; Compression and archiving
        tar
        gzip
        ;; Build tools
        cmake-minimal-4
        gnu-make
        ;; Scripting
        python-minimal ;; (3.12)
        ;; Git
        git-minimal)
  (let ((target (getenv "HOST")))
    (cond ((string-suffix? "-mingw32" target)
           (list gcc-toolchain-16
                 (make-mingw-pthreads-cross-toolchain target)))
          ((string-contains target "-linux-")
           (list gcc-toolchain-16
                 (list gcc-toolchain-16 "static")))
          ((string-contains target "darwin")
           (list clang-toolchain-19
                 libcxx ;; 19.1.7
                 lld-19))
          (else '())))))
