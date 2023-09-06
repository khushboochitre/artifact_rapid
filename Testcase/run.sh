#!/bin/bash
MIMALLCOC="$HOME/artifact_rapid/mimalloc_region"
MIMALLCOC_PROF="$HOME/artifact_rapid/mimalloc_prof"
export LD_LIBRARY_PATH=$MIMALLCOC:$MIMALLCOC_PROF:$LD_LIBRARY_PATH

LLVM_ROOT_PATH=$HOME/artifact_rapid/llvm_rapid/build
CLANG=$LLVM_ROOT_PATH/bin/clang

RAPID_PROF_FLAGS="-O3 $LLVM_ROOT_PATH/lib/Transforms/Vectorize/CMakeFiles/LLVMVectorize.dir/ProfileFunctions.cpp.o -g -mllvm -enable-wrapper-annotation -mllvm -instrument-dynamic-checks"
NATIVE_FLAGS="-O3 -mllvm -disable-additional-vectorize -g"
RAPID_FLAGS="-O3 -mllvm -disable-additional-vectorize -g -mllvm -region-wrapper-annotation -mllvm -use-check-info"

MI_PROF_LIB_FLAGS="-L$MIMALLCOC_PROF -lmimalloc1 -lm -z muldefs"
MI_LIB_FLAGS="-L$MIMALLCOC -lmimalloc -lm -z muldefs"

#Profiler.
$CLANG $RAPID_PROF_FLAGS -mllvm test1 test1.c -o test1 $MI_PROF_LIB_FLAGS
./test1 > out1.txt 2>&1

$CLANG $RAPID_PROF_FLAGS -mllvm test2 test2.c -o test2 $MI_PROF_LIB_FLAGS
./test2 > out2.txt 2>&1

$CLANG $RAPID_PROF_FLAGS -mllvm test3 test3.c -o test3 $MI_PROF_LIB_FLAGS
./test3 > out3.txt 2>&1

#Native.
echo "Native"
$CLANG $NATIVE_FLAGS test1.c -o test1 $MI_LIB_FLAGS
time ./test1 > out1.txt 2>&1

$CLANG $NATIVE_FLAGS test2.c -o test2 $MI_LIB_FLAGS
time ./test2 > out2.txt 2>&1

$CLANG $NATIVE_FLAGS test3.c -o test3 $MI_LIB_FLAGS
time ./test3 > out3.txt 2>&1

#Rapid.
echo "Rapid"
$CLANG $RAPID_FLAGS -mllvm test1 test1.c -o test1 $MI_LIB_FLAGS
time ./test1 > out1.txt 2>&1

$CLANG $RAPID_FLAGS -mllvm test2 test2.c -o test2 $MI_LIB_FLAGS
time ./test2 > out2.txt 2>&1

$CLANG -mllvm -region-threshold -mllvm 8 $RAPID_FLAGS -mllvm test3 test3.c -o test3 $MI_LIB_FLAGS
time ./test3 > out3.txt 2>&1

