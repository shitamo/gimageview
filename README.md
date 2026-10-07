# GImage View README

* **For ver:** 0.2.27+gtk4

* **Last Modified:** 2026/10/07(Wed)

* **Created:** 2001/09/08(Sat)

## GTK 4 port (0.2.27+gtk4)

This version is a port of GImageView 0.2.27 to GTK 4, built with Meson (see `INSTALL`).

Removed from 0.2.27:

* The support for Susie plugins and lib/dllloader.

* The bundled libexif and MD5 code (the system libexif and GLib are used).

* The GTK 1.2 / Imlib and GTK+ 2 builds.

Main additions (see `NEWS` for all changes):

* Movie and audio playback with GStreamer, and movie thumbnails.

* Archives are read with libarchive (no external programs).

* New loaders for Sun raster, WBMP, PNM and XPM; WebP, AVIF, HEIF, JPEG XL and other formats are read when their gdk-pixbuf loaders are installed.

* Printing, setting an image as the desktop wallpaper, and remembering the rotation of each image.

* The mouse side buttons (buttons 8 and 9) can be assigned.

* Color scheme: the desktop theme, light or dark.

* An English manual; the manuals have screenshots of this version.

## Contents

0. [What's this?](#0-whats-this)

1. [Introduction](#1-introduction)

2. [Requirements](#2-requirements)

3. [Download](#3-download)

4. [Install](#4-install)

5. [Usage](#5-usage)

6. [ChangeLog of README](#6-changelog-of-readme)

7. [License](#7-license)

8. [Remarks](#8-remarks)

## 0. What's this?

GImageView is an image viewer for the X Window System, made to break through the hopeless situation of viewing huge numbers of images (and movies).
This GTK 4 port has been tested on the X Window System (X11).
On Wayland, the MPlayer and xine plugins cannot embed movie playback.
(GTK+ 1.2 / 2 versions also ran on the Linux frame buffer and Cygwin.)

## 1. Introduction

GImageView has following features:

* **Simple, but useful/flexible user interface:**
  Thumbnail window has 3 paned view (directory tree view, thumbnail view and image preview), and its layout is fully customizable by Drag and Drop. Images can be also displayed by separated image window.

* **Managing image files:**
  It can copy, move and link image files by Drag and Drop.

* **Low waste memory.**

* **Also useful for command line usage:**
  If you specify image files and directories from command line, image files are opened by image window, and directories are opened by thumbnail window automatically.

* **Tabbed browsing:**
  It can open two or more directories at same time by tabbed thumbnail window which looks like tabbed web browser. If you drop files to tab, you can copy, move and link files to the directory.

* **Supported following graphic formats:**

  * JPEG, PNG, BMP, PCX, TGA, MAG, XBM, XCF, xvpics (own loaders)

  * PNM (PBM, PGM, PPM), XPM, Sun raster, WBMP (own loaders, GTK 4 port)

  * SVG (librsvg is required)

  * MNG (libmng is required)

  * WMF (libwmf is required)

  * GIF, TIFF, ICO and other formats gdk-pixbuf can read

  * WebP, AVIF, HEIF, JPEG XL, ... (when their gdk-pixbuf loaders are installed)

  *Files of formats that no installed loader can read are not listed.*

  *Help -> About -> Plugin Info shows the supported file extensions.*

* It can play animation such as animation GIF and MNG.

* **Supports movie files and sound files** using GStreamer, xine-lib and MPlayer. Supported file types are depended on them (with GStreamer, on the installed GStreamer plugins):

  * MPEG, AVI, QuickTime, WMV/ASF, MP4, Matroska, WebM, FLV, Ogg, MPEG-TS, 3GP, ...

  * MP3, WAV, Ogg, FLAC, AAC, ... (audio)

* **It can extract compressed file automatically.**
  (File name extensions are not registered by default. Register them in Preferences > Common > Filtering.)

  * gzip, bzip2

* **Supports archive files** using libarchive (no external programs). Archives are expanded as virtual directories:

  * zip (cbz), rar (cbr), 7z (cb7), lha (lzh), tar (.tar.gz, .tar.bz2, .tar.xz, .tar.zst, ...), cab, iso

* **Supports various thumbnail cache types** (all read and write):

  * GImageView (its own format, ~/.gimv/thumbnail), Nautilus, Nautilus-2.0, Konqueror (also the old format), GQview, Electric Eyes (Picview), xvpics

  *The freedesktop.org Thumbnail Managing Standard (~/.cache/thumbnails) is not supported.*

* Supports slideshow.

* Six display modes of the thumbnail view (Album, Album2, Album3, Detail, Detail + Icon, Detail + Thumbnail), and various sort types.

* **Finding duplicated images:**
  It can find duplicated image files by file size, md5sum and similarity, and display it on tree view.

## 2. Requirements

You need following environment/libraries to build GImageView:

* A C compiler, Meson and Ninja (build time only)

* GTK 4.10 or later (<https://www.gtk.org/>)

* GLib 2.72 or later, gdk-pixbuf 2.0

* X Window System (X11) (the tested environment)

Following libraries and programs are optional. They extend the functionality when present:

* **libarchive 3.3 or later:** archives (zip, rar, 7z, lha, tar, ...) can be opened.

* **libjpeg:** JPEG images are loaded faster.

* **libpng:** PNG loader and PNG saver plugins.

* **libexif:** EXIF information can be displayed, and images are rotated by the EXIF orientation.

* **librsvg:** SVG images can be displayed.

* **libmng:** MNG animations can be played.

* **libwmf:** WMF images can be displayed.

* **zlib, bzip2:** gzip and bzip2 compressed files can be expanded.

* **GStreamer 1.16 or later, xine-lib 1.2 or later, or MPlayer:** movie and audio files can be played. With GStreamer, the formats depend on the installed plugins (gst-plugins-good, -bad, -ugly, gst-libav, ...).

* **Additional gdk-pixbuf loaders** (webp-pixbuf-loader, libavif-gdk-pixbuf, heif-gdk-pixbuf, libjxl-gdk-pixbuf, ...): WebP, AVIF, HEIF, JPEG XL, ... can be displayed.

* **GNU file:** the file properties dialog shows the file type.

The GTK 4 port is confirmed on Debian GNU/Linux sid (GTK 4.24).

## 3. Download

The original GImageView (up to 0.2.27) was published at following place. It is not updated any more:

* <https://github.com/ashie/gimageview>

The GTK 4 port is based on 0.2.27.

## 4. Install

The GTK 4 port is built with Meson. In the top directory of the source:

```
$ meson setup _build --prefix=/usr/local
$ ninja -C _build
$ sudo ninja -C _build install
```

Each feature is enabled automatically when its library is present. To specify them explicitly, add options to `meson setup` (see `meson_options.txt` for the list, and `INSTALL`):

```
$ meson setup _build -Dgstreamer=enabled -Dxine=enabled -Dsvg=enabled -Dexif=enabled -Dmplayer=true
```

On Debian, you can also build and install a package:

```
$ dpkg-buildpackage -us -uc -b
$ sudo apt install ../gimageview_*.deb
```

The preference files (~/.gimv) of the GTK+ 2 version can be used as they are, except ~/.gimv/gtkrc: the look is changed with ~/.gimv/gtk.css instead.

## 5. Usage

You can show one or more image files by following command:

```
$ gimv [Image files...]
```

If you specify one or more directories with a -d option, you can show thumbnails of all images under the directory by thumbnail window.

```
$ gimv -d [Directory name]
```

If you specify both images and directories with a -d option, images are opened by image window, and thumbnails under directories are opened by thumbnail window.

```
$ gimv -d foo01.jpg ~/ ~/images images/foo02.jpg \
     /usr/share/pixmaps/ bar.png ......
```

If you add a -t option, all images are opened by thumbnail window.

Followings are other options:

```
Usage: gimv [OPTION...] [Image Files...]

  -d, --directory          Scan a directory at startup
  -R, --recursive          Scan directories recursively (use with "-d")
  -D, --scan-dot           Read dotfiles when scanning directories (use with "-d")
  -e, --ignore-ext         Ignore file name extension
  -s, --scale=SCALE        Image scale in the image window [%]
  -b, --buffer=ON/OFF      Keep the original image in memory
  -M, --menubar            Show the menubar in the image window
  -T, --toolbar            Show the toolbar in the image window
  -I, --imagewin           Open an empty image window at startup
  -w, --thumbwin           Open the thumbnail window at startup
  -i, --imageview          Open all images in the image window
  -t, --thumbview          Open all images in the thumbnail window
  -S, --slideshow          Open the image files in a slideshow
  -W, --wait=TIME          Slideshow interval (use with "-S") [sec]
  -v, --version            Print version information
  -h, --help               Show this message
```

To open a directory from the GUI, double-click it in the directory view, or type its path in the directory entry of the thumbnail window toolbar and press Enter (a new tab is opened only if the directory has images). You can also select directories in the file open dialog and press "Thumbnails of the selected files".

To show compressed image files, open Preferences > Common > Filtering and register file name extensions like gz or bz2.

## 6. ChangeLog of README

* **2026/10/07(Wed) shitamo**

  * Updated to match the HTML manual (changes of the GTK 4 port, features, requirements, install).

  * Removed the list of environments the GTK+ 1.2 / 2 versions were confirmed on.

* **2026/10/06(Tue) shitamo**

  * Converted to Markdown (README.md).

* **2026/09/26(Sat) shitamo**

  * Follow the GTK 4 port (0.2.27+gtk4): requirements, install, formats.

* **2003/06/01(Thu) Takuro Ashie**

  * Fixed typo (English Version)

* **2003/05/22(Thu) Takuro Ashie**

  * Translated to English

  * Follow latest version (0.2.21).

  * Remove ChangeLog and TODO

* **2001/11/06(Tue) nyan2**

  * for version 0.1.3

  * add CVS section in "get gimageview" chapter

* **2001/11/04(Sun) Takuro Ashie**

  * Modified TODO

  * Added ChangeLog and minor fix.

* **2001/11/04(Sun) nyan2**

  * for version 0.1.2

* **2001/10/18(Thu) nyan2**

  * Minor change

* **2001/10/03(Wed) nyan2**

  * for version 0.1.1

* **2001/09/12(Wed) - 2001/09/28(Fri) nyan2**

  * for nightly

  * Follow modified features and bugfixes.

* **2001/09/10(Mon) nyan2**

  * for 2001/10/10 nightly

  * Initial import

* **2001/09/08(Sat) nyan2**

  * for 2001/09/08 nightly

  * Started editing.

## 7. License

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

## 8. Remarks

This document is modified by Nyan2 based on <http://www.homa.ne.jp/~ashie/gimageview/>.

* **Copyright (C) 2001-2003**

  * Original : Takuro Ashie

  * Modified : Nyan2 <t-nyan2@nifty.com>
