(use-modules (gnu packages)
             ((gnu packages bash) #:select (bash-minimal))
             ((gnu packages cmake) #:select (cmake-minimal-4))
             (gnu packages commencement)
             ((gnu packages compression) #:select (gzip))
             (gnu packages cross-base)
             (gnu packages gawk)
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
        diffutils
        findutils
        ;; File transformation
        patch
        gawk
        sed
        ;; Compression and archiving
        tar
        gzip
        ;; Build tools
        gcc-toolchain-15
        cmake-minimal-4
        gnu-make
        ;; Scripting
        python-minimal ;; (3.12)
        ;; Git
        git-minimal)
  (let ((target (getenv "HOST")))
    (cond ((string-suffix? "-mingw32" target)
           (list (make-mingw-pthreads-cross-toolchain "x86_64-w64-mingw32")))
          ((string-contains target "-linux-")
           (list (list gcc-toolchain-14 "static")))
          ((string-contains target "darwin")
           (list clang-toolchain-19
                 lld-19
                 (make-lld-wrapper lld-19 #:lld-as-ld? #t)))
          (else '())))))
