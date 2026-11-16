# Decoder firmware

All three images are 16,382 bytes. The CLI uses `v4l-cx231xx-avcore-01.fw` by default and reads a different file when given `--firmware PATH`. These hashes identify the copies in this workspace.

| Image | Provenance | SHA-256 |
| --- | --- | --- |
| `v4l-cx231xx-avcore-01.fw` | Linux firmware archive at `29a5e75e5ddd7ffc48d9af47c08a8a692ea8b8d8`. Included with the source release and Monitor under the Conexant terms. | `09a39c139d8e47ebfa2e7f64472e7165dff66359277ca02bcfdcd79f515764ef` |
| `hcw10mlD.rom` | Hauppauge Driver10 build 39299. The driver's INF lists it for USB `2040:c200`. | `c2a75fc710f51c778abe7c7e8b54ed5686b17811dd203d1de3070d3df70d70f6` |
| `merlinFW.rom` | Hauppauge WinTV driver build 43164, `hcwembda` package member. Capture compatibility with `2040:c200` is unverified. | `5cf05375bf41c995099c11918422ad01d4dda2d9fbda098c543f81322453dff4` |

The packaged Linux image is governed by [LICENSE.conexant](LICENSE.conexant), separate from the project's GPL.

- [Linux Firmware Source](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/29a5e75e5ddd7ffc48d9af47c08a8a692ea8b8d8/v4l-cx231xx-avcore-01.fw)
- [Hauppauge Driver Downloads](https://hauppauge.com/pages/support/support_usblive2.html).
