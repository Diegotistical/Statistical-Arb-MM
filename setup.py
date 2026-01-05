from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import sys
import os

class get_pybind_include:
    def __str__(self):
        import pybind11
        return pybind11.get_include()

ext_modules = [
    Extension(
        'stat_arb_mm',
        sources=[
            'python/stat_arb_mm/bindings.cpp',
            'src/core/OrderBook.cpp',
        ],
        include_dirs=[
            get_pybind_include(),
            'src',
        ],
        language='c++',
        extra_compile_args=['-std=c++20', '-O3', '-march=native'] if sys.platform != 'win32' 
                           else ['/std:c++20', '/O2'],
    ),
]

class BuildExt(build_ext):
    def build_extensions(self):
        # Add C++20 flags
        for ext in self.extensions:
            if sys.platform == 'win32':
                ext.extra_compile_args = ['/std:c++20', '/O2', '/EHsc']
            else:
                ext.extra_compile_args = ['-std=c++20', '-O3', '-march=native', '-fPIC']
        build_ext.build_extensions(self)

setup(
    name='stat_arb_mm',
    version='1.0.0',
    ext_modules=ext_modules,
    cmdclass={'build_ext': BuildExt},
    zip_safe=False,
    python_requires='>=3.8',
)