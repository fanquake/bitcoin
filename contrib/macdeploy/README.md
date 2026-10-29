# MacOS Deployment

The `macdeployqtplus` script should not be run manually. Instead, after building as usual:

```bash
make deploy
```

When complete, it will have produced `Bitcoin-Core.zip`.

## SDK Extraction

### Step 1: Obtaining the Xcode `.xip`

A free Apple Developer Account is required to proceed.

Our macOS SDK can be extracted from
[Xcode_26.1.1_Apple_silicon.xip](https://download.developer.apple.com/Developer_Tools/Xcode_26.1.1/Xcode_26.1.1_Apple_silicon.xip).

Alternatively, after logging in to your account go to 'Downloads', then 'More'
and search for [`Xcode 26.1.1`](https://developer.apple.com/download/all/?q=Xcode%2026.1.1).

An Apple ID and cookies enabled for the hostname are needed to download this.

The `sha256sum` of the downloaded XIP archive should be `f4c65b01e2807372b61553c71036dbfef492d7c79d4c380a5afb61aa1018e555`.

### Step 2: Generating the SDK tarball from the `.xip`

To generate the SDK, run the script [`gen-sdk.py`](./gen-sdk.py) with the
path to the `.xip` as the first argument.

```bash
./contrib/macdeploy/gen-sdk.py '/path/to/Xcode_26.1.1_Apple_silicon.xip'
```

The generated archive should be: `Xcode-26.1.1-17B100-extracted-SDK-with-libcxx-headers.tar`.
The `sha256sum` should be `407e390f6f6e14bbf3e2534b92a1be41e80ae28582443389efbb838af8507324`.

## Deterministic macOS App Notes

macOS Applications are created on Linux using a recent LLVM.

All builds must target an Apple SDK. These SDKs are free to download, but not redistributable.
See the SDK Extraction notes above for how to obtain it.

The Guix build process has been designed to avoid including the SDK's files in Guix's outputs.
All interim tarballs are fully deterministic and may be freely redistributed.

Using an Apple-blessed key to sign binaries is a requirement to produce (distributable) macOS
binaries. Because this private key cannot be shared, we'll have to be a bit creative in order
for the build process to remain somewhat deterministic. Here's how it works:

- Builders use Guix to create an unsigned release. This outputs an unsigned ZIP which
  users may choose to bless, self-codesign, and run. It also outputs an unsigned app structure
  in the form of a tarball.
- The Apple keyholder uses this unsigned app to create a detached signature, using the
  included script. Detached signatures are available from this [repository](https://github.com/bitcoin-core/bitcoin-detached-sigs).
- Builders feed the unsigned app + detached signature back into Guix, which combines the
  pieces into a deterministic ZIP.
