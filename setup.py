from skbuild import setup

setup(
    name="stat_arb_mm",
    version="0.1.0",
    description="High-performance C++20/Python Market Microstructure Statistical Arbitrage Engine",
    author="Diego",
    license="MIT",
    packages=["stat_arb_mm"],
    package_dir={"": "python"},
    # scikit-build will run CMake and install the resulting library here
    cmake_install_dir="python/stat_arb_mm",
    python_requires=">=3.11",
)