#!/bin/sh
# Validate VirtIO-GPU modes in a MOS guest; --modeset requires inactive Xorg.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-virtgpu_modes.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import argparse
import ctypes as C
import errno
import os


U32, U64, U16 = C.c_uint32, C.c_uint64, C.c_uint16


class Mode(C.Structure):
    _fields_ = [("clock", U32)] + [(name, U16) for name in (
        "hdisplay", "hsync_start", "hsync_end", "htotal", "hskew",
        "vdisplay", "vsync_start", "vsync_end", "vtotal", "vscan",
    )] + [("vrefresh", U32), ("flags", U32), ("type", U32),
         ("name", C.c_char * 32)]


class Resources(C.Structure):
    _fields_ = [(name, U64) for name in (
        "fb_id_ptr", "crtc_id_ptr", "connector_id_ptr", "encoder_id_ptr",
    )] + [(name, U32) for name in (
        "count_fbs", "count_crtcs", "count_connectors", "count_encoders",
        "min_width", "max_width", "min_height", "max_height",
    )]


class Connector(C.Structure):
    _fields_ = [(name, U64) for name in (
        "encoders_ptr", "modes_ptr", "props_ptr", "prop_values_ptr",
    )] + [(name, U32) for name in (
        "count_modes", "count_props", "count_encoders", "encoder_id",
        "connector_id", "connector_type", "connector_type_id", "connection",
        "mm_width", "mm_height", "subpixel", "pad",
    )]


class Crtc(C.Structure):
    _fields_ = [("set_connectors_ptr", U64)] + [(name, U32) for name in (
        "count_connectors", "crtc_id", "fb_id", "x", "y", "gamma_size",
        "mode_valid",
    )] + [("mode", Mode)]


class Dumb(C.Structure):
    _fields_ = [(name, U32) for name in (
        "height", "width", "bpp", "flags", "handle", "pitch",
    )] + [("size", U64)]


class Framebuffer(C.Structure):
    _fields_ = [(name, U32) for name in (
        "fb_id", "width", "height", "pitch", "bpp", "depth", "handle",
    )]


def ioctl(fd, number, value=None):
    request = (ord("d") << 8) | number
    if value is not None:
        request |= (3 << 30) | (C.sizeof(value) << 16)
    libc = C.CDLL(None, use_errno=True)
    libc.ioctl.argtypes = [C.c_int, C.c_ulong, C.c_void_p]
    libc.ioctl.restype = C.c_int
    if libc.ioctl(fd, request, C.byref(value) if value is not None else None) < 0:
        error = C.get_errno()
        raise OSError(error, os.strerror(error))
    return value


def current_mode(fd, crtc):
    return ioctl(fd, 0xA1, Crtc(crtc_id=crtc))


def mode_state(value):
    return (value.fb_id, value.x, value.y, value.mode_valid, bytes(value.mode))


