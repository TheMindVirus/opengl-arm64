from __future__ import print_function

CopyRight = '''
/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
'''


import sys

from u_format_parse import *
import u_format_pack


def layout_map(layout):
    return 'UTIL_FORMAT_LAYOUT_' + str(layout).upper()


def colorspace_map(colorspace):
    return 'UTIL_FORMAT_COLORSPACE_' + str(colorspace).upper()


colorspace_channels_map = {
    'rgb': ['r', 'g', 'b', 'a'],
    'srgb': ['sr', 'sg', 'sb', 'a'],
    'zs': ['z', 's'],
    'yuv': ['y', 'u', 'v'],
}


type_map = {
    VOID:     "UTIL_FORMAT_TYPE_VOID",
    UNSIGNED: "UTIL_FORMAT_TYPE_UNSIGNED",
    SIGNED:   "UTIL_FORMAT_TYPE_SIGNED",
    FIXED:    "UTIL_FORMAT_TYPE_FIXED",
    FLOAT:    "UTIL_FORMAT_TYPE_FLOAT",
}


def bool_map(value):
    if value:
        return "TRUE"
    else:
        return "FALSE"


swizzle_map = {
    SWIZZLE_X:    "PIPE_SWIZZLE_X",
    SWIZZLE_Y:    "PIPE_SWIZZLE_Y",
    SWIZZLE_Z:    "PIPE_SWIZZLE_Z",
    SWIZZLE_W:    "PIPE_SWIZZLE_W",
    SWIZZLE_0:    "PIPE_SWIZZLE_0",
    SWIZZLE_1:    "PIPE_SWIZZLE_1",
    SWIZZLE_NONE: "PIPE_SWIZZLE_NONE",
}


