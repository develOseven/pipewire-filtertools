import shlex
import subprocess
from pathlib import Path
from setuptools.command.build_py import build_py as _build_py
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class build_py(_build_py):
    def run(self):
        src_dir = Path(__file__).parent / "pipewire_filtertools" / "pipewire-filtertools"
        dst_dir = Path(self.get_finalized_command("build_py").build_lib) / "pipewire_filtertools"
        dst_dir.mkdir(parents=True, exist_ok=True)

        libname = "libpipewire-filtertools.so"
        c_files = [src_dir / f for f in ["mainloop.c", "retargeting.c"]]
        libpath = dst_dir / libname

        print(f"Building {libname} from {c_files} -> {libpath}")

        # Collect pkg-config flags
        pkg_flags = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libpipewire-0.3"],
            text=True
        ).strip()

        cmd = ["gcc", "-shared", "-fPIC", "-O2", "-o", str(libpath)] + [str(f) for f in c_files] + shlex.split(pkg_flags)
        subprocess.check_call(cmd)
        print(f"Built {libpath}")

        super().run()


class bdist_wheel(_bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        # Force the wheel to be marked as platform-dependent
        self.root_is_pure = False