def modeset_checks(fd, connector, crtc, modes):
    ioctl(fd, 0x1E)
    assert not current_mode(fd, crtc).mode_valid, "Stop Xorg before modeset checks"
    dumb = ioctl(fd, 0xB2, Dumb(width=1600, height=1200, bpp=32))
    fb = None
    try:
        fb = ioctl(fd, 0xAE, Framebuffer(
            width=dumb.width, height=dumb.height, pitch=dumb.pitch,
            bpp=32, depth=24, handle=dumb.handle,
        ))
        connector_id = U32(connector)
        for width, height in ((640, 480), (1280, 720), (800, 600)):
            mode = next(m for m in modes if (m.hdisplay, m.vdisplay) == (width, height))
            request = Crtc(set_connectors_ptr=C.addressof(connector_id),
                           count_connectors=1, crtc_id=crtc, fb_id=fb.fb_id,
                           x=16, y=24, mode_valid=1, mode=mode)
            ioctl(fd, 0xA2, request)
            assert mode_state(current_mode(fd, crtc)) == mode_state(request)

        # A valid custom mode need not occur in the connector's mode list.
        custom = Mode(clock=52000, hdisplay=997, hsync_start=1045,
                      hsync_end=1077, htotal=1157, vdisplay=701,
                      vsync_start=704, vsync_end=709, vtotal=761,
                      vrefresh=59, flags=5, name=b"997x701")
        request.mode = custom
        ioctl(fd, 0xA2, request)
        assert mode_state(current_mode(fd, crtc)) == mode_state(request)

        # Invalid requests must leave the committed framebuffer and mode intact.
        baseline = mode_state(current_mode(fd, crtc))
        for field, value in (("x", 0xFFFFFFFF), ("y", 1200),
                             ("count_connectors", 0), ("fb_id", 0),
                             ("mode.hdisplay", 0), ("mode.hdisplay", 4097),
                             ("mode.clock", 0), ("mode.flags", 16),
                             ("mode.hsync_end", 1)):
            invalid = Crtc.from_buffer_copy(request)
            target = invalid
            if field.startswith("mode."):
                target = invalid.mode
                field = field.split(".")[1]
            setattr(target, field, value)
            try:
                ioctl(fd, 0xA2, invalid)
            except OSError as error:
                assert error.errno == errno.EINVAL, error
            else:
                raise AssertionError("Invalid modeset succeeded")
            assert mode_state(current_mode(fd, crtc)) == baseline

        ioctl(fd, 0xA2, Crtc(crtc_id=crtc))
        disabled = current_mode(fd, crtc)
        assert not disabled.mode_valid and not disabled.fb_id
        assert bytes(disabled.mode) == bytes(Mode())
        assert disabled.x == disabled.y == 0
    finally:
        try:
            ioctl(fd, 0xA2, Crtc(crtc_id=crtc))
            if fb is not None:
                ioctl(fd, 0xAF, U32(fb.fb_id))
            ioctl(fd, 0xB4, U32(dumb.handle))
        finally:
            ioctl(fd, 0x1F)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/dri/card0")
    parser.add_argument("--expect-preferred", metavar="WIDTHxHEIGHT")
    parser.add_argument("--modeset", action="store_true",
                        help="Exercise mode switching as root with Xorg stopped")
    args = parser.parse_args()
    assert [C.sizeof(t) for t in (Mode, Resources, Connector, Crtc, Dumb, Framebuffer)] == [68, 64, 80, 104, 32, 28]
    fd = os.open(args.device, os.O_RDWR)
    try:
        resources = ioctl(fd, 0xA0, Resources())
        assert resources.count_connectors == resources.count_crtcs == 1
        connectors, crtcs = (U32 * 1)(), (U32 * 1)()
        resources.connector_id_ptr = C.addressof(connectors)
        resources.crtc_id_ptr = C.addressof(crtcs)
        resources.count_encoders = resources.count_fbs = 0
        ioctl(fd, 0xA0, resources)
        connector = ioctl(fd, 0xA7, Connector(connector_id=connectors[0]))
        assert connector.count_modes > 1
        sentinel = Mode()
        C.memset(C.byref(sentinel), 0xA5, C.sizeof(sentinel))
        short = Connector(connector_id=connectors[0], count_modes=1,
                          modes_ptr=C.addressof(sentinel))
        ioctl(fd, 0xA7, short)
        assert bytes(sentinel) == b"\xa5" * C.sizeof(sentinel)
        modes = (Mode * connector.count_modes)()
        connector.modes_ptr = C.addressof(modes)
        connector.count_encoders = 0
        ioctl(fd, 0xA7, connector)
        assert connector.count_modes == len(modes)
        keys = [(m.hdisplay, m.vdisplay, m.vrefresh) for m in modes]
        assert len(set(keys)) == len(keys)
        preferred = [m for m in modes if m.type & (1 << 3)]
        assert len(preferred) == 1
        if args.expect_preferred:
            assert args.expect_preferred == f"{preferred[0].hdisplay}x{preferred[0].vdisplay}"
        assert {(640, 480, 60), (1920, 1080, 120), (2560, 1440, 120),
                (3840, 2160, 60)} <= set(keys)
        for mode in modes:
            assert 0 < mode.hdisplay < mode.hsync_start < mode.hsync_end <= mode.htotal
            assert 0 < mode.vdisplay < mode.vsync_start < mode.vsync_end <= mode.vtotal
            assert abs(mode.clock * 1000 / mode.htotal / mode.vtotal - mode.vrefresh) < 0.01
        if args.modeset:
            modeset_checks(fd, connectors[0], crtcs[0], modes)
        print("VirtIO-GPU mode checks passed")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
