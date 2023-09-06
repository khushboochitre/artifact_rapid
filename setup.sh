#Installing dependencies.
sudo apt-get install build-essential
sudo apt-get install cmake
sudo apt-get install autoconf
sudo apt-get install -y libffi-dev
sudo apt-get install ninja-build

#Move clang folders to the required locations.
cd $HOME/artifact_rapid/
cp -r clang_rapid llvm_rapid/tools/clang
cp -r clang_scout llvm_scout/tools/clang
cp -r clang_3.6.0 llvm-3.6.0/tools/clang

#Setting up RAPID.
cd llvm_rapid/build/
./build.sh
ninja
cd ../../

#Setting up the region-based allocator.
cd mimalloc_prof/
cmake .
make
cd ../

cd mimalloc_region/
cmake .
make
cd ../

#Setting up SCOUT.
cd llvm_scout/build/
./build.sh
ninja
cd ../../

#Setting up the segment-based allocator.
cd jemallocn/
./autogen.sh
make -j
cd ../

cd jemalloc2k/
./autogen.sh
make -j
cd ../

#Setting up LLVM-3.6.0.
cd llvm-3.6.0/build/
./build.sh
ninja
cd ../../

#Create a folders to run the benchmarks.
mkdir $HOME/stats
mkdir $HOME/artifact_rapid/polybench-c-4.2.1-beta/exe
mkdir $HOME/artifact_rapid/polybench-c-4.2.1-beta_1/exe