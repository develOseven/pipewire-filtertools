import ctypes
from pathlib import Path

_here = Path(__file__).resolve().parent
_lib_path = _here / "libpipewire-filtertools.so"
if not _lib_path.exists():
    raise RuntimeError(f"Shared library not found: {_lib_path}")

_lib = ctypes.CDLL(str(_lib_path))

# Function pointer type for buffer callbacks
PIPEWIRE_FILTERTOOLS_ON_PROCESS = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_uint32
)

# Bind C functions
_lib.pfts_init.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.pfts_init.restype = None

_lib.pfts_get_rate.argtypes = []
_lib.pfts_get_rate.restype = ctypes.c_uint32

_lib.pfts_main_loop_new.argtypes = []
_lib.pfts_main_loop_new.restype = ctypes.c_void_p

_lib.pfts_main_loop_run.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_bool,
    ctypes.c_uint32,
    ctypes.c_uint32,
    PIPEWIRE_FILTERTOOLS_ON_PROCESS,
]
_lib.pfts_main_loop_run.restype = ctypes.c_int

_lib.pfts_set_auto_link.argtypes = [ctypes.c_void_p, ctypes.c_bool]
_lib.pfts_set_auto_link.restype = None

_lib.pfts_main_loop_quit.argtypes = [ctypes.c_void_p]
_lib.pfts_main_loop_quit.restype = ctypes.c_int

_lib.pfts_main_loop_destroy.argtypes = [ctypes.c_void_p]
_lib.pfts_main_loop_destroy.restype = None

_lib.pfts_deinit.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.pfts_deinit.restype = None


# Expose Python-friendly wrapper functions
def init():
    _lib.pfts_init(None, None)


def get_rate():
    return _lib.pfts_get_rate()


def main_loop_new():
    return _lib.pfts_main_loop_new()


def main_loop_run(ctx, loop, name, auto_link, rate, quantum, on_process):
    _lib.pfts_main_loop_run(ctx, loop, name, auto_link, rate, quantum, on_process)


def set_auto_link(loop, auto_link):
    _lib.pfts_set_auto_link(loop, auto_link)


def main_loop_quit(loop):
    return _lib.pfts_main_loop_quit(loop)


def main_loop_destroy(loop):
    _lib.pfts_main_loop_destroy(loop)


def deinit():
    _lib.pfts_deinit(None, None)
