project('libbic', 'c',
    version: '0.1',
    license: 'GPL2',
    default_options: ['werror=true'],
    meson_version: '>=0.40')

# OpenSSL3, which is present in Kirkstone and above, deprecates some
# SHA hash functions we are using.  Turn off the warning until we can
# port to OpenSSL3.
add_project_arguments('-Wno-error=deprecated-declarations', language: 'c')

cc = meson.get_compiler('c')

bic_headers = [
    'bic_xfer.h',
    'bic_ipmi.h',
    'bic_fwupdate.h',
    'bic.h',
    'bic_bios_fwupdate.h'
]

bic_sources = [
    'bic_xfer.c',
    'bic_ipmi.c',
    'bic_fwupdate.c',
    'bic.c',
    'bic_bios_usb_fwupdate.c'
]

bic_deps = [
    cc.find_library('ipmb'),
    cc.find_library('fbwc_common'),
    cc.find_library('usb-1.0'),
    cc.find_library('kv'),
    cc.find_library('obmc-i2c'),
    cc.find_library('gpio-ctrl'),
    cc.find_library('crypto'),
]

install_headers(bic_headers, subdir: 'facebook')

# BIC library.
bic_lib = shared_library('bic', bic_sources,
    dependencies: bic_deps,
    version: meson.project_version(),
    install: true)

# pkgconfig for BIC library.
pkg = import('pkgconfig')
pkg.generate(libraries: [bic_lib],
    name: meson.project_name(),
    version: meson.project_version(),
    description: 'BIC Library')
