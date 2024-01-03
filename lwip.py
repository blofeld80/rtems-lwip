#
# RTEMS Project (https://www.rtems.org/)
#
# Copyright (c) 2021 Vijay Kumar Banerjee <vijay@rtems.org>.
# All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#  1. Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from rtems_waf import rtems
import json
import os


def removeprefix(data, prefix):
    if data.startswith(prefix):
        return data[len(prefix):]
    return data


def build(bld):
    source_files = []
    driver_source = []
    drv_incl = []
    arch_lib_path = rtems.arch_bsp_lib_path(bld.env.RTEMS_VERSION,
                                            bld.env.RTEMS_ARCH_BSP)
    arch = rtems.arch(bld.env.RTEMS_ARCH_BSP)
    bsp = rtems.bsp(bld.env.RTEMS_ARCH_BSP)

    # file-import.json is kept separate from the rest of the defs because it
    # describes which files are imported from upstream lwip
    with open('file-import.json', 'r') as cf:
        files = json.load(cf)
        for f in files['files-to-import']:
            if f[-2:] == '.c':
                source_files.append(os.path.join('lwip', f))

    def walk_sources(path):
        return bld.path.ant_glob([path + '/**/*.c', path + '/**/*.S'])

    def import_json_definition(prefix, path):
        sources = []
        includes = []
        with open(os.path.join(prefix, path), 'r') as bspconfig:
            files = json.load(bspconfig)
            if 'includes' in files:
                for f in files['includes']:
                    tmpsrc, tmpincl = import_json_definition(prefix, f+'.json')
                    sources.extend(tmpsrc)
                    includes.extend(tmpincl)
            if 'source-files-to-import' in files:
                sources.extend(files['source-files-to-import'])
            if 'header-paths-to-import' in files:
                includes.extend(files['header-paths-to-import'])
        return (sources, includes)

    # import additional lwip source
    more_lwip_sources, common_includes = import_json_definition(
        'defs/common', 'lwip.json')
    source_files.extend(more_lwip_sources)

    # import bsp files
    driver_source, drv_incl = import_json_definition(
        os.path.join('defs/bsps', arch), bsp+'.json')

    lwip_obj_incl = []
    lwip_obj_incl.extend(drv_incl)
    lwip_obj_incl.extend(common_includes)

    bld(features='c',
        target='lwip_obj',
        cflags='-g -Wall -O0',
        includes=' '.join(lwip_obj_incl),
        source=source_files,
        )

    drv_obj_incl = []
    drv_obj_incl.extend(drv_incl)
    drv_obj_incl.extend(common_includes)

    bld(features='c',
        target='driver_obj',
        cflags='-g -Wall -O0',
        includes=' '.join(drv_obj_incl),
        source=driver_source,
        )
    bld(features='c cstlib',
        target='lwip',
        cflags='-g -Wall -O0',
        use=['lwip_obj', 'driver_obj'])
    bld.install_files("${PREFIX}/" + arch_lib_path, ["liblwip.a"])

    def install_headers(root_path):
        for root, dirs, files in os.walk(root_path):
            for name in files:
                ext = os.path.splitext(name)[1]
                src_root = os.path.split(root)
                path = os.path.join(src_root[0], src_root[1])
                if ext == '.h':
                    subpath = removeprefix(removeprefix(path, root_path), "/")
                    bld.install_files(
                        "${PREFIX}/" + arch_lib_path + "/include/" + subpath,
                        os.path.join(path, name)
                    )

    [install_headers(path) for path in common_includes]
    [install_headers(path) for path in drv_incl]

    test_app_incl = []
    test_app_incl.extend(drv_incl)
    test_app_incl.extend(common_includes)
    test_app_incl.append('rtemslwip/test/')
    bld.program(features='c',
                target='networking01.exe',
                source='rtemslwip/test/networking01/sample_app.c',
                cflags='-g -Wall -O0',
                use='lwip',
                lib=['rtemscpu', 'rtemsbsp', 'rtemstest', 'lwip'],
                includes=' '.join(test_app_incl))

    lib_path = os.path.join(bld.env.PREFIX, arch_lib_path)
    rtems_lib_path = os.path.join(bld.env.RTEMS_PATH, arch_lib_path)
    bld.read_stlib('telnetd', paths=[lib_path, rtems_lib_path])
    bld.read_stlib('rtemstest', paths=[lib_path, rtems_lib_path])
    bld.read_stlib('ftpd', paths=[lib_path, rtems_lib_path])

    bld.program(features='c',
                target='telnetd01.exe',
                source='rtemslwip/test/telnetd01/init.c',
                use='telnetd lwip rtemstest ftpd',
                cflags='-g -Wall -O0',
                includes=' '.join(test_app_incl))


def add_flags(flags, new_flags):
    for flag in new_flags:
        if flag not in flags:
            flags.append(flag)


def bsp_configure(conf, arch_bsp):
    conf.env.LIB += ['m']
    section_flags = ["-fdata-sections", "-ffunction-sections"]
    add_flags(conf.env.CFLAGS, section_flags)
    add_flags(conf.env.CXXFLAGS, section_flags)
