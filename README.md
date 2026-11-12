## Dataset out-of-the-box




## llvm-project

Fix and build llvm. 
llvm tools including `opt`, `clang` will be in `./llvm-project/llvm/build/bin/`

```bash
git clone https://github.com/llvm/llvm-project.git
cd llvm-project
git switch -c llvm20-fixes llvmorg-20.1.8
git apply ../llvm20.1.8-fixes.patch

cmake 	-G Ninja \
    	-S llvm    \
    	-B llvm/build   \
    	-DCMAKE_BUILD_TYPE=release \
    	-DLLVM_ENABLE_PROJECTS="clang;lld" \
    	-DCMAKE_INSTALL_PREFIX="./llvm/build/install" \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DLLVM_LINK_LLVM_DYLIB=ON \
        -DLLVM_USE_LINKER=lld \
        -DLLVM_TARGETS_TO_BUILD=X86 \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C llvm/build

```

The llvm20.1.8-fixes.patch makes three changes to LLVM:

1. **Register two loop passes.** It adds the `eslu` pass, which runs
   `SimpleLoopUnswitchPass` through an `ExtraLoopPassManager`, and the `luf`
   pass, which exposes `LoopFullUnrollPass` with an optimization-level option
   (`O0` through `O3`) in the pass pipeline.
2. **Form LCSSA after loop strength reduction.** Both the legacy and new pass
   manager implementations of `LoopStrengthReduce` now call
   `formLCSSARecursively` for the loops in the function after strength
   reduction, keeping loop-carried values in Loop-Closed SSA form.
3. **Give split basic blocks stable names.**
   `SplitBlockAndInsertIfThenElse` now names the generated blocks using the
   original head block name with `.splittail`, `.splitthen`, and `.splitelse`
   suffixes, making the resulting CFG easier to inspect and debug.


## Open-source C Code Repo

coreutils + openssl + ffmpeg + testsuite

```bash
sudo apt install bear -y

git clone https://git.sv.gnu.org/coreutils
cd coreutils
git checkout v9.11
./bootstrap
./configure \
    CC=clang \
    CFLAGS="-O2" \
    --prefix=$(pwd)/build/v9.11-clang-O2 \
    --disable-gcc-warnings \
    --with-openssl=no
bear -- make -j16
# ./coreutils/compile_commands.json

git clone https://github.com/openssl/openssl.git
cd openssl
git checkout openssl-3.6.2
./Configure \
    CC=clang \
    CFLAGS="-O2" \
    no-tests
bear -- make -j16
# ./openssl/compile_commands.json

git clone https://github.com/ffmpeg/ffmpeg.git
cd ffmpeg
git checkout n8.1.2
./configure \
    --cc=clang \
    --extra-cflags="-w" \
    --optflags="-O2" \
    --disable-x86asm 
bear -- make -j16
# ./ffmpeg/compile_commands.json

git clone https://github.com/llvm/llvm-test-suite.git
cd llvm-test-suite
git checkout release/20.x
cmake 	-G "Unix Makefiles" \
    	-S .    \
    	-B build   \
	    -DCMAKE_C_COMPILER=/home/dxic/llvmlearn/llvm-project/llvm/build/bin/clang \
        -DCMAKE_CXX_COMPILER=/home/dxic/llvmlearn/llvm-project/llvm/build/bin/clang++ \
    	-C./cmake/caches/O3.cmake \
        -DTEST_SUITE_RUN_BENCHMARKS=OFF \
        -DTEST_SUITE_COLLECT_CODE_SIZE=OFF
cd build
bear --output ../compile_commands.json -- make -j8
# ./llvm-test-suite/compile_commands.json
```

Use `extractor.ipynb` to generate `./<repo_name>260808.json`
These are the compile commands and arguments for compiling all the c files into object files.

## IR change records in LLVM default pipeline

Use `count.ipynb` to generate `./test/260811/O*/<testcase_id>.txt`
These are the LLVM IR changes records for all the "coreutils" c file compilation.


## Pass-replayable IR function sets

Use `getopt.ipynb` to generate `./test/260831/ll/<pass_name>/<testcase_id>-<func_name>-<1,2>.ll`.

These are the pass-replayable IR functions. 
One folder for each LLVM loop optimization pass.
* -1: IR function before the pass. IR changes and source code location informations are in the comments.
* -2: IR function after the pass.

## Deduplicate the function sets

TODO
Deduplicate the 10 function sets. 
Only keep one function among the functions with the same assembly codes. 

## Stratify and Sample the IR functions from pass-replayable IR functions sets.

Use `selectobj.ipynb` to generate `./test/260831/ratio2/`
Stratify the deduplicated function sets. 
Sample at most 1000 IR functions from the stratified function sets.
Compile them into object files.