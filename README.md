## BlackMagic devices tools

Initially developed as an example integration between libavformat and the
bmd sdk, it ended up being a set of useful tools to use the BlackMagic Devices
decklink cards on Linux and macOS.

Thanks to TodoStreaming sponsoring its early development.

## Build instructions

In order to build it just clone/unpack this on your Sample directory from the
DeckLink SDK and then issue "make". If you have [Libav][1] and [pkg-config][2] or
[pkgconf][3] installed it will build fine.

    Make sure you are using at least Libav10 otherwise it will not build.

You can build it out of the Sample tree by issuing

```sh
make SDK_PATH=/path/to/the/bmd/include
```

### macOS Support

Should work out of box.

### Windows Support

The tools do not build on Windows currently, supporting it would either
require a working widl support in mingw64 or access to the native tools.

Patch and/or sponsorship welcome.

## Usage

```sh
./bmdcapture -C 1 -m 2 -F nut -o strict=experimental:syncpoints=none -f pipe:1 | avconv -vsync passthrough -y -i - <your options here>
```

-C select the capture device if more than one is present.

-F define the container format, I suggest using nut.

-f output file name, any libavformat compatible url is supported.

-m specific modeline, resolution+framerate

-o pass AVFormat AVOptions (expert)

> NOTE: make sure you are processing frames capture in real time or be
prepared to end up using all your memory quite quickly, HD raw data
fills up memory quickly.

```sh
avconv -vsync 1 -i <source> -c:v rawvideo -pix_fmt uyvy422 -c:a pcm_s16le -ar 48000 -f nut -f_strict experimental -syncpoints none - | ./bmdplay -f pipe:0
```

> NOTE: The default NUT syncpoint strategy uses additional memory and could
consume more memory than expected.


## Support

The github [issue tracker](https://github.com/lu-zero/bmdtools/issues) can
be used to track bugs and feature requests.

### Contact

You can directly contact me either at

lu_zero@gentoo.org or luca.barbato@luminem.it

### Paid Support

Paid support is offered, contact info@luminem.it for details.

[1]: http://libav.org
[2]: http://www.freedesktop.org/wiki/Software/pkg-config/
[3]: https://github.com/pkgconf/pkgconf
