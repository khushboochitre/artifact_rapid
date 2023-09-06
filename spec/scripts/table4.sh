#!/bin/bash
echo "Table 4: Rapid’s allocator CPU and memory overhead for CPU SPEC 2017 after handling memory allocations inside the loops."

echo
echo "Rapid: CPU overhead (8)"
./cpu_overhead_8.sh

echo
echo "Rapid: CPU overhead (16)"
./cpu_overhead_16.sh

echo
echo "Rapid: CPU overhead (32)"
./cpu_overhead_32.sh

echo
echo "Rapid: CPU overhead (128)"
./cpu_overhead_128.sh

echo
echo "Rapid: CPU overhead (256)"
./cpu_overhead_256.sh

echo
echo "Rapid: Memory overhead (8)"
./mem_overhead_8.sh

echo
echo "Rapid: Memory overhead (16)"
./mem_overhead_16.sh

echo
echo "Rapid: Memory overhead (32)"
./mem_overhead_32.sh

echo
echo "Rapid: Memory overhead (128)"
./mem_overhead_128.sh

echo
echo "Rapid: Memory overhead (256)"
./mem_overhead_256.sh