def write_format_table(formats):
    print('/* This file is autogenerated by u_format_table.py from u_format.csv. Do not edit directly. */')
    print()
    # This will print the copyright message on the top of this file
    print(CopyRight.strip())
    print()
    print('#include "u_format.h"')
    print('#include "u_format_bptc.h"')
    print('#include "u_format_s3tc.h"')
    print('#include "u_format_rgtc.h"')
    print('#include "u_format_latc.h"')
    print('#include "u_format_etc.h"')
    print()
    
    u_format_pack.generate(formats)
    
    def do_channel_array(channels, swizzles):
        print("   {")
        for i in range(4):
            channel = channels[i]
            if i < 3:
                sep = ","
            else:
                sep = ""
            if channel.size:
                print("      {%s, %s, %s, %u, %u}%s\t/* %s = %s */" % (type_map[channel.type], bool_map(channel.norm), bool_map(channel.pure), channel.size, channel.shift, sep, "xyzw"[i], channel.name))
            else:
                print("      {0, 0, 0, 0, 0}%s" % (sep,))
        print("   },")

    def do_swizzle_array(channels, swizzles):
        print("   {")
        for i in range(4):
            swizzle = swizzles[i]
            if i < 3:
                sep = ","
            else:
                sep = ""
            try:
                comment = colorspace_channels_map[format.colorspace][i]
            except (KeyError, IndexError):
                comment = 'ignored'
            print("      %s%s\t/* %s */" % (swizzle_map[swizzle], sep, comment))
        print("   },")

    for format in formats:
        sn = format.short_name()

        print('const struct util_format_description')
        print('util_format_%s_description = {' % (sn,))
        print("   %s," % (format.name,))
        print("   \"%s\"," % (format.name,))
        print("   \"%s\"," % (sn,))
        print("   {%u, %u, %u, %u},\t/* block */" % (format.block_width, format.block_height, format.block_depth, format.block_size()))
        print("   %s," % (layout_map(format.layout),))
        print("   %u,\t/* nr_channels */" % (format.nr_channels(),))
        print("   %s,\t/* is_array */" % (bool_map(format.is_array()),))
        print("   %s,\t/* is_bitmask */" % (bool_map(format.is_bitmask()),))
        print("   %s,\t/* is_mixed */" % (bool_map(format.is_mixed()),))
        print("   %s,\t/* is_unorm */" % (bool_map(format.is_unorm()),))
        print("   %s,\t/* is_snorm */" % (bool_map(format.is_snorm()),))
        u_format_pack.print_channels(format, do_channel_array)
        u_format_pack.print_channels(format, do_swizzle_array)
        print("   %s," % (colorspace_map(format.colorspace),))

        # We don't generate code for YUV formats, and many of the new ones lack pack/unpack
        # functions for softpipe/llvmpipe.
        noaccess_formats = [
            'yv12',
            'yv16',
            'iyuv',
            'nv12',
            'nv16',
            'nv21',
            'p010',
            'p012',
            'p016',
            'xyuv',
            'ayuv',
            'r8g8_r8b8_unorm',
            'g8r8_b8r8_unorm',
            'g8r8_g8b8_unorm',
            'y8_u8_v8_422_unorm',
            'y8_u8v8_422_unorm',
            'y8_u8_v8_444_unorm',
            'y16_u16_v16_420_unorm',
            'y16_u16_v16_422_unorm',
            'y16_u16v16_422_unorm',
            'y16_u16_v16_444_unorm',
        ]
        access = sn not in noaccess_formats
        if format.layout in ('astc', 'atc', 'fxt1'):
            access = False
        if format.layout == 'etc' and sn != 'etc1_rgb8':
            access = False
        if format.colorspace != ZS and not format.is_pure_color() and access:
            print("   .unpack_rgba_8unorm = &util_format_%s_unpack_rgba_8unorm," % sn)
            print("   .pack_rgba_8unorm = &util_format_%s_pack_rgba_8unorm," % sn)
            if format.layout == 's3tc' or format.layout == 'rgtc':
                print("   .fetch_rgba_8unorm = &util_format_%s_fetch_rgba_8unorm," % sn)
            print("   .unpack_rgba = &util_format_%s_unpack_rgba_float," % sn)
            print("   .pack_rgba_float = &util_format_%s_pack_rgba_float," % sn)
            print("   .fetch_rgba_float = &util_format_%s_fetch_rgba_float," % sn)

        if format.has_depth():
            print("   .unpack_z_32unorm = &util_format_%s_unpack_z_32unorm," % sn)
            print("   .pack_z_32unorm = &util_format_%s_pack_z_32unorm," % sn)
            print("   .unpack_z_float = &util_format_%s_unpack_z_float," % sn)
            print("   .pack_z_float = &util_format_%s_pack_z_float," % sn)

        if format.has_stencil():
            print("   .unpack_s_8uint = &util_format_%s_unpack_s_8uint," % sn)
            print("   .pack_s_8uint = &util_format_%s_pack_s_8uint," % sn)

        if format.is_pure_unsigned():
            print("   .unpack_rgba = &util_format_%s_unpack_unsigned," % sn)
            print("   .pack_rgba_uint = &util_format_%s_pack_unsigned," % sn)
            print("   .pack_rgba_sint = &util_format_%s_pack_signed," % sn)
            print("   .fetch_rgba_uint = &util_format_%s_fetch_unsigned," % sn)
        elif format.is_pure_signed():
            print("   .pack_rgba_uint = &util_format_%s_pack_unsigned," % sn)
            print("   .unpack_rgba = &util_format_%s_unpack_signed," % sn)
            print("   .pack_rgba_sint = &util_format_%s_pack_signed," % sn)
            print("   .fetch_rgba_sint = &util_format_%s_fetch_signed," % sn)
        print("};")
        print()
        
    print("const struct util_format_description *")
    print("util_format_description(enum pipe_format format)")
    print("{")
    print("   if (format >= PIPE_FORMAT_COUNT) {")
    print("      return NULL;")
    print("   }")
    print()
    print("   switch (format) {")
    for format in formats:
        print("   case %s:" % format.name)
        print("      return &util_format_%s_description;" % (format.short_name(),))
    print("   default:")
    print("      return NULL;")
    print("   }")
    print("}")
    print()


def main():

    formats = []
    for arg in sys.argv[1:]:
        formats.extend(parse(arg))
    write_format_table(formats)


if __name__ == '__main__':
    main()
