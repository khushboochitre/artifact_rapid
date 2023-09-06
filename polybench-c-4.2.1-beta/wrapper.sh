#!/bin/bash
POLYBENCH=$HOME/artifact_rapid/polybench-c-4.2.1-beta
LLVM_ROOT_PATH=$HOME/artifact_rapid/llvm_rapid/build
CLANG=$LLVM_ROOT_PATH/bin/clang
OPT=$LLVM_ROOT_PATH/bin/opt
LINK=$LLVM_ROOT_PATH/bin/llvm-link
EXE_FILES=$POLYBENCH/exe
CUSTOM_FLAGS="-identify-wrappers -disable-additional-vectorize"
NATIVE_FLAGS="-g -O3 -emit-llvm -S -mllvm -disable-additional-vectorize"

BENCHMARK="gesummv 2mm 3mm bicg doitgen jacobi-1d jacobi-2d"
: 'BENCHMARK="correlation covariance gemm gemver gesummv symm syr2k syrk trmm 2mm 3mm atax bicg doitgen mvt cholesky durbin gramschmidt lu ludcmp trisolv deriche floyd-warshall nussinov adi fdtd-2d heat-3d jacobi-1d jacobi-2d seidel-2d"'

for val in $BENCHMARK
do
	if [ $val == "correlation" ] || [ $val == "covariance" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/datamining/$val $POLYBENCH/datamining/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/datamining/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/datamining/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/datamining/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/datamining/$val/$val$MERGED -o $POLYBENCH/datamining/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/datamining/$val/out.bc -o $POLYBENCH/datamining/$val/out_1.bc
  elif [ $val == "gemm" ] || [ $val == "gemver" ] || [ $val == "gesummv" ] || [ $val == "symm" ] || [ $val == "syr2k" ] || [ $val == "syrk" ] || [ $val == "trmm" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/blas/$val $POLYBENCH/linear-algebra/blas/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/linear-algebra/blas/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/linear-algebra/blas/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/linear-algebra/blas/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/linear-algebra/blas/$val/$val$MERGED -o $POLYBENCH/linear-algebra/blas/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/linear-algebra/blas/$val/out.bc -o $POLYBENCH/linear-algebra/blas/$val/out_1.bc
        
  elif [ $val == "2mm" ] || [ $val == "3mm" ] || [ $val == "atax" ] || [ $val == "bicg" ] || [ $val == "doitgen" ] || [ $val == "mvt" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/kernels/$val $POLYBENCH/linear-algebra/kernels/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/linear-algebra/kernels/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/linear-algebra/kernels/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/linear-algebra/kernels/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/linear-algebra/kernels/$val/$val$MERGED -o $POLYBENCH/linear-algebra/kernels/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/linear-algebra/kernels/$val/out.bc -o $POLYBENCH/linear-algebra/kernels/$val/out_1.bc

  elif [ $val == "cholesky" ] || [ $val == "durbin" ] || [ $val == "gramschmidt" ] || [ $val == "lu" ] || [ $val == "ludcmp" ] || [ $val == "trisolv" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/solvers/$val $POLYBENCH/linear-algebra/solvers/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/linear-algebra/solvers/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/linear-algebra/solvers/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/linear-algebra/solvers/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/linear-algebra/solvers/$val/$val$MERGED -o $POLYBENCH/linear-algebra/solvers/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/linear-algebra/solvers/$val/out.bc -o $POLYBENCH/linear-algebra/solvers/$val/out_1.bc
  elif [ $val == "deriche" ] || [ $val == "floyd-warshall" ] || [ $val == "nussinov" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/medley/$val $POLYBENCH/medley/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/medley/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/medley/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/medley/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/medley/$val/$val$MERGED -o $POLYBENCH/medley/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/medley/$val/out.bc -o $POLYBENCH/medley/$val/out_1.bc
  elif [ $val == "adi" ] || [ $val == "fdtd-2d" ] || [ $val == "heat-3d" ] || [ $val == "jacobi-1d" ] || [ $val == "jacobi-2d" ] || [ $val == "seidel-2d" ];
    then
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/stencils/$val $POLYBENCH/stencils/$val/$val.c -DPOLYBENCH_TIME -o $POLYBENCH/stencils/$val/$val.bc
        $CLANG $NATIVE_FLAGS -I utilities $POLYBENCH/utilities/polybench.c -DPOLYBENCH_TIME -o $POLYBENCH/utilities/polybench.bc
        $LINK $POLYBENCH/stencils/$val/$val.bc $POLYBENCH/utilities/polybench.bc -o $POLYBENCH/stencils/$val/$val$MERGED
        $OPT -O0 $POLYBENCH/stencils/$val/$val$MERGED -o $POLYBENCH/stencils/$val/out.bc
        $OPT $CUSTOM_FLAGS $POLYBENCH/stencils/$val/out.bc -o $POLYBENCH/stencils/$val/out_1.bc
  else
            echo "Please enter valid benchmark name."
	fi
done
